// /init MiLin (M5): супервизор серверов + микроядерный IPC.
//
// M4: порождает серверы (execve), отдаёт им железо (UIO) и ФС (FUSE),
//     непривилегированный клиент читает файл с FUSE-сервера.
// M5: серверы больше не сироты — у них есть шина (mlbus, идея mbus
//     из Managarm). Клиент получает lane-хэндл через реестр и пишет
//     в log-сервер сообщениями bragi-формата. Хэндлы = fd, передача
//     хэндлов = SCM_RIGHTS, транспорт = AF_UNIX SOCK_SEQPACKET.
//
//   супервизор  /init        — порождает и следит за серверами
//   реестр      /sbin/mlbus  — именованные lane'и (mbus)
//   log-сервер  /sbin/logsrv — сервис, принимающий сообщения
//   драйвер     /sbin/uiod   — UIO: userspace-драйвер PCI-устройства
//   фс-сервер   /sbin/fused  — FUSE: файловая система в userspace
//   клиент      fork(setuid 1000) — IPC-демо + доказательство M4

#include "milin.h"
#include "ipc.h"

static struct stat64 {
    u64 dev, ino, nlink;
    u32 mode, uid, gid;
    i32 pad0;
    u64 rdev, size, blksize, blocks;
    u64 atime, atime_nsec, mtime, mtime_nsec, ctime, ctime_nsec;
    u64 unused[3];
} st_buf;

static int ok_status(int status) {
    // вышел с 0 — норма; убит SIGTERM супервизором — тоже норма:
    // так супервизор останавливает долгоживущие серверы
    if ((status & 0x7f) == 0 && ((status >> 8) & 0xff) == 0) return 1;
    if ((status & 0x7f) == SIGTERM) return 1;
    return 0;
}

static int run_client(void) {
    // непривилегированный пользователь: MULTIUSER вернулся
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("client: setuid(1000): FAIL\n");
        return 1;
    }
    P("client: uid=");
    print_dec(sys3(SYS_getuid, 0, 0, 0));
    P(" (был 0 → MULTIUSER работает)\n");

    // --- M5: IPC через реестр ---
    long bus = -1;
    for (int tries = 0; ; tries++) {
        bus = ml_connect("milin-bus");
        if (bus >= 0) break;
        if (tries > 200) {
            P("client: IPC: шина не отвечает\n");
            return 1;
        }
        sleep_ms(10);
    }

    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "log", 4);
    struct ml_msg resp;
    int lane_hooks[ML_MAXHOOKS], lane_nh = 0;
    int found = 0;
    for (int tries = 0; tries < 100; tries++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) break;
        lane_nh = 0;
        if (ml_recv(bus, &resp, 0, 0, lane_hooks, &lane_nh) <= 0) break;
        if (resp.err == ML_E_OK && lane_nh >= 1) { found = 1; break; }
        sleep_ms(20); // сервер ещё регистрируется
    }
    if (!found) {
        P("client: IPC: lookup \"log\": FAIL\n");
        return 1;
    }
    P("client: IPC: lookup \"log\" -> lane получен (fd пришёл SCM_RIGHTS)\n");
    sys3(SYS_close, bus, 0, 0); // шина больше не нужна: общаемся напрямую

    long lane = lane_hooks[0];
    const char *m1 = "привет из MiLin: IPC в стиле bragi/mbus";
    const char *m2 = "серверы общаются сообщениями, а не вызовами ядра";
    // длина строки, а не sizeof(указатель) — классическая ловушка nostdlib
    for (int i = 0; i < 2; i++) {
        const char *t = i == 0 ? m1 : m2;
        struct ml_msg w = { ML_PROTO_LOG, LOG_WRITE, 0, 0, {0} };
        if (ml_send(lane, &w, t, m_strlen(t), 0, 0) < 0) {
            P("client: IPC: WRITE: FAIL\n");
            return 1;
        }
        struct ml_msg wr;
        if (ml_recv(lane, &wr, 0, 0, 0, 0) <= 0 ||
            wr.msg_id != LOG_WRITE_R || wr.err != ML_E_OK) {
            P("client: IPC: ack не пришёл\n");
            return 1;
        }
    }
    struct ml_msg s = { ML_PROTO_LOG, LOG_STATS, 0, 0, {0} };
    ml_send(lane, &s, 0, 0, 0, 0);
    struct ml_msg sr;
    if (ml_recv(lane, &sr, 0, 0, 0, 0) <= 0 || *ml_h(&sr, 0) != 2) {
        P("client: IPC: STATS: FAIL\n");
        return 1;
    }
    P("client: IPC: 2 WRITE-ack + STATS served=2: OK\n");
    sys3(SYS_close, lane, 0, 0);

    // stat через FUSE: пока fused не смонтировал /mnt, системный вызов
    // честно блокируется в ядре — супервизору не нужен сон (регрессия M4)
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
    P("client: IPC-демо + непривилегированное чтение через FUSE: OK\n");
    return 0;
}

static int spawn(const char *path, const char *name) {
    int pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        char *argv[] = { (char *)name, 0 };
        if (sys3(SYS_execve, (long)path, (long)argv, 0) != 0)
            sys3(SYS_exit, 127, 0, 0);
    }
    return pid;
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

    // --- шина: реестр lane'ов (M5, идея mbus) ---
    int pid_b = spawn("/sbin/mlbus", "mlbus");
    // --- log-сервер: регистрируется в шине ---
    int pid_l = spawn("/sbin/logsrv", "logsrv");
    // --- сервер драйверов: UIO (M4) ---
    int pid_u = spawn("/sbin/uiod", "uiod");
    // --- сервер файловых систем: FUSE (M4) ---
    int pid_f = spawn("/sbin/fused", "fused");

    // --- клиент: другой uid, IPC-демо + чтение с FUSE-сервера ---
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
    if (ok_status(status)) { P("OK\n"); } else { P("FAIL\n"); ok = 0; }

    r = sys5(SYS_wait4, pid_c, (long)&status, 0, 0, 0);
    P("supervisor: client (pid ");
    print_dec(r);
    P("): ");
    if (ok_status(status)) { P("OK\n"); } else { P("FAIL\n"); ok = 0; }

    // graceful shutdown: размонтирование закрывает соединение,
    // fused видит EOF и выходит сам; mlbus и logsrv — долгоживущие
    // сервисы, их останавливает супервизор (SIGTERM), как в микроядре
    sys3(SYS_umount2, (long)"/mnt", MNT_DETACH, 0);
    sys5(SYS_wait4, pid_f, (long)&status, 0, 0, 0);
    P("supervisor: fused (pid ");
    print_dec(pid_f);
    P("): завершён (umount -> EOF)\n");

    sys3(SYS_kill, pid_b, SIGTERM, 0);
    sys3(SYS_kill, pid_l, SIGTERM, 0);
    r = sys5(SYS_wait4, pid_b, (long)&status, 0, 0, 0);
    P("supervisor: mlbus (pid ");
    print_dec(r);
    P("): остановлен супервизором\n");
    if (!ok_status(status)) ok = 0;
    r = sys5(SYS_wait4, pid_l, (long)&status, 0, 0, 0);
    P("supervisor: logsrv (pid ");
    print_dec(r);
    P("): остановлен супервизором\n");
    if (!ok_status(status)) ok = 0;

    if (ok) {
        P("M5 OK: mbus-реестр + lane-хэндлы + bragi-формат + FUSE-регрессия\n");
        sys3(SYS_exit, 0, 0, 0);
    }
    P("M5 FAILED\n");
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
