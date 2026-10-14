// /sbin/passwd (M20): смена пароля в /etc/shadow (sha512-crypt). root меняет
// любой аккаунт без текущего пароля; остальные — свой, подтверждая текущий.
// соль — 16 символов из /dev/urandom; запись атомарная: shadow.tmp ->
// fsync -> fchmod 0600 -> rename. бинарь setuid root.

#include "milin.h"
#include "crypt.h"

#define TCGETS 0x5401
#define TCSETS 0x5402
#define ECHOFL 0x00000008

struct termios_k {
    u32 i, o, c, l;
    u8 line;
    u8 cc[32];
    u8 pad[19];
};

static int read_line(char *buf, int max) {
    int n = 0;
    for (;;) {
        char ch;
        long r = sys3(SYS_read, 0, (long)&ch, 1);
        if (r <= 0) return -1;
        if (ch == '\n' || ch == '\r') break;
        if (ch == 8 || ch == 127) {
            if (n > 0) n--;
            continue;
        }
        if (n < max - 1) buf[n++] = ch;
    }
    buf[n] = 0;
    return n;
}

// подсказка + чтение с выключенным эхом, эхо восстанавливается всегда
static int ask(char *buf, int max) {
    u64 tio[8];
    m_memset(tio, 0, sizeof(tio));
    int echo_off = 0;
    if (sys3(SYS_ioctl, 0, TCGETS, (long)tio) == 0) {
        struct termios_k *t = (struct termios_k *)tio;
        t->l &= ~ECHOFL;
        sys3(SYS_ioctl, 0, TCSETS, (long)tio);
        echo_off = 1;
    }
    int n = read_line(buf, max);
    if (echo_off) {
        struct termios_k *t = (struct termios_k *)tio;
        t->l |= ECHOFL;
        sys3(SYS_ioctl, 0, TCSETS, (long)tio);
    }
    sys3(SYS_write, 1, (long)"\n", 1);
    return n;
}

static int str_eq(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}

static int parse_dec(const char *s) {
    int v = 0;
    while (*s >= '0' && *s <= '9') v = v * 10 + *s++ - '0';
    return v;
}

static void cut_field(const char *ln, int idx, char *dst, int max) {
    dst[0] = 0;
    const char *p = ln;
    for (int i = 0; i < idx; i++) {
        while (*p && *p != ':') p++;
        if (!*p) return;
        p++;
    }
    int i = 0;
    while (p[i] && p[i] != ':' && i < max - 1) { dst[i] = p[i]; i++; }
    dst[i] = 0;
}

static void scopy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) { dst[i] = src[i]; i++; }
    dst[i] = 0;
}

// поиск в /etc/passwd: по имени (name != 0) либо по uid, имя -> out
static int lookup_user(const char *name, long uid, char *out, int omax) {
    static char buf[4096];
    long fd = sys3(SYS_open, (long)"/etc/passwd", O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *ln = buf;
    while (*ln) {
        char *end = ln;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        char nm[32];
        cut_field(ln, 0, nm, sizeof(nm));
        int ok;
        if (name) {
            ok = nm[0] && str_eq(nm, name);
        } else {
            char us[12];
            cut_field(ln, 2, us, sizeof(us));
            ok = (long)parse_dec(us) == uid;
        }
        if (ok) {
            int i = 0;
            while (nm[i] && i < omax - 1) { out[i] = nm[i]; i++; }
            out[i] = 0;
            return 1;
        }
        ln = next;
    }
    return 0;
}

// salt/enc из "$6$salt$hash86" строки юзера в /etc/shadow
static int find_shadow(const char *user, char *salt, int smax, char *enc, int emax) {
    static char buf[4096];
    long fd = sys3(SYS_open, (long)"/etc/shadow", O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return 0;
    buf[n] = 0;
    char *ln = buf;
    while (*ln) {
        char *end = ln;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        *end = 0;
        char nm[32];
        cut_field(ln, 0, nm, sizeof(nm));
        if (nm[0] && str_eq(nm, user)) {
            char hf[160];
            cut_field(ln, 1, hf, sizeof(hf));
            if (hf[0] != '$' || hf[1] != '6' || hf[2] != '$') return 0;
            const char *p = hf + 3;
            int i = 0;
            while (*p && *p != '$' && i < smax - 1) salt[i++] = *p++;
            salt[i] = 0;
            if (*p != '$') return 0;
            p++;
            i = 0;
            while (*p && i < emax - 1) enc[i++] = *p++;
            enc[i] = 0;
            return i == 86;
        }
        ln = next;
    }
    return 0;
}

static long wr_all(long fd, const char *p, long n) {
    while (n > 0) {
        long w = sys3(SYS_write, fd, (long)p, n);
        if (w <= 0) return -1;
        p += w;
        n -= w;
    }
    return 0;
}

// замена hash-поля юзера, остальные строки байт в байт; откат через unlink
// при любом сбое — shadow либо старый, либо полный новый
static int rewrite_shadow(const char *user, const char *field) {
    static char buf[8192];
    long fd = sys3(SYS_open, (long)"/etc/shadow", O_RDONLY, 0);
    if (fd < 0) return 0;
    long n = sys3(SYS_read, fd, (long)buf, sizeof(buf) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return 0;
    buf[n] = 0;

    long out = sys3(SYS_open, (long)"/etc/shadow.tmp",
                    O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (out < 0) return 0;
    sys3(SYS_fchmod, out, 0600, 0);

    int ok = 1, replaced = 0;
    char *ln = buf;
    while (*ln) {
        char *end = ln;
        while (*end && *end != '\n') end++;
        char *next = *end ? end + 1 : end;
        int match = 0;
        int i = 0;
        while (user[i] && ln[i] == user[i]) i++;
        if (!user[i] && ln[i] == ':') match = 1;
        if (match) {
            replaced = 1;
            if (wr_all(out, user, m_strlen(user)) ||
                wr_all(out, ":", 1) ||
                wr_all(out, field, m_strlen(field)) ||
                wr_all(out, "\n", 1))
                ok = 0;
        } else {
            if (wr_all(out, ln, end - ln) ||
                (*end && wr_all(out, "\n", 1)))
                ok = 0;
        }
        ln = next;
    }
    sys3(SYS_fsync, out, 0, 0);
    sys3(SYS_close, out, 0, 0);

    if (!ok || !replaced ||
        sys3(SYS_rename, (long)"/etc/shadow.tmp", (long)"/etc/shadow", 0) != 0) {
        sys3(SYS_unlink, (long)"/etc/shadow.tmp", 0, 0);
        return 0;
    }
    return 1;
}

static int gen_salt(char *out, int n) {
    cb8 raw[16];
    long fd = sys3(SYS_open, (long)"/dev/urandom", O_RDONLY, 0);
    if (fd < 0) return 0;
    long g = sys3(SYS_read, fd, (long)raw, 16);
    sys3(SYS_close, fd, 0, 0);
    if (g != 16) return 0;
    for (int i = 0; i < n; i++) out[i] = sha512_b64t[raw[i] & 63];
    out[n] = 0;
    return 1;
}

void milin_main(void) {
    long uid = sys3(SYS_getuid, 0, 0, 0);
    char user[32];

    if (milin_argv && milin_argv[1]) {
        scopy(user, milin_argv[1], sizeof(user));
    } else {
        if (!lookup_user(0, uid, user, sizeof(user))) {
            P("passwd: нет пользователя\n");
            sys3(SYS_exit, 1, 0, 0);
        }
    }

    if (uid != 0) {
        char own[32];
        if (!lookup_user(0, uid, own, sizeof(own)) || !str_eq(user, own)) {
            P("passwd: permission denied\n");
            sys3(SYS_exit, 1, 0, 0);
        }
    }

    char probe[32];
    if (!lookup_user(user, 0, probe, sizeof(probe))) {
        P("passwd: нет пользователя\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    char salt[20], enc[96];
    int have = find_shadow(user, salt, sizeof(salt), enc, sizeof(enc));

    if (uid != 0) {
        if (!have) {
            P("passwd: некорректно\n");
            sys3(SYS_exit, 1, 0, 0);
        }
        P("Current password: ");
        char cur[64];
        int cn = ask(cur, sizeof(cur));
        if (cn < 0 || !sha512_verify(cur, salt, enc)) {
            P("passwd: некорректно\n");
            struct { long sec, nsec; } t = { 2, 0 };
            sys3(SYS_nanosleep, (long)&t, 0, 0);
            sys3(SYS_exit, 1, 0, 0);
        }
    }

    char pw1[64], pw2[64];
    P("New password: ");
    int n1 = ask(pw1, sizeof(pw1));
    if (n1 <= 0) {
        P("passwd: пустой пароль\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("Retype new password: ");
    int n2 = ask(pw2, sizeof(pw2));
    if (n2 < 0 || n1 != n2 || !str_eq(pw1, pw2)) {
        P("passwd: не совпали\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    char nsalt[17];
    if (!gen_salt(nsalt, 16)) {
        P("passwd: нет /dev/urandom\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    char enc86[88];
    sha512_crypt(pw1, n1, nsalt, 16, enc86);

    static char field[128];
    const char *h = "$6$";
    int fi = 0;
    for (int i = 0; h[i]; i++) field[fi++] = h[i];
    for (int i = 0; nsalt[i]; i++) field[fi++] = nsalt[i];
    field[fi++] = '$';
    for (int i = 0; enc86[i]; i++) field[fi++] = enc86[i];
    field[fi] = 0;

    if (!rewrite_shadow(user, field)) {
        P("passwd: запись FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    P("M20 PASSWD OK user=");
    print_str(user);
    P("\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
