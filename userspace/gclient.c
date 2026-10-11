// /sbin/gclient (M8/M8.2): IPC-клиент, собранный с glibc (динамически, ld.so).
// Доказывает, что ядро тянет обычный userspace: ELF-интерпретатор, stdio,
// malloc, mmap, futex, fork+waitpid, полный набор системных вызовов.
// M8.2 — полировка: тот же IPC-стек, что у nostdlib-клиента, но из libc:
//   * helq-транспорт: mmap очередей + futex-паркинг + RTT-бенчмарк
//     (userspace/helq.h общий, собирается с -DMILIN_GLIBC);
//   * bulk 100КБ через цепочку blob-слотов — целостность меряем stored_len;
//   * fork+waitpid: ребёнок читает FUSE-файл stdio'ем (fread/fwrite).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <gnu/libc-version.h>

#include "helq.h"     // -DMILIN_GLIBC: raw-хелперы заменяются libc-шимом
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

// LOOKUP с правами на хуки: transport=ML_TR_SEQPACKET -> 1 fd (lane),
// ML_TR_HELQ -> 2 fd (lane + shm-регион очередей)
static int lookup_log_tr(int bus, int tr, int *hooks, int *nhooks) {
    struct ml_msg q;
    memset(&q, 0, sizeof(q));
    q.proto = ML_PROTO_BUS;
    q.msg_id = BUS_LOOKUP;
    strcpy(q.head, "log");
    *ml_h(&q, 2) = tr;
    for (int t = 0; t < 100; t++) {
        if (send(bus, &q, sizeof(q), MSG_NOSIGNAL) < 0) return -1;
        struct ml_msg r;
        struct iovec iov = { &r, sizeof(r) };
        // creds + права + до 2 хуков: ядро прикладывает cmsg'и (SO_PASSCRED)
        char cbuf[2 * CMSG_SPACE(sizeof(int)) + CMSG_SPACE(3 * sizeof(unsigned))];
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
            *nhooks = 0;
            for (struct cmsghdr *c = CMSG_FIRSTHDR(&mh); c; c = CMSG_NXTHDR(&mh, c))
                if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS) {
                    int nfd = (int)(c->cmsg_len - CMSG_LEN(0)) / (int)sizeof(int);
                    memcpy(hooks + *nhooks, CMSG_DATA(c), (size_t)nfd * sizeof(int));
                    *nhooks += nfd;
                }
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

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

// дождаться ответа (zero copy из общей памяти), вернуть ml_msg или 0
static const struct ml_msg *helq_recv(struct mlq_in *in, u32 want) {
    for (;;) {
        struct mlq_view v;
        mlq_next(in, &v, MLQ_FUTEX);
        if (v.len < sizeof(struct ml_msg)) { mlq_consume(in, &v); continue; }
        const struct ml_msg *m = v.payload;
        if (m->msg_id != want) { mlq_consume(in, &v); continue; }
        mlq_consume(in, &v);
        return m;
    }
}

// M8.2: helq-транспорт из glibc — тот же стек очередей, libc-вызовы
static int helq_demo(int bus) {
    int hooks[4] = { -1, -1, -1, -1 }, nh = 0;
    if (lookup_log_tr(bus, ML_TR_HELQ, hooks, &nh) < 0 || nh < 2) {
        fprintf(stderr, "gclient: helq: lookup FAIL\n");
        return -1;
    }
    struct mlq_pair *p = mmap(0, MLQ_SIZE, PROT_READ | PROT_WRITE,
                              MAP_SHARED, hooks[1], 0);
    if (p == MAP_FAILED) {
        fprintf(stderr, "gclient: helq: mmap FAIL\n");
        return -1;
    }
    close(hooks[1]);
    if (p->magic != MLQ_MAGIC) {
        fprintf(stderr, "gclient: helq: magic FAIL\n");
        munmap(p, MLQ_SIZE);
        return -1;
    }
    struct mlq_out out;
    mlq_out_init(&out, p, 0);
    struct mlq_in in;
    mlq_in_init(&in, p, 1);

    // RTT на очередях: futex-паркинг libc'ей
    u64 sum = 0;
    struct ml_msg w = { ML_PROTO_LOG, LOG_WRITEQ, 0, 0, {0} };
    u8 buf[sizeof(struct ml_msg)];
    memcpy(buf, &w, sizeof(w));
    for (int i = 0; i < 128; i++) {
        u64 t0 = rdtsc();
        mlq_put(&out, LOG_WRITEQ, buf, sizeof(w), MLQ_FUTEX);
        mlq_flush(&out);
        if (!helq_recv(&in, LOG_WRITE_R)) {
            fprintf(stderr, "gclient: helq: ack FAIL\n");
            return -1;
        }
        sum += rdtsc() - t0;
    }
    printf("gclient: bench: helq-futex rtt avg=%llu cycles (n=128)\n",
           (unsigned long long)(sum / 128));

    // M8.1: bulk 100КБ цепочкой blob-слотов; целостность подтверждает
    // stored_len в ack — сервер получил все 4 слота как одно сообщение
    static u8 bulk[sizeof(struct ml_msg) + 102400];
    struct ml_msg bw = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
    memcpy(bulk, &bw, sizeof(bw));
    for (int i = 0; i < 102400; i++)
        bulk[sizeof(bw) + i] = (u8)('A' + i % 26);
    mlq_put(&out, LOG_WRITE, bulk, sizeof(bw) + 102400, MLQ_FUTEX);
    const struct ml_msg *r = helq_recv(&in, LOG_WRITE_R);
    if (!r || r->err != ML_E_OK || *ml_h((struct ml_msg *)r, 0) != 102400) {
        fprintf(stderr, "gclient: helq: bulk FAIL\n");
        return -1;
    }
    printf("gclient: helq: bulk 100КБ цепочкой blob-слотов: stored_len=%llu\n",
           (unsigned long long)*ml_h((struct ml_msg *)r, 0));

    // корректный выход: CLOSE -> сервер закрывает lane
    struct ml_msg cl = { ML_PROTO_LOG, LOG_CLOSE, 0, 0, {0} };
    memcpy(buf, &cl, sizeof(cl));
    mlq_put(&out, LOG_CLOSE, buf, sizeof(cl), MLQ_FUTEX);
    mlq_flush(&out);
    if (!helq_recv(&in, LOG_CLOSE_R)) {
        fprintf(stderr, "gclient: helq: CLOSE FAIL\n");
        return -1;
    }
    munmap(p, MLQ_SIZE);
    close(hooks[0]);
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

    int lane = -1, nh = 0;
    if (lookup_log_tr(bus, ML_TR_SEQPACKET, &lane, &nh) < 0 || lane < 0) {
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
    close(lane);

    // M8.2: helq-транспорт и bulk-цепочка тем же клиентом
    if (helq_demo(bus) < 0) return 1;

    // M8.2: fork+waitpid — ребёнок читает/пишет FUSE-файлы stdio'ем
    pid_t c = fork();
    if (c < 0) {
        perror("gclient: fork");
        return 1;
    }
    if (c == 0) {
        FILE *f = fopen("/mnt/README", "rb");
        if (!f) _exit(2);
        char rb[32];
        size_t n = fread(rb, 1, sizeof(rb) - 1, f);
        rb[n] = 0;
        int ok = strcmp(rb, "MiLin FUSE OK\n") == 0;
        fclose(f);
        FILE *g = fopen("/mnt/note.txt", "r+");
        if (!g) _exit(3);
        fputs("glibc stdio: запись через FUSE\n", g);
        if (fclose(g) != 0) _exit(4);
        _exit(ok ? 0 : 1);
    }
    int st = 0;
    if (waitpid(c, &st, 0) < 0 || !WIFEXITED(st) || WEXITSTATUS(st) != 0) {
        fprintf(stderr, "gclient: fork/stdio FAIL (status=0x%x)\n", st);
        return 1;
    }
    printf("gclient: fork+waitpid: ребёнок прочитал README и дописал note.txt через stdio\n");

    free(msg);
    close(bus);
    printf("gclient: OK\n");
    return 0;
}
