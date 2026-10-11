// /sbin/uiod (M4): userspace-драйвер через UIO.
// Драйверы больше не живут в ядре: ядро (uio_pci_generic) только
// перечисляет PCI-устройства и экспортирует их память через /dev/uio0.
// Вся логика — здесь: открываем ноду, мапим BAR0, читаем регистр.
//
// QEMU-железо: -device pci-testdev (vendor 1b36, device 0005).

#include "milin.h"

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/dev/uio0", O_RDWR, 0);
    if (fd < 0) {
        P("uiod: /dev/uio0: FAIL (uio_pci_generic не привязался)\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    // BAR0: смещение = индекс бара * размер страницы
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
    // M7: сервер-драйвер долгоживущий — в микроядре сервис не выходит
    // сам, он живёт до остановки супервизором (SIGTERM). Здесь просто
    // ждём событий устройства (демо читает регистр один раз).
    P("uiod: жду событий устройства (остановка — SIGTERM супервизора)\n");
    for (;;)
        sleep_ms(1000);
    __builtin_unreachable();
}
