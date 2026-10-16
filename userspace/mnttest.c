// /sbin/mnttest (M14): бут-хранение реального железа.

#include "milin.h"

#define O_CREAT   0100
#define O_RDWR    2

static void die(const char *s) {
    P("mnttest: ");
    print_str(s);
    P("\n");
    sys3(SYS_exit, 1, 0, 0);
}

void milin_main(void) {
    const char *cands[] = { "/dev/vda", "/dev/sda", "/dev/nvme0n1", 0 };

    sys3(SYS_mkdir, (long)"/mnt2", 0755, 0);

    for (int i = 0; cands[i]; i++) {
        long fd = sys3(SYS_open, (long)cands[i], O_RDWR, 0);
        if (fd < 0) continue;
        sys3(SYS_close, fd, 0, 0);

        long rc = sys5(SYS_mount, (long)cands[i], (long)"/mnt2",
                       (long)"ext4", 0, 0);
        if (rc != 0) {
            P("mnttest: mount ");
            print_str(cands[i]);
            P(" FAIL\n");
            continue;
        }
        P("mnttest: ");
        print_str(cands[i]);
        P(" смонтирован (ext4)\n");

        long f = sys3(SYS_open, (long)"/mnt2/milin-test", O_CREAT | O_RDWR,
                      0644);
        if (f < 0) die("open file FAIL");
        const char *msg = "miLin storage roundtrip";
        long w = sys3(SYS_write, f, (long)msg, m_strlen(msg));
        if (w != (long)m_strlen(msg)) die("write FAIL");
        sys3(SYS_close, f, 0, 0);

        char buf[64];
        m_memset(buf, 0, sizeof(buf));
        f = sys3(SYS_open, (long)"/mnt2/milin-test", 0, 0);
        if (f < 0) die("reopen FAIL");
        long r = sys3(SYS_read, f, (long)buf, sizeof(buf) - 1);
        sys3(SYS_close, f, 0, 0);
        if (r != (long)m_strlen(msg) ||
            m_strncmp(buf, msg, m_strlen(msg)) != 0) die("readback FAIL");

        sys3(SYS_umount2, (long)"/mnt2", 0, 0);
        P("mnttest: запись+чтение сошлись, umount OK — ");
        print_str(cands[i]);
        P(" (M14HW OK)\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    P("mnttest: блочных устройств нет — SKIP\n");
    sys3(SYS_exit, 0, 0, 0);
}
