// /sbin/poweroff + /sbin/reboot (M17): гейт ACPI через sys_reboot.

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
    sys3(SYS_sync, 0, 0, 0);
#ifdef ML_REBOOT
    sys6(SYS_reboot, RB_MAGIC1, RB_MAGIC2, RB_AUTOBOOT, 0, 0, 0);
#else
    sys6(SYS_reboot, RB_MAGIC1, RB_MAGIC2, RB_POWER_OFF, 0, 0, 0);
#endif
    P("acpipm: sys_reboot вернулся — FAIL\n");
    sys3(SYS_exit, 1, 0, 0);
}
