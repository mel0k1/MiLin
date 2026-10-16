// who: сессии из /var/run/utmp.

#include "milin.h"
#include "utmp.h"

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/var/run/utmp", O_RDONLY, 0);
    if (fd < 0) {
        P("who: utmp нет\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    static struct utmp_rec r;
    int n = 0;
    for (;;) {
        long g = sys3(SYS_read, fd, (long)&r, 384);
        if (g < 384) break;
        if (r.type != UTMP_USER) continue;
        print_str(r.user);
        P(" ");
        print_str(r.line);
        P(" ");
        print_dec_n(r.tv_sec);
        P("\n");
        n++;
    }
    sys3(SYS_close, fd, 0, 0);
    if (!n) {
        P("who: сессий нет\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("M23 WHO OK n=");
    print_dec_n(n);
    P("\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
