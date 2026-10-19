// btvhid: виртуальный BT-контроллер на /dev/vhci (M27).
// Создаёт primary-контроллер и отвечает на init-команды стека HCI.

#include "milin.h"

#define HCI_COMMAND_PKT 0x01
#define HCI_EVENT_PKT   0x04
#define HCI_VENDOR_PKT  0xff

#define HCI_EV_CMD_COMPLETE 0x0e

#define OPCODE(op) ((op) & 0xff), (((op) >> 8) & 0xff)

static int fd;

static void out(const char *s) { sys3(SYS_write, 1, (long)s, m_strlen(s)); }

static void send_cc(u16 op, const u8 *params, u8 n) {
    u8 ev[280];
    u8 i = 0;
    ev[i++] = HCI_EVENT_PKT;
    ev[i++] = HCI_EV_CMD_COMPLETE;
    ev[i++] = 4 + n;              // ncmd + opcode + status + params
    ev[i++] = 1;                  // ncmd
    ev[i++] = op & 0xff;
    ev[i++] = (op >> 8) & 0xff;
    ev[i++] = 0;                  // status OK
    for (u8 k = 0; k < n; k++) ev[i++] = params[k];
    sys3(SYS_write, fd, (long)ev, i);
}

static void cc_none(u16 op)      { send_cc(op, 0, 0); }
static void cc_byte(u16 op, u8 b){ send_cc(op, &b, 1); }

static void cc_bufsize(u16 op) {
    // acl_mtu(2), sco_mtu(1), acl_max(2), sco_max(2)
    u8 p[7] = { 0x00, 0x04, 0, 8, 0, 0, 0 };
    send_cc(op, p, 7);
}

static void cc_le_bufsize(u16 op) {
    // le_mtu(2), le_max(1)
    u8 p[3] = { 0x00, 0x04, 8 };
    send_cc(op, p, 3);
}

static void cc_version(u16 op) {
    // status уже в ev; параметры: hci_ver, hci_rev(2), lmp_ver, mfg(2), lmp_sub(2)
    u8 p[8] = { 0x0b, 0x00, 0x00, 0x0b, 0xff, 0xff, 0x00, 0x00 };
    send_cc(op, p, 8);
}

static void cc_features(u16 op) {
    // LE-бит (byte4 bit6) — включает LE-ветку init
    u8 p[8] = { 0, 0, 0, 0, 0x40, 0, 0, 0 };
    send_cc(op, p, 8);
}

static void cc_extfeat(u16 op) {
    // page(1) + max_page(1) + features[8]
    u8 p[10] = { 0, 0, 0, 0, 0, 0, 0x40, 0, 0, 0 };
    send_cc(op, p, 10);
}

static void cc_class(u16 op) {
    // dev_class[3] — «компьютер/настольная рабочая станция»
    u8 p[3] = { 0x04, 0x01, 0x1c };
    send_cc(op, p, 3);
}

static void cc_name(u16 op) {
    // name[248]
    u8 p[248];
    m_memset(p, 0, 248);
    const char *nm = "MiLin-VR";
    for (int i = 0; nm[i]; i++) p[i] = nm[i];
    send_cc(op, p, 248);
}

static void cc_bdaddr(u16 op) {
    // little-endian: 02:11:22:33:44:55
    u8 p[6] = { 0x55, 0x44, 0x33, 0x22, 0x11, 0x02 };
    send_cc(op, p, 6);
}

static void cc_cmds(u16 op) {
    u8 p[64];
    m_memset(p, 0, 64);
    send_cc(op, p, 64);
}

static void cc_le_states(u16 op) {
    // le_states[8] — нули (состояния не заявляем)
    u8 p[8];
    m_memset(p, 0, 8);
    send_cc(op, p, 8);
}

static void handle(u16 op) {
    if (op == 0x1001) out("btvhid: cmd 0x1001\n");
    switch (op) {
    case 0x0c03: cc_none(op); break;              // Reset
    case 0x0c01: cc_none(op); break;              // Set_Event_Mask
    case 0x0c23: cc_class(op); break;             // Read_Class_Of_Dev
    case 0x0c14: cc_name(op); break;              // Read_Local_Name
    case 0x1001: cc_version(op); break;           // Read_Local_Version_Information
    case 0x1003: cc_features(op); break;          // Read_Local_Features
    case 0x1009: cc_bdaddr(op); break;            // Read_BD_ADDR
    case 0x1002: cc_cmds(op); break;              // Read_Local_Supported_Commands
    case 0x1004: cc_extfeat(op); break;           // Read_Local_Extended_Features
    case 0x1005: cc_bufsize(op); break;           // Read_Buffer_Size
    case 0x2001: cc_none(op); break;              // LE_Set_Event_Mask
    case 0x2002: cc_le_bufsize(op); break;        // LE_Read_Buffer_Size
    case 0x2003: cc_features(op); break;          // LE_Read_Local_Supported_Features
    case 0x201c: cc_le_states(op); break;         // LE_Read_Supported_States
    default:    cc_byte(op, 0x01); break;         // Unknown Command
    }
}

void milin_main(void) {
    out("btvhid: старт\n");
    fd = sys3(SYS_open, (long)"/dev/vhci", O_RDWR, 0);
    if (fd < 0) { out("btvhid: /dev/vhci FAIL\n"); sys3(SYS_exit, 1, 0, 0); }

    // создать primary-контроллер (vendor packet: 0xff + opcode 0x00)
    u8 create[2] = { HCI_VENDOR_PKT, 0x00 };
    if (sys3(SYS_write, fd, (long)create, 2) != 2) {
        out("btvhid: create FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    // ответ ядра: {0xff, opcode, id_le16}
    u8 hello[8];
    long n = sys3(SYS_read, fd, (long)hello, sizeof(hello));
    if (n < 4 || hello[0] != HCI_VENDOR_PKT) {
        out("btvhid: handshake FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    out("btvhid: hci контроллер создан\n");

    u8 buf[300];
    for (;;) {
        long r = sys3(SYS_read, fd, (long)buf, sizeof(buf));
        if (r < 4 || buf[0] != HCI_COMMAND_PKT) continue;
        u16 op = buf[1] | (buf[2] << 8);
        handle(op);
    }
}
