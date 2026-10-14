// /sbin/poweroff + /sbin/reboot (M17): гейт ACPI через sys_reboot.
// один исходник, два бинаря: -DML_REBOOT=1 собирает перезагрузку.
// poweroff уходит в pm_power_off -> ACPI S5 (ядро печатает
// "reboot: Power down", qemu завершается сам); reboot -> machine_restart
// ("reboot: Restarting system", при -no-reboot qemu выходит).
// если sys_reboot вернулся — ACPI-пути нет, гейт провален.

#include "milin.h"

#define SYS_reboot   169
#define SYS_sync     162
#define RB_MAGIC1    0xfee1dead
#define RB_MAGIC2    672274793
#define RB_AUTOBOOT  0x01234567
#define RB_POWER_OFF 0x4321fedc

void milin_main(void) {
#ifdef ML_REBOOT
    P("acpipm: reboot oneshot\n");
#else
    P("acpipm: poweroff oneshot (S5)\n");
#endif
    // sys_reboot не синхронизирует — без sync хвост page cache теряется
    sys3(SYS_sync, 0, 0, 0);
#ifdef ML_REBOOT
    sys6(SYS_reboot, RB_MAGIC1, RB_MAGIC2, RB_AUTOBOOT, 0, 0, 0);
#else
    sys6(SYS_reboot, RB_MAGIC1, RB_MAGIC2, RB_POWER_OFF, 0, 0, 0);
#endif
    P("acpipm: sys_reboot вернулся — FAIL\n");
    sys3(SYS_exit, 1, 0, 0);
}
