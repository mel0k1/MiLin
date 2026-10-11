// /init MiLin (M5.2): супервизор на сервисных манифестах.
//
// M4: порождает серверы (execve), отдаёт им железо (UIO) и ФС (FUSE),
//     непривилегированный клиент читает файл с FUSE-сервера.
// M5: серверы не сироты — у них есть шина (mlbus, идея mbus из Managarm).
// M5.1: второй транспорт — асинхронные очереди HelQueue в разделяемой памяти.
// M7: capability-модель на lane'ах и отказоустойчивость: wait4(-1) на
//     любого ребёнка, павший сервис перезапускается с нарастающей паузой.
// M8: blob-путь для больших сообщений; glibc-клиент в userspace.
// M9: kdriver-домен — драйвер «между ядром и userspace».
// M5.2: таблица сервисов больше не зашита в код: /etc/milin/*.svc —
//     манифесты (name/exec/type/stop/order). Супервизор читает их
//     getdents64'ом, сортирует по order и порождает: служба = файл,
//     супервизор не пересобирается. Демо-клиент уехал из супервизора
//     в отдельный бинарник /sbin/client (oneshot в манифестах) — все
//     процессы равны перед манифестами.
//
//   супервизор  /init        — интерпретатор манифестов + reaper
//   реестр      /sbin/mlbus  — именованные lane'и (mbus) + права (M7)
//   log-сервер  /sbin/logsrv — сервис, принимающий сообщения
//   драйвер     /sbin/uiod   — UIO: userspace-драйвер PCI-устройства
//   фс-сервер   /sbin/fused  — FUSE: файловая система в userspace
//   клиент      /sbin/client — IPC-демо + capability + отказ-инъекция

#include "milin.h"
#include "ipc.h"

// --- манифест сервиса (/etc/milin/*.svc) ---
// name=имя в логах, exec=путь, type=service|oneshot,
// stop=term|umount (как супервизор его глушит), order=порядок старта
#define SVC_SERVICE 0
#define SVC_ONESHOT 1
#define SVC_STOP_TERM   0
#define SVC_STOP_UMOUNT 1
#define SVC_MAX 8

struct svc {
    char name[16];
    char exec[32];
    u8 type, stop;
    int order;
    int pid;
    int restarts;
    int stopping;   // остановка запланирована супервизором (graceful)
    int stopped;
};

#define MAX_RESTARTS 3

static struct svc svcs[SVC_MAX];
static int nsvcs;

static int died_by_signal(int status) { return (status & 0x7f) != 0; }
static int exit_code(int status) { return (status >> 8) & 0xff; }
static int signal_of(int status) { return status & 0x7f; }

static int parse_dec(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
    return v;
}

static void copy_field(char *dst, const char *val, int max) {
    int i = 0;
    while (val[i] && i < max - 1) { dst[i] = val[i]; i++; }
    dst[i] = 0;
}

// одну строку манифеста: key=value
static void parse_line(struct svc *s, const char *key, const char *val) {
    if (m_strncmp(key, "name", 5) == 0)
        copy_field(s->name, val, sizeof(s->name));
    else if (m_strncmp(key, "exec", 5) == 0)
        copy_field(s->exec, val, sizeof(s->exec));
    else if (m_strncmp(key, "type", 5) == 0)
        s->type = m_strncmp(val, "oneshot", 8) == 0 ? SVC_ONESHOT : SVC_SERVICE;
    else if (m_strncmp(key, "stop", 5) == 0)
        s->stop = m_strncmp(val, "umount", 7) == 0 ? SVC_STOP_UMOUNT : SVC_STOP_TERM;
    else if (m_strncmp(key, "order", 6) == 0)
        s->order = parse_dec(val);
}

static void load_svc(const char *fn) {
    if (nsvcs >= SVC_MAX) return;
    unsigned nl = m_strlen(fn);
    if (nl < 5 || m_strncmp(fn + nl - 4, ".svc", 5) != 0) return;

    static char path[64];
    char *p = path;
    const char *dir = "/etc/milin/";
    while (*dir) *p++ = *dir++;
    while (*fn) *p++ = *fn++;
    *p = 0;

    static char buf[512];
    long fd = sys3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        P("supervisor: манифест ");
        print_str(path);
        P(": FAIL\n");
        return;
    }
    long n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return;
    buf[n] = 0;

    struct svc s;
    m_memset(&s, 0, sizeof(s));
    s.type = SVC_SERVICE;
    s.stop = SVC_STOP_TERM;
    s.order = 100; // без order — в конец
    char *ln = buf;
    while (*ln) {
        char *end = ln;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        if (end > ln && *(end - 1) == '\r') *(end - 1) = 0;
        if (*ln && *ln != '#') {
            char *eq = 0;
            for (char *c = ln; *c; c++)
                if (*c == '=') { eq = c; break; }
            if (eq) {
                *eq = 0;
                parse_line(&s, ln, eq + 1);
            }
        }
        ln = next;
    }
    if (!s.name[0] || !s.exec[0]) {
        P("supervisor: манифест без name/exec: ");
        print_str(fn);
        P("\n");
        return;
    }
    svcs[nsvcs++] = s;
}

static u8 dbuf[1024];

static void load_manifests(void) {
    long fd = sys3(SYS_open, (long)"/etc/milin", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) {
        P("supervisor: /etc/milin: FAIL (манифестов нет)\n");
        return;
    }
    for (;;) {
        long r = sys3(SYS_getdents64, fd, (long)dbuf, sizeof(dbuf));
        if (r <= 0) break;
        for (long off = 0; off < r; ) {
            struct ldirent64 *d = (struct ldirent64 *)(dbuf + off);
            load_svc(d->d_name);
            off += d->d_reclen;
        }
    }
    sys3(SYS_close, fd, 0, 0);
}

// порядок старта задаёт order, не readdir
static void sort_svcs(void) {
    for (int i = 1; i < nsvcs; i++) {
        struct svc s = svcs[i];
        int j = i - 1;
        while (j >= 0 && svcs[j].order > s.order) { svcs[j + 1] = svcs[j]; j--; }
        svcs[j + 1] = s;
    }
}

static struct svc *find_svc(int pid) {
    for (int i = 0; i < nsvcs; i++)
        if (svcs[i].pid == pid && !svcs[i].stopped)
            return &svcs[i];
    return 0;
}

static struct svc *find_oneshot(void) {
    for (int i = 0; i < nsvcs; i++)
        if (svcs[i].type == SVC_ONESHOT)
            return &svcs[i];
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

static int spawn_svc(struct svc *s) {
    s->pid = spawn(s->exec, s->name);
    return s->pid;
}

static int svc_alive_count(void) {
    int n = 0;
    for (int i = 0; i < nsvcs; i++)
        if (!svcs[i].stopped) n++;
    return n;
}

// --- M9: kdriver-домен: драйвер «между ядром и userspace» ---
// /dev/kdemo: write = запрос, read = ответ; обрабатывает не syscall-контекст,
// а kthread домена (ring 0, свой pid). Размещение — политика: 'user' через
// sysfs отдаёт драйвер userspace-серверу (UIO-путь), домен отклоняет запросы
// (-ENODEV); 'kernel' возвращает драйвер в домен.
static char kd_rbuf[256];

static int kd_demo(void) {
    long fd = sys3(SYS_open, (long)"/dev/kdemo", O_RDWR, 0);
    if (fd < 0) {
        P("kdriver: /dev/kdemo: FAIL (домен не зарегистрировал драйвер)\n");
        return 1;
    }

    // эхо: запрос уезжает в домен и обрабатывается его kthread'ом
    const char *req = "Ekdriver: echo через домен ядра";
    long n = m_strlen(req);
    if (sys3(SYS_write, fd, (long)req, n) != n) {
        P("kdriver: write (echo): FAIL\n");
        return 1;
    }
    long r = sys3(SYS_read, fd, (long)kd_rbuf, sizeof(kd_rbuf));
    if (r != n - 1 || m_strncmp(kd_rbuf, req + 1, n - 1) != 0) {
        P("kdriver: echo FAIL\n");
        return 1;
    }
    P("kdriver: /dev/kdemo открыт, echo через домен ядра: OK\n");

    // счётчик: 'C' -> u32 LE; состояние живёт в kthread'е домена
    for (u32 i = 1; i <= 2; i++) {
        if (sys3(SYS_write, fd, (long)"C", 1) != 1) {
            P("kdriver: write (counter): FAIL\n");
            return 1;
        }
        r = sys3(SYS_read, fd, (long)kd_rbuf, 4);
        if (r != 4) {
            P("kdriver: read (counter): FAIL\n");
            return 1;
        }
        u32 v = (u8)kd_rbuf[0] | ((u8)kd_rbuf[1] << 8) |
                ((u8)kd_rbuf[2] << 16) | ((u8)kd_rbuf[3] << 24);
        if (v != i) {
            P("kdriver: счётчик разошёлся\n");
            return 1;
        }
    }
    P("kdriver: счётчик домена 1 -> 2 (state живёт в kthread'е): OK\n");

    // инфо: pid kthread'а виден userspace — обработка не в syscall-контексте
    if (sys3(SYS_write, fd, (long)"I", 1) != 1 ||
        sys3(SYS_read, fd, (long)kd_rbuf, sizeof(kd_rbuf) - 1) <= 0) {
        P("kdriver: info FAIL\n");
        return 1;
    }
    P("kdriver: ");
    print_str(kd_rbuf);
    P("\n");

    // размещение на лету: 'user' — драйвер отдан userspace-серверу
    long pf = sys3(SYS_open, (long)"/sys/class/mlkd/kdemo/placement", O_WRONLY, 0);
    if (pf < 0 || sys3(SYS_write, pf, (long)"user\n", 5) != 5) {
        P("kdriver: placement=user: FAIL\n");
        return 1;
    }
    sys3(SYS_close, pf, 0, 0);
    if (sys3(SYS_write, fd, (long)"Etest", 5) != -19 /*ENODEV*/) {
        P("kdriver: placement=user: запрос НЕ отклонён — дыра в политике!\n");
        return 1;
    }
    P("kdriver: placement=user: домен отклоняет запросы (-ENODEV): OK\n");

    // 'kernel' — драйвер снова в домене
    pf = sys3(SYS_open, (long)"/sys/class/mlkd/kdemo/placement", O_WRONLY, 0);
    if (pf < 0 || sys3(SYS_write, pf, (long)"kernel\n", 7) != 7) {
        P("kdriver: placement=kernel: FAIL\n");
        return 1;
    }
    sys3(SYS_close, pf, 0, 0);
    if (sys3(SYS_write, fd, (long)"Eok", 3) != 3) {
        P("kdriver: write после возврата в домен: FAIL\n");
        return 1;
    }
    r = sys3(SYS_read, fd, (long)kd_rbuf, 2);
    if (r != 2 || kd_rbuf[0] != 'o' || kd_rbuf[1] != 'k') {
        P("kdriver: echo после возврата в домен: FAIL\n");
        return 1;
    }
    sys3(SYS_close, fd, 0, 0);
    P("kdriver: placement=kernel: драйвер вернулся в домен: OK\n");
    return 0;
}

void milin_main(void) {
    P("MiLin supervisor: сервисные манифесты из /etc/milin (M7: reaper)\n");

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
    // раздачи fd, после unlink объект живёт в памяти). chown: шина после
    // setuid(1000) (M7.2) создаёт там регионы, а mkdir режется umask'ом
    sys3(SYS_mkdir, (long)"/tmp", 0777, 0);
    sys3(SYS_chown, (long)"/tmp", 1000, 1000);

    long bfd = sys3(SYS_open, (long)"/sys/bus/pci/drivers/uio_pci_generic/new_id", O_WRONLY, 0);
    if (bfd >= 0) {
        sys3(SYS_write, bfd, (long)"1b36 0005\n", 10);
        sys3(SYS_close, bfd, 0, 0);
        P("supervisor: pci-testdev отдан драйверу uio_pci_generic\n");
    } else {
        P("supervisor: sysfs недоступен, UIO-привязка невозможна\n");
    }

    // --- сервисы: из манифестов, в порядке order ---
    load_manifests();
    sort_svcs();
    P("supervisor: манифестов: ");
    print_dec_n(nsvcs);
    P("\n");
    for (int i = 0; i < nsvcs; i++) {
        struct svc *s = &svcs[i];
        spawn_svc(s);
        P("supervisor: старт ");
        print_str(s->name);
        P(" (");
        print_str(s->exec);
        P("): ");
        print_str(s->type == SVC_ONESHOT ? "oneshot" : "service");
        P("\n");
    }
    struct svc *client = find_oneshot();

    // --- M9: kdriver-домен (root: sysfs placement требует привилегий) ---
    if (kd_demo()) {
        P("M9 FAILED\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    // --- reaper: wait4(-1) на любого ребёнка ---
    int client_status = -1;
    for (;;) {
        int status = 0;
        // r10/r8 обязаны быть чистыми: мусор ядро читает как rusage (M4)
        long r = sys5(SYS_wait4, -1, (long)&status, 0, 0, 0);
        if (r < 0) break; // детей больше нет
        if (client && r == client->pid) {
            // oneshot закончил работу — система отработала свой демо-цикл
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
    for (int i = 0; i < nsvcs; i++)
        if (!svcs[i].stopped) svcs[i].stopping = 1;

    // остановка по манифесту: stop=umount — размонтирование закрывает
    // /dev/fuse, сервер видит EOF и выходит сам; stop=term — SIGTERM
    // тем, кто спит в poll/recv
    int umounted = 0;
    for (int i = 0; i < nsvcs; i++) {
        struct svc *s = &svcs[i];
        if (s->stopped || s->pid <= 0) continue;
        if (s->stop == SVC_STOP_UMOUNT && !umounted) {
            sys3(SYS_umount2, (long)"/mnt", MNT_DETACH, 0);
            umounted = 1;
        } else {
            sys3(SYS_kill, s->pid, SIGTERM, 0);
        }
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
    for (int i = 0; i < nsvcs; i++) restarts_total += svcs[i].restarts;
    P("supervisor: перезапусков сервисов за сессию: ");
    print_dec(restarts_total);

    if (client_ok) {
        P("M5.2 OK: сервисные манифесты + FUSE_WRITE/READ + readdir\n");
        P("M5 OK: mbus-реестр + lane-хэндлы + bragi-формат + FUSE-регрессия\n");
        P("M5.1 OK: helqueue async rings + futex + creds + bench\n");
        P("M7 OK: capability lanes + supervisor auto-restart\n");
        P("M7.1 OK: revoke capability on client death\n");
        P("M7.2 OK: bus-death cascade restart\n");
        P("M8 OK: blob path for large messages + glibc userspace\n");
        P("M9 OK: kdriver domain — drivers between kernel and userspace\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    P("M9 FAILED\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
