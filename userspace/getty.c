// /sbin/getty (M19): подсказка входа на консоли.
// открывает /dev/console, забирает управляющий tty, цикл fork -> /sbin/login;
// умерший getty супервизор перезапускает сам (M7 backoff).

#include "milin.h"

#define TIOCSCTTY 0x540E

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

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/dev/console", O_RDWR, 0);
    if (fd < 0) {
        P("getty: /dev/console: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    sys3(SYS_setsid, 0, 0, 0);
    sys3(SYS_ioctl, fd, TIOCSCTTY, 0);
    sys3(SYS_dup2, fd, 0, 0);
    sys3(SYS_dup2, fd, 1, 0);
    sys3(SYS_dup2, fd, 2, 0);
    if (fd > 2) sys3(SYS_close, fd, 0, 0);
    for (;;) {
        P("\n\nmilini 7.2.9-milin console\n\nlogin: ");
        char user[32];
        if (read_line(user, sizeof(user)) < 0) sys3(SYS_exit, 0, 0, 0);
        if (!user[0]) continue;
        int pid = sys3(SYS_fork, 0, 0, 0);
        if (pid == 0) {
            char *av[3] = { (char *)"login", user, 0 };
            sys3(SYS_execve, (long)"/sbin/login", (long)av, 0);
            P("getty: /sbin/login: FAIL\n");
            sys3(SYS_exit, 127, 0, 0);
        }
        int status = 0;
        sys5(SYS_wait4, pid, (long)&status, 0, 0, 0);
    }
}
