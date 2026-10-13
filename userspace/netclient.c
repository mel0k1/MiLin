// /sbin/netclient (M12): демон-клиент userspace-сети. Дергает сервис
// "net0" (netd): socket -> connect 10.0.2.2:8000 -> HTTP GET / ->
// печать первых байт ответа. Бут-гейт M12: в слайрпе хоста на 10.0.2.2
// слушает python http.server — TCP-стек netd ходит по настоящей сети.

#include "milin.h"
#include "ipc.h"

static int lookup_net(long bus, struct ml_aux *aux) {
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "net0", 5);
    *ml_h(&q, 2) = ML_TR_SEQPACKET;
    for (int tries = 0; tries < 100; tries++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) return -1;
        struct ml_msg resp;
        if (ml_recv(bus, &resp, 0, 0, aux) <= 0) return -1;
        if (resp.err == ML_E_OK && aux->nhooks >= 1) return 0;
        if (resp.err == ML_E_PERM) return -2;
        sleep_ms(20);
    }
    return -1;
}

// один запрос-ответ без ожидания: сервер отвечает R-сообщением.
// каждый шаг под poll с потолком: потере ответа (каскад шины) молча
// висеть нельзя — попытка обязана упасть и передать эстафету ретраю
static long net_rpc(long lane, u32 op, u32 h0, u32 h1, u32 h2,
                    const u8 *tail, u32 tlen, struct ml_msg *out) {
    struct ml_msg m = { ML_PROTO_NET, op, 0, 0, {0} };
    *ml_h(&m, 0) = h0; *ml_h(&m, 1) = h1; *ml_h(&m, 2) = h2;
    if (ml_send(lane, &m, tail, tlen, 0, 0) < 0) return -1;
    // R-сообщение; push'и (DATA/EOF) здесь не ждём — их читает recv-цикл
    for (;;) {
        struct pfd { int fd; short events, revents; } pf = {
            (int)lane, 1, 0
        };
        if (sys5(SYS_poll, (long)&pf, 1, 5000, 0, 0) <= 0) return -1;
        long r = ml_recv(lane, out, 0, 0, 0);
        if (r <= 0) return -1;
        u32 id = out->msg_id;
        if (id == op + 1) return 0;
        if (id == NET_DATA || id == NET_EOF) continue;
        return -1;
    }
}

static u8 rbuf[8192];
static u32 rxlen;

// одна HTTP-сессия: lookup lane -> OPEN -> CONNECT -> GET -> ответ.
// 1 — не сошлось (попытку можно повторить: шина и netd переподнимаются)
static int http_once(long bus) {
    struct ml_aux aux;
    if (lookup_net(bus, &aux) < 0) {
        P("netclient: lookup \"net0\" FAIL\n");
        return 1;
    }
    long lane = aux.hooks[0];
    P("netclient: lane \"net0\" получен\n");

    struct ml_msg r;
    if (net_rpc(lane, NET_OPEN, 1, 0, 0, 0, 0, &r) < 0 || r.err != ML_E_OK) {
        P("netclient: OPEN FAIL\n");
        return 1;
    }
    u32 sid = (u32)*ml_h(&r, 0);

    // 10.0.2.2:8000 (slirp -> host 127.0.0.1:8000)
    if (net_rpc(lane, NET_CONNECT, sid, 0x0A000202u, 8000, 0, 0, &r) < 0
        || r.err != ML_E_OK) {
        P("netclient: CONNECT FAIL\n");
        return 1;
    }
    P("netclient: TCP 10.0.2.2:8000 установлен\n");

    const char *req = "GET / HTTP/1.0\r\nHost: 10.0.2.2\r\n\r\n";
    if (net_rpc(lane, NET_SEND, sid, 0, 0, (const u8 *)req,
                m_strlen(req), &r) < 0 || r.err != ML_E_OK) {
        P("netclient: SEND FAIL\n");
        return 1;
    }

    int got = 0, eof = 0;
    rxlen = 0;
    for (int t = 0; t < 200 && !eof; t++) {
        // poll с таймаутом: тупой сервер не подвесит гейт навсегда
        struct pfd { int fd; short events, revents; } pf = {
            (int)lane, 1, 0
        };
        if (sys5(SYS_poll, (long)&pf, 1, 200, 0, 0) <= 0) continue;
        struct ml_msg m;
        long n = ml_recv(lane, &m, rbuf + rxlen,
                         sizeof(rbuf) - rxlen, 0);
        if (n <= 0) break;
        if (m.msg_id == NET_DATA && m.tail_len) {
            rxlen += m.tail_len;
            got = 1;
        } else if (m.msg_id == NET_EOF) {
            eof = 1;
        }
    }
    if (!got || rxlen < 12) {
        P("netclient: ответ не получен\n");
        return 1;
    }
    // HTTP/1.0 200 (или 1.1)
    int ok200 = (rbuf[0] == 'H') && rbuf[9] == '2' && rbuf[11] == '0';
    P("netclient: HTTP-ответ: ");
    for (u32 i = 0; i < 15 && i < rxlen; i++) {
        char c = (char)rbuf[i];
        if (c == '\r' || c == '\n') break;
        sys3(SYS_write, 1, (long)&c, 1);
    }
    P("\n");
    if (ok200) {
        P("M12 OK: userspace TCP/IP + HTTP через netd\n");
        return 0;
    }
    P("netclient: неожиданный ответ\n");
    return 1;
}

void milin_main(void) {
    // M7.2-каскад убивает шину параллельно с нами: транзакция может умереть
    // в любой фазе. Попытка = свежий коннект к шине + свежий lane + новый TCP
    for (int attempt = 0; attempt <= 3; attempt++) {
        if (attempt) sleep_ms(200);
        long bus = -1;
        for (int tries = 0; tries <= 200; tries++) {
            bus = ml_connect("milin-bus");
            if (bus >= 0) break;
            sleep_ms(10);
        }
        if (bus < 0) { P("netclient: шина недоступна\n"); continue; }
        if (http_once(bus) == 0) sys3(SYS_exit, 0, 0, 0);
    }
    sys3(SYS_exit, 1, 0, 0);
    __builtin_unreachable();
}
