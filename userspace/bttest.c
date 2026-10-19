// bttest: E2E-проверка BT-стека (M27).
// Ждёт hci0 (создан btvhid на /dev/vhci), поднимает через HCIDEVUP,
// шлёт Read_Local_Version_Information по raw-каналу HCI-сокета.

#include "milin.h"

#define AF_BLUETOOTH 31
#define BTPROTO_HCI  1
#define SOCK_RAW     3
#define SOCK_CLOEXEC 0x80000

#define _IOW_NR(t, n, sz) (((1UL) << 30) | ((sz) << 16) | ((t) << 8) | (n))
#define _IOR_NR(t, n, sz) ((1UL << 31) | ((sz) << 16) | ((t) << 8) | (n))
#define HCIDEVUP       _IOW_NR('H', 201, 4)
#define HCIGETDEVLIST  _IOR_NR('H', 210, 4)
#define HCIGETDEVINFO  _IOR_NR('H', 211, 4)

#define HCI_EV_CMD_COMPLETE 0x0e
#define HCI_UP_FLAG 1

#define SOL_HCI     0
#define HCI_FILTER  2

struct hci_filter_k {
    u32 type_mask;
    u32 event_mask[2];
    u16 opcode;
};

struct sockaddr_hci_k {
    u16 hci_family;
    u16 hci_dev;
    u16 hci_channel;
};

struct hci_dev_req {
    u16 dev_id;
    u32 dev_opt;
};

struct hci_dev_list_req {
    u16 dev_num;
    struct hci_dev_req dev_req[1];
};

struct hci_dev_info {
    u16 dev_id;
    char name[8];
    u8  bdaddr[6];
    u32 flags;
    u8  type;
    u8  features[8];
    u32 pkt_type;
    u32 link_policy;
    u32 link_mode;
    u16 acl_mtu;
    u16 acl_pkts;
    u16 sco_mtu;
    u16 sco_pkts;
    u32 err_rx, err_tx, cmd_tx, evt_rx;
    u32 acl_tx, acl_rx, sco_tx, sco_rx;
    u32 byte_rx, byte_tx;
};

static void out(const char *s) { sys3(SYS_write, 1, (long)s, m_strlen(s)); }
static void out_hexbyte(u8 b) {
    char h[2] = { "0123456789abcdef"[b >> 4], "0123456789abcdef"[b & 0xf] };
    sys3(SYS_write, 1, (long)h, 2);
}

void milin_main(void) {
    int fd = sys5(SYS_socket, AF_BLUETOOTH, SOCK_RAW | SOCK_CLOEXEC, BTPROTO_HCI, 0, 0);
    if (fd < 0) { out("BT FAIL socket\n"); sys3(SYS_exit, 1, 0, 0); }

    // дефолтный фильтр пуст — без него raw-сокет не получает событий
    // (layout ядра: hci_ufilter u32/у32/у16, opcode=0 — без фильтра команд)
    struct hci_filter_k uf = { 0xffffffff, { 0xffffffff, 0xffffffff }, 0 };
    if (sys5(SYS_setsockopt, fd, SOL_HCI, HCI_FILTER, (long)&uf, sizeof(uf)) != 0) {
        out("BT FAIL filter\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    // ждём появления hci0 (btvhid создаёт контроллер сразу, но нода после регистрации)
    struct hci_dev_list_req dl;
    int found = 0;
    for (int i = 0; i < 40; i++) {
        m_memset(&dl, 0, sizeof(dl));
        dl.dev_num = 1;
        if (sys3(SYS_ioctl, fd, HCIGETDEVLIST, (long)&dl) == 0 &&
            dl.dev_req[0].dev_id == 0) { found = 1; break; }
        sleep_ms(50);
    }
    if (!found) { out("BT FAIL no hci0\n"); sys3(SYS_exit, 1, 0, 0); }

    // power up: инициализация стека против btvhid (Reset..Read_BD_ADDR..LE);
    // -EALREADY = ядро уже подняло hci0 само — тоже успех
    int up = 0;
    for (int i = 0; i < 40; i++) {
        long r = sys3(SYS_ioctl, fd, HCIDEVUP, 0);
        if (r == 0 || r == -114) { up = 1; break; }
        sleep_ms(100);
    }
    if (!up) { out("BT FAIL up\n"); sys3(SYS_exit, 1, 0, 0); }

    struct hci_dev_info di;
    m_memset(&di, 0, sizeof(di));
    di.dev_id = 0;
    if (sys3(SYS_ioctl, fd, HCIGETDEVINFO, (long)&di) != 0 ||
        !(di.flags & HCI_UP_FLAG)) {
        out("BT FAIL flags\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    out("bttest: hci0 UP bdaddr=");
    for (int i = 5; i >= 0; i--) {
        out_hexbyte(di.bdaddr[i]);
        if (i) out(":");
    }
    out("\n");

    // raw-канал: полный транспортный круг — команда -> vhci -> событие
    struct sockaddr_hci_k sa = { AF_BLUETOOTH, 0, 0 };
    long br = sys3(SYS_bind, fd, (long)&sa, sizeof(sa));
    out("bttest: bind r=");
    out_hexbyte((u8)(br & 0xff));
    out("\n");
    if (br != 0) {
        out("BT FAIL bind\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    u8 cmd[4] = { 0x01, 0x01, 0x10, 0x00 }; // [тип] Read_Local_Version_Information (0x1001)
    long sr = sys6(SYS_sendto, fd, (long)cmd, 4, 0, 0, 0);
    out("bttest: bind+send done, sr=");
    out_hexbyte((u8)(sr & 0xff));
    out("\n");
    if (sr != 4) {
        out("BT FAIL send r=");
        out_hexbyte((u8)(-(long)sr));
        out("\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    u8 ev[64];
    long n = 0;
    for (int i = 0; i < 50; i++) {
        n = sys6(SYS_recvfrom, fd, (long)ev, sizeof(ev), 0x40, 0, 0); // MSG_DONTWAIT
        if (n > 0) break;
        sleep_ms(100);
    }
    out("bttest: recv n=");
    out_hexbyte((u8)(n & 0xff));
    out("\n");
    if (n > 0) {
        out("bttest: ev:");
        for (int i = 0; i < 12 && i < n; i++) { out(" "); out_hexbyte(ev[i]); }
        out("\n");
    }
    // сокет отдаёт [тип-байт] + CC: ev[0]=0x04, ev[1]=код, ev[2]=plen,
    // ev[3]=ncmd, ev[4..5]=opcode, ev[6]=status, ev[7]=params
    if (n < 8 || ev[0] != 0x04 || ev[1] != HCI_EV_CMD_COMPLETE ||
        ev[4] != 0x01 || ev[5] != 0x10) {
        out("BT FAIL evt\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (ev[6] != 0) { out("BT FAIL status\n"); sys3(SYS_exit, 1, 0, 0); }

    out("BT E2E OK hci_ver=");
    out_hexbyte(ev[7]);
    out("\n");
    sys3(SYS_exit, 0, 0, 0);
}
