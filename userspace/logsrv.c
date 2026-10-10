// /sbin/logsrv (M5 + M5.1): log-сервер как микроядерный сервис.
//
// Регистрируется в шине как "log" и обслуживает клиентов, которых шина
// приводит lane'ами. Полный цикл mbus-стиля:
//   REGISTER("log") -> [жду LaneRequest] -> обслуживаю lane до конца.
//
// Два транспорта lane'ов (выбирает клиент в LOOKUP):
//   * ML_TR_SEQPACKET (M5): bragi-сообщения поверх SOCK_SEQPACKET;
//     SO_PASSCRED на своём конце lane'а — ядро прикладывает к каждому
//     сообщению struct ucred, подделать его нельзя;
//   * ML_TR_HELQ (M5.1): асинхронные очереди в разделяемой памяти
//     (userspace/helq.h): элементы читаются прямо из общей памяти
//     (zero copy), будильник — futex. Очередь — голая память, ядро
//     сообщения не видит, поэтому uid приходит от шины (LaneRequest):
//     она получила его от ядра через SCM_CREDENTIALS при LOOKUP.

#include "milin.h"
#include "ipc.h"
#include "helq.h"

static u64 served = 0;

// ack/счётчик: ответы уходят либо в очередь (out), либо в сокет (lane)
static void log_ack(u32 req_id, u32 tlen, struct mlq_out *out, long lane) {
    if (req_id == LOG_WRITE || req_id == LOG_WRITEQ) {
        struct ml_msg resp = { ML_PROTO_LOG, LOG_WRITE_R, ML_E_OK, 0, {0} };
        *ml_h(&resp, 0) = tlen; // stored_len
        if (out) mlq_put(out, LOG_WRITE_R, &resp, sizeof(resp), MLQ_FUTEX);
        else if (lane >= 0) ml_send(lane, &resp, 0, 0, 0, 0);
    } else if (req_id == LOG_STATS) {
        struct ml_msg resp = { ML_PROTO_LOG, LOG_STATS_R, ML_E_OK, 0, {0} };
        *ml_h(&resp, 0) = served;
        if (out) mlq_put(out, LOG_STATS_R, &resp, sizeof(resp), MLQ_FUTEX);
        else if (lane >= 0) ml_send(lane, &resp, 0, 0, 0, 0);
    }
}

// лог-записи подписываются личностью клиента: uid приходит либо от ядра
// (SCM_CREDENTIALS), либо от шины, получившей его от ядра. uid=0 — creds
// не помечены (недоверенно), печатаем пометку.
static void log_print(u32 uid, const char *tail, u32 tlen) {
    P("log-srv: ");
    if (uid) {
        P("uid=");
        print_dec_n(uid);
        P(": ");
    } else {
        P("uid=? (без creds): ");
    }
    if (tlen) sys3(SYS_write, 1, (long)tail, tlen);
    sys3(SYS_write, 1, (long)"\n", 1);
}

// --- M5: lane поверх SOCK_SEQPACKET (до EOF) ---
static void serve_lane(long lane) {
    // creds: ядро будет прикладывать ucred к каждому сообщению клиента
    ml_passcred(lane);
    for (;;) {
        struct ml_msg m;
        char tail[ML_MAXTAIL];
        struct ml_aux aux;
        long r = ml_recv(lane, &m, tail, sizeof(tail), &aux);
        if (r <= 0) { sys3(SYS_close, lane, 0, 0); return; } // клиент ушёл
        if (m.proto != ML_PROTO_LOG) continue;
        u32 uid = aux.has_cred ? aux.cred.uid : 0;
        if (m.msg_id == LOG_WRITE)
            log_print(uid, tail, m.tail_len);
        if (m.msg_id == LOG_WRITE || m.msg_id == LOG_WRITEQ) served++;
        log_ack(m.msg_id, m.tail_len, 0, lane);
    }
}

// --- M5.1: lane поверх асинхронных очередей в разделяемой памяти ---
static void serve_queue_lane(struct mlq_pair *p, u32 uid, u32 pid) {
    struct mlq_in in;
    mlq_in_init(&in, p, 0);            // запросы: направление 0
    struct mlq_out out;
    mlq_out_init(&out, p, 1);          // ответы: направление 1
    (void)pid;

    for (;;) {
        struct mlq_view v;
        if (!mlq_next(&in, &v, MLQ_FUTEX)) continue;
        if (v.len < sizeof(struct ml_msg)) continue;
        const struct ml_msg *m = (const struct ml_msg *)v.payload; // zero copy
        if (m->proto != ML_PROTO_LOG) continue;

        if (m->msg_id == LOG_CLOSE) {
            // хвост: ack на CLOSE — и выходим (клиент завершает сессию)
            struct ml_msg resp = { ML_PROTO_LOG, LOG_CLOSE_R, ML_E_OK, 0, {0} };
            *ml_h(&resp, 0) = served;
            mlq_put(&out, LOG_CLOSE_R, &resp, sizeof(resp), MLQ_FUTEX);
            mlq_flush(&out);
            return;
        }
        // tail элемента лежит сразу за bragi-сообщением в общей памяти
        const char *tail = v.payload + sizeof(struct ml_msg);
        u32 tlen = v.len - (u32)sizeof(struct ml_msg);
        if (m->msg_id == LOG_WRITE) {
            log_print(uid, tail, tlen);
            served++;
        } else if (m->msg_id == LOG_WRITEQ) {
            served++; // тихий WRITE бенчмарка: только счёт
        }
        log_ack(m->msg_id, tlen, &out, -1);
        // ответы публикуем сразу: клиент ждёт ack на каждый запрос
        mlq_flush(&out);
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
    if (ml_recv(bus, &resp, 0, 0, 0) <= 0 || resp.err != ML_E_OK) {
        P("log-srv: register отклонён\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("log-srv: зарегистрирован в шине как \"log\"\n");

    for (;;) {
        struct ml_msg m2;
        struct ml_aux aux;
        // жду, пока шина приведёт клиента (LaneRequest + lane-хэндл)
        if (ml_recv(bus, &m2, 0, 0, &aux) <= 0) break;
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_LANE_REQ && aux.nhooks >= 1) {
            u32 transport = (u32)*ml_h(&m2, 2);
            if (transport == ML_TR_HELQ && aux.nhooks >= 2) {
                struct mlq_pair *p = mlq_map(aux.hooks[1]);
                if (!p) {
                    P("log-srv: helq-регион: mmap FAIL\n");
                    sys3(SYS_close, aux.hooks[0], 0, 0);
                    sys3(SYS_close, aux.hooks[1], 0, 0);
                    continue;
                }
                serve_queue_lane(p, (u32)*ml_h(&m2, 1), (u32)*ml_h(&m2, 0));
                sys6(SYS_munmap, (long)p, MLQ_SIZE, 0, 0, 0, 0);
                sys3(SYS_close, aux.hooks[1], 0, 0);
                sys3(SYS_close, aux.hooks[0], 0, 0);
                P("log-srv: helq-lane закрыт\n");
            } else {
                serve_lane(aux.hooks[0]);
            }
        }
    }
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
