// /sbin/uiod (M4): userspace-драйвер через UIO.

#include "milin.h"

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/dev/uio0", O_RDWR, 0);
    if (fd < 0) {
        P("uiod: /dev/uio0: FAIL (uio_pci_generic не привязался)\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    long map = sys6(SYS_mmap, 0, 4096, PROT_READ | 2 /*PROT_WRITE*/, MAP_SHARED, fd, 0);
    if (map < 0) {
        P("uiod: mmap BAR0: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    volatile u32 *bar0 = (volatile u32 *)map;
    u32 v = bar0[0];

    P("uiod: /dev/uio0 открыт, BAR0 замаплен\n");
    P("uiod: BAR0[0] = ");
    print_hex(v);
    P("uiod: userspace-драйвер говорит с PCI-устройством через UIO: OK\n");
    P("uiod: жду событий устройства (остановка — SIGTERM супервизора)\n");
    for (;;)
        sleep_ms(1000);
    __builtin_unreachable();
}
