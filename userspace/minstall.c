// /sbin/minstall <диск> (M19): MiLini на блочное устройство.
// MBR-таблица (1 партиция 83, bootable) -> ждём узел партиции в devtmpfs ->
// mkfs.ext4 (syslinux 6.03 не читает 64bit/metadata_csum) -> монтируем ->
// копия мира + /init -> /sbin/init -> extlinux.conf (консольные параметры
// текущего cmdline) -> extlinux --install -> mbr.bin -> MINSTALL OK.
// запуск: cmdline install=/dev/vdX (oneshot от супервизора) или из шелла.

#include "milin.h"

#define BLKGETSIZE64 0x80081272

struct mstat {
    u64 dev, ino, nlink;
    u32 mode, uid, gid, pad;
    u64 rdev, size, blk, blocks;
    u64 atime, mtime, ctime;
    u64 x[6];
};

static int m_stat(const char *p, struct mstat *st) {
    m_memset(st, 0, sizeof(*st));
    return sys3(SYS_stat, (long)p, (long)st, 0) == 0;
}

static int m_lstat(const char *p, struct mstat *st) {
    m_memset(st, 0, sizeof(*st));
    return sys3(SYS_lstat, (long)p, (long)st, 0) == 0;
}

static void copy_file(const char *src, const char *dst) {
    long in = sys3(SYS_open, (long)src, O_RDONLY, 0);
    if (in < 0) {
        P("minstall: open ");
        print_str(src);
        P("\n");
        return;
    }
    struct mstat st;
    u32 mode = 0644;
    if (m_lstat(src, &st)) mode = st.mode & 07777;
    long out = sys3(SYS_open, (long)dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (out < 0) {
        P("minstall: create ");
        print_str(dst);
        P("\n");
        sys3(SYS_close, in, 0, 0);
        return;
    }
    static char cb[65536];
    for (;;) {
        long n = sys3(SYS_read, in, (long)cb, sizeof(cb));
        if (n <= 0) break;
        if (sys3(SYS_write, out, (long)cb, n) != n) {
            P("minstall: short write ");
            print_str(dst);
            P("\n");
            break;
        }
    }
    sys3(SYS_close, in, 0, 0);
    sys3(SYS_close, out, 0, 0);
}

static void copy_entry(const char *sdir, const char *ddir,
                       const char *name, int depth);

static void copytree(const char *src, const char *dst, int depth) {
    if (depth > 8) return;
    long fd = sys3(SYS_open, (long)src, O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return;
    char dbuf[16384];
    for (;;) {
        long n = sys3(SYS_getdents64, fd, (long)dbuf, sizeof(dbuf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            struct ldirent64 *e = (struct ldirent64 *)(dbuf + off);
            const char *nm = e->d_name;
            if (!(nm[0] == '.' && (!nm[1] || (nm[1] == '.' && !nm[2])))) {
                char sp[160], dp[160];
                int a = 0, b = 0;
                while (src[a]) { sp[a] = src[a]; a++; }
                if (a && sp[a - 1] != '/') sp[a++] = '/';
                while (nm[b]) { sp[a + b] = nm[b]; b++; }
                sp[a + b] = 0;
                a = 0;
                while (dst[a]) { dp[a] = dst[a]; a++; }
                if (a && dp[a - 1] != '/') dp[a++] = '/';
                b = 0;
                while (nm[b]) { dp[a + b] = nm[b]; b++; }
                dp[a + b] = 0;

                struct mstat st;
                if (!m_lstat(sp, &st)) goto next;
                if ((st.mode & 0xF000) == 0x4000) {
                    sys3(SYS_mkdir, (long)dp, st.mode & 07777, 0);
                    copytree(sp, dp, depth + 1);
                } else if ((st.mode & 0xF000) == 0xA000) {
                    char tgt[160];
                    long r = sys3(SYS_readlink, (long)sp, (long)tgt, sizeof(tgt) - 1);
                    if (r > 0) {
                        tgt[r] = 0;
                        sys3(SYS_symlink, (long)tgt, (long)dp, 0);
                    }
                } else if ((st.mode & 0xF000) == 0x8000) {
                    copy_file(sp, dp);
                }
            }
        next:
            off += e->d_reclen;
        }
    }
    sys3(SYS_close, fd, 0, 0);
}

static void copy_entry(const char *sdir, const char *ddir,
                       const char *name, int depth) {
    char sp[160], dp[160];
    int a = 0, b = 0;
    while (sdir[a]) { sp[a] = sdir[a]; a++; }
    if (a && sp[a - 1] != '/') sp[a++] = '/';
    while (name[b]) { sp[a + b] = name[b]; b++; }
    sp[a + b] = 0;
    a = 0;
    while (ddir[a]) { dp[a] = ddir[a]; a++; }
    if (a && dp[a - 1] != '/') dp[a++] = '/';
    b = 0;
    while (name[b]) { dp[a + b] = name[b]; b++; }
    dp[a + b] = 0;

    struct mstat st;
    if (!m_lstat(sp, &st)) return;
    if ((st.mode & 0xF000) == 0x4000) {
        sys3(SYS_mkdir, (long)dp, st.mode & 07777, 0);
        copytree(sp, dp, depth + 1);
    } else if ((st.mode & 0xF000) == 0xA000) {
        char tgt[160];
        long r = sys3(SYS_readlink, (long)sp, (long)tgt, sizeof(tgt) - 1);
        if (r > 0) {
            tgt[r] = 0;
            sys3(SYS_symlink, (long)tgt, (long)dp, 0);
        }
    } else if ((st.mode & 0xF000) == 0x8000) {
        copy_file(sp, dp);
    }
}

static const char *inst_dirs[] = { "bin", "boot", "etc", "home", "lib", "lib64",
                                   "root", "sbin", "tmp", "usr", "var", 0 };

static int fork_check(const char *path, char *av[]) {
    int pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        if (sys3(SYS_execve, (long)path, (long)av, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }
    int status = 0;
    sys5(SYS_wait4, pid, (long)&status, 0, 0, 0);
    return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0;
}

void milin_main(void) {
    if (!milin_argv || !milin_argv[1]) {
        P("minstall: usage: minstall /dev/vdX\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    const char *dev = milin_argv[1];

    // цель — диск целиком (без таблицы партиций): ext4 оставляет первые
    // 1024 байта под boot-сектор, extlinux --install пишет его через ФС;
    // живой рескан BLKRRPART в этом ядре отдаёт устаревший кэш скана
    long fd = sys3(SYS_open, (long)dev, O_RDWR, 0);
    if (fd < 0) {
        P("minstall: ");
        print_str(dev);
        P(": FAIL (нет диска)\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    u64 size = 0;
    if (sys3(SYS_ioctl, fd, BLKGETSIZE64, (long)&size) != 0 || size < 32 << 20) {
        P("minstall: размер диска не читается или < 32M\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    sys3(SYS_close, fd, 0, 0);

    char *mkav[6] = { (char *)"mkfs.ext4", (char *)"-F", (char *)"-O",
                      (char *)"^64bit,^metadata_csum", (char *)dev, 0 };
    if (!fork_check("/usr/bin/mkfs.ext4", mkav)) {
        P("minstall: mkfs.ext4: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    sys3(SYS_mkdir, (long)"/mst", 0755, 0);
    if (sys5(SYS_mount, (long)dev, (long)"/mst", (long)"ext4", 0, 0) != 0) {
        P("minstall: mount ");
        print_str(dev);
        P(": FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("minstall: ");
    print_str(dev);
    P(" смонтирован, копия мира...\n");

    for (int i = 0; inst_dirs[i]; i++)
        copy_entry("", "/mst", inst_dirs[i], 0);
    sys3(SYS_mkdir, (long)"/mst/mnt", 0755, 0);
    sys3(SYS_mkdir, (long)"/mst/proc", 0755, 0);
    sys3(SYS_mkdir, (long)"/mst/sys", 0755, 0);
    sys3(SYS_mkdir, (long)"/mst/dev", 0755, 0);
    copy_file("/init", "/mst/sbin/init");
    long im = sys3(SYS_open, (long)"/mst/etc/milin/.installed",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (im >= 0) {
        sys3(SYS_write, im, (long)"minstall\n", 9);
        sys3(SYS_close, im, 0, 0);
    }
    sys3(SYS_chown, (long)"/mst/home/user", 1000, 1000);

    // APPEND: root= + rw + консольные параметры текущего cmdline
    static char append[512];
    int k = 0;
    const char *s = "root=";
    for (int i = 0; s[i]; i++) append[k++] = s[i];
    for (int i = 0; dev[i]; i++) append[k++] = dev[i];
    append[k++] = ' ';
    s = "rw";
    for (int i = 0; s[i]; i++) append[k++] = s[i];
    append[k++] = ' ';

    long cf = sys3(SYS_open, (long)"/proc/cmdline", O_RDONLY, 0);
    if (cf >= 0) {
        static char cl[512];
        long n = sys3(SYS_read, cf, (long)cl, sizeof(cl) - 1);
        sys3(SYS_close, cf, 0, 0);
        if (n > 0) {
            cl[n] = 0;
            long i = 0;
            while (i < n && k < (int)sizeof(append) - 64) {
                while (i < n && (cl[i] == ' ' || cl[i] == '\n')) i++;
                long st2 = i;
                while (i < n && cl[i] != ' ' && cl[i] != '\n') i++;
                if (i > st2) {
                    if (m_strncmp(cl + st2, "root=", 5) == 0) continue;
                    if (m_strncmp(cl + st2, "install=", 8) == 0) continue;
                    for (long j = st2; j < i; j++) append[k++] = cl[j];
                    append[k++] = ' ';
                }
            }
        }
    }
    append[k] = 0;

    static char conf[640];
    char *w = conf;
    s = "DEFAULT milin\nPROMPT 0\nTIMEOUT 30\n\nLABEL milin\n  KERNEL /boot/vmlinuz\n  INITRD /boot/initramfs.cpio.gz\n  APPEND ";
    while (*s) *w++ = *s++;
    for (int i = 0; append[i]; i++) *w++ = append[i];
    s = "\n";
    while (*s) *w++ = *s++;
    *w = 0;
    // ldlinux 6.03 ищет конфиг по нескольким путям — пишем во все канонические
    static const char *cfg_paths[] = {
        "/mst/extlinux/extlinux.conf",
        "/mst/syslinux/syslinux.cfg",
        "/mst/boot/syslinux/syslinux.cfg",
        "/mst/syslinux.cfg",
        0
    };
    for (int i = 0; cfg_paths[i]; i++) {
        const char *cp = cfg_paths[i];
        char dir[64];
        int di = 0;
        int last = 0;
        while (cp[di]) {
            if (cp[di] == '/') last = di;
            di++;
        }
        m_memcpy(dir, cp, last);
        dir[last] = 0;
        sys3(SYS_mkdir, (long)dir, 0755, 0);
        long bf = sys3(SYS_open, (long)cp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (bf >= 0) {
            sys3(SYS_write, bf, (long)conf, (long)(w - conf));
            sys3(SYS_fsync, bf, 0, 0);
            sys3(SYS_close, bf, 0, 0);
        } else {
            P("minstall: ");
            print_str(cp);
            P(": FAIL\n");
        }
    }

    char *exav[4] = { (char *)"extlinux", (char *)"--install", (char *)"/mst", 0 };
    if (!fork_check("/usr/bin/extlinux", exav)) {
        P("minstall: extlinux --install: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    sys5(SYS_umount2, (long)"/mst", 0, 0, 0, 0);
    P("MINSTALL OK ");
    print_str(dev);
    P("\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
