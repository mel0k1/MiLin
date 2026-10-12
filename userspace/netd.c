// /sbin/netd (M12): userspace-сетевой стек — NIC-драйвер + TCP/IP + сервис.
//
// Архитектура микроядра: в ядре нет ни стека, ни драйвера NIC. netd —
// userspace-сервер: e1000 через UIO (BAR0 в mmap, кольца/буферы в
// pinned-DMA регионе /dev/mlpinned — NIC обязан работать с физикой),
// поверх — ARP/IPv4/ICMP/TCP и сервис "net0" (ML_PROTO_NET) на шине:
// клиенты открывают сокеты, соединяются, шлют данные — всё lane'ами.
// QEMU: -device e1000,netdev=n0 -netdev user,id=n0 (10.0.2.15 <- slirp).
// M15: UDP-слой и DHCP-клиент (RFC 2131) — адресация больше не зашита:
// DISCOVER/OFFER/REQUEST/ACK, аренда с T1-renew, DNS-опция уезжает в
// /etc/resolv.conf (resolv.conf читают клиенты: netsh/xbps). Нет сервера
// в сети — fallback на slirp-статику, DHCP продолжает пробовать в фоне.

#include "milin.h"
#include "ipc.h"

#define SYS_ioctl        16
#define O_RDWR           2
#define O_WRONLY         1
#define O_RDONLY         0
#define O_CREAT          0100
#define O_TRUNC          01000

// --- адресация: DHCP-аренда, до неё 0 (fallback — slirp-статика) ---
#define ETH_HDR     14
#define ETH_MTU     1500
static u32 my_ip, my_mask, my_gw;
static u32 dns1, dns2;
static u8 addr_set;                        // адрес сконфигурирован (DHCP|fallback)
#define NET_MY_IP   my_ip                  // старые места читают переменные
#define NET_MASK    my_mask
#define NET_GW_IP   my_gw

// --- e1000 MMIO ---
#define E_CTRL   0x0000
#define E_RCTL   0x0100
#define E_TCTL   0x0400
#define E_RDBA   0x2800
#define E_RDLEN  0x2808
#define E_RDH    0x2810
#define E_RDT    0x2818
#define E_TDBA   0x3800
#define E_TDLEN  0x3808
#define E_TDT    0x3818
#define E_TDH    0x3810
#define E_STATUS 0x0008
#define E_RAL    0x5400
#define E_RAH    0x5404

#define NDESC      128
#define RBUFSZ     2048

#define OFF_TXRING (2u << 20)          // +2МБ: изоляция от адрес-зависимых странностей
#define OFF_RXRING (2u << 20) + 4096
#define OFF_TXBUF  8192
#define OFF_RXBUF  (8192 + NDESC * RBUFSZ)

// --- TCP ---
#define MSS        1460
#define RBUF_MAX   (32 * 1024)
#define SNDBUF_MAX (16 * 1024)
#define TSEG_MAX   24
#define NSOCK      16
#define SS_FREE    0
#define SS_ALLOC   1
#define SS_SYN     2
#define SS_EST     3
#define SS_FINW    4

#define F_FIN  0x01
#define F_SYN  0x02
#define F_RST  0x04
#define F_ACK  0x10

struct tseg { u32 seq, len, flags, t0, tries, off; u8 sent; };

struct sock {
    u8 state, ln, want_close, fin_queued, eof;
    u32 rip;
    u16 rport, lport;
    u32 snd_una, snd_nxt, rcv_nxt, iss;
    u16 snd_win;
    u8 rxb[RBUF_MAX];
    u32 rx_len;
    u8 snd[SNDBUF_MAX];
    u32 snd_len;
    struct tseg q[TSEG_MAX];
    u32 qn;
};

static volatile u32 *mmio;
static u8 *dma;
static u64 dma_phys;
static u8 mac[6];
static u32 rx_cur, tx_cur;

static struct sock S[NSOCK];
static u32 next_port = 40000, ip_id = 1;

#define NLANE 4
struct lane { long fd; u32 rights; };
static struct lane L[NLANE];
static long busfd = -1;

static u32 now_t;

static u16 rbe16(const u8 *p) { return (u16)((p[0] << 8) | p[1]); }
static u32 rbe32(const u8 *p) {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}
static void wbe16(u8 *p, u16 v) { p[0] = (u8)(v >> 8); p[1] = (u8)v; }
static void wbe32(u8 *p, u32 v) {
    p[0] = (u8)(v >> 24); p[1] = (u8)(v >> 16); p[2] = (u8)(v >> 8); p[3] = (u8)v;
}
static void arp_input(const u8 *p, u32 n);
static void ip_input(const u8 *f, u32 n);
static void udp_input(const u8 *ip, const u8 *u, u32 n);
static void dhcp_tick(void);
static u8 dhcp_st;
static u32 dhcp_yi;
static u8 *tx_frame(void);
static int nic_tx_commit(u32 len);
static void nic_poll(void);

// --- NIC-слой (M15): e1000 / e1000e / r8169 за одним интерфеймом ---
struct nic_ops {
    const char *name;
    u8 * (*txf)(void);
    int  (*txc)(u32 len);
    void (*poll)(void);
    int  (*init)(long uiofd);
};
static struct nic_ops *nic;

// RFC 1071: бегущая сумма (без финального дополнения)
static u32 csum_run(const u8 *b, u32 n, u32 sum) {
    for (u32 i = 0; i + 1 < n; i += 2)
        sum += (u32)((b[i] << 8) | b[i + 1]);
    if (n & 1) sum += (u32)(b[n - 1] << 8);
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return sum & 0xFFFF;
}
static u16 csum_fin(u32 sum) {
    while (sum >> 16) sum = (sum & 0xFFFF) + (sum >> 16);
    return (u16)(~sum & 0xFFFF);
}

// --- e1000 ---
static u32 er(u32 reg) { return mmio[reg >> 2]; }
static void ew(u32 reg, u32 v) { mmio[reg >> 2] = v; }

// legacy TX-дескриптор: addr(8,LE) len(2) cso(1) cmd(1) status(1) css(1) special(2)
// legacy RX-дескриптор: addr(8,LE) len(2,от hw) csum(2) status(1) errors(1) special(2)
static volatile u8 *txd(int i) { return dma + OFF_TXRING + (u32)i * 16; }
static volatile u8 *rxd(int i) { return dma + OFF_RXRING + (u32)i * 16; }
static u32 txbuf(int i) { return OFF_TXBUF + (u32)i * RBUFSZ; }
static u32 rxbuf(int i) { return OFF_RXBUF + (u32)i * RBUFSZ; }

// буфер кадра текущего TX-дескриптора
static u8 *e1000_txf(void) { return dma + txbuf((int)tx_cur); }

static int e1000_txc(u32 len) {
    volatile u8 *d = txd((int)tx_cur);
    d[8] = (u8)(len & 0xFF); d[9] = (u8)(len >> 8);   // length (LE)
    d[10] = 0;                                         // cso
    d[11] = 0x01 | 0x08;                               // cmd: EOP | RS
    d[12] = 0;                                         // status: ждём DD
    d[13] = 0; d[14] = 0; d[15] = 0;
    ew(E_TDT, (tx_cur + 1) % NDESC);
    for (int w = 0; w < 3000; w++) {
        if (d[12] & 0x01) {                            // DD
            tx_cur = (tx_cur + 1) % NDESC;
            return 0;
        }
        sleep_ms(1);
    }
    P("netd: TX DD timeout len="); print_dec_n((long)len); P("\n");
    return -1;
}

static void e1000_poll(void) {
    for (int n = 0; n < NDESC; n++) {
        volatile u8 *d = rxd((int)rx_cur);
        if (!(d[12] & 0x01)) break;                    // DD
        u32 len = (u32)((u32)d[8] | ((u32)d[9] << 8));
        if (len >= ETH_HDR + 20 && !(d[13] & 0x80)) {  // errors.bit7 = RXE
            const u8 *f = dma + rxbuf((int)rx_cur);
            u16 et = rbe16(f + 12);
            if (et == 0x0806) arp_input(f + ETH_HDR, len - ETH_HDR);
            else if (et == 0x0800) ip_input(f, len);
        }
        d[12] = 0;                                     // вернули буфер
        ew(E_RDT, rx_cur);
        rx_cur = (rx_cur + 1) % NDESC;
    }
}

// --- ARP ---
static u32 ac_ip; static u8 ac_mac[6]; static u8 ac_ok; static u32 ac_t0;

static int arp_get(u32 ip, u8 *out) {
    if (!addr_set) return -1;
    if (ac_ok && ac_ip == ip) { m_memcpy(out, ac_mac, 6); return 0; }
    if (now_t - ac_t0 >= 4) {
        ac_t0 = now_t;
        u8 *f = tx_frame();
        m_memset(f, 0xFF, 6);
        m_memcpy(f + 6, mac, 6);
        wbe16(f + 12, 0x0806);
        u8 *a = f + ETH_HDR;
        wbe16(a, 1); wbe16(a + 2, 0x0800);
        a[4] = 6; a[5] = 4;
        wbe16(a + 6, 1);                               // request
        m_memcpy(a + 8, mac, 6);
        wbe32(a + 14, NET_MY_IP);
        m_memset(a + 18, 0, 6);
        wbe32(a + 24, ip);
        nic_tx_commit((u32)(ETH_HDR + 28));
    }
    return -1;
}

static void arp_input(const u8 *p, u32 n) {
    if (n < 28) return;
    u16 oper = rbe16(p + 6);
    u32 spa = rbe32(p + 14);
    // кэш только при сконфигурированном адресе: до DHCP маска 0, и
    // первая же ARP-попутка с любым spa попала бы в кэш шлюза
    if (spa && addr_set && (spa & NET_MASK) == (NET_MY_IP & NET_MASK)) {
        ac_ip = spa; m_memcpy(ac_mac, p + 8, 6); ac_ok = 1;
    }
    if (oper == 1 && addr_set && rbe32(p + 24) == NET_MY_IP) {
        u8 *f = tx_frame();
        m_memcpy(f, p + 8, 6);
        m_memcpy(f + 6, mac, 6);
        wbe16(f + 12, 0x0806);
        u8 *a = f + ETH_HDR;
        wbe16(a, 1); wbe16(a + 2, 0x0800);
        a[4] = 6; a[5] = 4;
        wbe16(a + 6, 2);                               // reply
        m_memcpy(a + 8, mac, 6);
        wbe32(a + 14, NET_MY_IP);
        m_memcpy(a + 18, p + 8, 6);
        wbe32(a + 24, spa);
        nic_tx_commit((u32)(ETH_HDR + 28));
    }
}

// next-hop: адрес в своём сабнете — напрямую, иначе через шлюз (M14:
// без этого HTTPS-репо за пределами 10.0.2.0/24 недостижимы)
// src отдельным параметром: DHCP до бинда шлёт с 0.0.0.0
static int ip_tx_from(u32 src, u32 dst, u8 proto, const u8 *payload, u32 plen) {
    u8 macdst[6];
    if (dst == 0xFFFFFFFFu) {
        m_memset(macdst, 0xFF, 6);                 // limited broadcast: мимо ARP
    } else {
        u32 nh = ((dst & NET_MASK) == (src & NET_MASK)) ? dst : NET_GW_IP;
        if (arp_get(nh, macdst) < 0) return -1;
    }
    if (plen > ETH_MTU - 20) return -1;
    u8 *f = tx_frame();
    m_memcpy(f, macdst, 6);
    m_memcpy(f + 6, mac, 6);
    wbe16(f + 12, 0x0800);
    u8 *ip = f + ETH_HDR;
    ip[0] = 0x45; ip[1] = 0;
    wbe16(ip + 2, (u16)(20 + plen));
    wbe16(ip + 4, (u16)(ip_id++ & 0xFFFF));
    wbe16(ip + 6, 0x4000);
    ip[8] = 64; ip[9] = proto;
    wbe32(ip + 12, src);
    wbe32(ip + 16, dst);
    wbe16(ip + 10, 0);
    u32 s = csum_run(ip, 20, 0);
    wbe16(ip + 10, csum_fin(s));
    m_memcpy(ip + 20, payload, plen);
    return nic_tx_commit((u32)(ETH_HDR + 20 + plen));
}

static int ip_tx(u32 dst, u8 proto, const u8 *payload, u32 plen) {
    return ip_tx_from(NET_MY_IP, dst, proto, payload, plen);
}

// --- ICMP echo reply ---
static void icmp_input(const u8 *ip, u32 n) {
    u32 ihl = (u32)(ip[0] & 0x0F) * 4;
    if (n < ihl + 8) return;
    u32 src = rbe32(ip + 12);
    const u8 *ic = ip + ihl;
    u32 plen = n - ihl;
    if (ic[0] != 8) return;                            // только echo request
    u8 macdst[6];
    if (arp_get(src, macdst) < 0) return;
    u8 *f = tx_frame();
    m_memcpy(f, macdst, 6);
    m_memcpy(f + 6, mac, 6);
    wbe16(f + 12, 0x0800);
    u8 *o = f + ETH_HDR;
    o[0] = 0x45; o[1] = 0;
    wbe16(o + 2, (u16)(20 + plen));
    wbe16(o + 4, (u16)(ip_id++ & 0xFFFF));
    wbe16(o + 6, 0);
    o[8] = 64; o[9] = 1;
    wbe32(o + 12, NET_MY_IP);
    wbe32(o + 16, src);
    wbe16(o + 10, 0);
    wbe16(o + 10, csum_fin(csum_run(o, 20, 0)));
    m_memcpy(o + 20, ic, plen);
    o[20] = 0;                                         // echo reply
    wbe16(o + 22, csum_fin(csum_run(o + 20, plen, 0)));
    nic_tx_commit((u32)(ETH_HDR + 20 + plen));
}

// --- UDP + DHCP (M15, RFC 2131) ---
#define PROT_UDP      17
#define DHCP_SRV_PORT 67
#define DHCP_CLI_PORT 68
#define DHCP_MAGIC    0x63825363u

// UDP-датаграмма: заголовок + payload, csum с pseudo-header (RFC 768)
static int udp_tx(u32 src, u32 dst, u16 sport, u16 dport,
                  const u8 *pl, u32 n) {
    u8 pkt[8 + 320];
    if (n > 320) return -1;
    wbe16(pkt, sport); wbe16(pkt + 2, dport);
    wbe16(pkt + 4, (u16)(8 + n)); wbe16(pkt + 6, 0);
    m_memcpy(pkt + 8, pl, n);
    u32 s = csum_run(pkt, 8 + n, 0);
    s += (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF)
       + (PROT_UDP + 8 + n);
    u16 c = csum_fin(s);
    if (!c) c = 0xFFFF;                      // 0 запрещён (читается как "нет csum")
    wbe16(pkt + 6, c);
    return ip_tx_from(src, dst, PROT_UDP, pkt, 8 + n);
}

static void dhcp_input(const u8 *p, u32 n);

static void udp_input(const u8 *ip, const u8 *u, u32 n) {
    if (n < 8) return;
    u32 len = (u32)rbe16(u + 4);
    if (len < 8 || len > n) return;
    u16 c = rbe16(u + 6);
    if (c) {                                 // 0 = отправитель не считал
        u32 src = rbe32(ip + 12), dst = rbe32(ip + 16);
        u32 s = csum_run(u, len, 0);
        s += (src >> 16) + (src & 0xFFFF) + (dst >> 16) + (dst & 0xFFFF)
           + (PROT_UDP + len);
        if (csum_fin(s)) return;
    }
    u16 dport = rbe16(u + 2);
    if (dport == DHCP_CLI_PORT) dhcp_input(u + 8, len - 8);
}

#define DDISCOVER 1
#define DOFFER    2
#define DREQUEST  3
#define DACK      5
#define DNAK      6

#define DS_OFF    0    // ждёт старта/пауза между раундами
#define DS_DISC   1    // DISCOVER в полёте
#define DS_REQ    2    // REQUEST (SELECTING) в полёте
#define DS_BOUND  3    // аренда живёт, ждём T1
#define DS_RENEW  4    // unicast-renewal, ждём ACK (до T2/expiry)

static u32 dhcp_xid, dhcp_srv, dhcp_srvip, dhcp_timer, dhcp_tries;
static u32 t1_at, exp_at;                    // тики: 1 тик = 50мс
static u8 first_bind;
static u8 fallback_set;

// DHCP-кадр: 236 байт фикс + magic + опции
static void dhcp_send(u8 type, u32 req_ip, u32 srv_id, u32 dst_ip, int bcast) {
    u8 pkt[300];
    m_memset(pkt, 0, 240);
    pkt[0] = 1;                                          // BOOTREQUEST
    pkt[1] = 1;                                          // htype: Ethernet
    pkt[2] = 6;                                          // hlen
    wbe32(pkt + 4, dhcp_xid);
    if (!bcast) wbe32(pkt + 12, my_ip);                  // ciaddr (RENEW)
    else wbe16(pkt + 10, 0x8000);                        // broadcast-флаг
    m_memcpy(pkt + 28, mac, 6);                          // chaddr
    pkt[236] = 99; pkt[237] = 130; pkt[238] = 83; pkt[239] = 99;
    u8 *o = pkt + 240;
    *o++ = 53; *o++ = 1; *o++ = type;
    *o++ = 55; *o++ = 4; *o++ = 1; *o++ = 3; *o++ = 6; *o++ = 51;
    if (type == DREQUEST && dhcp_st == DS_REQ) {         // SELECTING
        *o++ = 50; *o++ = 4; wbe32(o, req_ip); o += 4;
        *o++ = 54; *o++ = 4; wbe32(o, srv_id); o += 4;
    }
    *o++ = 255;
    u32 src = bcast ? 0 : my_ip;
    u32 dst = bcast ? 0xFFFFFFFFu : dst_ip;
    udp_tx(src, dst, DHCP_CLI_PORT, DHCP_SRV_PORT, pkt, (u32)(o - pkt));
}

static int ip2str(u32 ip, char *out) {
    int n = 0;
    u32 v[4] = { ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF };
    for (int i = 0; i < 4; i++) {
        u32 x = v[i];
        if (x >= 100) out[n++] = (char)('0' + x / 100);
        if (x >= 10) out[n++] = (char)('0' + (x / 10) % 10);
        out[n++] = (char)('0' + x % 10);
        if (i < 3) out[n++] = '.';
    }
    return n;
}

// DNS-опция 6 -> /etc/resolv.conf (его читают netsh/xbps; resolv.conf
// из mkinitramfs — только первый загрузочный мир, DHCP его перекрывает)
static void resolv_conf_write(void) {
    if (!dns1) return;
    long fd = sys3(SYS_open, (long)"/etc/resolv.conf",
                   O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return;
    char b[80];
    int n = 0;
    m_memcpy(b, "nameserver ", 11); n += 11;
    n += ip2str(dns1, b + n);
    b[n++] = '\n';
    if (dns2) {
        m_memcpy(b + n, "nameserver ", 11); n += 11;
        n += ip2str(dns2, b + n);
        b[n++] = '\n';
    }
    sys3(SYS_write, fd, (long)b, n);
    sys3(SYS_close, fd, 0, 0);
}

static void print_ip(u32 ip) {
    print_dec_n((long)(ip >> 24)); P(".");
    print_dec_n((long)((ip >> 16) & 0xFF)); P(".");
    print_dec_n((long)((ip >> 8) & 0xFF)); P(".");
    print_dec_n((long)(ip & 0xFF));
}

static void dhcp_unconfig(void) {
    my_ip = 0; my_mask = 0; my_gw = 0;
    addr_set = 0; ac_ok = 0;
    t1_at = 0; exp_at = 0;
}

static void dhcp_bind(u32 yi, u32 mask, u32 gw, u32 d1, u32 d2, u32 lease_s) {
    my_ip = yi;
    my_mask = mask ? mask : 0xFFFFFF00u;
    my_gw = gw;
    dns1 = d1; dns2 = d2;
    addr_set = 1; ac_ok = 0;                 // ARP-кэш под новый адрес
    if (lease_s && lease_s != 0xFFFFFFFFu) {
        t1_at = now_t + (lease_s / 2) * 20;
        exp_at = now_t + (lease_s / 2 + lease_s / 4) * 20;   // T2 = 75%
    } else {
        t1_at = 0; exp_at = 0;
    }
    dhcp_st = DS_BOUND;
    if (!first_bind) {
        first_bind = 1;
        P("netd: DHCP OK ip="); print_ip(my_ip);
        P(" mask="); print_ip(my_mask);
        P(" gw="); print_ip(my_gw);
        if (dns1) { P(" dns="); print_ip(dns1); }
        P("\n");
        resolv_conf_write();
    } else {
        P("netd: DHCP renew ip="); print_ip(my_ip); P("\n");
    }
}

static void dhcp_input(const u8 *p, u32 n) {
    if (n < 240 || p[0] != 2 || p[1] != 1 || p[2] != 6) return;
    if (rbe32(p + 236) != DHCP_MAGIC) return;
    if (rbe32(p + 4) != dhcp_xid) return;
    for (int i = 0; i < 6; i++)
        if (p[28 + i] != mac[i]) return;
    u8 t = 0, seen_end = 0;
    u32 mask = 0, gw = 0, d1 = 0, d2 = 0, lease = 0, sid = 0;
    const u8 *o = p + 240;
    u32 on = n - 240;
    while (on >= 2 && !seen_end) {
        u8 c = o[0];
        if (c == 0) { o++; on--; continue; }
        if (c == 255) { seen_end = 1; break; }
        u8 l = o[1];
        if (l + 2 > on) break;
        if (c == 53) t = o[2];
        else if (c == 1 && l >= 4) mask = rbe32(o + 2);
        else if (c == 3 && l >= 4) gw = rbe32(o + 2);
        else if (c == 6 && l >= 4) {
            d1 = rbe32(o + 2);
            if (l >= 8) d2 = rbe32(o + 6);
        } else if (c == 51 && l >= 4) lease = rbe32(o + 2);
        else if (c == 54 && l >= 4) sid = rbe32(o + 2);
        o += l + 2; on -= l + 2;
    }
    if (!seen_end) return;                   // оборванный хвост — мимо
    u32 yi = rbe32(p + 16);
    if (dhcp_st == DS_DISC && t == DOFFER && yi) {
        dhcp_yi = yi;
        dhcp_srv = sid;
        dhcp_srvip = sid ? sid : rbe32(p + 20);
        dhcp_st = DS_REQ;
        dhcp_tries = 0;
        dhcp_timer = now_t + 2;              // REQUEST сразу
        P("netd: DHCP OFFER ip="); print_ip(yi); P("\n");
        return;
    }
    if ((dhcp_st == DS_REQ || dhcp_st == DS_RENEW) && t == DACK) {
        dhcp_bind(yi, mask, gw, d1, d2, lease);
        return;
    }
    if ((dhcp_st == DS_REQ || dhcp_st == DS_RENEW) && t == DNAK) {
        P("netd: DHCP NAK — заново\n");
        dhcp_unconfig();
        dhcp_st = DS_OFF;                    // новый DISCOVER без паузы
        dhcp_timer = 0;
    }
}

static void dhcp_fallback(void) {
    // slirp-статика: живёт на ISO-бутах/машинах без DHCP-сервера,
    // DHCP продолжает пробовать в фоне
    fallback_set = 1;
    my_ip = 0x0A00020Fu; my_mask = 0xFFFFFF00u; my_gw = 0x0A000202u;
    addr_set = 1;
    P("netd: DHCP недоступен — fallback slirp-статика\n");
}

static void dhcp_tick(void) {
    u32 t = now_t;
    if (dhcp_st == DS_OFF) {
        if (t < dhcp_timer) return;
        dhcp_xid = 0x2B1D0000u ^ (t * 7919u) ^ ((u32)mac[3] << 16)
                 ^ ((u32)mac[4] << 8) ^ mac[5];
        dhcp_tries = 0;
        dhcp_yi = 0;
        dhcp_send(DDISCOVER, 0, 0, 0, 1);
        dhcp_st = DS_DISC;
        dhcp_timer = t + 80;                 // 4с
        return;
    }
    if (dhcp_st == DS_DISC || dhcp_st == DS_REQ) {
        if (t < dhcp_timer) return;
        if (dhcp_tries < 3) {
            dhcp_tries++;
            if (dhcp_st == DS_DISC) dhcp_send(DDISCOVER, 0, 0, 0, 1);
            else dhcp_send(DREQUEST, dhcp_yi, dhcp_srv, 0, 1);
            dhcp_timer = t + 80 * (dhcp_tries + 1);
            return;
        }
        dhcp_st = DS_OFF;
        dhcp_timer = t + 320;                // 16с — следующая попытка
        if (!addr_set && !fallback_set && t >= 480) dhcp_fallback();
        return;
    }
    if (dhcp_st == DS_BOUND) {
        if (exp_at && t >= exp_at) {         // аренда истекла — всё заново
            P("netd: DHCP lease expired\n");
            dhcp_unconfig();
            dhcp_st = DS_OFF; dhcp_timer = 0;
            return;
        }
        if (t1_at && t >= t1_at) {           // T1: unicast-продление
            dhcp_st = DS_RENEW;
            dhcp_tries = 0;
            dhcp_xid ^= t * 31337u;
            dhcp_send(DREQUEST, 0, 0, dhcp_srvip, 0);
            dhcp_timer = t + 160;            // 8с между ретраями
        }
        return;
    }
    if (dhcp_st == DS_RENEW) {
        if (exp_at && t >= exp_at) {
            P("netd: DHCP lease expired (T2)\n");
            dhcp_unconfig();
            dhcp_st = DS_OFF; dhcp_timer = 0;
            return;
        }
        if (t < dhcp_timer) return;
        if (dhcp_tries < 8) {
            dhcp_tries++;
            dhcp_send(DREQUEST, 0, 0, dhcp_srvip, 0);
            dhcp_timer = t + 160;
        } else {                             // сервер умер: ищем новый
            dhcp_unconfig();
            dhcp_st = DS_OFF; dhcp_timer = 0;
        }
    }
}

// --- TCP ---
static struct sock *sk_by_sid(u32 sid) {
    if (sid >= NSOCK) return 0;
    return S[sid].state == SS_FREE ? 0 : &S[sid];
}

static void push_msg(u32 sid, u32 msg_id, u32 err, const u8 *data, u32 n) {
    u8 ln = S[sid].ln;
    if (ln >= NLANE || L[ln].fd < 0) return;
    struct ml_msg m = { ML_PROTO_NET, msg_id, err, 0, {0} };
    *ml_h(&m, 0) = sid;
    long r = ml_send(L[ln].fd, &m, data, n, 0, 0);
    if (r < 0 || (u32)r < 24 + n) {
        P("netd: PUSH FAIL sid="); print_dec_n((long)sid);
        P(" n="); print_dec_n((long)n);
        P(" r="); print_dec_n(r); P("\n");
    }
}

static void sk_free(struct sock *s) {
    u32 sid = (u32)(s - S);
    m_memset(s, 0, sizeof(*s));
    S[sid].state = SS_FREE;
}

// сброс с уведомлением владельца: SYN-этап — CONNECT_R с ошибкой,
// живое соединение — EOF
static void sk_fail(struct sock *s, u32 err) {
    u32 sid = (u32)(s - S);
    u8 was_syn = (s->state == SS_SYN);
    u8 ln = s->ln;
    sk_free(s);
    (void)ln;
    if (was_syn) push_msg(sid, NET_CONNECT_R, err, 0, 0);
    else push_msg(sid, NET_EOF, err, 0, 0);
}

// сегмент в очередь передачи; snd_nxt уходит вперёд на len (+1 SYN/FIN)
static void seg_queue(struct sock *s, u32 off, u32 len, u32 flags) {
    if (s->qn >= TSEG_MAX) return;
    struct tseg *t = &s->q[s->qn++];
    t->seq = s->snd_nxt; t->len = len; t->flags = flags;
    t->t0 = now_t; t->tries = 0; t->off = off;
    s->snd_nxt += len + ((flags & (F_SYN | F_FIN)) ? 1 : 0);
}

static int tcp_seg_tx(struct sock *s, u32 seq, u32 off, u32 len, u32 flags) {
    u8 pkt[MSS + 40];
    u8 *t = pkt;
    wbe16(t, s->lport); wbe16(t + 2, s->rport);
    wbe32(t + 4, seq);
    wbe32(t + 8, s->rcv_nxt);
    t[12] = 5 << 4;
    t[13] = (u8)flags;
    wbe16(t + 14, 16384);                              // окно приёма
    wbe16(t + 16, 0); wbe16(t + 18, 0);
    u32 tcplen = 20 + len;
    if (len) m_memcpy(t + 20, s->snd + off, len);
    u32 sum = csum_run(pkt, tcplen, 0);
    sum += (NET_MY_IP >> 16) + (NET_MY_IP & 0xFFFF)
         + (s->rip >> 16) + (s->rip & 0xFFFF) + (6 + tcplen);
    wbe16(t + 16, csum_fin(sum));
    return ip_tx(s->rip, 6, pkt, tcplen);
}

static int syn_tx(struct sock *s) {
    u8 pkt[24];
    u8 *t = pkt;
    wbe16(t, s->lport); wbe16(t + 2, s->rport);
    wbe32(t + 4, s->iss); wbe32(t + 8, 0);
    t[12] = 6 << 4;                                    // 6 слов: MSS-опция
    t[13] = F_SYN;
    wbe16(t + 14, 16384);
    wbe16(t + 16, 0); wbe16(t + 18, 0);
    t[20] = 2; t[21] = 4; wbe16(t + 22, MSS);          // opt MSS
    u32 sum = csum_run(t, 24, 0);
    sum += (NET_MY_IP >> 16) + (NET_MY_IP & 0xFFFF)
         + (s->rip >> 16) + (s->rip & 0xFFFF) + (6 + 24);
    wbe16(t + 16, csum_fin(sum));
    return ip_tx(s->rip, 6, pkt, 24);
}

// данные идут по порядку: seg_queue нумерует их от snd_nxt
static void try_tx(struct sock *s) {
    if (s->state != SS_EST && s->state != SS_FINW) return;
    u32 nxt = s->snd_nxt - s->iss - 1;                 // data offset след. байта
    u32 flight = s->snd_nxt - s->snd_una;
    while (nxt < s->snd_len && flight < s->snd_win && s->qn < TSEG_MAX) {
        u32 len = s->snd_len - nxt;
        if (len > MSS) len = MSS;
        seg_queue(s, nxt, len, F_ACK);
        // сразу в провод: очередь — для ретрансмиссии, а не для ожидания
        tcp_seg_tx(s, s->snd_nxt - len, nxt, len, F_ACK);
        s->q[s->qn - 1].sent = 1;
        nxt += len;
        flight += len;
    }
    if (s->want_close && !s->fin_queued && nxt >= s->snd_len
        && s->qn < TSEG_MAX && s->state == SS_EST) {
        seg_queue(s, 0, 0, F_ACK | F_FIN);
        s->q[s->qn - 1].sent = 1;
        s->fin_queued = 1;
        s->state = SS_FINW;
    }
}

static void tcp_input(const u8 *ip, const u8 *t, u32 plen) {
    if (plen < 20) return;
    u32 src = rbe32(ip + 12);
    u16 sport = rbe16(t), dport = rbe16(t + 2);
    u32 seq = rbe32(t + 4), ack = rbe32(t + 8);
    u32 hlen = (u32)(t[12] >> 4) * 4;
    u32 flags = t[13];
    if (hlen > plen) return;
    u32 dlen = plen - hlen;

    struct sock *s = 0;
    for (int i = 0; i < NSOCK; i++)
        if (S[i].state != SS_FREE && S[i].rport == sport && S[i].rip == src
            && S[i].lport == dport) { s = &S[i]; break; }
    if (!s) return;

    if (s->state == SS_SYN) {
        if ((flags & (F_SYN | F_ACK)) == (F_SYN | F_ACK) && ack == s->iss + 1) {
            s->rcv_nxt = seq + 1;
            s->snd_una = ack;
            s->snd_win = rbe16(t + 14) ? rbe16(t + 14) : 4096;
            if (s->snd_win > 16384) s->snd_win = 16384;
            s->state = SS_EST;
            s->qn = 0;                                 // SYN подтверждён SYN-ACK'ом
            P("netd: EST sid="); print_dec_n((long)(s - S)); P("\n");
            tcp_seg_tx(s, s->snd_nxt, 0, 0, F_ACK);
            push_msg((u32)(s - S), NET_CONNECT_R, 0, 0, 0);
            try_tx(s);                                 // данные, ушедшие в SYN
        } else if (flags & F_RST) {
            sk_fail(s, 4);
        }
        return;
    }
    if (s->state != SS_EST && s->state != SS_FINW) return;

    if (flags & F_RST) { sk_fail(s, 4); return; }

    if (flags & F_ACK) {
        if ((long)(ack - s->snd_una) > 0 && (long)(s->snd_nxt - ack) >= 0) {
            s->snd_una = ack;
            u32 w = 0;
            for (u32 i = 0; i < s->qn; i++) {
                struct tseg *t2 = &s->q[i];
                if ((long)(s->snd_una - (t2->seq + t2->len)) >= 0) continue;
                s->q[w++] = *t2;
            }
            s->qn = w;
            if (s->state == SS_FINW && s->fin_queued
                && s->snd_una == s->snd_nxt) {
                sk_free(s);
                return;
            }
        }
        if (rbe16(t + 14)) s->snd_win = rbe16(t + 14);
    }

    if (dlen) {
        if (seq == s->rcv_nxt && s->rx_len + dlen <= RBUF_MAX) {
            m_memcpy(s->rxb + s->rx_len, t + hlen, dlen);
            s->rx_len += dlen;
            s->rcv_nxt += dlen;
            tcp_seg_tx(s, s->snd_nxt, 0, 0, F_ACK);
            // данные клиенту сразу: EOF может прийти в том же батче
            while (s->rx_len) {
                u32 n = s->rx_len > 256 ? 256 : s->rx_len;
                push_msg((u32)(s - S), NET_DATA, 0, s->rxb, n);
                m_memcpy(s->rxb, s->rxb + n, s->rx_len - n);
                s->rx_len -= n;
            }
        } else {
            tcp_seg_tx(s, s->snd_nxt, 0, 0, F_ACK);    // дублирующий ACK
        }
    }

    if (flags & F_FIN) {
        s->rcv_nxt = seq + dlen + 1;
        if (!dlen) tcp_seg_tx(s, s->snd_nxt, 0, 0, F_ACK);
        if (!s->eof) { s->eof = 1; push_msg((u32)(s - S), NET_EOF, 0, 0, 0); }
    }

    if (flags & F_ACK) try_tx(s);
}

static void ip_input(const u8 *f, u32 n) {
    if (n < 20) return;
    const u8 *ip = f + ETH_HDR;
    u32 dst = rbe32(ip + 16);
    // DHCP-транзакция: ACK может прийти unicast'ом на предложенный адрес,
    // которого у нас ещё нет
    u32 yi = (dhcp_st == DS_REQ) ? dhcp_yi : 0;
    if (dst != NET_MY_IP && dst != 0xFFFFFFFFu && dst != yi) return;
    // длина — из IP total length: slirp добивает короткие кадры нулями
    // до 60 байт (Ethernet минимум), нули не должны попадать в поток
    u32 plen = (u32)((ip[2] << 8) | ip[3]);
    u32 ihl = (u32)(ip[0] & 0x0F) * 4;
    if (plen < ihl || plen > n - ETH_HDR) plen = n - ETH_HDR;
    if (ip[9] == 1) icmp_input(ip, plen);
    else if (ip[9] == 6) tcp_input(ip, ip + ihl, plen - ihl);
    else if (ip[9] == 17) udp_input(ip, ip + ihl, plen - ihl);
}

static int e1000_init(long uiofd) {
    mmio = (volatile u32 *)sys6(SYS_mmap, 0, 0x20000, 1 | 2, 1, uiofd, 0);
    if ((long)mmio < 0) { P("netd: mmap BAR0 FAIL\n"); return -1; }

    // pinned-DMA: кольца и буферы должны быть видны NIC по физике
    long pd = sys3(SYS_open, (long)"/dev/mlpinned", O_RDWR, 0);
    if (pd < 0) { P("netd: /dev/mlpinned FAIL\n"); return -1; }
    dma = (u8 *)sys6(SYS_mmap, 0, 4u << 20, 1 | 2, 1, pd, 0);
    if ((long)dma < 0) { P("netd: mmap pinned FAIL\n"); return -1; }
    u64 phys = 0;
    // MLPIO_GETPHYS = _IOR('m', 1, u64) = 0x80086D01
    if (sys3(SYS_ioctl, pd, 0x80086D01, (long)&phys) < 0) {
        P("netd: ioctl GETPHYS FAIL\n");
        return -1;
    }
    dma_phys = phys;

    m_memset(dma, 0, 4u << 20);
    for (int i = 0; i < NDESC; i++) {
        volatile u8 *t = txd(i);
        volatile u8 *r = rxd(i);
        u64 ta = dma_phys + txbuf(i);
        u64 ra = dma_phys + rxbuf(i);
        for (int k = 0; k < 8; k++) {
            t[k] = (u8)(ta >> (8 * k));                // addr LE
            r[k] = (u8)(ra >> (8 * k));
        }
    }

    u32 ral = er(E_RAL), rah = er(E_RAH);
    mac[0] = (u8)ral; mac[1] = (u8)(ral >> 8); mac[2] = (u8)(ral >> 16);
    mac[3] = (u8)(ral >> 24); mac[4] = (u8)rah; mac[5] = (u8)(rah >> 8);

    ew(E_CTRL, er(E_CTRL) | (1u << 26));               // RST
    sleep_ms(20);
    ew(E_CTRL, er(E_CTRL) | (1u << 6) | (1u << 5) | 1); // SLU|ASDE|FD

    ew(E_TDBA, (u32)(dma_phys + OFF_TXRING));
    ew(E_TDBA + 4, (u32)(dma_phys >> 32));
    ew(E_TDLEN, NDESC * 16);
    ew(E_TDT, 0);
    ew(E_TCTL, 0x02 | 0x08 | (0x0F << 4) | (0x40 << 12));

    ew(E_RDBA, (u32)(dma_phys + OFF_RXRING));
    ew(E_RDBA + 4, (u32)(dma_phys >> 32));
    ew(E_RDLEN, NDESC * 16);
    ew(E_RDT, NDESC - 1);
    rx_cur = 0;
    ew(E_RCTL, 0x02 | 0x08 | 0x10 | 0x8000);           // EN|UPE|MPE|BAM

    P("netd: e1000 через UIO, MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) P(":");
        print_hex_flat((u32)mac[i] << 24);
    }
    P("\n");
    return 0;
}

// e1000e (82574): карта регистров совместима с e1000, дескрипторы — те же
// legacy (DTYP=0 в RCTL; расширенные включаются только packet-split'ом).
// Отличия: PCIe, ASPM-эррата — гасится в probe'е через config space.
static int e1000e_init(long uiofd) {
    if (e1000_init(uiofd) < 0) return -1;
    P("netd: e1000e — legacy-дескрипторы, интрофф (polling)\n");
    return 0;
}

// --- r8169 (RTL8168/8111, M15): C+ дескрипторы, polling ---
// Регистры/биты — из ванильного r8169_main.c (7.2.9): опции в DW0/DW1,
// адрес в DW2/DW3, Own=1 — владение NIC. QEMU rtl8169 не моделирует —
// валидация только на реальном железе (BOOTS-гейт «r8169»).
#define R_MAC0      0x0000
#define R_MAC4      0x0004
#define R_TXDLOW    0x0020
#define R_TXDHIGH   0x0024
#define R_CHIPCMD   0x0037
#define R_TXCFG     0x0040
#define R_RXCFG     0x0044
#define R_RXMAXSZ   0x00da
#define R_CPLUSCMD  0x00e0
#define R_RXDLOW    0x00e4
#define R_RXDHIGH   0x00e8
#define R_MAXTXPKT  0x00ec

#define RTL_OWN     (1u << 31)      // 1 = владение NIC
#define RTL_EOR     (1u << 30)      // конец кольца
#define RTL_FIRST   (1u << 29)
#define RTL_LAST    (1u << 28)
#define RTL_RXRES   (1u << 21)      // RxRES: ошибка приёма

static void wle32(volatile u8 *p, u32 v) {
    p[0] = (u8)v; p[1] = (u8)(v >> 8); p[2] = (u8)(v >> 16); p[3] = (u8)(v >> 24);
}
static u32 rle32(const volatile u8 *p) {
    return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24);
}

static volatile u32 *r_mmio;
static u32 r_tx_cur, r_rx_cur;

static u32 rr(u32 reg) { return r_mmio[reg >> 2]; }
static void rw(u32 reg, u32 v) { r_mmio[reg >> 2] = v; }
static u8 rr8(u32 reg) { return ((volatile u8 *)r_mmio)[reg]; }
static void rw8(u32 reg, u8 v) { ((volatile u8 *)r_mmio)[reg] = v; }
static void rw16(u32 reg, u16 v) { ((volatile u16 *)r_mmio)[reg >> 1] = v; }

static volatile u8 *r_txd(int i) { return dma + OFF_TXRING + (u32)i * 16; }
static volatile u8 *r_rxd(int i) { return dma + OFF_RXRING + (u32)i * 16; }
static u32 r_txbuf(int i) { return OFF_TXBUF + (u32)i * RBUFSZ; }
static u32 r_rxbuf(int i) { return OFF_RXBUF + (u32)i * RBUFSZ; }

static u8 *r8169_txf(void) { return dma + r_txbuf((int)r_tx_cur); }

static int r8169_txc(u32 len) {
    volatile u8 *d = r_txd((int)r_tx_cur);
    u64 a = dma_phys + r_txbuf((int)r_tx_cur);
    wle32(d, RTL_OWN | RTL_FIRST | RTL_LAST | len);    // opts1
    wle32(d + 4, 0);                                   // opts2: без offload
    wle32(d + 8, (u32)a);
    wle32(d + 12, (u32)(a >> 32));
    for (int w = 0; w < 3000; w++) {
        if (!(rle32(d) & RTL_OWN)) {                   // NIC подтвердил
            r_tx_cur = (r_tx_cur + 1) % NDESC;
            return 0;
        }
        sleep_ms(1);
    }
    P("r8169: TX OWN timeout len="); print_dec_n((long)len); P("\n");
    return -1;
}

static void r8169_poll(void) {
    for (int n = 0; n < NDESC; n++) {
        volatile u8 *d = r_rxd((int)r_rx_cur);
        u32 o1 = rle32(d);
        if (o1 & RTL_OWN) break;                       // NIC не отдал кадр
        u32 len = o1 & 0x1FFF;
        if (len >= ETH_HDR + 20 && !(o1 & RTL_RXRES)) {
            const u8 *f = dma + r_rxbuf((int)r_rx_cur);
            u16 et = rbe16(f + 12);
            if (et == 0x0806) arp_input(f + ETH_HDR, len - ETH_HDR);
            else if (et == 0x0800) ip_input(f, len);
        }
        wle32(d, RTL_OWN | (o1 & RTL_EOR) | RBUFSZ);   // вернуть NIC
        r_rx_cur = (r_rx_cur + 1) % NDESC;
    }
}

static int r8169_init(long uiofd) {
    r_mmio = (volatile u32 *)sys6(SYS_mmap, 0, 0x10000, 1 | 2, 1, uiofd, 0);
    if ((long)r_mmio < 0) { P("r8169: mmap MMIO FAIL\n"); return -1; }

    long pd = sys3(SYS_open, (long)"/dev/mlpinned", O_RDWR, 0);
    if (pd < 0) { P("r8169: /dev/mlpinned FAIL\n"); return -1; }
    dma = (u8 *)sys6(SYS_mmap, 0, 4u << 20, 1 | 2, 1, pd, 0);
    if ((long)dma < 0) { P("r8169: mmap pinned FAIL\n"); return -1; }
    u64 phys = 0;
    if (sys3(SYS_ioctl, pd, 0x80086D01, (long)&phys) < 0) {
        P("r8169: ioctl GETPHYS FAIL\n");
        return -1;
    }
    dma_phys = phys;
    m_memset(dma, 0, 4u << 20);

    rw8(R_CHIPCMD, 0x10);                              // CmdReset
    int ok = 0;
    for (int w = 0; w < 100; w++) {
        if (!(rr8(R_CHIPCMD) & 0x10)) { ok = 1; break; }
        sleep_ms(1);
    }
    if (!ok) { P("r8169: reset timeout\n"); return -1; }

    ((volatile u16 *)r_mmio)[0x3c >> 1] = 0;           // IntrMask: polling
    rw8(R_MAXTXPKT, 0x3F);                             // NoEarlyTx/TxPacketMax
    rw16(R_CPLUSCMD, (u16)(rr(R_CPLUSCMD) | 0x0008));  // PCIMulRW
    ((volatile u16 *)r_mmio)[0xe2 >> 1] = 0;           // IntrMitigate
    rw16(R_RXMAXSZ, RBUFSZ + 1);
    rw8(0x56, (u8)(rr8(0x56) & ~0x01u));               // Config5: ASPM off

    u32 m0 = rr(R_MAC0), m4 = rr(R_MAC4);
    mac[0] = (u8)m0; mac[1] = (u8)(m0 >> 8);
    mac[2] = (u8)(m0 >> 16); mac[3] = (u8)(m0 >> 24);
    mac[4] = (u8)m4; mac[5] = (u8)(m4 >> 8);

    u64 txp = dma_phys + OFF_TXRING;
    u64 rxp = dma_phys + OFF_RXRING;
    rw(R_TXDHIGH, (u32)(txp >> 32)); rw(R_TXDLOW, (u32)txp);
    rw(R_RXDHIGH, (u32)(rxp >> 32)); rw(R_RXDLOW, (u32)rxp);

    for (int i = 0; i < NDESC; i++) {                  // RX: отдать NIC
        volatile u8 *d = r_rxd(i);
        u64 a = dma_phys + r_rxbuf(i);
        wle32(d + 4, 0);
        wle32(d, RTL_OWN | ((i == NDESC - 1) ? RTL_EOR : 0) | RBUFSZ);
        wle32(d + 8, (u32)a);
        wle32(d + 12, (u32)(a >> 32));
    }

    rw8(R_CHIPCMD, 0x0C);                              // TE | RE
    rw(R_RXCFG, (7u << 13) | (7u << 8) | (1u << 15) | 0x0F);
    rw(R_TXCFG, (7u << 8) | (0x03u << 24));

    P("netd: r8169 через UIO, MAC ");
    for (int i = 0; i < 6; i++) {
        if (i) P(":");
        print_hex_flat((u32)mac[i] << 24);
    }
    P("\n");
    return 0;
}

// --- таблица NIC + диспетчеризация ---
static struct nic_ops e1000_ops = {
    "e1000", e1000_txf, e1000_txc, e1000_poll, e1000_init
};
static struct nic_ops e1000e_ops = {
    "e1000e", e1000_txf, e1000_txc, e1000_poll, e1000e_init
};
static struct nic_ops r8169_ops = {
    "r8169", r8169_txf, r8169_txc, r8169_poll, r8169_init
};

static u8 *tx_frame(void) { return nic->txf(); }
static int nic_tx_commit(u32 len) { return nic->txc(len); }
static void nic_poll(void) { nic->poll(); }

// "0x100e\n" -> 0x100e
static int parse_hex16(const char *buf, long n) {
    long v = 0;
    int started = 0;
    for (long i = 0; i < n; i++) {
        char c = buf[i];
        if (c == 'x' || c == 'X') { started = 1; v = 0; continue; }
        if (c >= '0' && c <= '9') { v = v * 16 + c - '0'; started = 1; }
        else if (c >= 'a' && c <= 'f') { v = v * 16 + c - 'a' + 10; started = 1; }
        else if (c >= 'A' && c <= 'F') { v = v * 16 + c - 'A' + 10; started = 1; }
        else if (started) break;
    }
    return started ? (int)v : -1;
}

// ASPM-эррата e1000e/r8169: LNKCTL (cap id 0x10) — очистить L0s/L1
static void pcie_aspm_off(int idx) {
    char path[48];
    const char *cfg = "/sys/class/uio/uio /device/config";
    m_memcpy(path, cfg, 34);
    path[18] = (char)('0' + idx);
    path[34] = 0;
    long cf = sys3(SYS_open, (long)path, O_RDWR, 0);
    if (cf < 0) return;
    u8 c[256];
    if (sys3(SYS_read, cf, (long)c, 256) < 48) {
        sys3(SYS_close, cf, 0, 0);
        return;
    }
    u8 next = c[0x34];
    int guard = 0;
    while (next >= 0x40 && next < 256 && guard++ < 16) {
        if (c[next] == 0x10) {
            u16 lnk = (u16)(c[next + 0x10] | (c[next + 0x11] << 8));
            lnk &= (u16)~0x0003;
            sys3(SYS_lseek, cf, next + 0x10, 0);
            sys3(SYS_write, cf, (long)&lnk, 2);
        }
        next = c[next + 1];
    }
    sys3(SYS_close, cf, 0, 0);
}

static int nic_probe(void) {
    static const struct {
        u16 vendor, device;
        const char *name;
        struct nic_ops *ops;
    } tbl[] = {
        { 0x8086, 0x100e, "e1000",  &e1000_ops },
        { 0x8086, 0x10d3, "e1000e", &e1000e_ops },
        { 0x10ec, 0x8168, "r8169",  &r8169_ops },
    };
    int found = -1, idx = -1;
    long uiofd = -1;
    char path[48];
    char buf[16];
    for (int i = 0; i < 8; i++) {
        const char *p = "/dev/uio ";
        m_memcpy(path, p, 9);
        path[8] = (char)('0' + i);
        path[9] = 0;
        long fd = sys3(SYS_open, (long)path, O_RDWR, 0);
        if (fd < 0) continue;
        const char *sys = "/sys/class/uio/uio /device/vendor";
        m_memcpy(path, sys, 34);
        path[18] = (char)('0' + i);
        path[34] = 0;
        long v = sys3(SYS_open, (long)path, O_RDONLY, 0);
        if (v < 0) { sys3(SYS_close, fd, 0, 0); continue; }
        long n = sys3(SYS_read, v, (long)buf, 12);
        sys3(SYS_close, v, 0, 0);
        int ven = parse_hex16(buf, n);
        const char *dev = "/sys/class/uio/uio /device/device";
        m_memcpy(path, dev, 34);
        path[18] = (char)('0' + i);
        path[34] = 0;
        v = sys3(SYS_open, (long)path, O_RDONLY, 0);
        if (v < 0) { sys3(SYS_close, fd, 0, 0); continue; }
        n = sys3(SYS_read, v, (long)buf, 12);
        sys3(SYS_close, v, 0, 0);
        int dev_ = parse_hex16(buf, n);
        for (u32 k = 0; k < sizeof(tbl) / sizeof(tbl[0]); k++) {
            if (ven == (int)tbl[k].vendor && dev_ == (int)tbl[k].device) {
                found = (int)k;
                idx = i;
                uiofd = fd;
                break;
            }
        }
        if (found >= 0) break;
        sys3(SYS_close, fd, 0, 0);
    }
    if (found < 0) {
        P("netd: /dev/uioN не найден (e1000/e1000e/r8169)\n");
        return -1;
    }
    // BUS MASTER: uio_pci_generic не ставит бит сам — без него DMA мимо
    const char *cfg = "/sys/class/uio/uio /device/config";
    m_memcpy(path, cfg, 34);
    path[18] = (char)('0' + idx);
    path[34] = 0;
    long cf = sys3(SYS_open, (long)path, O_RDWR, 0);
    if (cf >= 0) {
        u16 cmd = 0;
        sys3(SYS_lseek, cf, 4, 0);                     // PCI_COMMAND
        if (sys3(SYS_read, cf, (long)&cmd, 2) == 2) {
            cmd |= 0x0007;                             // IO | MEM | MASTER
            sys3(SYS_lseek, cf, 4, 0);
            sys3(SYS_write, cf, (long)&cmd, 2);
        }
        sys3(SYS_close, cf, 0, 0);
    }
    pcie_aspm_off(idx);
    nic = tbl[found].ops;
    P("netd: NIC "); print_str(tbl[found].name); P("\n");
    return nic->init(uiofd);
}

// --- сервис ---
static void sk_open(u32 type, struct lane *ln) {
    struct ml_msg r = { ML_PROTO_NET, NET_OPEN_R, ML_E_OK, 0, {0} };
    if (type != 1) { r.err = ML_E_NO_SUCH_ENTITY; }
    else {
        u32 sid = 0xFFFFFFFFu;
        for (int i = 0; i < NSOCK; i++)
            if (S[i].state == SS_FREE) { sid = (u32)i; break; }
        if (sid == 0xFFFFFFFFu) r.err = ML_E_BUSY;
        else {
            m_memset(&S[sid], 0, sizeof(struct sock));
            S[sid].state = SS_ALLOC;                   // до CONNECT живёт выделенным
            S[sid].snd_win = 4096;
            S[sid].ln = (u8)(ln - L);
            *ml_h(&r, 0) = sid;
        }
    }
    ml_send(ln->fd, &r, 0, 0, 0, 0);
}

static void sk_connect(struct lane *ln, u32 sid, u32 rip, u16 rport) {
    struct ml_msg r = { ML_PROTO_NET, NET_CONNECT_R, ML_E_OK, 0, {0} };
    *ml_h(&r, 0) = sid;
    struct sock *s = sk_by_sid(sid);
    // libfetch дергает connect() повторно (happy-eyeballs ретраи):
    // ALLOC — стартуем; SYN — уже устанавливается (ответ придёт async);
    // EST — уже установлено, подтверждаем сразу
    if (!s) { r.err = ML_E_NO_SUCH_ENTITY; goto out; }
    if (s->state == SS_SYN) return;
    if (s->state == SS_EST) goto out;
    s->rip = rip; s->rport = rport;
    s->lport = (u16)(40000 + (next_port++ % 20000));
    s->iss = 0x1200 + now_t * 7919 + sid * 131;
    s->snd_nxt = s->iss;                       // SYN-сегмент сам займёт seq
    s->snd_una = s->iss;
    s->state = SS_SYN;
    syn_tx(s);
    seg_queue(s, 0, 0, F_SYN);                 // SYN в ретрансмиссии (ARP/потери)
    P("netd: CONNECT sid="); print_dec_n((long)sid);
    P(" ip="); print_hex_flat(rip);
    P(":"); print_dec_n((long)rport); P("\n");
    s->q[0].sent = 1;                          // syn_tx уже гонял кадр
    return;                                            // ответ — с SYN-ACK'ом
out:
    ml_send(ln->fd, &r, 0, 0, 0, 0);
}

static void sk_send(struct lane *ln, u32 sid, const u8 *d, u32 n) {
    struct ml_msg r = { ML_PROTO_NET, NET_SEND_R, ML_E_OK, 0, {0} };
    *ml_h(&r, 0) = sid;
    struct sock *s = sk_by_sid(sid);
    if (!s || (s->state != SS_EST && s->state != SS_SYN)) {
        r.err = ML_E_NO_SUCH_ENTITY; goto out;
    }
    if (s->snd_len + n > SNDBUF_MAX) {
        if (s->snd_len >= SNDBUF_MAX) n = 0;
        else n = SNDBUF_MAX - s->snd_len;
    }
    m_memcpy(s->snd + s->snd_len, d, n);
    s->snd_len += n;
    try_tx(s);
out:
    P("netd: SEND_R sid="); print_dec_n((long)sid);
    P(" err="); print_dec_n((long)r.err); P("\n");
    ml_send(ln->fd, &r, 0, 0, 0, 0);
}

static void sk_close(struct lane *ln, u32 sid) {
    struct ml_msg r = { ML_PROTO_NET, NET_CLOSE_R, ML_E_OK, 0, {0} };
    *ml_h(&r, 0) = sid;
    struct sock *s = sk_by_sid(sid);
    if (!s) { r.err = ML_E_NO_SUCH_ENTITY; goto out; }
    if (s->state == SS_ALLOC) { sk_free(s); goto out; }
    s->want_close = 1;
    try_tx(s);
out:
    ml_send(ln->fd, &r, 0, 0, 0, 0);
}

static void lane_input(struct lane *ln) {
    // ОДНО сообщение за итерацию: длинное блокирующее чтение внутри
    // event-loop'а остановило бы nic_poll/timers — ретрансмиссии бы умерли
    struct ml_msg m;
    char tail[ML_MAXTAIL];
    struct ml_aux aux;
    long r = ml_recv(ln->fd, &m, tail, sizeof(tail), &aux);
    if (r <= 0) {
        P("netd: lane закрыт клиентом\n");
        sys3(SYS_close, ln->fd, 0, 0);
        ln->fd = -1;
        return;
    }
    if (m.proto != ML_PROTO_NET) return;
    u32 sid = (u32)*ml_h(&m, 0);
    switch (m.msg_id) {
    case NET_OPEN:    sk_open((u32)*ml_h(&m, 0), ln); break;
    case NET_CONNECT: sk_connect(ln, sid, (u32)*ml_h(&m, 1),
                                (u16)*ml_h(&m, 2)); break;
    case NET_SEND:    sk_send(ln, sid, (const u8 *)tail, m.tail_len); break;
    case NET_CLOSE:   sk_close(ln, sid); break;
    default: break;
    }
}

static long bus_register(void) {
    long bus = -1;
    int tries = 0;
    for (; ; tries++) {
        bus = ml_connect("milin-bus");
        if (bus >= 0) break;
        if (tries > 2000) { P("netd: шина не поднялась за 20с\n"); return -1; }
        sleep_ms(10);
    }
    struct ml_msg m = { ML_PROTO_BUS, BUS_REGISTER, 0, 0, {0} };
    m_memcpy(m.head, "net0", 5);
    u32 masks[2] = { 0, NET_CAP_OPEN };
    if (ml_send(bus, &m, masks, sizeof(masks), 0, 0) < 0) return -1;
    struct ml_msg resp;
    if (ml_recv(bus, &resp, 0, 0, 0) <= 0 || resp.err != ML_E_OK) return -1;
    return bus;
}

static void timers(void) {
    now_t++;
    dhcp_tick();
    for (int i = 0; i < NSOCK; i++) {
        struct sock *s = &S[i];
        if (s->state == SS_FREE) continue;
        for (u32 j = 0; j < s->qn; j++) {
            struct tseg *t = &s->q[j];
            u32 rto = 4 + (t->tries * 2);              // тики по 50 мс
            if (now_t - t->t0 < rto) continue;
            if (t->tries > 8) { sk_fail(s, 4); break; }
            t->tries++; t->t0 = now_t;
            if (t->flags & F_SYN) syn_tx(s);
            else tcp_seg_tx(s, t->seq, t->off, t->len, t->flags | F_ACK);
        }
        while (s->rx_len && s->state == SS_EST) {
            u32 n = s->rx_len > 256 ? 256 : s->rx_len;
            push_msg((u32)i, NET_DATA, 0, s->rxb, n);
            m_memcpy(s->rxb, s->rxb + n, s->rx_len - n);
            s->rx_len -= n;
        }
    }
}

struct pfd { int fd; short events, revents; };

void milin_main(void) {
    for (int i = 0; i < NLANE; i++) L[i].fd = -1;

    if (nic_probe() < 0) {
        // без NIC (ISO-бут, реальная машина без e1000-класса) — не умирать:
        // рестарт-петля супервизора роняет бут; паркуемся, сеть просто нет
        P("netd: NIC не найден — паркуюсь\n");
        for (;;) sleep_ms(1000);
    }

    busfd = bus_register();
    if (busfd < 0) {
        P("netd: шина не отвечает\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("netd: сервис \"net0\" — адрес по DHCP\n");

    for (;;) {
        struct pfd fds[1 + NLANE];
        int nf = 0;
        fds[nf].fd = (int)busfd; fds[nf].events = 1; fds[nf].revents = 0; nf++;
        for (int i = 0; i < NLANE; i++)
            if (L[i].fd >= 0) {
                fds[nf].fd = (int)L[i].fd; fds[nf].events = 1; nf++;
            }
        long r = sys5(SYS_poll, (long)fds, nf, 50, 0, 0);
        if (r > 0) {
            if (fds[0].revents & 1) {
                struct ml_msg m;
                char tail[8];
                struct ml_aux aux;
                if (ml_recv(busfd, &m, tail, sizeof(tail), &aux) <= 0) {
                    P("netd: шина умерла — перерегистрация\n");
                    sys3(SYS_close, busfd, 0, 0);
                    busfd = bus_register();
                    if (busfd < 0) sys3(SYS_exit, 1, 0, 0);
                } else if (m.proto == ML_PROTO_BUS
                           && m.msg_id == BUS_LANE_REQ && aux.nhooks >= 1) {
                    for (int i = 0; i < NLANE; i++)
                        if (L[i].fd < 0) {
                            L[i].fd = aux.hooks[0];
                            if (m.tail_len >= 4)
                                m_memcpy(&L[i].rights, tail, 4);
                            P("netd: lane клиента принят\n");
                            break;
                        }
                }
            }
            for (int i = 0; i < NLANE; i++)
                if (L[i].fd >= 0) {
                    int ready = 0;
                    for (int k = 1; k < nf; k++)
                        if (fds[k].fd == (int)L[i].fd && (fds[k].revents & 1))
                            ready = 1;
                    if (ready) lane_input(&L[i]);
                }
        }
        nic_poll();
        timers();
        if ((now_t & 0x3F) == 0) P("netd: жив (тик ");
        if ((now_t & 0x3F) == 0) { print_dec_n((long)now_t); P(")\n"); }
    }
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
