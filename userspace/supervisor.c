// /init MiLin (M5 + M5.1): супервизор серверов + микроядерный IPC.
//
// M4: порождает серверы (execve), отдаёт им железо (UIO) и ФС (FUSE),
//     непривилегированный клиент читает файл с FUSE-сервера.
// M5: серверы не сироты — у них есть шина (mlbus, идея mbus из Managarm).
//     Клиент получает lane-хэндл через реестр и пишет в log-сервер
//     сообщениями bragi-формата. Хэндлы = fd, передача = SCM_RIGHTS,
//     транспорт = AF_UNIX SOCK_SEQPACKET.
// M5.1: второй транспорт — асинхронные очереди HelQueue в разделяемой
//     памяти (userspace/helq.h): zero-copy элементы, будильник futex,
//     creds проверяются ядром (SCM_CREDENTIALS) и шиной. Плюс честный
//     бенчмарк транспортов для отчёта M6 (маркер ipc-bench в boot.log).
// M7: capability-модель на lane'ах (права выдает шина по политике
//     сервера, сервер enforced'ит по операциям) и отказоустойчивость:
//     супервизор следит за сервисами через wait4(-1) и перезапускает
//     павших; клиент демонстрирует полный цикл — узнаёт pid сервера
//     через WHEREIS, убивает его SIGKILL'ом и продолжает работу на
//     lane'е, выданном уже ПЕРЕЗАПУЩЕННЫМ сервером.
//
//   супервизор  /init        — порождает, следит и перезапускает серверы
//   реестр      /sbin/mlbus  — именованные lane'и (mbus) + права (M7)
//   log-сервер  /sbin/logsrv — сервис, принимающий сообщения
//   драйвер     /sbin/uiod   — UIO: userspace-драйвер PCI-устройства
//   фс-сервер   /sbin/fused  — FUSE: файловая система в userspace
//   клиент      fork(setuid 1000) — IPC-демо + capability + отказ-инъекция

#include "milin.h"
#include "ipc.h"
#include "helq.h"

static struct stat64 {
    u64 dev, ino, nlink;
    u32 mode, uid, gid;
    i32 pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    u64 unused[3];
} st_buf;

// --- бенчмарк (M5.1): числа уезжают в docs/ отчёта M6 ---
#define BENCH_N 256   // RTT-итераций на транспорт
#define BENCH_K 64    // enqueue-пачка (один чанк = одна партия)

static inline u64 rdtsc(void) {
    u32 lo, hi;
    __asm__ volatile ("rdtsc" : "=a"(lo), "=d"(hi));
    return ((u64)hi << 32) | lo;
}

// дождаться шины (форк не гарантирует порядок старта)
static long wait_bus(void) {
    for (int tries = 0; ; tries++) {
        long bus = ml_connect("milin-bus");
        if (bus >= 0) return bus;
        if (tries > 200) return -1;
        sleep_ms(10);
    }
}

// LOOKUP(name, transport) -> lane (+ shm для helq); повторяет попытки.
// имя живёт в head[0..15] (строка), transport — в head-слоте 2
// (слот 0 занят именем: bragi-поля не должны пересекаться).
// M7: шина возвращает права на lane (head-слот 0 ответа) — capability
// маска, выданная по uid-политике сервера
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

// --- M5: классический обмен поверх SOCK_SEQPACKET ---
static int demo_seqpacket(long bus, int bench) {
    struct ml_aux aux;
    if (lookup_log(bus, ML_TR_SEQPACKET, &aux, 0) < 0) {
        P("client: seqpacket: lookup FAIL\n");
        return 1;
    }
    P("client: seqpacket: lookup \"log\" -> lane (fd пришёл SCM_RIGHTS)\n");
    long lane = aux.hooks[0];

    // функциональные WRITE: только в demo-режиме; bench-lane чистый,
    // чтобы счётчик сервера сходился с арифметикой клиента
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
        // RTT: sendmsg+recvmsg на каждый пинг-понг — ядро в data path
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

// --- M5.1: обмен поверх асинхронных очередей в разделяемой памяти ---
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

    // helper: отправить bragi-сообщение ОДНИМ элементом (ml_msg + tail
    // в одном payload — сервер парсит их как единое целое)
    static u8 _sbuf[sizeof(struct ml_msg) + ML_MAXTAIL];
    #define MLQ_SEND(msgid, tptr, tlen, mode) do { \
        struct ml_msg _w = { ML_PROTO_LOG, (msgid), 0, 0, {0} }; \
        m_memcpy(_sbuf, &_w, sizeof(_w)); \
        if ((tlen)) m_memcpy(_sbuf + sizeof(_w), (tptr), (tlen)); \
        mlq_put(&out, (msgid), _sbuf, sizeof(_w) + (tlen), (mode)); \
        mlq_flush(&out); \
    } while (0)

    // функция: дождаться ответа нужного типа (zero copy из общей памяти)
    #define MLQ_RECV(want, mode, outvar) do { \
        for (;;) { \
            struct mlq_view _v; \
            mlq_next(&in, &_v, (mode)); \
            if (_v.len < sizeof(struct ml_msg)) continue; \
            const struct ml_msg *_m = (const struct ml_msg *)_v.payload; \
            if (_m->msg_id != (want)) continue; \
            (outvar) = _m; \
            break; \
        } \
    } while (0)

    // функциональный демо-цикл: сервер печатает с uid (creds через шину)
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

    // RTT futex-режим: парковка настоящим futex
    u64 sum = 0;
    for (int i = 0; i < BENCH_N; i++) {
        u64 t0 = rdtsc();
        MLQ_SEND(LOG_WRITEQ, "x", 1, MLQ_FUTEX);
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
        sum += rdtsc() - t0;
    }
    P("ipc-bench: helq-futex rtt avg=");
    print_dec_n(sum / BENCH_N);
    P(" cycles (парковка futex'ом, n=256)\n");

    // RTT yield-режим: поллинг с sched_yield — ни одного syscall'а
    // на сообщение кроме yield'а; это потолок скорости транспорта
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

    // enqueue-пачка: K сообщений в один чанк одним flush — цена ассинхронной
    // постановки (у Managarm так ездят SQ-элементы: батчами, без ожидания)
    u64 t0 = rdtsc();
    for (int i = 0; i < BENCH_K; i++) {
        struct ml_msg w = { ML_PROTO_LOG, LOG_WRITEQ, 0, 0, {0} };
        mlq_put(&out, LOG_WRITEQ, &w, sizeof(w), MLQ_FUTEX);
    }
    mlq_flush(&out);
    u64 dt = rdtsc() - t0;
    for (int i = 0; i < BENCH_K; i++)
        MLQ_RECV(LOG_WRITE_R, MLQ_FUTEX, r);
    P("ipc-bench: helq enqueue avg=");
    print_dec_n(dt / BENCH_K);
    P(" cycles/msg (пачка в один чанк, k=64)\n");

    // корректный выход: CLOSE -> сервер закрывает lane
    MLQ_SEND(LOG_CLOSE, 0, 0, MLQ_FUTEX);
    MLQ_RECV(LOG_CLOSE_R, MLQ_FUTEX, r);
    sys6(SYS_munmap, (long)p, MLQ_SIZE, 0, 0, 0, 0);
    sys3(SYS_close, lane, 0, 0);
    P("client: helq: демо + бенчмарк завершены, lane закрыт\n");
    return 0;
    #undef MLQ_SEND
    #undef MLQ_RECV
}

static int run_client(void) {
    // непривилегированный пользователь: MULTIUSER вернулся
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

    // 1) M5-регрессия: seqpacket-обмен (без бенчмарка)
    if (demo_seqpacket(bus, 0)) return 1;
    P("client: M5-регрессия (seqpacket WRITE x2 + ack): OK\n");

    // 2) M5.1: helq-демо + бенчмарки очередей
    if (demo_helq(bus)) return 1;

    // 3) бенчмарк seqpacket: те же RTT, что у очередей — честное сравнение
    if (demo_seqpacket(bus, 1)) return 1;

    // 4) контрольная STATS: суммарный счётчик сервера
    //    2 (M5) + 2 (helq демо) + 3*BENCH_N (RTT x3) + BENCH_K (пачка)
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
        *ml_h(&sr, 0) != 2 + 2 + 3 * BENCH_N + BENCH_K) {
        P("client: STATS: счётчик разошёлся\n");
        return 1;
    }
    P("client: STATS served=");
    print_dec((long)*ml_h(&sr, 0));
    P(": все сообщения доставлены\n");

    // 5) M7: capability-демо. uid 1000 получил от шины только world-права:
    //    RESET — админ-операция (ML_CAP_ADMIN), сервер обязан отклонить
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

    // 6) M7: отказоустойчивость. Клиент узнаёт pid log-сервера через
    //    WHEREIS, убивает его SIGKILL'ом (одинаковый uid — разрешено),
    //    супервизор обязан перезапустить сервер; клиент ретраями LOOKUP
    //    дожидается нового экземпляра и продолжает работу на новом lane'е
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
    sys3(SYS_close, bus, 0, 0);

    // stat через FUSE: пока fused не смонтировал /mnt, системный вызов
    // честно блокируется в ядре — супервизору не нужен сон (регрессия M4)
    for (int tries = 0; ; tries++) {
        if (sys3(SYS_stat, (long)"/mnt/README", (long)&st_buf, 0) == 0)
            break;
        if (tries > 100) {
            P("client: stat /mnt/README: FAIL (сервер не ответил)\n");
            return 1;
        }
        sleep_ms(20);
    }
    P("client: /mnt/README: size=");
    print_dec((long)st_buf.size);
    P("client: /mnt/README: mode=0");
    print_oct(st_buf.mode);
    P("client: IPC-демо + capability + отказ-инъекция + FUSE: OK\n");
    return 0;
}

static int spawn(const char *path, const char *name) {
    int pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        char *argv[] = { (char *)name, 0 };
        if (sys3(SYS_execve, (long)path, (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }
    return pid;
}

// --- M7: таблица сервисов + reaper ---
// Супервизор больше не ждёт конкретные pid в фиксированном порядке:
// wait4(-1) на ЛЮБОГО ребёнка; нежданная смерть сервиса (сигнал или
// любой код выхода, пока не объявлена остановка) — это сбой, и сервис
// перезапускается с нарастающей паузой. Так выглядит отказоустойчивость
// микроядра: падение сервера не рушит систему, а обрабатывается
// супервизором (в монолитном ядре то же самое делает panic()).
struct svc {
    const char *path;
    const char *name;
    int pid;
    int restarts;
    int stopping;   // остановка запланирована супервизором (graceful)
    int stopped;
};

#define NSVC 4
#define MAX_RESTARTS 3

static struct svc svcs[NSVC] = {
    { "/sbin/mlbus",  "mlbus",  0, 0, 0, 0 },
    { "/sbin/logsrv", "logsrv", 0, 0, 0, 0 },
    { "/sbin/uiod",   "uiod",   0, 0, 0, 0 },
    { "/sbin/fused",  "fused",  0, 0, 0, 0 },
};

static struct svc *find_svc(int pid) {
    for (int i = 0; i < NSVC; i++)
        if (svcs[i].pid == pid && !svcs[i].stopped)
            return &svcs[i];
    return 0;
}

static int spawn_svc(struct svc *s) {
    s->pid = spawn(s->path, s->name);
    return s->pid;
}

static int svc_alive_count(void) {
    int n = 0;
    for (int i = 0; i < NSVC; i++)
        if (!svcs[i].stopped) n++;
    return n;
}

// классификация статуса wait4: 1 = умер от сигнала
static int died_by_signal(int status) { return (status & 0x7f) != 0; }
static int exit_code(int status) { return (status >> 8) & 0xff; }
static int signal_of(int status) { return status & 0x7f; }

void milin_main(void) {
    P("MiLin supervisor: порождаю серверы (M7: reaper + автоперезапуск)\n");

    // devtmpfs: /dev/fuse и /dev/uio0 появляются сами (UIO берёт
    // динамический major, узнаваемый только через devtmpfs)
    if (sys5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0) == 0) {
        P("supervisor: devtmpfs смонтирован на /dev\n");
    } else {
        // запасной путь: /dev/fuse — misc 10:229 (фиксированный minor)
        sys3(SYS_mknod, (long)"/dev/fuse", 0600 | 0020000, (10 << 8) | 229);
        P("supervisor: devtmpfs нет, /dev/fuse создан mknod\n");
    }

    // Сборка системы супервизором: uio_pci_generic сознательно не имеет
    // таблицы device-id — супервизор сам решает, какое железо отдаётся
    // userspace-драйверу (1b36:0005 = qemu pci-testdev).
    // sysfs монтируем как discovery-интерфейс серверов.
    sys3(SYS_mkdir, (long)"/sys", 0755, 0);
    long sysfs_ok = sys5(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
    if (sysfs_ok == 0) P("supervisor: sysfs смонтирован\n");

    // /tmp под shm-регионы очередей (ramfs: имя нужно только на момент
    // раздачи fd, после unlink объект живёт в памяти)
    sys3(SYS_mkdir, (long)"/tmp", 0755, 0);

    long bfd = sys3(SYS_open, (long)"/sys/bus/pci/drivers/uio_pci_generic/new_id", 1 /*O_WRONLY*/, 0);
    if (bfd >= 0) {
        sys3(SYS_write, bfd, (long)"1b36 0005\n", 10);
        sys3(SYS_close, bfd, 0, 0);
        P("supervisor: pci-testdev отдан драйверу uio_pci_generic\n");
    } else {
        P("supervisor: sysfs недоступен, UIO-привязка невозможна\n");
    }

    // --- сервисы: шина, log-сервер, драйвер, фс ---
    for (int i = 0; i < NSVC; i++)
        spawn_svc(&svcs[i]);
    P("supervisor: сервисы запущены: mlbus, logsrv, uiod, fused\n");

    // --- клиент: другой uid, IPC-демо + capability + отказ-инъекция ---
    int pid_c = sys3(SYS_fork, 0, 0, 0);
    if (pid_c == 0)
        sys3(SYS_exit, (long)run_client(), 0, 0);

    // --- reaper: wait4(-1) на любого ребёнка ---
    int client_status = -1;
    for (;;) {
        int status = 0;
        // r10/r8 обязаны быть чистыми: мусор ядро читает как rusage (M4)
        long r = sys5(SYS_wait4, -1, (long)&status, 0, 0, 0);
        if (r < 0) break; // детей больше нет
        if (r == pid_c) {
            // клиент закончил работу — система отработала свой демо-цикл
            client_status = status;
            break;
        }
        struct svc *s = find_svc((int)r);
        if (!s) continue;

        if (s->stopping) {
            // запланированная остановка — норма graceful shutdown
            s->stopped = 1;
            continue;
        }

        // нежданная смерть сервиса: перезапуск с нарастающей паузой
        if (s->restarts >= MAX_RESTARTS) {
            P("supervisor: ");
            print_str(s->name);
            P(": лимит перезапусков исчерпан — система нездорова\n");
            sys3(SYS_exit, 1, 0, 0);
        }
        P("supervisor: ");
        print_str(s->name);
        P(" (pid ");
        print_dec_n(r);
        P(") умер (");
        if (died_by_signal(status)) {
            P("сигнал ");
            print_dec_n(signal_of(status));
        } else {
            P("код ");
            print_dec_n(exit_code(status));
        }
        P(") — перезапуск #");
        print_dec_n(s->restarts + 1);
        P("\n");
        sleep_ms(40 * (s->restarts + 1)); // backoff
        s->restarts++;
        spawn_svc(s);
        P("supervisor: ");
        print_str(s->name);
        P(" перезапущен (pid ");
        print_dec_n(s->pid);
        P(")\n");
    }

    // --- graceful shutdown: остановить оставшиеся сервисы ---
    for (int i = 0; i < NSVC; i++)
        if (!svcs[i].stopped) svcs[i].stopping = 1;

    // FUSE: размонтирование закрывает /dev/fuse — fused видит EOF и
    // выходит сам; остальные (в poll/recv/сне) — SIGTERM
    sys3(SYS_umount2, (long)"/mnt", MNT_DETACH, 0);
    for (int i = 0; i < NSVC; i++) {
        if (svcs[i].stopped || svcs[i].pid <= 0) continue;
        sys3(SYS_kill, svcs[i].pid, SIGTERM, 0);
    }

    // допросить всех: в фазе остановки любой статус — норма
    while (svc_alive_count() > 0) {
        int status = 0;
        long r = sys5(SYS_wait4, -1, (long)&status, 0, 0, 0);
        if (r < 0) break;
        struct svc *s = find_svc((int)r);
        if (s) s->stopped = 1;
    }
    P("supervisor: все сервисы остановлены\n");

    int client_ok = (client_status != -1) && !died_by_signal(client_status)
                    && exit_code(client_status) == 0;
    P("supervisor: клиент: ");
    if (client_ok) P("OK\n"); else P("FAIL\n");

    int restarts_total = 0;
    for (int i = 0; i < NSVC; i++) restarts_total += svcs[i].restarts;
    P("supervisor: перезапусков сервисов за сессию: ");
    print_dec(restarts_total);

    if (client_ok) {
        P("M5 OK: mbus-реестр + lane-хэндлы + bragi-формат + FUSE-регрессия\n");
        P("M5.1 OK: helqueue async rings + futex + creds + bench\n");
        P("M7 OK: capability lanes + supervisor auto-restart\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    P("M7 FAILED\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
