// /sbin/logsrv (M5): log-сервер как микроядерный сервис.
//
// Не делает write сам по себе никому, кроме UART: регистрируется в
// шине как "log" и обслуживает клиентов, которых шина приводит ему
// lane'ами. Полный цикл mbus-стиля:
//   REGISTER("log") -> [жду LaneRequest] -> обслуживаю lane до EOF.
// Клиенты пишут через IPC, сервер отвечает ack'ами и ведёт счётчик.

#include "milin.h"
#include "ipc.h"

static u64 served = 0;

static void serve_lane(long lane) {
    for (;;) {
        struct ml_msg m;
        char tail[ML_MAXTAIL];
        int hooks[ML_MAXHOOKS], nh;
        long r = ml_recv(lane, &m, tail, sizeof(tail), hooks, &nh);
        if (r <= 0) { sys3(SYS_close, lane, 0, 0); return; } // клиент ушёл

        if (m.proto == ML_PROTO_LOG && m.msg_id == LOG_WRITE) {
            if (m.tail_len) sys3(SYS_write, 1, (long)tail, m.tail_len);
            sys3(SYS_write, 1, (long)"\n", 1);
            served++;
            struct ml_msg resp = { ML_PROTO_LOG, LOG_WRITE_R, ML_E_OK, 0, {0} };
            *ml_h(&resp, 0) = m.tail_len; // stored_len
            ml_send(lane, &resp, 0, 0, 0, 0);
        } else if (m.proto == ML_PROTO_LOG && m.msg_id == LOG_STATS) {
            struct ml_msg resp = { ML_PROTO_LOG, LOG_STATS_R, ML_E_OK, 0, {0} };
            *ml_h(&resp, 0) = served;
            ml_send(lane, &resp, 0, 0, 0, 0);
        }
    }
}

void milin_main(void) {
    // шина стартует раньше, но форк не гарантирует порядок —
    // честно подожду (так же делает и клиент, и FUSE-клиент в M4)
    long bus = -1;
    for (int tries = 0; ; tries++) {
        bus = ml_connect("milin-bus");
        if (bus >= 0) break;
        if (tries > 200) {
            P("log-srv: шина не отвечает\n");
            sys3(SYS_exit, 1, 0, 0);
        }
        sleep_ms(10);
    }

    struct ml_msg m = { ML_PROTO_BUS, BUS_REGISTER, 0, 0, {0} };
    m_memcpy(m.head, "log", 4);
    if (ml_send(bus, &m, 0, 0, 0, 0) < 0) {
        P("log-srv: register: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    struct ml_msg resp;
    if (ml_recv(bus, &resp, 0, 0, 0, 0) <= 0 || resp.err != ML_E_OK) {
        P("log-srv: register отклонён\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("log-srv: зарегистрирован в шине как \"log\"\n");

    for (;;) {
        struct ml_msg m2;
        int hooks[ML_MAXHOOKS], nh = 0;
        // жду, пока шина приведёт клиента (LaneRequest + lane-хэндл)
        // жду, пока шина приведёт клиента (LaneRequest + lane-хэндл)
        if (ml_recv(bus, &m2, 0, 0, hooks, &nh) <= 0) break;
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_LANE_REQ && nh >= 1)
            serve_lane(hooks[0]);
    }
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
