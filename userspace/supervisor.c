// /init MiLin (M4): супервизор серверов.
// Больше не "hello world": монтирует devtmpfs, порождает серверы
// (execve), затем запускает непривилегированного клиента, который
// читает файл через FUSE-сервер. Готовность системы = все серверы
// отработали и клиент получил данные.
//
//   супервизор  /init       — порождает и следит за серверами
//   драйвер     /sbin/uiod  — UIO: userspace-драйвер PCI-устройства
//   фс-сервер   /sbin/fused — FUSE: файловая система в userspace
//   клиент      fork(setuid 1000) — доказательство MULTIUSER + FUSE

#include "milin.h"

static struct stat64 {
    u64 dev, ino, nlink;
    u32 mode, uid, gid;
    i32 pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    u64 unused[3];
} st_buf;

static int run_client(void) {
    // непривилегированный пользователь: MULTIUSER вернулся
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("client: setuid(1000): FAIL\n");
        return 1;
    }
    P("client: uid=");
    print_dec(sys3(SYS_getuid, 0, 0, 0));
    P(" (был 0 → MULTIUSER работает)\n");

    // stat через FUSE: пока fused не смонтировал /mnt, системный вызов
    // честно блокируется в ядре — супервизору не нужен сон
    for (int tries = 0; ; tries++) {
        if (sys3(SYS_stat, (long)"/mnt/README", (long)&st_buf, 0) == 0)
            break;
        if (tries > 100) {
            P("client: stat /mnt/README: FAIL (сервер не ответил)\n");
            return 1;
        }
        sleep_ms(20);
    }
    P("client: /mnt/README: size=");
    print_dec((long)st_buf.size);
    P("client: /mnt/README: mode=0");
    print_oct(st_buf.mode);
    P("client: непривилегированное чтение через FUSE-сервер: OK\n");
    return 0;
}

void milin_main(void) {
    P("MiLin supervisor: порождаю серверы\n");

    // devtmpfs: /dev/fuse и /dev/uio0 появляются сами (UIO берёт
    // динамический major, узнаваемый только через devtmpfs)
    if (sys5(SYS_mount, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0) == 0) {
        P("supervisor: devtmpfs смонтирован на /dev\n");
    } else {
        // запасной путь: /dev/fuse — misc 10:229 (фиксированный minor)
        sys3(SYS_mknod, (long)"/dev/fuse", 0600 | 0020000, (10 << 8) | 229);
        P("supervisor: devtmpfs нет, /dev/fuse создан mknod\n");
    }

    // Сборка системы супервизором: uio_pci_generic сознательно не имеет
    // таблицы device-id — супервизор сам решает, какое железо отдаётся
    // userspace-драйверу (1b36:0005 = qemu pci-testdev).
    // sysfs монтируем как discovery-интерфейс серверов.
    sys3(SYS_mkdir, (long)"/sys", 0755, 0);
    long sysfs_ok = sys5(SYS_mount, (long)"sysfs", (long)"/sys", (long)"sysfs", 0, 0);
    if (sysfs_ok == 0) P("supervisor: sysfs смонтирован\n");

    long bfd = sys3(SYS_open, (long)"/sys/bus/pci/drivers/uio_pci_generic/new_id", 1 /*O_WRONLY*/, 0);
    if (bfd >= 0) {
        sys3(SYS_write, bfd, (long)"1b36 0005\n", 10);
        sys3(SYS_close, bfd, 0, 0);
        P("supervisor: pci-testdev отдан драйверу uio_pci_generic\n");
    } else {
        P("supervisor: sysfs недоступен, UIO-привязка невозможна\n");
    }

    // --- сервер драйверов: UIO ---
    int pid_u = sys3(SYS_fork, 0, 0, 0);
    if (pid_u == 0) {
        char *argv[] = { "uiod", 0 };
        if (sys3(SYS_execve, (long)"/sbin/uiod", (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }

    // --- сервер файловых систем: FUSE ---
    int pid_f = sys3(SYS_fork, 0, 0, 0);
    if (pid_f == 0) {
        char *argv[] = { "fused", 0 };
        if (sys3(SYS_execve, (long)"/sbin/fused", (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }

    // --- клиент: другой uid, читает файл с FUSE-сервера ---
    int pid_c = sys3(SYS_fork, 0, 0, 0);
    if (pid_c == 0)
        sys3(SYS_exit, (long)run_client(), 0, 0);

    // --- супервизор собирает статусы ---
    int ok = 1;
    int status = 0;
    long r;

    r = sys5(SYS_wait4, pid_u, (long)&status, 0, 0, 0);
    P("supervisor: uiod (pid ");
    print_dec(r);
    P("): ");
    if ((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0) { P("OK\n"); } else { P("FAIL\n"); ok = 0; }

    r = sys5(SYS_wait4, pid_c, (long)&status, 0, 0, 0);
    P("supervisor: client (pid ");
    print_dec(r);
    P("): ");
    if ((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0) { P("OK\n"); } else { P("FAIL\n"); ok = 0; }

    // graceful shutdown: размонтирование закрывает соединение,
    // fused видит EOF и выходит сам — как положено серверу
    sys3(SYS_umount2, (long)"/mnt", MNT_DETACH, 0);
    sys5(SYS_wait4, pid_f, (long)&status, 0, 0, 0);
    P("supervisor: fused (pid ");
    print_dec(pid_f);
    P("): завершён (umount -> EOF)\n");

    if (ok) {
        P("M4 OK: супервизор + uiod + fused + непривилегированный клиент\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    P("M4 FAILED\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
