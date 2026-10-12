// netsh.so (M12): socket-шима поверх MiLin IPC. LD_PRELOAD для
// динамического xbps: socket/connect/send/recv/close уезжают в сервис
// "net0" (netd) lane'ом по шине, poll/read ждут push'ей NET_DATA.
// Микроядро честно: ни одного socket-сисколла ядру про TCP — только IPC.

#define _GNU_SOURCE
#include <arpa/inet.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <netdb.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

// --- wire: зеркало userspace/mlproto.h (glibc-мир, свои typedef'ы) ---
typedef uint32_t u32;
typedef uint64_t u64;
typedef uint8_t u8;
typedef uint16_t u16;
#define ML_HEAD 24
#define ML_MAXTAIL 256
struct ml_msg {
    u32 proto, msg_id, err, tail_len;
    char head[ML_HEAD];
};
static inline u64 *ml_h(struct ml_msg *m, int i) { return (u64 *)(m->head + i * 8); }

#define ML_PROTO_BUS 1
#define BUS_LOOKUP   3
#define ML_PROTO_NET 4
#define NET_OPEN      1
#define NET_OPEN_R    2
#define NET_CONNECT   3
#define NET_CONNECT_R 4
#define NET_SEND      5
#define NET_SEND_R    6
#define NET_CLOSE     7
#define NET_CLOSE_R   8
#define NET_DATA      9
#define NET_EOF       10
#define ML_TR_SEQPACKET 0
#define ML_E_OK 0

struct iovec_ { void *base; u64 len; };
struct msghdr_ {
    void *name; u32 namelen;
    struct iovec_ *iov; u64 iovlen;
    void *ctrl; u64 ctrllen;
    int flags;
};
struct cmsghdr_ { u64 len; int level, type; };
#define CMSG_ALIGN_(a) (((a) + sizeof(u64) - 1) & ~(sizeof(u64) - 1))
#define CMSG_LEN_(a) (CMSG_ALIGN_(sizeof(struct cmsghdr_)) + (a))

static ssize_t (*real_read)(int, void *, size_t);
static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_send)(int, const void *, size_t, int);
static ssize_t (*real_recv)(int, void *, size_t, int);
static int (*real_close)(int);
static int (*real_fcntl)(int, int, ...);
static int (*real_poll)(struct pollfd *, nfds_t, int);
static int (*real_socket)(int, int, int);
static int (*real_connect)(int, const struct sockaddr *, socklen_t);
static int (*real_setsockopt)(int, int, int, const void *, socklen_t);
static int (*real_getsockopt)(int, int, int, void *, socklen_t *);
static ssize_t (*real_recvfrom)(int, void *, size_t, int,
                                struct sockaddr *, socklen_t *);

// --- таблица сокетов ---
#define NSOCK 16
struct nsock {
    int used;              // слот занят
    int efd;               // событийный fd (он же "дескриптор" сокета)
    u32 sid;               // идентификатор на стороне netd
    int state;             // 0=new 1=connecting 2=est 3=eof 4=err
    int err;
    u8 *rx; size_t rxlen, rxsz;
    int wait_id;           // какой R-ответ ждём (-1 = нет)
    int done;              // ответ пришёл
    struct ml_msg resp;
    u8 resp_tail[ML_MAXTAIL];
    u32 resp_tlen;
    pthread_mutex_t m;
    pthread_cond_t c;
};
static struct nsock S[NSOCK];
static pthread_mutex_t tab_m = PTHREAD_MUTEX_INITIALIZER;
static int lane = -1;      // lane к netd
static int inited;

static long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// --- обмен с шиной (одноразовый конструктор) ---
static long bus_lookup_net(void) {
    int s = real_socket(AF_UNIX, SOCK_SEQPACKET, 0);
    if (s < 0) return -1;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    a.sun_path[0] = 0;
    const char *name = "milin-bus";
    int i;
    for (i = 0; name[i] && i < 100; i++) a.sun_path[1 + i] = name[i];
    a.sun_path[1 + i] = 0;
    socklen_t al = 4 + i + 1;
    for (int t = 0; t < 100; t++) {
        if (real_connect(s, (struct sockaddr *)&a, al) == 0) break;
        usleep(20000);
        if (t == 99) { real_close(s); return -1; }
    }
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    strcpy(q.head, "net0");
    *ml_h(&q, 2) = ML_TR_SEQPACKET;
    for (int t = 0; t < 100; t++) {
        if (send(s, &q, sizeof(q), MSG_NOSIGNAL) < 0) break;
        char cbuf[256];
        struct iovec_ iov = { &q, sizeof(q) };
        struct msghdr_ mh;
        memset(&mh, 0, sizeof(mh));
        mh.iov = &iov; mh.iovlen = 1;
        mh.ctrl = cbuf; mh.ctrllen = sizeof(cbuf);
        ssize_t n = recvmsg(s, (struct msghdr *)&mh, 0);
        if (n <= 0) break;
        if (q.err == ML_E_OK) {
            for (struct cmsghdr_ *c = (struct cmsghdr_ *)cbuf;
                 (char *)c + sizeof(*c) <= cbuf + mh.ctrllen;
                 c = (struct cmsghdr_ *)((char *)c + CMSG_ALIGN_(c->len))) {
                if (c->len < CMSG_LEN_(4) || c->level != SOL_SOCKET) continue;
                if (c->type == SCM_RIGHTS) {
                    int fd;
                    memcpy(&fd, (char *)c + CMSG_ALIGN_(sizeof(struct cmsghdr_)), 4);
                    return fd;      // lane к netd
                }
            }
        }
        usleep(20000);
    }
    real_close(s);
    return -1;
}

// --- reader: единственный читатель lane'а ---
static void *reader(void *arg) {
    (void)arg;
    for (;;) {
        struct ml_msg m;
        u8 tail[ML_MAXTAIL];
        struct iovec_ iov[2];
        int niov = 0;
        iov[niov].base = &m; iov[niov].len = sizeof(m); niov++;
        iov[niov].base = tail; iov[niov].len = sizeof(tail); niov++;
        struct msghdr_ mh;
        memset(&mh, 0, sizeof(mh));
        mh.iov = iov; mh.iovlen = 2;
        ssize_t r = recvmsg(lane, (struct msghdr *)&mh, 0);
        if (r <= 0) return 0;
        u32 tlen = m.tail_len;
        if (tlen > ML_MAXTAIL) tlen = ML_MAXTAIL;
        u32 sid = (u32)*ml_h(&m, 0);

        fprintf(stderr, "netsh: reader msg id=%u sid=%u err=%u tlen=%u\n",
                m.msg_id, sid, m.err, tlen);
        pthread_mutex_lock(&tab_m);
        struct nsock *k = 0;
        if (m.msg_id >= NET_OPEN_R && m.msg_id <= NET_CLOSE_R) {
            // R-ответы ищем по ждущему слоту (OPEN_R приходит ДО used=1)
            for (int i = 0; i < NSOCK; i++)
                if (S[i].used && S[i].wait_id == (int)m.msg_id) { k = &S[i]; break; }
        } else {
            // push'и адресуются sid'ом netd, а не слотом таблицы
            for (int i = 0; i < NSOCK; i++)
                if (S[i].used && S[i].sid == sid) { k = &S[i]; break; }
        }
        if (k) {
            if (m.msg_id >= NET_OPEN_R && m.msg_id <= NET_CLOSE_R
                && k->wait_id == (int)m.msg_id) {
                k->resp = m;
                k->resp_tlen = tlen;
                memcpy(k->resp_tail, tail, tlen);
                k->done = 1;
                pthread_cond_broadcast(&k->c);
            } else if (m.msg_id == NET_DATA && tlen) {
                if (k->rxlen + tlen > k->rxsz) {
                    size_t nsz = k->rxsz ? k->rxsz * 2 : 8192;
                    while (nsz < k->rxlen + tlen) nsz *= 2;
                    u8 *nr = realloc(k->rx, nsz);
                    if (nr) { k->rx = nr; k->rxsz = nsz; }
                }
                if (k->rxlen + tlen <= k->rxsz) {
                    memcpy(k->rx + k->rxlen, tail, tlen);
                    k->rxlen += tlen;
                    u64 one = 1;
                    real_write(k->efd, &one, 8);
                    // M14-fix: читатель мог зайти в cond_wait ДО прихода
                    // данных (реальная сеть = RTT >> локального) — без
                    // broadcast он спал бы до EOF
                    pthread_cond_broadcast(&k->c);
                }
            } else if (m.msg_id == NET_EOF) {
                k->state = (m.err == ML_E_OK) ? 3 : 4;
                if (m.err != ML_E_OK) k->err = (int)m.err;
                u64 one = 1;
                real_write(k->efd, &one, 8);
                pthread_cond_broadcast(&k->c);
            }
        }
        pthread_mutex_unlock(&tab_m);
    }
}

static void init_once(void) {
    pthread_mutex_lock(&tab_m);
    if (inited) { pthread_mutex_unlock(&tab_m); return; }
    inited = 1;
    pthread_mutex_unlock(&tab_m);

    for (int i = 0; i < NSOCK; i++) {
        pthread_mutex_init(&S[i].m, 0);
        pthread_cond_init(&S[i].c, 0);
    }
    real_read = dlsym(RTLD_NEXT, "read");
    real_write = dlsym(RTLD_NEXT, "write");
    real_send = dlsym(RTLD_NEXT, "send");
    real_recv = dlsym(RTLD_NEXT, "recv");
    real_close = dlsym(RTLD_NEXT, "close");
    real_fcntl = dlsym(RTLD_NEXT, "fcntl");
    real_poll = dlsym(RTLD_NEXT, "poll");
    real_socket = dlsym(RTLD_NEXT, "socket");
    real_connect = dlsym(RTLD_NEXT, "connect");
    real_setsockopt = dlsym(RTLD_NEXT, "setsockopt");
    real_getsockopt = dlsym(RTLD_NEXT, "getsockopt");
    real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");

    lane = bus_lookup_net();
    if (lane >= 0) {
        pthread_t t;
        pthread_create(&t, 0, reader, 0);
        pthread_detach(t);
    }
    fprintf(stderr, "netsh: сервис net0 %s\n",
            lane >= 0 ? "найден" : "НЕ найден (сеть недоступна)");
}

// --- запрос-ответ на lane'е ---
static int net_rpc(u32 op, u32 h0, u32 h1, u32 h2, const void *tail, u32 tlen,
                   struct nsock *k, int to_ms) {
    struct ml_msg m = { ML_PROTO_NET, op, 0, 0, {0} };
    *ml_h(&m, 0) = h0; *ml_h(&m, 1) = h1; *ml_h(&m, 2) = h2;
    struct iovec_ iov[2];
    int niov = 0;
    iov[niov].base = &m; iov[niov].len = sizeof(m); niov++;
    if (tlen) { iov[niov].base = (void *)tail; iov[niov].len = tlen; niov++; }
    struct msghdr_ mh;
    memset(&mh, 0, sizeof(mh));
    mh.iov = iov; mh.iovlen = niov;
    long deadline = to_ms > 0 ? now_ms() + to_ms : -1;
    int rc;
    pthread_mutex_lock(&tab_m);
    if (sendmsg(lane, (struct msghdr *)&mh, MSG_NOSIGNAL) < 0) {
        pthread_mutex_unlock(&tab_m);
        return -1;
    }
    k->wait_id = (int)op + 1;
    k->done = 0;
    while (!k->done) {
        if (deadline < 0) {
            pthread_cond_wait(&k->c, &tab_m);
        } else {
            struct timespec ts;
            long left = deadline - now_ms();
            if (left <= 0) break;
            ts.tv_sec = deadline / 1000;
            ts.tv_nsec = (deadline % 1000) * 1000000;
            pthread_cond_timedwait(&k->c, &tab_m, &ts);
        }
    }
    k->wait_id = -1;
    rc = k->done ? (int)k->resp.err : -2;   // -2 = таймаут
    pthread_mutex_unlock(&tab_m);
    return rc;
}

static struct nsock *sock_by_fd(int fd) {
    for (int i = 0; i < NSOCK; i++)
        if (S[i].used && S[i].efd == fd) return &S[i];
    return 0;
}

// --- перехваты ---
int socket(int domain, int type, int protocol) {
    init_once();
    fprintf(stderr, "netsh: socket(%d,%d,%d) lane=%d\n", domain, type, protocol, lane);
    if ((domain != AF_INET && domain != 2) || lane < 0)
        return real_socket(domain, type, protocol);
    if (lane < 0) { errno = EAFNOSUPPORT; return -1; }

    pthread_mutex_lock(&tab_m);
    int slot = -1;
    for (int i = 0; i < NSOCK; i++)
        if (!S[i].used) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&tab_m); errno = ENFILE; return -1; }

    struct nsock *k = &S[slot];
    memset(k, 0, sizeof(*k));
    k->used = 1;
    k->efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    fprintf(stderr, "netsh: slot=%d efd=%d\n", slot, k->efd);
    if (k->efd < 0) { k->used = 0; pthread_mutex_unlock(&tab_m); return -1; }
    pthread_mutex_unlock(&tab_m);
    int rc = net_rpc(NET_OPEN, 1 /*TCP*/, 0, 0, 0, 0, k, 5000);
    fprintf(stderr, "netsh: NET_OPEN rc=%d efd=%d\n", rc, k->efd);
    if (rc != ML_E_OK) {
        pthread_mutex_lock(&tab_m);
        k->used = 0;
        pthread_mutex_unlock(&tab_m);
        real_close(k->efd);
        errno = rc == -2 ? ETIMEDOUT : EAFNOSUPPORT;
        return -1;
    }
    k->sid = (u32)*ml_h(&k->resp, 0);      // идентификатор на стороне netd
    return k->efd;
}

int connect(int fd, const struct sockaddr *sa, socklen_t salen) {
    struct nsock *k = sock_by_fd(fd);
    fprintf(stderr, "netsh: connect(fd=%d fam=%d) k=%d\n", fd,
            sa ? (int)sa->sa_family : -1, k ? 1 : 0);
    if (!k) return real_connect(fd, sa, salen);
    if (sa->sa_family != AF_INET) { errno = EAFNOSUPPORT; return -1; }
    // протокол NET_* хранит адреса в host-order (как netclient): у netd
    // свой wbe32 на проводе, sockaddr же несёт network-order
    u32 ip = ntohl(((struct sockaddr_in *)sa)->sin_addr.s_addr);
    u16 port = ntohs(((struct sockaddr_in *)sa)->sin_port);
    pthread_mutex_lock(&tab_m);
    int st = k->state;
    if (st == 2) { pthread_mutex_unlock(&tab_m); return 0; }
    k->state = 1;
    pthread_mutex_unlock(&tab_m);
    // установление синхронно для клиента: NET_CONNECT_R придёт с SYN-ACK'ом
    int rc = net_rpc(NET_CONNECT, k->sid, ip, port, 0, 0, k, 15000);
    fprintf(stderr, "netsh: connect rc=%d\n", rc);
    if (rc != ML_E_OK) {
        pthread_mutex_lock(&tab_m);
        k->state = 4;
        k->err = rc;
        pthread_mutex_unlock(&tab_m);
        errno = rc == -2 ? ETIMEDOUT : ECONNREFUSED;
        return -1;
    }
    pthread_mutex_lock(&tab_m);
    k->state = 2;
    pthread_mutex_unlock(&tab_m);
    return 0;
}

static ssize_t sock_read(struct nsock *k, void *buf, size_t n) {
    pthread_mutex_lock(&tab_m);
    while (k->rxlen == 0 && k->state == 2)
        pthread_cond_wait(&k->c, &tab_m);
    size_t take = k->rxlen < n ? k->rxlen : n;
    ssize_t ret;
    if (take) {
        memcpy(buf, k->rx, take);
        memmove(k->rx, k->rx + take, k->rxlen - take);
        k->rxlen -= take;
        if (k->rxlen == 0) {
            u64 v;
            real_read(k->efd, &v, 8);      // сброс события
        }
        ret = (ssize_t)take;
    } else {
        ret = 0;                           // EOF / ошибка читается как EOF
    }
    pthread_mutex_unlock(&tab_m);
    return ret;
}

ssize_t read(int fd, void *buf, size_t n) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) return real_read(fd, buf, n);
    return sock_read(k, buf, n);
}

ssize_t recv(int fd, void *buf, size_t n, int flags) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) return real_recv(fd, buf, n, flags);
    if (flags & MSG_PEEK) {
        pthread_mutex_lock(&tab_m);
        size_t take = k->rxlen < n ? k->rxlen : n;
        memcpy(buf, k->rx, take);
        pthread_mutex_unlock(&tab_m);
        return (ssize_t)take;
    }
    return sock_read(k, buf, n);
}

// openssl 3.x может идти через recvmsg/readv — хэндлим честно
ssize_t recvmsg(int fd, struct msghdr *mh, int flags) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) {
        static ssize_t (*real_recvmsg)(int, struct msghdr *, int);
        if (!real_recvmsg) real_recvmsg = dlsym(RTLD_NEXT, "recvmsg");
        return real_recvmsg(fd, mh, flags);
    }
    if (mh->msg_iovlen != 1) { errno = EINVAL; return -1; }
    return recv(fd, mh->msg_iov[0].iov_base, mh->msg_iov[0].iov_len, flags);
}

ssize_t write(int fd, const void *buf, size_t n) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) return real_write(fd, buf, n);
    size_t off = 0;
    while (off < n) {
        u32 chunk = (u32)(n - off);
        if (chunk > 200) chunk = 200;      // tail с запасом в ML_MAXTAIL
        int rc = net_rpc(NET_SEND, k->sid, 0, 0, (const u8 *)buf + off, chunk,
                         k, 10000);
        if (rc != ML_E_OK) {
            errno = EIO;
            return -1;
        }
        off += chunk;
    }
    return (ssize_t)n;
}

ssize_t send(int fd, const void *buf, size_t n, int flags) {
    (void)flags;
    return write(fd, buf, n);
}

// libfetch ходит именно через sendto/recvfrom
ssize_t sendto(int fd, const void *buf, size_t n, int flags,
               const struct sockaddr *sa, socklen_t sl) {
    (void)sa; (void)sl;
    return send(fd, buf, n, flags);
}

ssize_t recvfrom(int fd, void *buf, size_t n, int flags,
                 struct sockaddr *sa, socklen_t *sl) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) {
        if (!real_recvfrom)
            real_recvfrom = dlsym(RTLD_NEXT, "recvfrom");
        return real_recvfrom(fd, buf, n, flags, sa, sl);
    }
    ssize_t r = recv(fd, buf, n, flags);
    if (sa && sl && *sl >= sizeof(struct sockaddr_in)) {
        struct sockaddr_in *in = (struct sockaddr_in *)sa;
        memset(in, 0, sizeof(*in));
        in->sin_family = AF_INET;
        *sl = sizeof(*in);
    }
    return r;
}

int close(int fd) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) return real_close(fd);
    net_rpc(NET_CLOSE, k->sid, 0, 0, 0, 0, k, 3000);
    pthread_mutex_lock(&tab_m);
    k->used = 0;
    free(k->rx);
    k->rx = 0;
    pthread_mutex_unlock(&tab_m);
    return real_close(fd);
}

static int poll_wait(struct pollfd *, nfds_t, int);

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_once();
    // есть ли наши сокеты среди дескрипторов?
    int ours = 0;
    for (nfds_t i = 0; i < nfds; i++)
        if (sock_by_fd(fds[i].fd)) ours = 1;
    if (!ours) return real_poll(fds, nfds, timeout);
    return poll_wait(fds, nfds, timeout);
}

static int poll_wait(struct pollfd *fds, nfds_t nfds, int timeout) {

    long deadline = timeout >= 0 ? now_ms() + timeout : -1;
    for (;;) {
        int ready = 0;
        pthread_mutex_lock(&tab_m);
        for (nfds_t i = 0; i < nfds; i++) {
            struct nsock *k = sock_by_fd(fds[i].fd);
            if (!k) continue;
            short r = 0;
            if (fds[i].events & POLLOUT) {
                if (k->state == 2) r |= POLLOUT;
            }
            if (fds[i].events & POLLIN) {
                if (k->rxlen || k->state == 3 || k->state == 4) r |= POLLIN;
            }
            fds[i].revents = r;
            if (r) ready = 1;
        }
        pthread_mutex_unlock(&tab_m);
        if (ready) return 1;
        if (deadline >= 0 && now_ms() >= deadline) return 0;
        usleep(20000);
    }
}

int fcntl(int fd, int cmd, ...) {
    init_once();
    va_list ap;
    va_start(ap, cmd);
    long arg = va_arg(ap, long);
    va_end(ap);
    if (!sock_by_fd(fd)) {
        if (cmd == F_GETFL || cmd == F_SETFL || cmd == F_GETFD || cmd == F_SETFD)
            return real_fcntl(fd, cmd, arg);
        return real_fcntl(fd, cmd, arg);
    }
    // сетевые fd: флаги храним в ядре eventfd'а, остальное — успех
    if (cmd == F_GETFL) return 0;
    if (cmd == F_SETFL) return 0;
    return 0;
}
static int (*real_getaddrinfo)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);
static int getaddrinfo_m14(const char *host, const char *serv,
                           const struct addrinfo *hints, struct addrinfo **res);
int getaddrinfo(const char *host, const char *serv,
                const struct addrinfo *hints, struct addrinfo **res) {
    init_once();
    // glibc-резолвер требует UDP (netd его не умеет) — резолвим сами
    // по TCP:53 через netd; AF_INET6-запросы честно не поддержаны
    if (hints && hints->ai_family == AF_INET6) return EAI_NONAME;
    // AI_NUMERICHOST:caller требует только литерал (libfetch так проверяет,
    // имя ли host — резолвить нельзя, иначе ломается SAN-проверка TLS)
    if (hints && (hints->ai_flags & AI_NUMERICHOST)) {
        struct in_addr lit;
        if (!host || inet_pton(AF_INET, host, &lit) != 1) return EAI_NONAME;
        return getaddrinfo_m14(host, serv, hints, res);
    }
    return getaddrinfo_m14(host, serv, hints, res);
}

// --- DNS (M14): /etc/resolv.conf -> TCP:53 через netd. UDP-стека в netd
// нет, а DNS-over-TCP обязателен для резолверов (RFC 7766): query = 2-байтовая
// длина + сообщение, ответ так же. A-записи достаточно (двойные запросы
// A/AAAA libfetch сам переваривает).

static unsigned short dns_port(const char *serv) {
    if (!serv) return 80;
    if (serv[0] >= '0' && serv[0] <= '9') return (unsigned short)atoi(serv);
    if (!strcmp(serv, "https")) return 443;
    if (!strcmp(serv, "http")) return 80;
    if (!strcmp(serv, "ftp")) return 21;
    return 80;
}

// resolv.conf: до 2 nameserver'ов (путь переопределяется для тестов)
static int dns_nameservers(unsigned int *out, int max) {
    const char *path = getenv("MILIN_RESOLV_CONF");
    if (!path) path = "/etc/resolv.conf";
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[256];
    int n = 0;
    while (n < max && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "nameserver", 10)) continue;
        const char *p = line + 10;
        while (*p == ' ' || *p == '\t') p++;
        char ip[64];
        size_t il = 0;
        while (p[il] && p[il] != '\n' && p[il] != '\r' && p[il] != ' ' &&
               p[il] != '\t' && il + 1 < sizeof(ip)) {
            ip[il] = p[il];
            il++;
        }
        ip[il] = 0;
        struct in_addr a;
        if (!il || inet_pton(AF_INET, ip, &a) != 1) continue;
        out[n++] = ntohl(a.s_addr);
    }
    fclose(f);
    return n;
}

// имена -> QNAME (метки длиной+байты), ответ: пропускаем question
static int dns_query(unsigned int ns, const char *name,
                     unsigned int *out_a, int max_a) {
    char q[512];
    memset(q, 0, sizeof(q));
    int ql = 0;
    // header: id, RD, QDCOUNT=1
    q[ql++] = 0x12; q[ql++] = 0x34;
    q[ql++] = 0x01; q[ql++] = 0x00;
    q[ql++] = 0; q[ql++] = 1; q[ql++] = 0; q[ql++] = 0;
    q[ql++] = 0; q[ql++] = 0; q[ql++] = 0; q[ql++] = 0;
    const char *p = name;
    while (*p) {
        const char *dot = strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : strlen(p);
        if (len == 0 || len > 63 || ql + 1 + len > 400) return -1;
        q[ql++] = (char)len;
        memcpy(q + ql, p, len);
        ql += len;
        if (!dot) break;
        p = dot + 1;
    }
    q[ql++] = 0;
    q[ql++] = 0; q[ql++] = 1;    // QTYPE=A
    q[ql++] = 0; q[ql++] = 1;    // QCLASS=IN

    // TCP: 2-байтовая длина префиксом
    unsigned char pkt[600];
    pkt[0] = (unsigned char)(ql >> 8);
    pkt[1] = (unsigned char)(ql & 0xFF);
    memcpy(pkt + 2, q, ql);

    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(53);
    sa.sin_addr.s_addr = htonl(ns);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
        close(fd);
        return -1;
    }
    if (send(fd, pkt, ql + 2, 0) != (ssize_t)(ql + 2)) {
        close(fd);
        return -1;
    }
    // ответ: 2 байта длины, потом сообщение (может прийти кусками)
    unsigned char hdr[2];
    size_t got = 0;
    while (got < 2) {
        ssize_t r = recv(fd, hdr + got, 2 - got, 0);
        if (r <= 0) { close(fd); return -1; }
        got += r;
    }
    int plen = (hdr[0] << 8) | hdr[1];
    if (plen < 12 || plen > 4096) { close(fd); return -1; }
    unsigned char *resp = malloc(plen);
    if (!resp) { close(fd); return -1; }
    got = 0;
    while (got < (size_t)plen) {
        ssize_t r = recv(fd, resp + got, plen - got, 0);
        if (r <= 0) { free(resp); close(fd); return -1; }
        got += r;
    }
    close(fd);
    int qd = (resp[4] << 8) | resp[5];
    int an = (resp[6] << 8) | resp[7];
    if (!an) { free(resp); return -1; }
    // question-секции: QNAME + 4 байта
    int off = 12;
    for (int i = 0; i < qd; i++) {
        while (off < plen && resp[off]) {
            if (resp[off] & 0xC0) { off += 2; goto qdone; }
            off += resp[off] + 1;
        }
        off++;
qdone:
        off += 4;
    }
    int found = 0;
    for (int i = 0; i < an && found < max_a; i++) {
        // name: метки или указатель
        int jump = 0, saved = 0;
        for (;;) {
            if (off >= plen) break;
            int l = resp[off];
            if ((l & 0xC0) == 0xC0) {
                if (!jump) saved = off + 2;
                off = ((l & 0x3F) << 8) | resp[off + 1];
                jump = 1;
                continue;
            }
            off += l + 1;
            if (!l) { if (jump) off = saved; break; }
        }
        if (off + 10 > plen) break;
        int type = (resp[off] << 8) | resp[off + 1];
        int rdlen = (resp[off + 8] << 8) | resp[off + 9];
        off += 10;
        if (type == 1 && rdlen == 4 && off + 4 <= plen) {
            unsigned int ip = ((unsigned int)resp[off] << 24) |
                              ((unsigned int)resp[off + 1] << 16) |
                              ((unsigned int)resp[off + 2] << 8) |
                              resp[off + 3];
            out_a[found++] = ip;
        }
        off += rdlen;
    }
    free(resp);
    return found;
}

int getaddrinfo_m14(const char *host, const char *serv,
                    const struct addrinfo *hints, struct addrinfo **res) {
    *res = 0;
    if (!host || !*host) return EAI_NONAME;

    // литералы — мимо DNS
    struct in_addr lit;
    if (inet_pton(AF_INET, host, &lit) == 1) {
        struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
        if (!ai) return EAI_MEMORY;
        ai->ai_family = AF_INET;
        ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
        ai->ai_protocol = IPPROTO_TCP;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)(ai + 1);
        struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;
        in->sin_family = AF_INET;
        in->sin_port = htons(dns_port(serv));
        in->sin_addr = lit;
        *res = ai;
        return 0;
    }

    unsigned int ns[2];
    int nn = dns_nameservers(ns, 2);
    if (!nn) return EAI_FAIL;

    unsigned int a[8];
    int found = 0;
    for (int i = 0; i < nn && !found; i++) {
        found = dns_query(ns[i], host, a, 8);
        if (found < 0) found = 0;
    }
    if (!found) return EAI_NONAME;

    struct addrinfo *head = 0, *tail = 0;
    for (int i = 0; i < found; i++) {
        struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_in));
        if (!ai) break;
        ai->ai_family = AF_INET;
        ai->ai_socktype = hints ? hints->ai_socktype : SOCK_STREAM;
        ai->ai_protocol = IPPROTO_TCP;
        ai->ai_addrlen = sizeof(struct sockaddr_in);
        ai->ai_addr = (struct sockaddr *)(ai + 1);
        struct sockaddr_in *in = (struct sockaddr_in *)ai->ai_addr;
        in->sin_family = AF_INET;
        in->sin_port = htons(dns_port(serv));
        in->sin_addr.s_addr = htonl(a[i]);
        if (tail) tail->ai_next = ai; else head = ai;
        tail = ai;
    }
    *res = head;
    return head ? 0 : EAI_MEMORY;
}

int setsockopt(int fd, int level, int opt, const void *v, socklen_t l) {
    init_once();
    if (!sock_by_fd(fd)) return real_setsockopt(fd, level, opt, v, l);
    return 0;                              // таймауты/флаги — норма IPC-мира
}

int getsockopt(int fd, int level, int opt, void *v, socklen_t *l) {
    init_once();
    struct nsock *k = sock_by_fd(fd);
    if (!k) return real_getsockopt(fd, level, opt, v, l);
    if (level == SOL_SOCKET && opt == SO_ERROR) {
        int e = k->err;
        if (l && *l >= 4) memcpy(v, &e, 4);
        return 0;
    }
    if (level == SOL_SOCKET && opt == SO_TYPE && l && *l >= 4) {
        int t = SOCK_STREAM;
        memcpy(v, &t, 4);
        return 0;
    }
    return 0;
}
