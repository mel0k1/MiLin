// login (M19): проверка /etc/shadow, смена uid, exec шелла.

#include "milin.h"
#include "crypt.h"
#include "utmp.h"

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

static int find_passwd(const char *user, char *home, int hmax,
                       char *shell, int smax, int *uid, int *gid) {
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
        char name[32];
        cut_field(ln, 0, name, sizeof(name));
        if (str_eq(name, user)) {
            char u[12], g[12];
            cut_field(ln, 2, u, sizeof(u));
            cut_field(ln, 3, g, sizeof(g));
            *uid = parse_dec(u);
            *gid = parse_dec(g);
            cut_field(ln, 5, home, hmax);
            cut_field(ln, 6, shell, smax);
            return home[0] && shell[0];
        }
        ln = next;
    }
    return 0;
}

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
        char name[32];
        cut_field(ln, 0, name, sizeof(name));
        if (str_eq(name, user)) {
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

static void fail(void) {
    P("login: некорректно\n");
    struct { long sec, nsec; } t = { 2, 0 };
    sys3(SYS_nanosleep, (long)&t, 0, 0);
    sys3(SYS_exit, 1, 0, 0);
}

void milin_main(void) {
    char user[32];
    if (milin_argv && milin_argv[1]) {
        const char *u = milin_argv[1];
        int i = 0;
        while (u[i] && i < (int)sizeof(user) - 1) { user[i] = u[i]; i++; }
        user[i] = 0;
    } else {
        P("login: ");
        if (read_line(user, sizeof(user)) < 0) sys3(SYS_exit, 1, 0, 0);
        if (!user[0]) fail();
    }

    char home[64], shell[32];
    int uid = 0, gid = 0;
    if (!find_passwd(user, home, sizeof(home), shell, sizeof(shell), &uid, &gid))
        fail();

    char salt[20], enc[96];
    if (!find_shadow(user, salt, sizeof(salt), enc, sizeof(enc))) fail();

    P("Password: ");
    u64 tio[8];
    m_memset(tio, 0, sizeof(tio));
    int echo_off = 0;
    if (sys3(SYS_ioctl, 0, TCGETS, (long)tio) == 0) {
        struct termios_k *t = (struct termios_k *)tio;
        t->l &= ~ECHOFL;
        sys3(SYS_ioctl, 0, TCSETS, (long)tio);
        echo_off = 1;
    }
    char pw[64];
    int pn = read_line(pw, sizeof(pw));
    if (echo_off) {
        struct termios_k *t = (struct termios_k *)tio;
        t->l |= ECHOFL;
        sys3(SYS_ioctl, 0, TCSETS, (long)tio);
    }
    P("\n");
    if (pn < 0 || !sha512_verify(pw, salt, enc)) fail();

    utmp_write(UTMP_USER, (int)sys3(SYS_getpid, 0, 0, 0), "console", user);

    sys3(SYS_setgid, gid, 0, 0);
    sys3(SYS_setuid, uid, 0, 0);
    sys3(SYS_chdir, (long)home, 0, 0);
    sys3(SYS_umask, 022, 0, 0);
    {
        static char ok[48];
        char *p = ok;
        const char *m = "M19 LOGIN OK user=";
        int i = 0;
        while (m[i]) *p++ = m[i++];
        i = 0;
        while (user[i]) *p++ = user[i++];
        *p++ = '\n';
        sys3(SYS_write, 1, (long)ok, (long)(p - ok));
    }

    static char e_home[80], e_user[48], e_shell[48];
    char *envp[6];
    const char *s = "HOME=";
    int j = 0, i = 0;
    while (s[j]) e_home[i] = s[j], i++, j++;
    j = 0;
    while (home[j]) e_home[i] = home[j], i++, j++;
    e_home[i] = 0;
    envp[0] = e_home;
    s = "USER=";
    i = 0; j = 0;
    while (s[j]) e_user[i] = s[j], i++, j++;
    j = 0;
    while (user[j]) e_user[i] = user[j], i++, j++;
    e_user[i] = 0;
    envp[1] = e_user;
    s = "SHELL=";
    i = 0; j = 0;
    while (s[j]) e_shell[i] = s[j], i++, j++;
    j = 0;
    while (shell[j]) e_shell[i] = shell[j], i++, j++;
    e_shell[i] = 0;
    envp[2] = e_shell;
    envp[3] = (char *)"PATH=/bin:/sbin:/usr/bin:/usr/sbin";
    envp[4] = (char *)"TERM=linux";
    envp[5] = 0;

    char *av[2] = { (char *)"-sh", 0 };
    sys3(SYS_execve, (long)shell, (long)av, (long)envp);
    P("login: exec ");
    print_str(shell);
    P(": FAIL\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
