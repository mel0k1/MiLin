// /sbin/mlbus (M5): реестр серверов — идея mbus из Managarm.
//
// Единственная работа шины — рандеву:
//   * сервер: RegisterRequest{имя} и остаётся на связи;
//   * клиент: LookupRequest{имя} -> шина создаёт socketpair и раздаёт
//     концы: клиенту LookupResponse{hooks:[lane]}, серверу LaneRequest.
// Дальше клиент и сервер общаются напрямую, шина данных не видит.
// Хэндлы (fd) передаются SCM_RIGHTS — как hooks в bragi.

#include "milin.h"
#include "ipc.h"

#define MAXREG 8

struct reg {
    int used;
    char name[24];
    long conn;      // соединение с сервером (его канал к шине)
};

static struct reg regs[MAXREG];

static void serve_lookup(struct ml_msg *m, long client) {
    const char *name = m->head;
    struct reg *r = 0;
    for (int i = 0; i < MAXREG; i++)
        if (regs[i].used && m_strncmp(regs[i].name, name, 24) == 0)
            r = &regs[i];

    if (!r) {
        P("mlbus: lookup ");
        print_str(name);
        P(": нет такой сущности (как NO_SUCH_ENTITY в mbus)\n");
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_NO_SUCH_ENTITY, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }

    // socketpair: [0] -> клиенту, [1] -> серверу. После отправки оба
    // конца живут у адресатов, шина свои копии закрывает.
    int sv[2];
    if (ml_socketpair(sv) < 0) {
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, 2 /*E*/, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }

    struct ml_msg rc = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_OK, 0, {0} };
    ml_send(client, &rc, 0, 0, (int[]){ sv[0] }, 1);

    struct ml_msg rs = { ML_PROTO_BUS, BUS_LANE_REQ, ML_E_OK, 0, {0} };
    ml_send(r->conn, &rs, 0, 0, (int[]){ sv[1] }, 1);

    sys3(SYS_close, sv[0], 0, 0);
    sys3(SYS_close, sv[1], 0, 0);
    P("mlbus: lookup ");
    print_str(name);
    P(" -> lane выдан (два конца, SCM_RIGHTS)\n");
}

void milin_main(void) {
    long lane = ml_listen("milin-bus");
    if (lane < 0) {
        P("mlbus: bind @milin-bus: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("mlbus: реестр lane'ов слушает @milin-bus\n");

    for (;;) {
        long c = ml_accept(lane);
        if (c < 0) continue;
        // обслуживаю подключения в цикле: REGISTER кладёт сервера в
        // реестр и держит его соединение открытым до конца работы шины
        for (;;) {
            struct ml_msg m;
            char tail[ML_MAXTAIL];
            int hooks[ML_MAXHOOKS], nh;
            long r = ml_recv(c, &m, tail, sizeof(tail), hooks, &nh);
            if (r <= 0) { sys3(SYS_close, c, 0, 0); break; }

            if (m.proto == ML_PROTO_BUS && m.msg_id == BUS_REGISTER) {
                int slot = -1;
                for (int i = 0; i < MAXREG; i++)
                    if (!regs[i].used) { slot = i; break; }
                struct ml_msg resp = { ML_PROTO_BUS, BUS_REGISTER_R,
                                       slot < 0 ? 2 : ML_E_OK, 0, {0} };
                ml_send(c, &resp, 0, 0, 0, 0);
                if (slot < 0) continue;
                regs[slot].used = 1;
                m_memcpy(regs[slot].name, m.head, 24);
                regs[slot].conn = c;
                P("mlbus: register \"");
                print_str(m.head);
                P("\"\n");
                // сервер держит это соединение; выходим из цикла чтения
                // (дальше сюда придут только LOOKUP других клиентов)
                break;
            } else if (m.proto == ML_PROTO_BUS && m.msg_id == BUS_LOOKUP) {
                serve_lookup(&m, c);
            }
        }
    }
    __builtin_unreachable();
}
