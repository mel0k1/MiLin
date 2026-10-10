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

// --- syscall-номера сокетов (x86_64) ---
#define SYS_socket    41
#define SYS_connect   42
#define SYS_accept    43
#define SYS_sendmsg   46
#define SYS_recvmsg   47
#define SYS_bind      49
#define SYS_listen    50
#define SYS_socketpair 53
#define SYS_shutdown  48
#define SYS_kill      62

#define AF_UNIX        1
#define SOCK_SEQPACKET 5
#define SOL_SOCKET     1
#define SCM_RIGHTS     1
#define MSG_TRUNC      0x20
#define MSG_CTRUNC     0x08
#define SIGTERM        15

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

// --- формат сообщения (bragi-стиль) ---
// один пакет SOCK_SEQPACKET = одно сообщение:
//   [ struct ml_msg ][ tail: tail_len байт ]  (+ хэндлы в cmsg)
// head — inline-поля фиксированного размера (как head(128) в bragi).
// Трактовка полей head у каждого msg_id своя, см. протоколы ниже.

#define ML_HEAD 24
#define ML_MAXTAIL 256
#define ML_MAXHOOKS 4

struct ml_msg {
    u32 proto;
    u32 msg_id;
    u32 err;        // 0 = OK, иначе код ошибки протокола
    u32 tail_len;
    char head[ML_HEAD];
};

// inline-поля head как u64-слоты (bragi хранит скаляры в head)
static inline u64 *ml_h(struct ml_msg *m, int i) { return (u64 *)(m->head + i * 8); }

// коды ошибок — как enum Error в mbus.bragi
#define ML_E_OK              0
#define ML_E_NO_SUCH_ENTITY  1

// --- транспорт ---

// абстрактное имя: namespace сокетов, файл в ФС не нужен
static long ml_addr(struct sockaddr_un *a, const char *name) {
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
    long r = sys3(SYS_sendmsg, fd, (long)&mh, 0);
    return (int)r;
}

// получить сообщение (блокирующе); 0 = канал закрыт (EOF)
static int ml_recv(long fd, struct ml_msg *m, void *tail, u32 tailmax,
                   int *hooks, int *nhooks) {
    struct iovec iov[2];
    int n = 0;
    iov[n].base = m; iov[n].len = sizeof(*m); n++;
    if (tail) { iov[n].base = tail; iov[n].len = tailmax; n++; }

    char cbuf[CMSG_SPACE(ML_MAXHOOKS * 4)];
    struct msghdr mh;
    __builtin_memset(&mh, 0, sizeof(mh));
    mh.msg_iov = iov;
    mh.msg_iovlen = n;
    mh.msg_control = cbuf;
    mh.msg_controllen = sizeof(cbuf);

    m->tail_len = 0;
    if (nhooks) *nhooks = 0;
    long r = sys3(SYS_recvmsg, fd, (long)&mh, 0);
    if (r <= 0) return (int)r;
    if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -1;

    if (r > (long)sizeof(*m)) m->tail_len = (u32)(r - sizeof(*m));
    if (nhooks && mh.msg_controllen >= sizeof(struct cmsghdr)) {
        struct cmsghdr *c = (struct cmsghdr *)cbuf;
        if (c->level == SOL_SOCKET && c->type == SCM_RIGHTS) {
            int cnt = (int)((c->len - CMSG_LEN(0)) / 4);
            if (cnt > ML_MAXHOOKS) cnt = ML_MAXHOOKS;
            __builtin_memcpy(hooks, c + 1, cnt * 4);
            *nhooks = cnt;
        }
    }
    return (int)r;
}

// --- протокол реестра (ML_PROTO_BUS, идея mbus) ---
// RegisterRequest  1 { head: name[24] }              сервер -> шина
// RegisterResponse 2 { head: err }
// LookupRequest    3 { head: name[24] }              клиент -> шина
// LookupResponse   4 { head: err; hooks: [lane] }    шина -> клиент
// LaneRequest      5 { hooks: [lane] }               шина -> сервер

#define ML_PROTO_BUS 1
#define BUS_REGISTER   1
#define BUS_REGISTER_R 2
#define BUS_LOOKUP     3
#define BUS_LOOKUP_R   4
#define BUS_LANE_REQ   5

// --- протокол log-сервера (ML_PROTO_LOG) ---
// WriteRequest  1 { tail: текст }                    клиент -> сервер
// WriteResponse 2 { head: stored_len }
// StatsRequest  3 { }
// StatsResponse 4 { head: served }

#define ML_PROTO_LOG 2
#define LOG_WRITE    1
#define LOG_WRITE_R  2
#define LOG_STATS    3
#define LOG_STATS_R  4

#endif
