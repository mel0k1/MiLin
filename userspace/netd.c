// /sbin/netd (M12): userspace-сетевой стек — NIC-драйвер + TCP/IP + сервис.
//
// Архитектура микроядра: в ядре нет ни стека, ни драйвера NIC. netd —
// userspace-сервер: e1000 через UIO (BAR0 в mmap, кольца/буферы в
// pinned-DMA регионе /dev/mlpinned — NIC обязан работать с физикой),
// поверх — ARP/IPv4/ICMP/TCP и сервис "net0" (ML_PROTO_NET) на шине:
// клиенты открывают сокеты, соединяются, шлют данные — всё lane'ами.
// QEMU: -device e1000,netdev=n0 -netdev user,id=n0 (10.0.2.15 <- slirp).

#include "milin.h"
#include "ipc.h"

#define SYS_ioctl        16
#define O_RDWR           2
#define O_RDONLY         0

// --- адресация (slirp по умолчанию) ---
#define NET_MY_IP   0x0A00020Fu     // 10.0.2.15
#define NET_GW_IP   0x0A000202u     // 10.0.2.2
#define NET_MASK    0xFFFFFF00u
#define ETH_HDR     14
#define ETH_MTU     1500

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
static u8 *tx_frame(void) { return dma + txbuf((int)tx_cur); }

static int nic_tx_commit(u32 len) {
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

static void nic_poll(void) {
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
    if (spa && (spa & NET_MASK) == (NET_MY_IP & NET_MASK)) {
        ac_ip = spa; m_memcpy(ac_mac, p + 8, 6); ac_ok = 1;
    }
    if (oper == 1 && rbe32(p + 24) == NET_MY_IP) {
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
static int ip_tx(u32 dst, u8 proto, const u8 *payload, u32 plen) {
    u8 macdst[6];
    u32 nh = ((dst & NET_MASK) == (NET_MY_IP & NET_MASK)) ? dst : NET_GW_IP;
    if (arp_get(nh, macdst) < 0) return -1;
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
    wbe32(ip + 12, NET_MY_IP);
    wbe32(ip + 16, dst);
    wbe16(ip + 10, 0);
    u32 s = csum_run(ip, 20, 0);
    wbe16(ip + 10, csum_fin(s));
    m_memcpy(ip + 20, payload, plen);
    return nic_tx_commit((u32)(ETH_HDR + 20 + plen));
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
    if (dst != NET_MY_IP) return;
    // длина — из IP total length: slirp добивает короткие кадры нулями
    // до 60 байт (Ethernet минимум), нули не должны попадать в поток
    u32 plen = (u32)((ip[2] << 8) | ip[3]);
    u32 ihl = (u32)(ip[0] & 0x0F) * 4;
    if (plen < ihl || plen > n - ETH_HDR) plen = n - ETH_HDR;
    if (ip[9] == 1) icmp_input(ip, plen);
    else if (ip[9] == 6) tcp_input(ip, ip + ihl, plen - ihl);
}

static int nic_init(void) {
    // e1000 в UIO: /dev/uioN, чей PCI vendor/device = 0x8086/0x100e
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
        int ok = 1;
        // /sys/class/uio/uioN/device/{vendor,device}
        const char *sys = "/sys/class/uio/uio /device/vendor";
        m_memcpy(path, sys, 34);
        path[18] = (char)('0' + i);
        path[34] = 0;
        long v = sys3(SYS_open, (long)path, O_RDONLY, 0);
        if (v < 0) {
            P("netd: ");
            print_str(path);
            P(": vendor недоступен\n");
            ok = 0;
        } else {
            long n = sys3(SYS_read, v, (long)buf, 12);
            sys3(SYS_close, v, 0, 0);
            if (n < 6 || buf[2] != '8' || buf[3] != '0' || buf[4] != '8'
                || buf[5] != '6') ok = 0;
        }
        if (ok) {
            const char *dev = "/sys/class/uio/uio /device/device";
            m_memcpy(path, dev, 34);
            path[18] = (char)('0' + i);
            path[34] = 0;
            v = sys3(SYS_open, (long)path, O_RDONLY, 0);
            if (v < 0) ok = 0;
            else {
                long n = sys3(SYS_read, v, (long)buf, 12);
                sys3(SYS_close, v, 0, 0);
                if (n < 6 || buf[2] != '1' || buf[3] != '0' || buf[4] != '0'
                    || buf[5] != 'e') ok = 0;
            }
        }
        if (!ok) { sys3(SYS_close, fd, 0, 0); continue; }
        // BUS MASTER обязателен: uio_pci_generic не ставит бит сам,
        // без него DMA NIC'а уходит в никуда (MMIO при этом работает)
        const char *cfg = "/sys/class/uio/uio /device/config";
        m_memcpy(path, cfg, 34);
        path[18] = (char)('0' + i);
        path[34] = 0;
        long cf = sys3(SYS_open, (long)path, O_RDWR, 0);
        if (cf >= 0) {
            u16 cmd = 0;
            sys3(SYS_lseek, cf, 4, 0);          // PCI_COMMAND
            if (sys3(SYS_read, cf, (long)&cmd, 2) == 2) {
                cmd |= 0x0007;                  // IO | MEM | MASTER
                sys3(SYS_lseek, cf, 4, 0);
                sys3(SYS_write, cf, (long)&cmd, 2);
            }
            sys3(SYS_close, cf, 0, 0);
        }
        uiofd = fd;
        break;
    }
    if (uiofd < 0) {
        P("netd: /dev/uioN не найден\n");
        return -1;
    }

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

    if (nic_init() < 0) {
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
    P("netd: сервис \"net0\" (10.0.2.15 <- slirp)\n");

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
