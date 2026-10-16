// /sbin/client (M5.2): непривилегированный демо-клиент.

#include "milin.h"
#include "ipc.h"
#include "helq.h"
#include "bulk.h"

static struct stat64 {
    u64 dev, ino, nlink;
    u32 mode, uid, gid;
    i32 pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    u64 unused[3];
} st_buf;

#define BENCH_N 256    // RTT-итераций на транспорт
#define BENCH_K 64     // enqueue-пачка (один чанк = одна партия)
#define BENCH_BN 64    // M8: RTT крупных (blob-слот)
#define BENCH_TN 32    // M8: пачка крупных (flow control blob'ов)
#define BENCH_BIG 20480 // M8: размер большого WRITE в демо
#define BENCH_CN 32     // M8.1: RTT bulk-цепочкой
#define BULK_BYTES 102400 // M8.1: bulk-демо WRITE (4 blob-слота)
#define BENCH_CLEN 65536  // M8.1: bulk-RTT (3 blob-слота + заголовок)

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

static long wait_bus(void) {
    for (int tries = 0; ; tries++) {
        long bus = ml_connect("milin-bus");
        if (bus >= 0) return bus;
        if (tries > 200) return -1;
        sleep_ms(10);
    }
}

static int lookup_log(long bus, int transport, struct ml_aux *aux, u32 *rights) {
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "log", 4);
    *ml_h(&q, 2) = transport;
    for (int tries = 0; tries < 100; tries++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) return -1;
        struct ml_msg resp;
        if (ml_recv(bus, &resp, 0, 0, aux) <= 0) return -1;
        if (resp.err == ML_E_OK && aux->nhooks >= 1) {
            if (rights) *rights = (u32)*ml_h(&resp, 0);
            return 0;
        }
        if (resp.err == ML_E_PERM) return -2; // прав нет — ретраи не помогут
        sleep_ms(20); // сервер ещё регистрируется или обслуживает другой lane
    }
    return -1;
}

static int demo_seqpacket(long bus, int bench) {
    struct ml_aux aux;
    if (lookup_log(bus, ML_TR_SEQPACKET, &aux, 0) < 0) {
        P("client: seqpacket: lookup FAIL\n");
        return 1;
    }
    P("client: seqpacket: lookup \"log\" -> lane (fd пришёл SCM_RIGHTS)\n");
    long lane = aux.hooks[0];

    if (!bench) {
        const char *m1 = "привет из MiLin: IPC в стиле bragi/mbus";
        const char *m2 = "серверы общаются сообщениями, а не вызовами ядра";
        for (int i = 0; i < 2; i++) {
            const char *t = i == 0 ? m1 : m2;
            struct ml_msg w = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
            if (ml_send(lane, &w, t, m_strlen(t), 0, 0) < 0) {
                P("client: seqpacket: WRITE FAIL\n");
                return 1;
            }
            struct ml_msg wr;
            struct ml_aux raux;
            if (ml_recv(lane, &wr, 0, 0, &raux) <= 0 ||
                wr.msg_id != LOG_WRITE_R || wr.err != ML_E_OK) {
                P("client: seqpacket: ack не пришёл\n");
                return 1;
            }
        }
    }

    if (bench) {
        u64 sum = 0;
        for (int i = 0; i < BENCH_N; i++) {
            u64 t0 = rdtsc();
            struct ml_msg w = { ML_PROTO_LOG, LOG_WRITEQ, 0, 0, {0} };
            ml_send(lane, &w, "x", 1, 0, 0);
            struct ml_msg wr;
            struct ml_aux raux;
            if (ml_recv(lane, &wr, 0, 0, &raux) <= 0) {
                P("client: bench: ack FAIL\n");
                return 1;
            }
            sum += rdtsc() - t0;
        }
        P("ipc-bench: seqpacket rtt avg=");
        print_dec_n(sum / BENCH_N);
        P(" cycles (ядро в data path: sendmsg+recvmsg, n=256)\n");
    }

    sys3(SYS_close, lane, 0, 0); // EOF на сервере -> он возьмёт следующий lane
    return 0;
}

static int demo_helq(long bus) {
    struct ml_aux aux;
    if (lookup_log(bus, ML_TR_HELQ, &aux, 0) < 0 || aux.nhooks < 2) {
        P("client: helq: lookup FAIL\n");
        return 1;
    }
    P("client: helq: lookup \"log\" -> lane + shm-регион очередей\n");
    long lane = aux.hooks[0];      // запасной канал (close-семантика)
    int shmfd = aux.hooks[1];

    struct mlq_pair *p = mlq_map(shmfd);
    if (!p) {
        P("client: helq: mmap FAIL\n");
        return 1;
    }
    sys3(SYS_close, shmfd, 0, 0);  // маппинг живёт своей жизнью

    struct mlq_out out;
    mlq_out_init(&out, p, 0);      // запросы
    struct mlq_in in;
    mlq_in_init(&in, p, 1);        // ответы

    static u8 _sbuf[sizeof(struct ml_msg) + ML_MAXTAIL];
    #define MLQ_SEND(msgid, tptr, tlen, mode) do { \
        struct ml_msg _w = { ML_PROTO_LOG, (msgid), 0, 0, {0} }; \
        m_memcpy(_sbuf, &_w, sizeof(_w)); \
        if ((tlen)) m_memcpy(_sbuf + sizeof(_w), (tptr), (tlen)); \
        mlq_put(&out, (msgid), _sbuf, sizeof(_w) + (tlen), (mode)); \
        mlq_flush(&out); \
    } while (0)

    #define MLQ_RECV(want, mode, outvar) do { \
        for (;;) { \
            struct mlq_view _v; \
            mlq_next(&in, &_v, (mode)); \
            if (_v.len < sizeof(struct ml_msg)) { mlq_consume(&in, &_v); continue; } \
            const struct ml_msg *_m = (const struct ml_msg *)_v.payload; \
            if (_m->msg_id != (want)) { mlq_consume(&in, &_v); continue; } \
            (outvar) = _m; \
            mlq_consume(&in, &_v); \
            break; \
        } \
    } while (0)

    const char *h1 = "асинхронные очереди в разделяемой памяти: zero copy";
    const char *h2 = "будильник — futex, ядро в data path не участвует";
    const struct ml_msg *r;
    for (int i = 0; i < 2; i++) {
        const char *t = i == 0 ? h1 : h2;
        MLQ_SEND(LOG_WRITE, t, m_strlen(t), MLQ_FUTEX);
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
        if (r->err != ML_E_OK) {
            P("client: helq: ack FAIL\n");
            return 1;
        }
    }
    MLQ_SEND(LOG_STATS, 0, 0, MLQ_FUTEX);
    MLQ_RECV(LOG_STATS_R, MLQ_FUTEX, r);
    P("client: helq: WRITE x2 напечатаны с uid, STATS served=");
    print_dec((long)*ml_h((struct ml_msg *)r, 0) % 1000000); // счётчик общий, покажем младшие
    P("\n");

    u64 sum = 0, t0 = 0, dt = 0;

    static u8 bigbuf[sizeof(struct ml_msg) + BENCH_BIG];
    struct ml_msg bw = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
    m_memcpy(bigbuf, &bw, sizeof(bw));
    for (int i = 0; i < BENCH_BIG; i++)
        bigbuf[sizeof(bw) + i] = (u8)('A' + i % 26);
    mlq_put(&out, LOG_WRITE, bigbuf, sizeof(bw) + BENCH_BIG, MLQ_FUTEX);
    mlq_flush(&out);
    MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
    if (r->err != ML_E_OK) {
        P("client: helq: big WRITE FAIL\n");
        return 1;
    }
    P("client: helq: WRITE ");
    print_dec_n(BENCH_BIG);
    P(" байт уехал через blob-слот (в очереди — только ссылка)\n");

    static u8 blobbuf[MLQ_BLOB];
    struct ml_msg qb = { ML_PROTO_LOG, LOG_WRITEQ, 0, 0, {0} };
    m_memcpy(blobbuf, &qb, sizeof(qb));
    m_memset(blobbuf + sizeof(qb), 'x', MLQ_BLOB - sizeof(qb));
    sum = 0;
    for (int i = 0; i < BENCH_BN; i++) {
        t0 = rdtsc();
        mlq_put(&out, LOG_WRITEQ, blobbuf, MLQ_BLOB, MLQ_FUTEX);
        mlq_flush(&out);
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
        sum += rdtsc() - t0;
    }
    P("ipc-bench: helq-blob 32K rtt avg=");
    print_dec_n(sum / BENCH_BN);
    P(" cycles/msg (blob-слот, n=64)\n");

    t0 = rdtsc();
    for (int i = 0; i < BENCH_TN / 4; i++) {
        for (int j = 0; j < 4; j++)
            mlq_put(&out, LOG_WRITEQ, blobbuf, MLQ_BLOB, MLQ_FUTEX);
        for (int j = 0; j < 4; j++)
            MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
    }
    dt = rdtsc() - t0;
    P("ipc-bench: helq-blob 32K pipe avg=");
    print_dec_n(dt / BENCH_TN);
    P(" cycles/msg (конвейер, 4 в полёте, n=32)\n");

    static u8 bulkbuf[sizeof(struct ml_msg) + BULK_BYTES];
    m_memcpy(bulkbuf, &bw, sizeof(bw));
    for (int i = 0; i < BULK_BYTES; i++)
        bulkbuf[sizeof(bw) + i] = (u8)('a' + i % 26);
    mlq_put(&out, LOG_WRITE, bulkbuf, sizeof(bw) + BULK_BYTES, MLQ_FUTEX);
    MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
    if (r->err != ML_E_OK) {
        P("client: helq: bulk WRITE FAIL\n");
        return 1;
    }
    P("client: helq: WRITE ");
    print_dec_n(BULK_BYTES);
    P(" байт уехал цепочкой из 4 blob-слотов (в очереди — одна ссылка)\n");

    m_memcpy(bulkbuf, &qb, sizeof(qb));
    m_memset(bulkbuf + sizeof(qb), 'x', BENCH_CLEN);
    sum = 0;
    for (int i = 0; i < BENCH_CN; i++) {
        t0 = rdtsc();
        mlq_put(&out, LOG_WRITEQ, bulkbuf, sizeof(qb) + BENCH_CLEN, MLQ_FUTEX);
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
        sum += rdtsc() - t0;
    }
    P("ipc-bench: helq-bulk 64K rtt avg=");
    print_dec_n(sum / BENCH_CN);
    P(" cycles/msg (цепочка blob-слотов, n=32)\n");

    sum = 0;
    for (int i = 0; i < BENCH_N; i++) {
        u64 t0 = rdtsc();
        MLQ_SEND(LOG_WRITEQ, "x", 1, MLQ_FUTEX);
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
        sum += rdtsc() - t0;
    }
    P("ipc-bench: helq-futex rtt avg=");
    print_dec_n(sum / BENCH_N);
    P(" cycles (парковка futex'ом, n=256)\n");

    sum = 0;
    for (int i = 0; i < BENCH_N; i++) {
        u64 t0 = rdtsc();
        MLQ_SEND(LOG_WRITEQ, "x", 1, MLQ_YIELD);
        MLQ_RECV(LOG_WRITE_R, MLQ_YIELD, r);
        sum += rdtsc() - t0;
    }
    P("ipc-bench: helq-yield rtt avg=");
    print_dec_n(sum / BENCH_N);
    P(" cycles (поллинг+yield, n=256)\n");

    t0 = rdtsc();
    for (int i = 0; i < BENCH_K; i++) {
        struct ml_msg w = { ML_PROTO_LOG, LOG_WRITEQ, 0, 0, {0} };
        mlq_put(&out, LOG_WRITEQ, &w, sizeof(w), MLQ_FUTEX);
    }
    mlq_flush(&out);
    dt = rdtsc() - t0;
    for (int i = 0; i < BENCH_K; i++)
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
    P("ipc-bench: helq enqueue avg=");
    print_dec_n(dt / BENCH_K);
    P(" cycles/msg (пачка в один чанк, k=64)\n");

    MLQ_SEND(LOG_CLOSE, 0, 0, MLQ_FUTEX);
    MLQ_RECV(LOG_CLOSE_R, MLQ_FUTEX, r);
    sys6(SYS_munmap, (long)p, MLQ_SIZE, 0, 0, 0, 0);
    sys3(SYS_close, lane, 0, 0);
    P("client: helq: демо + бенчмарк завершены, lane закрыт\n");
    return 0;
    #undef MLQ_SEND
    #undef MLQ_RECV
}

static int demo_fuse(void) {
    static const char msg[] = "записано клиентом через FUSE_WRITE (MiLin M5.2)\n";

    long fd = sys3(SYS_open, (long)"/mnt/note.txt", O_RDWR, 0);
    if (fd < 0) {
        P("client: open /mnt/note.txt: FAIL\n");
        return 1;
    }
    long n = sys3(SYS_write, fd, (long)msg, sizeof(msg) - 1);
    if (n != (long)(sizeof(msg) - 1)) {
        P("client: FUSE_WRITE: FAIL\n");
        return 1;
    }
    sys3(SYS_lseek, fd, 0, SEEK_SET);
    static char rbuf[256];
    n = sys3(SYS_read, fd, (long)rbuf, sizeof(rbuf));
    if (n != (long)(sizeof(msg) - 1)) {
        P("client: FUSE_READ: FAIL\n");
        return 1;
    }
    for (long i = 0; i < n; i++)
        if (rbuf[i] != msg[i]) {
            P("client: FUSE: прочитанное не совпало\n");
            return 1;
        }
    sys3(SYS_close, fd, 0, 0);
    P("client: FUSE_WRITE/READ: записано ");
    print_dec_n(sizeof(msg) - 1);
    P(" байт, прочитано обратно — совпало\n");

    long dfd = sys3(SYS_open, (long)"/mnt", O_RDONLY | O_DIRECTORY, 0);
    if (dfd < 0) {
        P("client: open /mnt: FAIL\n");
        return 1;
    }
    static u8 dbuf[1024];
    int saw_readme = 0, saw_note = 0;
    for (;;) {
        long r = sys3(SYS_getdents64, dfd, (long)dbuf, sizeof(dbuf));
        if (r <= 0) break;
        for (long off = 0; off < r; ) {
            struct ldirent64 *d = (struct ldirent64 *)(dbuf + off);
            P("client: /mnt: ");
            print_str(d->d_name);
            P(d->d_type == 4 ? " (dir)\n" : " (reg)\n");
            if (m_strncmp(d->d_name, "README", 7) == 0) saw_readme = 1;
            if (m_strncmp(d->d_name, "note.txt", 9) == 0) saw_note = 1;
            off += d->d_reclen;
        }
    }
    sys3(SYS_close, dfd, 0, 0);
    if (!saw_readme || !saw_note) {
        P("client: readdir: список неполный\n");
        return 1;
    }
    P("client: readdir: README + note.txt найдены: OK\n");

    if (sys3(SYS_stat, (long)"/mnt/note.txt", (long)&st_buf, 0) != 0 ||
        st_buf.size != sizeof(msg) - 1) {
        P("client: stat /mnt/note.txt: FAIL\n");
        return 1;
    }
    P("client: stat /mnt/note.txt: size=");
    print_dec_n((long)st_buf.size);
    P(" (WRITE виден в атрибутах)\n");
    return 0;
}

static int demo_bulk(void) {
    long bus = wait_bus();
    if (bus < 0) {
        P("client: bulk: шина не отвечает\n");
        return 1;
    }
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "log", 4);
    *ml_h(&q, 2) = ML_TR_BULK;
    struct ml_aux aux;
    int ok = 0;
    for (int t = 0; t < 100 && !ok; t++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) return 1;
        struct ml_msg resp;
        if (ml_recv(bus, &resp, 0, 0, &aux) <= 0) return 1;
        if (resp.err == ML_E_OK && aux.nhooks >= 2) ok = 1;
        else if (resp.err != ML_E_OK) return 1;
        else sleep_ms(20); // сервер ещё обслуживает другой lane
    }
    if (!ok) {
        P("client: bulk: lookup FAIL\n");
        return 1;
    }
    struct mlb_hdr *ar = mlb_map(aux.hooks[1]);
    if (!ar) {
        P("client: bulk: mmap FAIL\n");
        return 1;
    }
    sys3(SYS_close, aux.hooks[1], 0, 0);
    long lane = aux.hooks[0];

    static u8 pat[MLB_SLOT];
    for (u32 i = 0; i < sizeof(pat); i++)
        pat[i] = (u8)('A' + i % 26);

    u64 t0 = rdtsc();
    for (int it = 0; it < 8; it++) {
        int slot = mlb_take(ar, MLB_WAIT);
        if (!slot) {
            P("client: bulk: слот не выдан (сервер умер?)\n");
            return 1;
        }
        m_memcpy(mlb_ptr(ar, slot), pat, sizeof(pat));
        struct ml_msg w = { ML_PROTO_LOG, LOG_WRITEB, 0, 0, {0} };
        *ml_h(&w, 0) = (u32)slot;
        *ml_h(&w, 1) = sizeof(pat);
        if (ml_send(lane, &w, 0, 0, 0, 0) < 0) {
            P("client: bulk: send FAIL\n");
            return 1;
        }
        struct ml_msg r;
        if (ml_recv(lane, &r, 0, 0, 0) <= 0 || r.msg_id != LOG_WRITE_R
            || r.err != ML_E_OK) {
            P("client: bulk: ack FAIL\n");
            return 1;
        }
        if (*ml_h(&r, 0) != sizeof(pat)) {
            P("client: bulk: stored_len FAIL\n");
            return 1;
        }
    }
    u64 dt = (rdtsc() - t0) / 8;
    P("client: bulk: 8 x 128KB через арену, ack/len сошлись (avg ");
    print_dec_n((long)dt);
    P(" cycles/write)\n");
    P("M22 BULK OK\n");

    sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
    sys3(SYS_close, lane, 0, 0);
    sys3(SYS_close, bus, 0, 0);
    return 0;
}

static int run(void) {
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("client: setuid(1000): FAIL\n");
        return 1;
    }
    P("client: uid=");
    print_dec_n(sys3(SYS_getuid, 0, 0, 0));
    P(" (был 0 → MULTIUSER работает)\n");

    long bus = wait_bus();
    if (bus < 0) {
        P("client: IPC: шина не отвечает\n");
        return 1;
    }
    ml_passcred(bus);

    if (demo_seqpacket(bus, 0)) return 1;
    P("client: M5-регрессия (seqpacket WRITE x2 + ack): OK\n");

    if (demo_helq(bus)) return 1;

    if (demo_seqpacket(bus, 1)) return 1;

    struct ml_aux aux;
    u32 cap = 0;
    if (lookup_log(bus, ML_TR_SEQPACKET, &aux, &cap) < 0) {
        P("client: stats lane: lookup FAIL\n");
        return 1;
    }
    struct ml_msg s = { ML_PROTO_LOG, LOG_STATS, 0, 0, {0} };
    ml_send(aux.hooks[0], &s, 0, 0, 0, 0);
    struct ml_msg sr;
    struct ml_aux saux;
    if (ml_recv(aux.hooks[0], &sr, 0, 0, &saux) <= 0 ||
        *ml_h(&sr, 0) != 2 + 2 + 1 + 1 + 3 * BENCH_N + BENCH_K + BENCH_BN +
                          BENCH_TN + BENCH_CN) {
        P("client: STATS: счётчик разошёлся\n");
        return 1;
    }
    P("client: STATS served=");
    print_dec((long)*ml_h(&sr, 0));
    P(": все сообщения доставлены\n");
    u32 bus_pid = aux.has_cred ? aux.cred.pid : 0;

    P("client: capability: права на lane: 0x");
    print_hex_flat(cap);
    P(" (выдала шина по uid-политике сервера)\n");
    struct ml_msg rq = { ML_PROTO_LOG, LOG_RESET, 0, 0, {0} };
    ml_send(aux.hooks[0], &rq, 0, 0, 0, 0);
    struct ml_msg rr;
    struct ml_aux rraux;
    if (ml_recv(aux.hooks[0], &rr, 0, 0, &rraux) <= 0 ||
        rr.msg_id != LOG_RESET_R || rr.err != ML_E_PERM) {
        P("client: capability: RESET НЕ отклонён — дыра в модели прав!\n");
        return 1;
    }
    P("client: capability: RESET отклонён (ML_E_PERM, нет ML_CAP_ADMIN): OK\n");
    sys3(SYS_close, aux.hooks[0], 0, 0);

    struct ml_msg wq = { ML_PROTO_BUS, BUS_WHEREIS, 0, 0, {0} };
    m_memcpy(wq.head, "log", 4);
    if (ml_send(bus, &wq, 0, 0, 0, 0) < 0) return 1;
    struct ml_msg wr;
    struct ml_aux waux;
    if (ml_recv(bus, &wr, 0, 0, &waux) <= 0 || wr.err != ML_E_OK) {
        P("client: whereis: FAIL\n");
        return 1;
    }
    int lpid = (int)*ml_h(&wr, 0);
    P("client: whereis: \"log\" -> pid ");
    print_dec_n(lpid);
    P("\n");
    sys3(SYS_kill, lpid, SIGKILL, 0);
    P("client: fault: SIGKILL отправлен log-серверу (pid ");
    print_dec_n(lpid);
    P(")\n");
    sleep_ms(100); // шине нужно заметить EOF и подчистить имя

    u32 newcap = 0;
    struct ml_aux a2;
    if (lookup_log(bus, ML_TR_SEQPACKET, &a2, &newcap) < 0) {
        P("client: fault: перезапущенный сервер так и не появился\n");
        return 1;
    }
    P("client: fault: шина выдала lane на ПЕРЕЗАПУЩЕННЫЙ сервер\n");
    const char *after = "IPC пережил падение сервера: супервизор перезапустил его";
    struct ml_msg w2 = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
    if (ml_send(a2.hooks[0], &w2, after, m_strlen(after), 0, 0) < 0) {
        P("client: fault: WRITE на новый lane: FAIL\n");
        return 1;
    }
    struct ml_msg a2r;
    struct ml_aux a2aux;
    if (ml_recv(a2.hooks[0], &a2r, 0, 0, &a2aux) <= 0 || a2r.err != ML_E_OK) {
        P("client: fault: ack на новый lane: FAIL\n");
        return 1;
    }
    P("client: fault: WRITE на новый lane подтверждён — автоперезапуск работает\n");
    sys3(SYS_close, a2.hooks[0], 0, 0);

    int vpid = sys3(SYS_fork, 0, 0, 0);
    if (vpid == 0) {
        long vbus = wait_bus();
        if (vbus >= 0) {
            struct ml_aux vaux;
            u32 vrights;
            if (lookup_log(vbus, ML_TR_HELQ, &vaux, &vrights) == 0 && vaux.nhooks >= 2) {
                struct mlq_pair *vp = mlq_map(vaux.hooks[1]);
                if (vp) {
                    struct mlq_out vout;
                    mlq_out_init(&vout, vp, 0);
                    static u8 vbuf[sizeof(struct ml_msg) + 64];
                    struct ml_msg vw = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
                    m_memcpy(vbuf, &vw, sizeof(vw));
                    const char *vt = "жертва: жива, держу helq-lane";
                    m_memcpy(vbuf + sizeof(vw), vt, m_strlen(vt));
                    mlq_put(&vout, LOG_WRITE, vbuf,
                            sizeof(vw) + m_strlen(vt), MLQ_FUTEX);
                    mlq_flush(&vout);
                    for (;;) sleep_ms(1000); // дальше — только смерть
                }
            }
        }
        sys3(SYS_exit, 0, 0, 0);
    }
    sleep_ms(300); // жертва успевает взять lane и написать
    P("client: revoke: жертва (pid ");
    print_dec_n(vpid);
    P(") держит helq-lane — SIGKILL\n");
    sys3(SYS_kill, vpid, SIGKILL, 0);
    int vst = 0;
    sys5(SYS_wait4, vpid, (long)&vst, 0, 0, 0); // реап: иначе зомби сбивает sig0

    struct ml_aux nxaux;
    u32 nxr = 0;
    if (lookup_log(bus, ML_TR_HELQ, &nxaux, &nxr) < 0 || nxaux.nhooks < 2) {
        P("client: revoke: сервер не отпустил lane мертвеца — FAIL\n");
        return 1;
    }
    struct mlq_pair *np = mlq_map(nxaux.hooks[1]);
    if (!np) {
        P("client: revoke: mmap FAIL\n");
        return 1;
    }
    sys3(SYS_close, nxaux.hooks[1], 0, 0);
    struct mlq_out nout;
    mlq_out_init(&nout, np, 0);
    struct mlq_in nin;
    mlq_in_init(&nin, np, 1);
    static u8 nbuf[sizeof(struct ml_msg) + 80];
    struct ml_msg nw = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
    m_memcpy(nbuf, &nw, sizeof(nw));
    const char *nt = "capability мертвеца отозваны — сервер жив";
    m_memcpy(nbuf + sizeof(nw), nt, m_strlen(nt));
    mlq_put(&nout, LOG_WRITE, nbuf, sizeof(nw) + m_strlen(nt), MLQ_FUTEX);
    mlq_flush(&nout);
    for (;;) {
        struct mlq_view nv;
        mlq_next(&nin, &nv, MLQ_FUTEX);
        if (nv.len < sizeof(struct ml_msg)) continue;
        const struct ml_msg *nm = (const struct ml_msg *)nv.payload;
        if (nm->msg_id != LOG_WRITE_R) continue;
        if (nm->err != ML_E_OK) {
            P("client: revoke: ack FAIL\n");
            return 1;
        }
        break;
    }
    static u8 cbuf2[sizeof(struct ml_msg)];
    struct ml_msg cl = { ML_PROTO_LOG, LOG_CLOSE, 0, 0, {0} };
    m_memcpy(cbuf2, &cl, sizeof(cl));
    mlq_put(&nout, LOG_CLOSE, cbuf2, sizeof(cl), MLQ_FUTEX);
    mlq_flush(&nout);
    for (;;) {
        struct mlq_view cv;
        mlq_next(&nin, &cv, MLQ_FUTEX);
        if (cv.len < sizeof(struct ml_msg)) continue;
        const struct ml_msg *cm = (const struct ml_msg *)cv.payload;
        if (cm->msg_id != LOG_CLOSE_R) continue;
        break;
    }
    sys6(SYS_munmap, (long)np, MLQ_SIZE, 0, 0, 0, 0);
    sys3(SYS_close, nxaux.hooks[0], 0, 0);
    P("client: revoke: права мертвеца отозваны, сервер обслужил новый lane: OK\n");

    P("client: cascade: убиваю ШИНУ (pid ");
    print_dec_n(bus_pid);
    P(") — система должна самоисцелиться\n");
    sys3(SYS_kill, bus_pid, SIGKILL, 0);
    for (int i = 0; i < 200; i++) {
        if (sys3(SYS_kill, bus_pid, 0, 0) != 0) break;
        sleep_ms(10);
    }
    sys3(SYS_close, bus, 0, 0); // конн умер вместе с шиной
    bus = wait_bus();
    if (bus < 0) {
        P("client: cascade: шина не вернулась\n");
        return 1;
    }
    P("client: cascade: шина перезапущена супервизором, клиент переподключился\n");

    struct ml_aux caux;
    u32 ccap = 0;
    if (lookup_log(bus, ML_TR_SEQPACKET, &caux, &ccap) < 0) {
        P("client: cascade: перерегистрировавшийся сервер не найден\n");
        return 1;
    }
    P("client: cascade: log-сервер перерегистрировался в новой шине\n");
    const char *cmsg = "шина умерла — IPC пережил каскадный перезапуск";
    struct ml_msg cw = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
    if (ml_send(caux.hooks[0], &cw, cmsg, m_strlen(cmsg), 0, 0) < 0) {
        P("client: cascade: WRITE FAIL\n");
        return 1;
    }
    struct ml_msg cwr;
    struct ml_aux cwaux;
    if (ml_recv(caux.hooks[0], &cwr, 0, 0, &cwaux) <= 0 || cwr.err != ML_E_OK) {
        P("client: cascade: ack FAIL\n");
        return 1;
    }
    struct ml_msg cs = { ML_PROTO_LOG, LOG_STATS, 0, 0, {0} };
    ml_send(caux.hooks[0], &cs, 0, 0, 0, 0);
    struct ml_msg csr;
    struct ml_aux csaux;
    if (ml_recv(caux.hooks[0], &csr, 0, 0, &csaux) <= 0 || *ml_h(&csr, 0) != 4) {
        P("client: cascade: счётчик разошёлся (ожидал 4)\n");
        return 1;
    }
    P("client: cascade: STATS served=4 — state пережил смерть сервера и шины: OK\n");
    sys3(SYS_close, caux.hooks[0], 0, 0);
    sys3(SYS_close, bus, 0, 0);

    P("client: glibc: запускаю /sbin/gclient (динамический)\n");
    int gpid = sys3(SYS_fork, 0, 0, 0);
    if (gpid == 0) {
        char *argv[] = { "gclient", 0 };
        if (sys3(SYS_execve, (long)"/sbin/gclient", (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }
    int gst = 0;
    sys5(SYS_wait4, gpid, (long)&gst, 0, 0, 0);
    if ((gst & 0x7f) != 0 || ((gst >> 8) & 0xff) != 0) {
        P("client: glibc: gclient FAIL\n");
        return 1;
    }
    P("client: glibc: gclient OK — динамический userspace работает\n");

    long rfd = sys3(SYS_open, (long)"/mnt/README", 0 /*O_RDONLY*/, 0);
    if (rfd < 0) {
        P("client: open /mnt/README: FAIL\n");
        return 1;
    }
    char fbuf[64];
    long fn = sys3(SYS_read, rfd, (long)fbuf, sizeof(fbuf) - 1);
    sys3(SYS_close, rfd, 0, 0);
    if (fn <= 0) {
        P("client: read /mnt/README: FAIL\n");
        return 1;
    }
    fbuf[fn] = 0;
    P("client: /mnt/README: ");
    print_str(fbuf);
    const char *want = "MiLin FUSE OK\n";
    if ((long)m_strlen(want) != fn || m_strncmp(fbuf, want, fn) != 0) {
        P("client: /mnt/README: содержимое не совпало\n");
        return 1;
    }
    P("client: FUSE-регрессия + чтение содержимого: OK\n");

    if (demo_fuse()) return 1;

    if (demo_bulk()) return 1;

    P("client: IPC-демо + capability + отказ-инъекция + FUSE: OK\n");
    return 0;
}

void milin_main(void) {
    sys3(SYS_exit, run(), 0, 0);
    __builtin_unreachable();
}
