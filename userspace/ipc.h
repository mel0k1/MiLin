// MiLin IPC (M5): микроядерные сообщения поверх простого транспорта.
//
// Идея подсмотрена у Managarm, упрощена и смешана с классическим IPC:
//   * bragi (managarm/protocols): сообщение = заголовок с идентификатором
//     + inline-поля (head) + переменная часть (tail) + хэндлы (hooks).
//   * mbus: именованный реестр серверов. Сервер делает REGISTER(name),
//     клиент LOOKUP(name) и получает lane-хэндл для прямого общения
//     (аналог GetRemoteLane); реестр сам создаёт соединение.
//   * Hel-lane: канал endpoint-to-endpoint, по которому ходят сообщения.
//
// Транспорт — самый простой из возможных: AF_UNIX SOCK_SEQPACKET.
// Одно сообщение = один пакет: граница сообщения атомарна, как в
// микроядре. Хэндлы — файловые дескрипторы, передаются SCM_RIGHTS.
// Никакой общей памяти и синхронизации: ядро очередит пакеты само.
#ifndef MILIN_IPC_H
#define MILIN_IPC_H

#include "milin.h"
#include "mlproto.h"

// --- syscall-номера (x86_64) ---
#define SYS_socket    41
#define SYS_connect   42
#define SYS_accept    43
#define SYS_sendmsg   46
#define SYS_recvmsg   47
#define SYS_bind      49
#define SYS_listen    50
#define SYS_socketpair 53
#define SYS_shutdown  48
#define SYS_setsockopt 54
#define SYS_poll     7
#define SYS_kill      62
#define SYS_futex     202
#define SYS_sched_yield 24
#define SYS_unlink    87

#define AF_UNIX        1
#define SOCK_SEQPACKET 5
#define SOL_SOCKET     1
#define SCM_RIGHTS     1
#define SCM_CREDENTIALS 2
#define SO_PASSCRED    16
#define MSG_TRUNC      0x20
#define MSG_CTRUNC     0x08
#define MSG_NOSIGNAL   0x4000
#define SIGTERM        15
#define SIGKILL        9

// --- futex: примитив парковки очередей (аналог HelFutex из Managarm) ---
#define FUTEX_WAIT 0
#define FUTEX_WAKE 1

static int futex_wait(volatile int *uaddr, int val) {
    // таймаут NULL = ждать бесконечно; EAGAIN = значение изменилось
    return (int)sys6(SYS_futex, (long)uaddr, FUTEX_WAIT, val, 0, 0, 0);
}

// паркинг с будильником (M7.1): ETIMEDOUT (-110) = тишина ms миллисекунд
static int futex_wait_to(volatile int *uaddr, int val, long ms) {
    struct { long sec, nsec; } t = { 0, ms * 1000000L };
    return (int)sys6(SYS_futex, (long)uaddr, FUTEX_WAIT, val, (long)&t, 0, 0);
}
static int futex_wake(volatile int *uaddr) {
    return (int)sys6(SYS_futex, (long)uaddr, FUTEX_WAKE, 1, 0, 0, 0);
}

// --- credentials (ядро проверяет само, SCM_CREDENTIALS) ---
struct ucred { u32 pid, uid, gid; };

static int ml_passcred(long fd) {
    int one = 1;
    return (int)sys5(SYS_setsockopt, fd, SOL_SOCKET, SO_PASSCRED, (long)&one, 4);
}

struct sockaddr_un {
    u16 family;
    char path[108];
};

struct iovec {
    void *base;
    u64 len;
};

struct msghdr {
    void *msg_name;
    u32 msg_namelen;
    struct iovec *msg_iov;
    u64 msg_iovlen;
    void *msg_control;
    u64 msg_controllen;
    int msg_flags;
};

struct cmsghdr {
    u64 len;
    int level;
    int type;
};

#define CMSG_ALIGN(a)   (((a) + sizeof(u64) - 1) & ~(sizeof(u64) - 1))
#define CMSG_SPACE(a)   (CMSG_ALIGN(a) + CMSG_ALIGN(sizeof(struct cmsghdr)))
#define CMSG_LEN(a)     (CMSG_ALIGN(sizeof(struct cmsghdr)) + (a))

// побочные данные recvmsg: hooks (SCM_RIGHTS) + creds (SCM_CREDENTIALS).
// creds ставит ядро (SO_PASSCRED) — клиент их подделать не может.
struct ml_aux {
    int hooks[ML_MAXHOOKS];
    int nhooks;
    struct ucred cred;
    int has_cred;
};

// --- транспорт ---

// абстрактное имя: namespace сокетов, файл в ФС не нужен
static long ml_addr(struct sockaddr_un *a, const char *name) {
    m_memset(a, 0, sizeof(*a)); // хвост адреса обязан быть нулями, а не стеком
    a->family = AF_UNIX;
    a->path[0] = 0;
    int i = 0;
    while (name[i] && i < 100) { a->path[1 + i] = name[i]; i++; }
    a->path[1 + i] = 0;
    return 4 + i + 1; // offsetof(sun_path)+1+len — длину адреса ядро срезает само
}

static long ml_socket(void) {
    return sys5(SYS_socket, AF_UNIX, SOCK_SEQPACKET, 0, 0, 0);
}

// создать lane, слушающий абстрактное имя (для реестра)
static long ml_listen(const char *name) {
    struct sockaddr_un a;
    long len = ml_addr(&a, name);
    long s = ml_socket();
    if (s < 0) return s;
    if (sys3(SYS_bind, s, (long)&a, len) < 0) return -1;
    if (sys3(SYS_listen, s, 8, 0) < 0) return -1;
    return s;
}

static long ml_connect(const char *name) {
    struct sockaddr_un a;
    long len = ml_addr(&a, name);
    long s = ml_socket();
    if (s < 0) return s;
    if (sys3(SYS_connect, s, (long)&a, len) < 0) return -1;
    return s;
}

static long ml_accept(long lane) {
    return sys3(SYS_accept, lane, 0, 0);
}

static long ml_socketpair(int sv[2]) {
    return sys5(SYS_socketpair, AF_UNIX, SOCK_SEQPACKET, 0, (long)sv, 0);
}

// отправить сообщение: head + tail + хэндлы одним атомарным пакетом
static int ml_send(long fd, struct ml_msg *m, const void *tail, u32 tlen,
                   const int *hooks, int nhooks) {
    m->tail_len = tlen;
    struct iovec iov[2];
    int n = 0;
    iov[n].base = m; iov[n].len = sizeof(*m); n++;
    if (tlen) { iov[n].base = (void *)tail; iov[n].len = tlen; n++; }

    char cbuf[CMSG_SPACE(ML_MAXHOOKS * 4)];
    struct msghdr mh;
    __builtin_memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = n;
    if (nhooks > 0) {
        struct cmsghdr *c = (struct cmsghdr *)cbuf;
        c->len = CMSG_LEN(nhooks * 4);
        c->level = SOL_SOCKET;
        c->type = SCM_RIGHTS;
        __builtin_memcpy(c + 1, hooks, nhooks * 4);
        mh.msg_control = cbuf;
        mh.msg_controllen = CMSG_SPACE(nhooks * 4);
    }
    // MSG_NOSIGNAL обязателен: lane может оказаться мёртвым (сервер убит,
    // клиент ушёл) — SIGPIPE вместо EPIPE убил бы шину (M7)
    long r = sys3(SYS_sendmsg, fd, (long)&mh, MSG_NOSIGNAL);
    return (int)r;
}

// получить сообщение (блокирующе); 0 = канал закрыт (EOF)
// aux (может быть 0): разбирает ВСЕ cmsg — права (hooks) и creds (uid/pid)
static int ml_recv(long fd, struct ml_msg *m, void *tail, u32 tailmax,
                   struct ml_aux *aux) {
    struct iovec iov[2];
    int n = 0;
    iov[n].base = m; iov[n].len = sizeof(*m); n++;
    if (tail) { iov[n].base = tail; iov[n].len = tailmax; n++; }

    // буфер под SCM_RIGHTS и SCM_CREDENTIALS сразу (два cmsg)
    char cbuf[2 * CMSG_SPACE(ML_MAXHOOKS * 4)];
    struct msghdr mh;
    __builtin_memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = n;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);

    m->tail_len = 0;
    if (aux) { aux->nhooks = 0; aux->has_cred = 0; }
    long r = sys3(SYS_recvmsg, fd, (long)&mh, 0);
    if (r <= 0) return (int)r;
    if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) {
        // сообщение не влезло: доставленные fd обязаны быть закрыты
        char *c = cbuf, *end = cbuf + sizeof(cbuf);
        while (c + sizeof(struct cmsghdr) <= end) {
            struct cmsghdr *cm = (struct cmsghdr *)c;
            if (cm->len < CMSG_LEN(4) || cm->len > CMSG_SPACE(ML_MAXHOOKS * 4)
                || c + cm->len > end) break;
            if (cm->level == SOL_SOCKET && cm->type == SCM_RIGHTS) {
                int *f = (int *)(c + CMSG_ALIGN(sizeof(struct cmsghdr)));
                int cnt = (int)((cm->len - CMSG_LEN(0)) / 4);
                for (int i = 0; i < cnt && i < ML_MAXHOOKS; i++)
                    sys3(SYS_close, f[i], 0, 0);
            }
            c += CMSG_ALIGN(cm->len);
        }
        return -1;
    }

    if (r > (long)sizeof(*m)) m->tail_len = (u32)(r - sizeof(*m));

    // шаг по цепочке cmsg: могут прийти и права, и creds
    char *c = cbuf, *end = cbuf + sizeof(cbuf);
    while (c + sizeof(struct cmsghdr) <= (char *)mh.msg_control + mh.msg_controllen
           && c + sizeof(struct cmsghdr) <= end) {
        struct cmsghdr *cm = (struct cmsghdr *)c;
        if (cm->len < sizeof(struct cmsghdr) || c + cm->len > end) break;
        void *data = c + CMSG_ALIGN(sizeof(struct cmsghdr));
        if (cm->level == SOL_SOCKET && cm->type == SCM_RIGHTS && aux) {
            int cnt = (int)((cm->len - CMSG_LEN(0)) / 4);
            if (cnt > ML_MAXHOOKS) cnt = ML_MAXHOOKS;
            __builtin_memcpy(aux->hooks, data, cnt * 4);
            aux->nhooks = cnt;
        } else if (cm->level == SOL_SOCKET && cm->type == SCM_CREDENTIALS && aux) {
            __builtin_memcpy(&aux->cred, data, sizeof(struct ucred));
            aux->has_cred = 1;
        }
        c += CMSG_ALIGN(cm->len);
    }
    return (int)r;
}

// протоколы (BUS_*/LOG_*/ML_TR_*) и формат ml_msg — в mlproto.h

#endif
