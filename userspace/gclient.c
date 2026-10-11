// /sbin/gclient (M8): IPC-клиент, собранный с glibc (динамически, ld.so).
// Доказывает, что ядро тянет обычный userspace: ELF-интерпретатор,
// stdio, malloc, полный набор системных вызовов.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gnu/libc-version.h>

// контракт mlproto.h: типы даёт инклюдер
typedef unsigned int u32;
typedef unsigned long long u64;
#include "mlproto.h"

// адресная арифметика как ml_addr (ipc.h): len = 4 + strlen + 1,
// хвост адреса обязан быть нулями — иначе это ДРУГОЕ абстрактное имя
static long ml_addr_g(struct sockaddr_un *a, const char *name) {
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    a->sun_path[0] = 0;
    int i = 0;
    while (name[i] && i < 100) { a->sun_path[1 + i] = name[i]; i++; }
    a->sun_path[1 + i] = 0;
    return 4 + i + 1;
}

static int bus_connect(void) {
    for (int t = 0; t < 200; t++) {
        int s = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (s < 0) return -1;
        struct sockaddr_un a;
        socklen_t al = (socklen_t)ml_addr_g(&a, "milin-bus");
        if (connect(s, (struct sockaddr *)&a, al) == 0) return s;
        close(s);
        usleep(10000);
    }
    return -1;
}

static int lookup_log(int bus, int *lane) {
    struct ml_msg q;
    memset(&q, 0, sizeof(q));
    q.proto = ML_PROTO_BUS;
    q.msg_id = BUS_LOOKUP;
    strcpy(q.head, "log");
    *ml_h(&q, 2) = ML_TR_SEQPACKET;
    for (int t = 0; t < 100; t++) {
        if (send(bus, &q, sizeof(q), MSG_NOSIGNAL) < 0) return -1;
        struct ml_msg r;
        struct iovec iov = { &r, sizeof(r) };
        // creds + права: ядро прикладывает ДВА cmsg (SO_PASSCRED включён)
        char cbuf[CMSG_SPACE(sizeof(int)) + CMSG_SPACE(3 * sizeof(unsigned))];
        struct msghdr mh;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov;
        mh.msg_iovlen = 1;
        mh.msg_control = cbuf;
        mh.msg_controllen = sizeof(cbuf);
        ssize_t n = recvmsg(bus, &mh, 0);
        if (n <= 0) return -1;
        if (mh.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) return -1;
        if (r.err == ML_E_OK) {
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS)
                    memcpy(lane, CMSG_DATA(c), sizeof(int));
            return 0;
        }
        usleep(20000);
    }
    return -1;
}

static int log_req(int lane, u32 id, const void *tail, u32 tlen, u64 *ans) {
    char pkt[ML_HEAD + 4096];
    struct ml_msg w;
    memset(&w, 0, sizeof(w));
    w.proto = ML_PROTO_LOG;
    w.msg_id = id;
    w.tail_len = tlen;
    memcpy(pkt, &w, sizeof(w));
    if (tlen) memcpy(pkt + sizeof(w), tail, tlen);
    if (send(lane, pkt, sizeof(w) + tlen, MSG_NOSIGNAL) < 0) return -1;
    struct ml_msg r;
    if (recv(lane, &r, sizeof(r), 0) <= 0) return -1;
    if (r.err != ML_E_OK) return -1;
    if (ans) *ans = *ml_h(&r, 0);
    return 0;
}

int main(void) {
    int one = 1;
    int bus = bus_connect();
    if (bus < 0) {
        fprintf(stderr, "gclient: шина не отвечает\n");
        return 1;
    }
    setsockopt(bus, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));

    int lane = -1;
    if (lookup_log(bus, &lane) < 0) {
        fprintf(stderr, "gclient: lookup FAIL\n");
        return 1;
    }

    char *msg = malloc(4096);
    if (!msg) return 1;
    snprintf(msg, 4096,
             "привет от glibc %s: stdio+malloc+seqpacket через шину MiLin",
             gnu_get_libc_version());
    u64 stored = 0;
    if (log_req(lane, LOG_WRITE, msg, strlen(msg), &stored) < 0) {
        fprintf(stderr, "gclient: WRITE FAIL\n");
        return 1;
    }
    printf("gclient: WRITE подтверждён (stored_len=%llu)\n", (unsigned long long)stored);

    u64 served = 0;
    if (log_req(lane, LOG_STATS, 0, 0, &served) < 0) {
        fprintf(stderr, "gclient: STATS FAIL\n");
        return 1;
    }
    printf("gclient: STATS served=%llu\n", (unsigned long long)served);
    printf("gclient: OK\n");
    free(msg);
    close(lane);
    close(bus);
    return 0;
}
