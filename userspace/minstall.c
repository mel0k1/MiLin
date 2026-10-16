// /sbin/minstall <диск> [uuid_disk=GUID] [uuid_esp=GUID] [uuid_root=GUID]

#include "milin.h"

#define BLKGETSIZE64 0x80081272
#define BLKRRPART    0x125F

#define ESP_SECTORS 131072u
#define P1_START    2048u
#define P2_START    (P1_START + ESP_SECTORS)
#define NENT        128u
#define ESZ         128u

static const u8 GID_ESP[16] = {0x28,0x73,0x2A,0xC1,0x1F,0xF8,0xD2,0x11,
                               0xBA,0x4B,0x00,0xA0,0xC9,0x3E,0xC9,0x3B};
static const u8 GID_ROOTT[16] = {0xE3,0xBC,0x68,0x4F,0xCD,0xE8,0xB1,0x4D,
                                 0x96,0xE7,0xFB,0xCA,0xF9,0x84,0xB7,0x09};

static int gen_guid(u8 g[16]) {
    long fd = sys3(SYS_open, (long)"/dev/urandom", O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = sys3(SYS_read, fd, (long)g, 16);
    sys3(SYS_close, fd, 0, 0);
    if (n != 16) return 0;
    g[7] = (g[7] & 0x0F) | 0x40;
    g[8] = (g[8] & 0x3F) | 0x80;
    return 1;
}

static const char HEXD[17] = "0123456789abcdef";

static int guid_eq(const u8 *a, const u8 *b) {
    for (int i = 0; i < 16; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static int hexv(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int guid_parse(const char *s, u8 g[16]) {
    u8 h[16];
    int n = 0;
    for (int i = 0; s[i]; i++) {
        if (s[i] == '-') continue;
        int v = hexv(s[i]);
        if (v < 0 || n >= 32) return 0;
        if (!(n & 1)) h[n >> 1] = (u8)(v << 4);
        else h[n >> 1] |= (u8)v;
        n++;
    }
    if (n != 32 || s[8] != '-' || s[13] != '-' || s[18] != '-' || s[23] != '-' || s[36])
        return 0;
    g[0] = h[3]; g[1] = h[2]; g[2] = h[1]; g[3] = h[0];
    g[4] = h[5]; g[5] = h[4];
    g[6] = h[7]; g[7] = h[6];
    for (int i = 8; i < 16; i++) g[i] = h[i];
    return 1;
}

static int ebput(char *o, int p, const char *s) {
    while (*s) o[p++] = *s++;
    return p;
}

static void guid_str(const u8 g[16], char *out) {
    int p = 0;
    for (int f = 0; f < 16; f++) {
        u8 b = g[f];
        if (f < 4) b = g[3 - f];        // data1: LE
        else if (f < 6) b = g[9 - f];   // data2: LE
        else if (f < 8) b = g[13 - f];  // data3: LE
        out[p++] = HEXD[b >> 4];
        out[p++] = HEXD[b & 15];
        if (f == 3 || f == 5 || f == 7 || f == 9) out[p++] = '-';
    }
    out[p] = 0;
}

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

static int streq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static char *scat(char *dst, const char *a, const char *b) {
    int i = 0, j = 0;
    while (a[i]) { dst[i] = a[i]; i++; }
    while (b[j]) { dst[i + j] = b[j]; j++; }
    dst[i + j] = 0;
    return dst;
}

static u32 crc_tab[256];

static void crc_init(void) {
    for (u32 i = 0; i < 256; i++) {
        u32 c = i;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
        crc_tab[i] = c;
    }
}

static u32 crc32b(const u8 *b, unsigned n) {
    u32 c = 0xFFFFFFFFu;
    while (n--) c = crc_tab[(c ^ *b++) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

static void put32(u8 *p, u32 v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

static void put16(u8 *p, u32 v) {
    p[0] = v; p[1] = v >> 8;
}

static void put64(u8 *p, u64 v) {
    for (int i = 0; i < 8; i++) p[i] = v >> (8 * i);
}

static void putname(u8 *p, const char *s) {
    int i = 0;
    while (s[i] && i < 35) { p[2 * i] = s[i]; p[2 * i + 1] = 0; i++; }
}

static int write_at(long fd, u64 off, const void *buf, unsigned n) {
    if (sys3(SYS_lseek, fd, (long)off, SEEK_SET) < 0) return 0;
    return sys3(SYS_write, fd, (long)buf, n) == (long)n;
}

static int gpt_write(long fd, u64 nsect,
                     const u8 *disk_guid, const u8 *esp_guid,
                     const u8 *root_guid) {
    static u8 hdr[512], hdrb[512], ent[16384];
    static u8 mbr[512];
    u64 last = nsect - 1;

    m_memset(mbr, 0, 512);
    mbr[0x1BE + 4] = 0xEE;
    mbr[0x1BE + 5] = 0xFF; mbr[0x1BE + 6] = 0xFF; mbr[0x1BE + 7] = 0xFF;
    put32(mbr + 0x1BE + 8, 1);
    put32(mbr + 0x1BE + 12, nsect > 0xFFFFFFFFu ? 0xFFFFFFFFu : (u32)(nsect - 1));
    mbr[0x1FE] = 0x55; mbr[0x1FF] = 0xAA;

    m_memset(ent, 0, 16384);
    m_memcpy(ent, GID_ESP, 16);
    m_memcpy(ent + 16, esp_guid, 16);
    put64(ent + 32, P1_START);
    put64(ent + 40, P1_START + ESP_SECTORS - 1);
    putname(ent + 56, "ESP");
    m_memcpy(ent + ESZ, GID_ROOTT, 16);
    m_memcpy(ent + ESZ + 16, root_guid, 16);
    put64(ent + ESZ + 32, P2_START);
    put64(ent + ESZ + 40, last - 33);
    put64(ent + ESZ + 48, 4);
    putname(ent + ESZ + 56, "root");

    m_memset(hdr, 0, 512);
    m_memcpy(hdr, "EFI PART", 8);
    put32(hdr + 8, 0x00010000);
    put32(hdr + 12, 92);
    put64(hdr + 24, 1);
    put64(hdr + 32, last);
    put64(hdr + 40, 34);
    put64(hdr + 48, last - 33);
    m_memcpy(hdr + 56, disk_guid, 16);
    put64(hdr + 72, 2);
    put32(hdr + 80, NENT);
    put32(hdr + 84, ESZ);
    put32(hdr + 88, crc32b(ent, 16384));
    put32(hdr + 16, crc32b(hdr, 92));

    m_memcpy(hdrb, hdr, 512);
    m_memset(hdrb + 16, 0, 4);
    put64(hdrb + 24, last);
    put64(hdrb + 32, 1);
    put64(hdrb + 72, last - 32);
    put32(hdrb + 16, crc32b(hdrb, 92));

    if (!write_at(fd, 0, mbr, 512)) return 0;
    if (!write_at(fd, 512, hdr, 512)) return 0;
    if (!write_at(fd, 2 * 512, ent, 16384)) return 0;
    if (!write_at(fd, (last - 32) * 512, ent, 16384)) return 0;
    if (!write_at(fd, last * 512, hdrb, 512)) return 0;
    return sys3(SYS_fsync, fd, 0, 0) == 0;
}

static long file_size(const char *p) {
    long fd = sys3(SYS_open, (long)p, O_RDONLY, 0);
    if (fd < 0) return -1;
    long n = sys3(SYS_lseek, fd, 0, 2 /*SEEK_END*/);
    sys3(SYS_close, fd, 0, 0);
    return n;
}

static int esp_put_file(long fd, long start_cl, const char *path) {
    static u8 cl[8192];
    long kf = sys3(SYS_open, (long)path, O_RDONLY, 0);
    if (kf < 0) return 0;
    long ncl = 0;
    for (;;) {
        m_memset(cl, 0, sizeof(cl));
        long have = 0;
        while (have < (long)sizeof(cl)) {
            long n = sys3(SYS_read, kf, (long)cl + have, sizeof(cl) - have);
            if (n <= 0) break;
            have += n;
        }
        long off = (97 + (start_cl + ncl - 2) * 16) * 512;
        if (!write_at(fd, off, cl, sizeof(cl))) {
            sys3(SYS_close, kf, 0, 0);
            return 0;
        }
        ncl++;
        if (have < (long)sizeof(cl)) break;
    }
    sys3(SYS_close, kf, 0, 0);
    return 1;
}

static int esp_write(const char *esp, const char *loader, const char *kern,
                     const char *cmdtext, u32 cmdlen) {
    static u8 sec[512], fat[16384], dirc[8192], dirt[8192], root[16384];
    static u8 cl[8192];

    long lsz = file_size(loader);
    long ksz = file_size(kern);
    if (lsz <= 0 || ksz <= 0 || ksz > 0xFFFFFFF0u) return 0;
    if (cmdlen == 0 || cmdlen > 8192) return 0;
    long nl = (lsz + 8191) / 8192;
    long nk = (ksz + 8191) / 8192;
    if (3 + nl + nk + 1 > 8180) return 0;
    long cl_loader = 5, cl_kern = 5 + nl, cl_cmd = 5 + nl + nk;

    long fd = sys3(SYS_open, (long)esp, O_RDWR, 0);
    if (fd < 0) return 0;

    m_memset(sec, 0, 512);
    sec[0] = 0xEB; sec[1] = 0x3C; sec[2] = 0x90;
    m_memcpy(sec + 3, "MSDOS5.0", 8);
    put16(sec + 11, 512);
    sec[13] = 16;
    put16(sec + 14, 1);
    sec[16] = 2;
    put16(sec + 17, 512);
    put16(sec + 19, 0);
    sec[21] = 0xF8;
    put16(sec + 22, 32);
    put16(sec + 24, 32);
    put16(sec + 26, 64);
    put32(sec + 28, P1_START);
    put32(sec + 32, ESP_SECTORS);
    sec[36] = 0x80;
    sec[38] = 0x29;
    put32(sec + 39, 0x4D494C49);
    m_memcpy(sec + 43, "MILINI     ", 11);
    m_memcpy(sec + 54, "FAT16   ", 8);
    sec[510] = 0x55; sec[511] = 0xAA;

    m_memset(fat, 0, sizeof(fat));
    fat[0] = 0xF8; fat[1] = 0xFF;
    fat[2] = 0xFF; fat[3] = 0xFF;
    put16(fat + 2 * 2, 0xFFFF);
    put16(fat + 3 * 2, 0xFFFF);
    put16(fat + 4 * 2, 0xFFFF);   // /EFI/MILIN: кластер 4 — одно-кластерный каталог (EOF)
    for (long i = 0; i < nl - 1; i++)
        put16(fat + (cl_loader + i) * 2, (u16)(cl_loader + i + 1));
    put16(fat + (cl_loader + nl - 1) * 2, 0xFFFF);
    for (long i = 0; i < nk - 1; i++)
        put16(fat + (cl_kern + i) * 2, (u16)(cl_kern + i + 1));
    put16(fat + (cl_kern + nk - 1) * 2, 0xFFFF);
    put16(fat + cl_cmd * 2, 0xFFFF);

    m_memset(root, 0, sizeof(root));
    m_memcpy(root, "EFI     ", 8);
    root[11] = 0x10;
    put16(root + 26, 2);

    m_memset(dirc, 0, sizeof(dirc));
    dirc[0] = '.';
    dirc[11] = 0x10;
    put16(dirc + 26, 2);
    dirc[32] = '.';
    dirc[33] = '.';
    dirc[43] = 0x10;
    m_memcpy(dirc + 64, "BOOT    ", 8);
    dirc[75] = 0x10;
    put16(dirc + 90, 3);
    m_memcpy(dirc + 96, "MILIN   ", 8);
    dirc[107] = 0x10;
    put16(dirc + 122, 4);
    if (!write_at(fd, 97 * 512, dirc, 8192)) goto fail;

    m_memset(dirt, 0, sizeof(dirt));
    dirt[0] = '.';
    dirt[11] = 0x10;
    put16(dirt + 26, 4);
    dirt[32] = '.';
    dirt[33] = '.';
    dirt[43] = 0x10;
    put16(dirt + 58, 2);
    m_memcpy(dirt + 64, "BOOTX64 ", 8);
    m_memcpy(dirt + 72, "EFI", 3);
    dirt[75] = 0x20;
    put16(dirt + 90, (u16)cl_loader);
    put32(dirt + 92, (u32)lsz);
    if (!write_at(fd, (97 + 2 * 16) * 512, dirt, 8192)) goto fail;

    m_memset(dirc, 0, sizeof(dirc));
    dirc[0] = '.';
    dirc[11] = 0x10;
    put16(dirc + 26, 3);
    dirc[32] = '.';
    dirc[33] = '.';
    dirc[43] = 0x10;
    put16(dirc + 58, 2);
    m_memcpy(dirc + 64, "BOOTX64 ", 8);
    m_memcpy(dirc + 72, "EFI", 3);
    dirc[75] = 0x20;
    put16(dirc + 90, (u16)cl_loader);
    put32(dirc + 92, (u32)lsz);
    m_memcpy(dirc + 96, "VMLINUZ ", 8);
    m_memcpy(dirc + 104, "EFI", 3);
    dirc[107] = 0x20;
    put16(dirc + 122, (u16)cl_kern);
    put32(dirc + 124, (u32)ksz);
    m_memcpy(dirc + 128, "CMDLINE", 8);
    dirc[139] = 0x20;
    put16(dirc + 154, (u16)cl_cmd);
    put32(dirc + 156, cmdlen);
    if (!write_at(fd, 97 * 512 + 8192, dirc, 8192)) goto fail;

    if (!write_at(fd, 0, sec, 512)) goto fail;
    if (!write_at(fd, 512, fat, 16384)) goto fail;
    if (!write_at(fd, 33 * 512, fat, 16384)) goto fail;
    if (!write_at(fd, 65 * 512, root, 16384)) goto fail;

    if (!esp_put_file(fd, cl_loader, loader)) goto fail;
    if (!esp_put_file(fd, cl_kern, kern)) goto fail;
    m_memset(cl, 0, sizeof(cl));
    m_memcpy(cl, cmdtext, cmdlen);
    if (!write_at(fd, (97 + (cl_cmd - 2) * 16) * 512, cl, 8192)) goto fail;

    sys3(SYS_fsync, fd, 0, 0);
    sys3(SYS_close, fd, 0, 0);
    return 1;
fail:
    sys3(SYS_close, fd, 0, 0);
    return 0;
}

static int dev_has(const char *name) {
    static char buf[16384];
    if (name[0] == '/' && name[1] == 'd' && name[2] == 'e' &&
        name[3] == 'v' && name[4] == '/')
        name += 5;
    long fd = sys3(SYS_open, (long)"/dev", O_RDONLY | O_DIRECTORY, 0);
    if (fd < 0) return 0;
    int found = 0;
    for (;;) {
        long n = sys3(SYS_getdents64, fd, (long)buf, sizeof(buf));
        if (n <= 0) break;
        long off = 0;
        while (off < n) {
            struct ldirent64 *e = (struct ldirent64 *)(buf + off);
            if (streq(e->d_name, name)) found = 1;
            off += e->d_reclen;
        }
        if (found) break;
    }
    sys3(SYS_close, fd, 0, 0);
    return found;
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
    if ((status & 0x7f) || ((status >> 8) & 0xff)) {
        P("minstall: fork ");
        print_str(path);
        P(" rc=");
        print_dec_n((status >> 8) & 0xff);
        P(" sig=");
        print_dec_n(status & 0x7f);
        P("\n");
    }
    return (status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0;
}

void milin_main(void) {
    if (!milin_argv || !milin_argv[1]) {
        P("minstall: usage: minstall /dev/vdX [uuid_disk=GUID] [uuid_esp=GUID] [uuid_root=GUID]\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    const char *dev = milin_argv[1];
    const char *ud = 0, *ue = 0, *ur = 0;
    for (int i = 2; milin_argv[i]; i++) {
        if (m_strncmp(milin_argv[i], "uuid_disk=", 10) == 0) ud = milin_argv[i] + 10;
        else if (m_strncmp(milin_argv[i], "uuid_esp=", 9) == 0) ue = milin_argv[i] + 9;
        else if (m_strncmp(milin_argv[i], "uuid_root=", 10) == 0) ur = milin_argv[i] + 10;
        else {
            P("minstall: неизвестный аргумент: ");
            print_str(milin_argv[i]);
            P("\n");
            sys3(SYS_exit, 1, 0, 0);
        }
    }
    char p1[64], p2[64];
    scat(p1, dev, "1");
    scat(p2, dev, "2");

    long fd = sys3(SYS_open, (long)dev, O_RDWR, 0);
    if (fd < 0) {
        P("minstall: ");
        print_str(dev);
        P(": FAIL (нет диска)\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    u64 size = 0;
    if (sys3(SYS_ioctl, fd, BLKGETSIZE64, (long)&size) != 0 || size < 200u << 20) {
        P("minstall: размер диска не читается или < 200M\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    u64 nsect = size >> 9;

    crc_init();

    u8 g_disk[16], g_esp[16], g_root[16];
    if (ud && !guid_parse(ud, g_disk)) {
        P("minstall: uuid_disk= — GUID не 8-4-4-4-12\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (ue && !guid_parse(ue, g_esp)) {
        P("minstall: uuid_esp= — GUID не 8-4-4-4-12\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (ur && !guid_parse(ur, g_root)) {
        P("minstall: uuid_root= — GUID не 8-4-4-4-12\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if ((!ud && !gen_guid(g_disk)) || (!ue && !gen_guid(g_esp)) ||
        (!ur && !gen_guid(g_root))) {
        P("minstall: /dev/urandom: FAIL (GUID установки не сгенерировать)\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (guid_eq(g_disk, g_esp) || guid_eq(g_disk, g_root) || guid_eq(g_esp, g_root)) {
        P("minstall: GUID'ы партиций совпадают — недопустимо\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (ud || ue || ur) {
        char gt[40], eb[160];
        int ep = 0;
        const char *m = "minstall: заданы оператором:";
        while (*m) eb[ep++] = *m++;
        if (ud) { guid_str(g_disk, gt); ep = ebput(eb, ep, " uuid_disk="); ep = ebput(eb, ep, gt); }
        if (ue) { guid_str(g_esp, gt); ep = ebput(eb, ep, " uuid_esp="); ep = ebput(eb, ep, gt); }
        if (ur) { guid_str(g_root, gt); ep = ebput(eb, ep, " uuid_root="); ep = ebput(eb, ep, gt); }
        eb[ep++] = '\n';
        sys3(SYS_write, 1, (long)eb, ep);
    }
    P("minstall: GPT ");
    print_dec_n(nsect);
    P(" секторов: ESP 64M + root\n");
    if (!gpt_write(fd, nsect, g_disk, g_esp, g_root)) {
        P("minstall: запись GPT: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    {
        static u8 ent[16384];
        sys3(SYS_lseek, fd, 2 * 512, SEEK_SET);
        long n = sys3(SYS_read, fd, (long)ent, 16384);
        if (n != 16384) {
            P("minstall: чтение записей GPT: FAIL\n");
            sys3(SYS_exit, 1, 0, 0);
        }
        for (int i = 0; i < 16; i++) {
            if (ent[ESZ + 16 + i] != g_root[i]) {
                P("minstall: GUID корня не совпал при чтении назад\n");
                sys3(SYS_exit, 1, 0, 0);
            }
        }
    }
    char gs[40];
    guid_str(g_root, gs);
    {
        char eb[64];
        int ep = 0;
        const char *m = "minstall: PARTUUID ";
        while (*m) eb[ep++] = *m++;
        m_memcpy(eb + ep, gs, 36);
        ep += 36;
        eb[ep++] = '\n';
        sys3(SYS_write, 1, (long)eb, ep);
    }

    long rr = sys3(SYS_ioctl, fd, BLKRRPART, 0);
    P("minstall: BLKRRPART -> ");
    print_dec_n(rr);
    P("\n");
    if (rr != 0) {
        P("minstall: рескан не удался\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    int ok = 0;
    for (int t = 0; t < 100; t++) {
        if (dev_has(p1) && dev_has(p2)) { ok = 1; break; }
        sleep_ms(150);
    }
    if (!ok) {
        P("minstall: ");
        print_str(p1);
        P("/");
        print_str(p2);
        P(" не появились в devtmpfs\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("minstall: ");
    print_str(p1);
    P(" + ");
    print_str(p2);
    P(" в devtmpfs (рескан без ребута)\n");

    static u8 s0[512];
    sys3(SYS_lseek, fd, 0, SEEK_SET);
    sys3(SYS_read, fd, (long)s0, 512);
    P("minstall: защитный MBR тип ");
    print_hex(s0[0x1C2]);

    u64 sz = 0;
    long p2fd = sys3(SYS_open, (long)p2, O_RDONLY, 0);
    if (p2fd >= 0) {
        sys3(SYS_ioctl, p2fd, BLKGETSIZE64, (long)&sz);
        static u8 head[16];
        sys3(SYS_lseek, p2fd, 0, SEEK_SET);
        sys3(SYS_read, p2fd, (long)head, 16);
        sys3(SYS_close, p2fd, 0, 0);
        P("minstall: ");
        print_str(p2);
        P(" размер ");
        print_dec_n(sz);
        P(" первые байты ");
        print_hex_flat((u32)head[0] | ((u32)head[1] << 8) |
                       ((u32)head[2] << 16) | ((u32)head[3] << 24));
        P("\n");
    }

    char *mkav[9] = { (char *)"mkfs.ext4", (char *)"-F", (char *)"-O",
                      (char *)"^64bit,^metadata_csum", (char *)"-b",
                      (char *)"4096", p2, (char *)"32768", 0 };
    if (!fork_check("/usr/bin/mkfs.ext4", mkav)) {
        P("minstall: mkfs.ext4: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("minstall: mkfs.ext4 128M OK (мир 71M, resize2fs в конце)\n");

    sys3(SYS_mkdir, (long)"/mst", 0755, 0);
    if (sys5(SYS_mount, (long)p2, (long)"/mst", (long)"ext4", 0, 0) != 0) {
        P("minstall: mount ");
        print_str(p2);
        P(": FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("minstall: ");
    print_str(p2);
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

    static char append[512];
    int k = 0;
    const char *s = "root=PARTUUID=";
    for (int i = 0; s[i]; i++) append[k++] = s[i];
    for (int i = 0; gs[i]; i++) append[k++] = gs[i];
    append[k++] = ' ';
    s = "rw rootwait";
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
                    if (m_strncmp(cl + st2, "uuid_disk=", 10) == 0) continue;
                    if (m_strncmp(cl + st2, "uuid_esp=", 9) == 0) continue;
                    if (m_strncmp(cl + st2, "uuid_root=", 10) == 0) continue;
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

    long gm = sys3(SYS_open, (long)"/usr/lib/syslinux/gptmbr.bin", O_RDONLY, 0);
    if (gm >= 0) {
        static u8 gmb[440];
        long gn = sys3(SYS_read, gm, (long)gmb, 440);
        sys3(SYS_close, gm, 0, 0);
        if (gn > 0 && write_at(fd, 0, gmb, gn)) {
            sys3(SYS_fsync, fd, 0, 0);
            P("minstall: BIOS-загрузчик gptmbr OK\n");
        } else {
            P("minstall: gptmbr FAIL — только UEFI\n");
        }
    } else {
        P("minstall: gptmbr.bin нет — только UEFI\n");
    }

    const char *ldr = "/boot/efiboot.pefi";
    if (file_size(ldr) <= 0) {
        P("minstall: efiboot.pefi нет — BOOTX64.EFI = ядро (cmdline из CONFIG_CMDLINE)\n");
        ldr = "/boot/vmlinuz";
    }
    if (esp_write(p1, ldr, "/boot/vmlinuz", append, (u32)k)) {
        P("minstall: ESP OK — BOOTX64.EFI + MILIN/BOOTX64.EFI + VMLINUZ.EFI + CMDLINE\n");
    } else {
        P("minstall: ESP FAIL — только BIOS\n");
    }

    sys5(SYS_umount2, (long)"/mst", 0, 0, 0, 0);

    if (sz > (u64)32768 * 4096) {
        char *rav[4] = { (char *)"resize2fs", (char *)"-f", p2, 0 };
        if (!fork_check("/usr/bin/resize2fs", rav)) {
            P("minstall: resize2fs: FAIL (продолжаю — fs 128M)\n");
        } else {
            P("minstall: RESIZE OK — ext4 на весь p2\n");
        }
    } else {
        P("minstall: RESIZE SKIP — p2 не больше 128M\n");
    }

    P("MINSTALL OK ");
    print_str(dev);
    P("\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
