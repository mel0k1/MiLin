// last (M21): читает /var/log/wtmp (glibc-формат utmp), печатает USER_PROCESS/DEAD_PROCESS.

#include "milin.h"
#include "utmp.h"

void milin_main(void) {
    const char *path = "/var/log/wtmp";
    long fd = sys3(SYS_open, (long)path, O_RDONLY, 0);
    if (fd < 0) {
        path = "/var/run/utmp";
        fd = sys3(SYS_open, (long)path, O_RDONLY, 0);
    }
    if (fd < 0) {
        P("last: wtmp/utmp нет\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    long n = 0;
    int users = 0;
    static struct utmp_rec r;
    for (;;) {
        long g = sys3(SYS_read, fd, (long)&r, 384);
        if (g < 384) break;
        n++;
        if (r.type != UTMP_USER && r.type != UTMP_DEAD) continue;
        print_dec_n(r.type);
        P(" ");
        print_str(r.user[0] ? r.user : "-");
        P(" ");
        print_str(r.line);
        P(" ");
        print_dec_n(r.tv_sec);
        P("\n");
        if (r.type == UTMP_USER && r.user[0]) users++;
    }
    sys3(SYS_close, fd, 0, 0);
    if (!users) {
        P("last: USER_PROCESS нет\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("M21 WTMP OK n=");
    print_dec_n(users);
    P("\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
