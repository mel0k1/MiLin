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
// M11: mlkd-фасад amdgpu — протокол mlgpu, placement user/kernel.
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
#include "mlgpu.h"

// --- манифест сервиса (/etc/milin/*.svc) ---
// name=имя в логах, exec=путь, type=service|oneshot,
// stop=term|umount (как супервизор его глушит), order=порядок старта
#define SVC_SERVICE 0
#define SVC_ONESHOT 1
#define SVC_STOP_TERM   0
#define SVC_STOP_UMOUNT 1
#define SVC_MAX 16

struct svc {
    char name[16];
    char exec[32];
    u8 type, stop;
    int order;
    int pid;
    int restarts;
    int stopping;   // остановка запланирована супервизором (graceful)
    int stopped;
    int done;       // oneshot завершился
    int status;     // статус выхода oneshot (wait4) — вердикты M11/M22
};

#define MAX_RESTARTS 3

static struct svc svcs[SVC_MAX];
static int nsvcs;

// M19: установленная система (нет /init) не завершается после oneshot'ов;
// install=<диск> в cmdline запускает инсталлятор oneshot'ом
static int g_installed;
static char install_dev[64];

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

// сессия закончена, когда завершились ВСЕ oneshot'ы (не только первый):
// демо-клиенты M5.2/M12 идут отдельными oneshot'ами в общем порядке
static int all_oneshots_done(void) {
    for (int i = 0; i < nsvcs; i++)
        if (svcs[i].type == SVC_ONESHOT && !svcs[i].done)
            return 0;
    return 1;
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

static int spawn_arg(const char *path, const char *name, const char *arg) {
    int pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        char *argv[] = { (char *)name, (char *)arg, 0 };
        if (sys3(SYS_execve, (long)path, (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }
    return pid;
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

// --- M15: switch_root на постоянный ext4 (cmdline root=) ---
// /init есть только в initramfs — его отсутствие = мы уже на реальном
// корне (guard от повторного переезда у нового /sbin/init).
// Путь: root= из /proc/cmdline -> mount ext4 -> пустой? сеем копию мира
// (installer-примитив) -> move /dev /sys /proc -> mount --move / -> chroot
// -> exec /sbin/init. Диск/корень не годятся — остаёмся в initramfs.

struct mstat {
    u64 dev, ino, nlink;
    u32 mode, uid, gid, pad;
    u64 rdev, size, blk, blocks;
    u64 atime, mtime, ctime;
    u64 x[6];   // до 144: ядро пишет полный struct stat, иначе стек рвётся
};

static int m_stat(const char *p, struct mstat *st) {
    m_memset(st, 0, sizeof(*st));
    long r = sys3(SYS_stat, (long)p, (long)st, 0);
    return r == 0;
}

// lstat: не следовать симлинкам — copytree сохраняет их как есть
static int m_lstat(const char *p, struct mstat *st) {
    m_memset(st, 0, sizeof(*st));
    long r = sys3(SYS_lstat, (long)p, (long)st, 0);
    return r == 0;
}

static void copy_file(const char *src, const char *dst) {
    long in = sys3(SYS_open, (long)src, O_RDONLY, 0);
    if (in < 0) { P("seed: open "); print_str(src); P("\n"); return; }
    struct mstat st;
    u32 mode = 0644;
    if (m_stat(src, &st) && (st.mode & 0111)) mode = 0755;
    long out = sys3(SYS_open, (long)dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) { P("seed: create "); print_str(dst); P("\n"); sys3(SYS_close, in, 0, 0); return; }
    static char cb[65536];
    for (;;) {
        long n = sys3(SYS_read, in, (long)cb, sizeof(cb));
        if (n <= 0) break;
        long w = sys3(SYS_write, out, (long)cb, n);
        if (w != n) { P("seed: short write "); print_str(dst); P("\n"); break; }
    }
    sys3(SYS_close, in, 0, 0);
    sys3(SYS_close, out, 0, 0);
}

static void copytree(const char *src, const char *dst, int depth);

static void copy_entry(const char *sdir, const char *ddir,
                       const char *name, int depth) {
    char sp[128], dp[128];
    int a = 0, b = 0;
    while (sdir[a]) { sp[a] = sdir[a]; a++; }
    sp[a++] = '/'; sp[a] = 0;
    while (name[b]) { sp[a + b] = name[b]; b++; }
    sp[a + b] = 0;
    a = 0;
    while (ddir[a]) { dp[a] = ddir[a]; a++; }
    dp[a++] = '/';
    b = 0;
    while (name[b]) { dp[a + b] = name[b]; b++; }
    dp[a + b] = 0;

    struct mstat st;
    if (!m_lstat(sp, &st)) return;
    if ((st.mode & 0xF000) == 0x4000) {                    // S_IFDIR
        sys3(SYS_mkdir, (long)dp, 0755, 0);
        copytree(sp, dp, depth + 1);
    } else if ((st.mode & 0xF000) == 0xA000) {             // S_IFLNK
        char tgt[128];
        long n = sys3(SYS_readlink, (long)sp, (long)tgt, sizeof(tgt) - 1);
        if (n > 0) {
            tgt[n] = 0;
            sys3(SYS_symlink, (long)tgt, (long)dp, 0);
        }
    } else if ((st.mode & 0xF000) == 0x8000) {             // S_IFREG
        copy_file(sp, dp);
    }
}

static void copytree(const char *src, const char *dst, int depth) {
    if (depth > 8) return;
    long fd = sys3(SYS_open, (long)src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return;
    // СТРОГО локальный буфер: рекурсия из copy_entry заменила бы статический
    char dbuf[16384];
    for (;;) {
        long n = sys3(SYS_getdents64, fd, (long)dbuf, sizeof(dbuf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            struct ldirent64 *e = (struct ldirent64 *)(dbuf + off);
            const char *nm = e->d_name;
            if (!(nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2]))))
                copy_entry(src, dst, nm, depth);
            off += e->d_reclen;
        }
    }
    sys3(SYS_close, fd, 0, 0);
}

// верхний уровень: белые списки — внутри всё, снаружи ничего
static const char *seed_dirs[] = { "bin", "etc", "lib", "lib64", "root",
                                   "sbin", "tmp", "usr", "var", 0 };

static void seed_root(void) {
    P("supervisor: seed постоянного корня (копия мира ~17МБ)...\n");
    for (int i = 0; seed_dirs[i]; i++)
        copy_entry("", "/newroot", seed_dirs[i], 0);
    sys3(SYS_mkdir, (long)"/newroot/mnt", 0755, 0);
    sys3(SYS_mkdir, (long)"/newroot/proc", 0755, 0);
    sys3(SYS_mkdir, (long)"/newroot/sys", 0755, 0);
    sys3(SYS_mkdir, (long)"/newroot/dev", 0755, 0);
    // /init -> /sbin/init: реальный корень живёт без initramfs-/init
    copy_file("/init", "/newroot/sbin/init");
    P("supervisor: SEED OK\n");
}

// --- M22: PARTUUID -> устройство ---
// mount(2) имена PARTUUID не понимает: mini-initramfs монтирует корень
// сам. Сканируем /dev, у каждого дискового узла читаем GPT (LBA1:
// заголовок + записи) и ищем партицию с этим уникальным GUID — тот же
// разбор, что и в ядре (init/do_mounts.c), но в userspace.

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// "8-4-4-4-12" -> 16 дисковых байт (первые три поля little-endian)
static int guid_parse(const char *s, u8 out[16]) {
    int f = 0;
    while (*s && f < 16) {
        if (*s == '-') { s++; continue; }
        int h = hexval(s[0]), l = hexval(s[1]);
        if (h < 0 || l < 0) return 0;
        u8 b = (u8)(h * 16 + l);
        if (f < 4) out[3 - f] = b;      // data1: LE
        else if (f < 6) out[9 - f] = b; // data2: LE
        else if (f < 8) out[13 - f] = b; // data3: LE
        else out[f] = b;
        s += 2;
        f++;
    }
    return f == 16 && *s == 0;
}

// дисковый узел (не партиция): sd/hd/vd? | nvme?n? | mmcblk?
static int disk_node_name(const char *s) {
    int n = 0;
    while (s[n]) n++;
    if (n == 3 && s[1] == 'd' && (s[0] == 's' || s[0] == 'h' || s[0] == 'v')
        && s[2] >= 'a' && s[2] <= 'z') return 1;
    int has_p = 0, has_n = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == 'p') has_p = 1;
        if (s[i] == 'n' && i > 4) has_n = 1;
    }
    if (has_p) return 0;
    if (n >= 7 && m_strncmp(s, "mmcblk", 6) == 0) return 1;
    if (n >= 6 && m_strncmp(s, "nvme", 4) == 0 && has_n) return 1;
    return 0;
}

// номер партиции диска с GUID want (0 = не нашлась)
static long gpt_find_part(const char *devname, const u8 want[16]) {
    static u8 lba1[512], ent[16384];
    char path[64];
    int a = 0;
    for (; "/dev/"[a]; a++) path[a] = "/dev/"[a];
    for (int b = 0; devname[b] && a < 60; b++) path[a++] = devname[b];
    path[a] = 0;

    long fd = sys3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) return 0;
    sys3(SYS_lseek, fd, 512, SEEK_SET);
    long n = sys3(SYS_read, fd, (long)lba1, 512);
    if (n != 512 || m_strncmp((const char *)lba1, "EFI PART", 8) != 0) {
        sys3(SYS_close, fd, 0, 0);
        return 0;
    }
    u64 ent_lba = 0;
    u32 nent = 0, esz = 0;
    m_memcpy(&ent_lba, lba1 + 72, 8);
    m_memcpy(&nent, lba1 + 80, 4);
    m_memcpy(&esz, lba1 + 84, 4);
    if (!nent || !esz || esz > 512 || nent > 128) {
        sys3(SYS_close, fd, 0, 0);
        return 0;
    }
    sys3(SYS_lseek, fd, (long)(ent_lba * 512), SEEK_SET);
    n = sys3(SYS_read, fd, (long)ent, (long)(nent * esz));
    sys3(SYS_close, fd, 0, 0);
    if (n < (long)(nent * esz)) return 0;
    for (u32 i = 0; i < nent; i++) {
        const u8 *e = ent + i * esz;
        int zero = 1;
        for (int k = 0; k < 16; k++)
            if (e[k]) { zero = 0; break; }
        if (zero) continue;
        int eq = 1;
        for (int k = 0; k < 16; k++)
            if (e[16 + k] != want[k]) { eq = 0; break; }
        if (eq) return (long)i + 1;
    }
    return 0;
}

// PARTUUID -> имя узла партиции (/dev/sda2, /dev/nvme0n1p2, ...)
static int partuuid_resolve(const char *hex, char *out, int omax) {
    u8 want[16];
    if (!guid_parse(hex, want)) return 0;

    static char buf[16384];
    long fd = sys3(SYS_open, (long)"/dev", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return 0;
    int found = 0;
    for (;;) {
        long n = sys3(SYS_getdents64, fd, (long)buf, sizeof(buf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            struct ldirent64 *e = (struct ldirent64 *)(buf + off);
            off += e->d_reclen;
            if (!disk_node_name(e->d_name)) continue;
            long pn = gpt_find_part(e->d_name, want);
            if (pn <= 0) continue;
            // полный путь: mount(2) хочет "/dev/<узел>"
            int a = 0;
            for (const char *d = "/dev/"; *d; d++) out[a++] = *d;
            int b = 0;
            while (e->d_name[b] && a < omax - 4) out[a++] = e->d_name[b++];
            if (a && out[a - 1] >= '0' && out[a - 1] <= '9' && a < omax - 3)
                out[a++] = 'p';
            char num[8];
            int k = 0;
            if (!pn) num[k++] = '0';
            while (pn) { num[k++] = '0' + pn % 10; pn /= 10; }
            while (k && a < omax - 1) out[a++] = num[--k];
            out[a] = 0;
            found = 1;
            break;
        }
        if (found) break;
    }
    sys3(SYS_close, fd, 0, 0);
    return found;
}

static void cmdline_root(char *out, int max) {
    // /proc/cmdline: "root=/dev/vda1 console=..."; последний root=
    // выигрывает — та же семантика, что и у ядра (bootloader-приложение
    // перекрывает встроенный cmdline)
    sys3(SYS_mkdir, (long)"/proc", 0555, 0);
    if (sys5(SYS_mount, (long)"proc", (long)"/proc", (long)"proc", 0, 0) != 0)
        return;
    long fd = sys3(SYS_open, (long)"/proc/cmdline", O_RDONLY, 0);
    if (fd < 0) return;
    char b[512];
    long n = sys3(SYS_read, fd, (long)b, sizeof(b) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return;
    b[n] = 0;
    for (long i = 0; i < n; i++) {
        if (b[i] != 'r') continue;
        if (m_strncmp(b + i, "root=", 5) != 0) continue;
        long j = i + 5;
        int k = 0;
        while (j < n && b[j] != ' ' && b[j] != '\n' && k < max - 1)
            out[k++] = b[j++];
        out[k] = 0;
    }
}

static void cmdline_install(char *out, int max) {
    long fd = sys3(SYS_open, (long)"/proc/cmdline", O_RDONLY, 0);
    if (fd < 0) return;
    char b[512];
    long n = sys3(SYS_read, fd, (long)b, sizeof(b) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return;
    b[n] = 0;
    for (long i = 0; i < n; i++) {
        if (b[i] != 'i') continue;
        if (m_strncmp(b + i, "install=", 8) != 0) continue;
        long j = i + 8;
        int k = 0;
        while (j < n && b[j] != ' ' && b[j] != '\n' && k < max - 1)
            out[k++] = b[j++];
        out[k] = 0;
        return;
    }
}

static void do_switch_root(void) {
    sys3(SYS_mkdir, (long)"/newroot/dev", 0755, 0);
    sys3(SYS_mkdir, (long)"/newroot/sys", 0755, 0);
    sys3(SYS_mkdir, (long)"/newroot/proc", 0755, 0);
    sys5(SYS_mount, (long)"/dev", (long)"/newroot/dev", 0, MS_MOVE, 0);
    sys5(SYS_mount, (long)"/sys", (long)"/newroot/sys", 0, MS_MOVE, 0);
    sys5(SYS_mount, (long)"/proc", (long)"/newroot/proc", 0, MS_MOVE, 0);
    sys3(SYS_chdir, (long)"/newroot", 0, 0);
    sys5(SYS_mount, (long)".", (long)"/", 0, MS_MOVE, 0);
    sys3(SYS_chroot, (long)".", 0, 0);
    P("supervisor: switch_root OK — exec /sbin/init\n");
    char *av[2] = { (char *)"/sbin/init", 0 };
    sys3(SYS_execve, (long)"/sbin/init", (long)av, 0);
    P("supervisor: exec /sbin/init FAIL\n");
    sys3(SYS_exit, 1, 0, 0);
}

static void try_switch_root(void) {
    long f = sys3(SYS_open, (long)"/init", O_RDONLY, 0);
    if (f < 0) return;                       // уже реальный корень
    sys3(SYS_close, f, 0, 0);

    char rootdev[64];
    cmdline_root(rootdev, sizeof(rootdev));
    if (!rootdev[0]) return;

    // M22: root=PARTUUID=... -> узел в /dev (уникальный GUID установки)
    if (m_strncmp(rootdev, "PARTUUID=", 9) == 0) {
        char node[64];
        if (!partuuid_resolve(rootdev + 9, node, sizeof(node))) {
            P("supervisor: PARTUUID ");
            print_str(rootdev + 9);
            P(" не нашлась — остаюсь в initramfs\n");
            return;
        }
        P("supervisor: PARTUUID ");
        print_str(rootdev + 9);
        P(" -> ");
        print_str(node);
        P("\n");
        int i = 0;
        while (node[i] && i < (int)sizeof(rootdev) - 1) {
            rootdev[i] = node[i];
            i++;
        }
        rootdev[i] = 0;
    }

    sys3(SYS_mkdir, (long)"/newroot", 0755, 0);
    long rc = sys5(SYS_mount, (long)rootdev, (long)"/newroot",
                   (long)"ext4", 0, 0);
    if (rc != 0) {
        P("supervisor: root="); print_str(rootdev);
        P(" не смонтировался — остаюсь в initramfs\n");
        return;
    }
    P("supervisor: root="); print_str(rootdev); P(" смонтирован (ext4)\n");

    struct mstat st;
    if (!m_stat("/newroot/sbin/init", &st)) {
        // пустой/нераспакованный корень: сеем копию мира (installer)
        seed_root();
        if (!m_stat("/newroot/sbin/init", &st)) {
            P("supervisor: seed не удался — остаюсь в initramfs\n");
            sys5(SYS_umount2, (long)"/newroot", 0, 0, 0, 0);
            return;
        }
    } else {
        // уже сеянный корень: только монт-пойнты
        sys3(SYS_mkdir, (long)"/newroot/mnt", 0755, 0);
    }
    do_switch_root();
}

void milin_main(void) {
    P("MiLin supervisor: сервисные манифесты из /etc/milin (M7: reaper)\n");

    // /init есть только в initramfs; маркер /etc/milin/.installed ставит
    // minstall — установленная система не завершается после oneshot'ов
    g_installed = 0;
    {
        long f = sys3(SYS_open, (long)"/init", O_RDONLY, 0);
        if (f < 0) {
            long m = sys3(SYS_open, (long)"/etc/milin/.installed", O_RDONLY, 0);
            if (m >= 0) {
                g_installed = 1;
                sys3(SYS_close, m, 0, 0);
            }
        } else {
            sys3(SYS_close, f, 0, 0);
        }
    }

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

    // PCI -> UIO: список пар "vendor device" из /etc/milin/pci.ids
    // (по строке на устройство); файла нет — дефолт qemu pci-testdev
    long bfd = sys3(SYS_open, (long)"/sys/bus/pci/drivers/uio_pci_generic/new_id", O_WRONLY, 0);
    if (bfd >= 0) {
        long ids = sys3(SYS_open, (long)"/etc/milin/pci.ids", O_RDONLY, 0);
        if (ids >= 0) {
            char b[512];
            long n = sys3(SYS_read, ids, (long)b, sizeof(b) - 1);
            sys3(SYS_close, ids, 0, 0);
            if (n > 0) {
                b[n] = 0;
                long i = 0;
                while (i < n) {
                    long j = i;
                    while (j < n && b[j] != '\n') j++;
                    if (j > i) {
                        b[j] = 0;
                        if (b[i] != '#')
                            sys3(SYS_write, bfd, (long)(b + i),
                                 (unsigned)(j - i + 1));
                    }
                    i = j + 1;
                }
                P("supervisor: PCI-устройства из pci.ids отданы uio_pci_generic\n");
            }
            sys3(SYS_close, bfd, 0, 0);
        } else {
            sys3(SYS_write, bfd, (long)"1b36 0005\n", 10);
            sys3(SYS_close, bfd, 0, 0);
            P("supervisor: pci-testdev отдан драйверу uio_pci_generic\n");
        }
    } else {
        P("supervisor: sysfs недоступен, UIO-привязка невозможна\n");
    }

    // --- M15: root= из cmdline -> постоянный ext4 -> exec /sbin/init ---
    // (не возвращается при успехе; без root= или с плохим диском — дальше)
    try_switch_root();

    cmdline_install(install_dev, sizeof(install_dev));

    // --- сервисы: из манифестов, в порядке order ---
    load_manifests();
    sort_svcs();
    P("supervisor: манифестов: ");
    print_dec_n(nsvcs);
    P("\n");
    // install= : сессия установки — сервисы + инсталлятор, без демо-oneshot'ов
    for (int i = 0; i < nsvcs; i++) {
        struct svc *s = &svcs[i];
        if (install_dev[0] && s->type == SVC_ONESHOT) {
            s->done = 1;
            continue;
        }
        spawn_svc(s);
        P("supervisor: старт ");
        print_str(s->name);
        P(" (");
        print_str(s->exec);
        P("): ");
        print_str(s->type == SVC_ONESHOT ? "oneshot" : "service");
        P("\n");
    }
    if (install_dev[0] && nsvcs < SVC_MAX) {
        struct svc *s = &svcs[nsvcs++];
        copy_field(s->name, "minstall", sizeof(s->name));
        copy_field(s->exec, "/sbin/minstall", sizeof(s->exec));
        s->type = SVC_ONESHOT;
        s->order = 98;
        s->pid = spawn_arg("/sbin/minstall", "minstall", install_dev);
        P("supervisor: minstall ");
        print_str(install_dev);
        P(" — установка\n");
    }
    struct svc *client = find_oneshot();
    // install-сессия: манифестные oneshot'ы не спавнятся (done=1), вердикт —
    // по minstall (последний в массиве), иначе клиентский статус всегда -1
    if (install_dev[0] && nsvcs > 0)
        client = &svcs[nsvcs - 1];

    // --- M9: kdriver-домен (root: sysfs placement требует привилегий) ---
    if (kd_demo()) {
        P("M9 FAILED\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    // --- reaper: wait4(-1) на любого ребёнка ---
    // M22: GPU-фасад e2e ведёт /sbin/gputest (oneshot): kernel/user
    // placement — политика клиента, /init placement не трогает
    int client_status = -1;
    for (;;) {
        int status = 0;
        // r10/r8 обязаны быть чистыми: мусор ядро читает как rusage (M4)
        long r = sys5(SYS_wait4, -1, (long)&status, 0, 0, 0);
        if (r < 0) break; // детей больше нет
        if (client && r == client->pid) {
            // статус первого oneshot — для итоговой строки "клиент: OK"
            client_status = status;
        }
        struct svc *s = find_svc((int)r);
        if (!s) continue;
        if (s->type == SVC_ONESHOT) {
            s->done = 1;
            s->status = status;
        }

        if (s->stopping) {
            // запланированная остановка — норма graceful shutdown
            s->stopped = 1;
        } else if (s->type == SVC_ONESHOT) {
            // oneshot закончил работу; в initramfs-сессии система завершается,
            // когда завершатся ВСЕ (демо-клиенты M5.2/M12 — отдельные oneshot'ы);
            // установленная система живёт дальше (getty держит вход, M19)
            if (all_oneshots_done()) {
                if (!g_installed) break;
                continue;
            }
        } else {
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

    // M22: вердикт фасада — по статусу oneshot'а gputest (в установленной
    // системе /init тоже placement не трогает — один владелец)
    int gpu_ok = 0;
    for (int i = 0; i < nsvcs; i++)
        if (m_strncmp(svcs[i].name, "gputest", 8) == 0 && svcs[i].done
            && !died_by_signal(svcs[i].status)
            && exit_code(svcs[i].status) == 0)
            gpu_ok = 1;
    P("supervisor: gpu e2e: ");
    if (gpu_ok) P("OK\n"); else P("FAIL\n");

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
        if (!gpu_ok) {
            P("M11 FAILED: gpu e2e клиент не прошёл\n");
            sys3(SYS_exit, 1, 0, 0);
        }
        P("M11 OK: mlkd amdgpu facade — one protocol, user/kernel placement\n");
        P("M22 OK: gpu facade e2e — dev/lane cycles + deny paths (cap/placement)\n");
        // M10: amdgpu в ядре виден как built-in модуль в /sys/module/amdgpu
        {
            long fd = sys3(SYS_open, (long)"/sys/module/amdgpu", 0, 0);
            if (fd >= 0) {
                sys3(SYS_close, fd, 0, 0);
                P("M10 OK: amdgpu в ядре (kdrv-сборка; в QEMU нет AMD-железа)\n");
            } else {
                P("kdrv: amdgpu нет в ядре (diet-сборка) — норма\n");
            }
        }
        sys3(SYS_exit, 0, 0, 0);
    }
    P("M9 FAILED\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
