// /sbin/persistcheck (M15): доказательство персистентности switch_root.
// Читает /var/milin/installed — маркер, записанный xbpstest'ом на реальном
// корне предыдущего бута. Свежий QEMU-процесс рождается с чистым
// initramfs: маркер может прийти только с постоянного ext4.
// order=95 (раньше xbpstest: первый бут — маркера ещё нет, это норма).

#include "milin.h"

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/var/milin/installed", O_RDONLY, 0);
    if (fd < 0) {
        P("persistcheck: маркера нет — первый бут (пишется xbpstest'ом)\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    char b[64];
    long n = sys3(SYS_read, fd, (long)b, sizeof(b) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n > 0) b[n] = 0;
    P("persistcheck: маркер с прошлого бута: ");
    print_str(b);
    P("persistcheck: установка легла на постоянный ext4 (PERSIST OK)\n");
    sys3(SYS_exit, 0, 0, 0);
}
