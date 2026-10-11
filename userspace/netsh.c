// netsh.so (M12): socket-шима поверх MiLin IPC. LD_PRELOAD для
// динамического xbps: socket/connect/send/recv/close уезжают в сервис
// "net0" (netd) lane'ом по шине, poll/read ждут push'ей NET_DATA.
// Микроядро честно: ни одного socket-сисколла ядру про TCP — только IPC.

#define _GNU_SOURCE
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

        pthread_mutex_lock(&tab_m);
        struct nsock *k = 0;
        if (m.msg_id >= NET_OPEN_R && m.msg_id <= NET_CLOSE_R) {
            // R-ответы ищем по ждущему слоту (OPEN_R приходит ДО used=1)
            for (int i = 0; i < NSOCK; i++)
                if (S[i].used && S[i].wait_id == (int)m.msg_id) { k = &S[i]; break; }
        } else if (sid < NSOCK && S[sid].used) {
            k = &S[sid];
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
    while (k->rxlen == 0 && k->state == 2) {
        pthread_cond_wait(&k->c, &tab_m);
    }
    size_t take = k->rxlen < n ? k->rxlen : n;
    if (take) {
        memcpy(buf, k->rx, take);
        memmove(k->rx, k->rx + take, k->rxlen - take);
        k->rxlen -= take;
        if (k->rxlen == 0) {
            u64 v;
            real_read(k->efd, &v, 8);      // сброс события
        }
    } else if (k->state == 3 || k->state == 4) {
        pthread_mutex_unlock(&k->m);
        return 0;                          // EOF / ошибка читается как EOF
    }
    pthread_mutex_unlock(&k->m);
    return (ssize_t)take;
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
        pthread_mutex_lock(&k->m);
        size_t take = k->rxlen < n ? k->rxlen : n;
        memcpy(buf, k->rx, take);
        pthread_mutex_unlock(&k->m);
        return (ssize_t)take;
    }
    return sock_read(k, buf, n);
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

int poll(struct pollfd *fds, nfds_t nfds, int timeout) {
    init_once();
    // есть ли наши сокеты среди дескрипторов?
    int ours = 0;
    for (nfds_t i = 0; i < nfds; i++)
        if (sock_by_fd(fds[i].fd)) ours = 1;
    if (!ours) return real_poll(fds, nfds, timeout);

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

// DEBUG: перехват getaddrinfo — что и почему резолвится
static int (*real_getaddrinfo)(const char *, const char *,
                               const struct addrinfo *, struct addrinfo **);
int getaddrinfo(const char *host, const char *serv,
                const struct addrinfo *hints, struct addrinfo **res) {
    init_once();
    if (!real_getaddrinfo)
        real_getaddrinfo = dlsym(RTLD_NEXT, "getaddrinfo");
    int rc = real_getaddrinfo(host, serv, hints, res);
    fprintf(stderr, "netsh: getaddrinfo(%s, %s, fam=%d) rc=%d%s\n",
            host ? host : "(null)", serv ? serv : "(null)",
            hints ? hints->ai_family : 0, rc,
            rc ? gai_strerror(rc) : "");
    return rc;
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
