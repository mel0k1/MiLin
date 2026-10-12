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
//
// M7 (capabilities): сервер работает с минимальными привилегиями —
// setuid(1000) сразу на старте: log-серверу не нужен root ни для чего.
// При REGISTER сервер объявляет шине свою политику прав (tail: маски
// world/root); шина выдаёт lane клиенту только по ней, а маску
// доставляет серверу в LaneRequest. Каждая операция enforced'ится:
// WRITE/WRITEQ -> ML_CAP_WRITE, STATS -> ML_CAP_STATS, CLOSE ->
// ML_CAP_CLOSE, RESET (админ: сброс счётчика) -> ML_CAP_ADMIN.
// Нарушение — ответ ML_E_PERM, операция не выполняется.

#include "milin.h"
#include "ipc.h"
#include "helq.h"

static u64 served = 0;

// --- M7: capability-политика log-сервера ---
// мир: писать, спрашивать счётчик, закрывать сессию; root: плюс админ
#define LOG_WORLD (ML_CAP_WRITE | ML_CAP_STATS | ML_CAP_CLOSE)
#define LOG_ROOT  (LOG_WORLD | ML_CAP_ADMIN)

// какая операция какое право требует (0 = разрешено всем)
static u32 need_cap(u32 msg_id) {
    if (msg_id == LOG_WRITE || msg_id == LOG_WRITEQ) return ML_CAP_WRITE;
    if (msg_id == LOG_STATS) return ML_CAP_STATS;
    if (msg_id == LOG_RESET) return ML_CAP_ADMIN;
    if (msg_id == LOG_CLOSE) return ML_CAP_CLOSE;
    return 0;
}

// id ответа на запрос (WRITEQ отвечает WRITE_R)
static u32 resp_id(u32 msg_id) {
    if (msg_id == LOG_WRITEQ) return LOG_WRITE_R;
    return msg_id + 1;
}

// отказ по правам: единый ответ ML_E_PERM на обоих транспортах
static void cap_deny(u32 msg_id, u32 need, u32 rights, struct mlq_out *out,
                     long lane) {
    P("log-srv: отказ msg ");
    print_dec_n(msg_id);
    P(": нет права 0x");
    print_hex_flat(need);
    P(" (lane 0x");
    print_hex_flat(rights);
    P(")\n");
    struct ml_msg resp = { ML_PROTO_LOG, resp_id(msg_id), ML_E_PERM, 0, {0} };
    if (out) {
        mlq_put(out, resp.msg_id, &resp, sizeof(resp), MLQ_FUTEX);
        mlq_flush(out);
    } else if (lane >= 0) {
        ml_send(lane, &resp, 0, 0, 0, 0);
    }
}

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
    } else if (req_id == LOG_RESET) {
        struct ml_msg resp = { ML_PROTO_LOG, LOG_RESET_R, ML_E_OK, 0, {0} };
        *ml_h(&resp, 0) = served;
        if (out) mlq_put(out, LOG_RESET_R, &resp, sizeof(resp), MLQ_FUTEX);
        else if (lane >= 0) ml_send(lane, &resp, 0, 0, 0, 0);
    }
}

// uid=0 — creds не помечены (недоверенно), печатаем пометку.
// большие tail'ы (M8, blob-путь) в UART не льём: первые 64 байта + размер
static void log_print(u32 uid, const char *tail, u32 tlen) {
    P("log-srv: ");
    if (uid) {
        P("uid=");
        print_dec_n(uid);
        P(": ");
    } else {
        P("uid=? (без creds): ");
    }
    u32 n = tlen > 64 ? 64 : tlen;
    if (n) sys3(SYS_write, 1, (long)tail, n);
    if (tlen > n) {
        P("...(+");
        print_dec_n(tlen - n);
        P(" байт)");
    }
    sys3(SYS_write, 1, (long)"\n", 1);
}

// --- M5: lane поверх SOCK_SEQPACKET (до EOF) ---
static void serve_lane(long lane, u32 rights) {
    // creds: ядро будет прикладывать ucred к каждому сообщению клиента
    ml_passcred(lane);
    for (;;) {
        struct ml_msg m;
        char tail[ML_MAXTAIL];
        struct ml_aux aux;
        long r = ml_recv(lane, &m, tail, sizeof(tail), &aux);
        if (r <= 0) {
            // M7.1: клиент ушёл или умер — права на lane с ним умирают
            if (r < 0)
                P("log-srv: seqpacket: битый пакет — capability отозвана (lane закрыт)\n");
            else
                P("log-srv: seqpacket: клиент отключился — capability отозвана (EOF)\n");
            sys3(SYS_close, lane, 0, 0);
            return;
        }
        if (m.proto != ML_PROTO_LOG) continue;
        u32 uid = aux.has_cred ? aux.cred.uid : 0;

        // M7: per-op enforcement — права выдала шина при создании lane'а
        u32 need = need_cap(m.msg_id);
        if (need && !(rights & need)) {
            cap_deny(m.msg_id, need, rights, 0, lane);
            continue;
        }
        if (m.msg_id == LOG_RESET) {
            P("log-srv: счётчик сброшен (ADMIN, uid ");
            print_dec_n(uid);
            P(")\n");
            served = 0;
        }
        if (m.msg_id == LOG_WRITE)
            log_print(uid, tail, m.tail_len);
        if (m.msg_id == LOG_WRITE || m.msg_id == LOG_WRITEQ) served++;
        log_ack(m.msg_id, m.tail_len, 0, lane);
    }
}

// --- M5.1: lane поверх асинхронных очередей в разделяемой памяти ---
static void serve_queue_lane(struct mlq_pair *p, u32 uid, u32 pid, u32 rights) {
    struct mlq_in in;
    mlq_in_init(&in, p, 0);            // запросы: направление 0
    struct mlq_out out;
    mlq_out_init(&out, p, 1);          // ответы: направление 1

    for (;;) {
        struct mlq_view v;
        // M7.1: сторож 50мс — мёртвый клиент в shm невидим (EOF не бывает)
        if (!mlq_next(&in, &v, MLQ_WATCHDOG)) {
            // sig0: зомби жив, поэтому родня обязана реапнуть; ESRCH = умер
            if (sys3(SYS_kill, pid, 0, 0) != 0) {
                P("log-srv: helq: клиент uid ");
                print_dec_n(uid);
                P(" (pid ");
                print_dec_n(pid);
                P(") умер — capability REVOKED, lane закрыт\n");
                return;
            }
            continue;
        }
        if (v.len >= sizeof(struct ml_msg)) {
            const struct ml_msg *m = (const struct ml_msg *)v.payload; // zero copy
            if (m->proto == ML_PROTO_LOG) {
                // M7: per-op enforcement — права выдала шина (LaneRequest tail)
                u32 need = need_cap(m->msg_id);
                int denied = need && !(rights & need);
                u32 mid = m->msg_id;
                u32 tlen = v.len - (u32)sizeof(struct ml_msg);
                if (!denied) {
                    if (mid == LOG_RESET) {
                        P("log-srv: счётчик сброшен (ADMIN, uid ");
                        print_dec_n(uid);
                        P(", helq)\n");
                        served = 0;
                    }
                    // tail элемента лежит сразу за bragi-сообщением (inline
                    // — в чанке, крупный — в blob-слотах; указатель один)
                    if (mid == LOG_WRITE) {
                        log_print(uid, v.payload + sizeof(struct ml_msg), tlen);
                        served++;
                    } else if (mid == LOG_WRITEQ) {
                        served++; // тихий WRITE бенчмарка: только счёт
                    }
                }
                // M8.1: запрос освобождаем ДО ответа. Слоты/чанк запроса —
                // ресурс того же пула, из которого берётся чанк ответа;
                // ответ до освобождения = flow-control дедлок на конвейере
                // (сервер ждёт чанк, клиент — ack, чанк вернёт только recv)
                mlq_consume(&in, &v);
                if (denied) {
                    cap_deny(mid, need, rights, &out, -1);
                    continue;
                }
                if (mid == LOG_CLOSE) {
                    // хвост: ack на CLOSE — и выходим (клиент завершает сессию)
                    struct ml_msg resp = { ML_PROTO_LOG, LOG_CLOSE_R, ML_E_OK, 0, {0} };
                    *ml_h(&resp, 0) = served;
                    mlq_put(&out, LOG_CLOSE_R, &resp, sizeof(resp), MLQ_FUTEX);
                    mlq_flush(&out);
                    return;
                }
                log_ack(mid, tlen, &out, -1);
                // ответы публикуем сразу: клиент ждёт ack на каждый запрос
                mlq_flush(&out);
                continue;
            }
        }
        mlq_consume(&in, &v); // blob-слот вернуть producer'у (inline — no-op)
    }
}

// коннект к шине с ожиданием старта + REGISTER "log" с политикой прав;
// возвращаемый fd шины или -1. Используется и на старте, и после смерти
// шины (M7.2: каскад — каждый зависимый сервис сам встаёт в строй)
static long bus_register(void) {
    long bus = -1;
    for (int tries = 0; ; tries++) {
        bus = ml_connect("milin-bus");
        if (bus >= 0) break;
        if (tries > 2000) return -1;
        sleep_ms(10);
    }
    struct ml_msg m = { ML_PROTO_BUS, BUS_REGISTER, 0, 0, {0} };
    m_memcpy(m.head, "log", 4);
    u32 masks[2] = { LOG_WORLD, LOG_ROOT };
    if (ml_send(bus, &m, masks, sizeof(masks), 0, 0) < 0) {
        sys3(SYS_close, bus, 0, 0);
        return -1;
    }
    struct ml_msg resp;
    if (ml_recv(bus, &resp, 0, 0, 0) <= 0 || resp.err != ML_E_OK) {
        sys3(SYS_close, bus, 0, 0);
        return -1;
    }
    return bus;
}

void milin_main(void) {
    // M7: наименьшие привилегии — log-сервер не делает ничего, для чего
    // нужен root. Заодно клиент (тоже uid 1000) может его убить в демо
    // отказа — kill разрешён между процессами одного uid.
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("log-srv: setuid(1000): FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("log-srv: привилегии сброшены (uid ");
    print_dec_n(sys3(SYS_getuid, 0, 0, 0));
    P(")\n");

    long bus = bus_register();
    if (bus < 0) {
        P("log-srv: шина не отвечает\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("log-srv: зарегистрирован в шине как \"log\"\n");

    for (;;) {
        struct ml_msg m2;
        char req_tail[8];
        struct ml_aux aux;
        // жду, пока шина приведёт клиента (LaneRequest + lane-хэндл + права)
        if (ml_recv(bus, &m2, req_tail, sizeof(req_tail), &aux) <= 0) {
            // M7.2: шина умерла — регистрация умерла вместе с ней, а мой
            // state (счётчик, права) — мой. Каскад: переподключаюсь и
            // встаю в строй сам, супервизору перезапускать меня не нужно
            P("log-srv: шина умерла — перерегистрация (счётчик пережил: ");
            print_dec_n(served % 1000000);
            P(")\n");
            sys3(SYS_close, bus, 0, 0);
            bus = bus_register();
            if (bus < 0) {
                P("log-srv: шина не вернулась\n");
                sys3(SYS_exit, 1, 0, 0);
            }
            P("log-srv: перерегистрирован в новой шине\n");
            continue;
        }
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_REVOKE) {
            // M7.1: шина — владелец прав, она подтверждает смерть клиента;
            // свою часть (сторож очереди) сервер уже сделал или сделает
            P("log-srv: шина: revoke клиента pid ");
            print_dec_n((long)*ml_h(&m2, 0));
            P("\n");
            continue;
        }
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_LANE_REQ && aux.nhooks >= 1) {
            u32 transport = (u32)*ml_h(&m2, 2);
            // M7: права на этот lane — шина передала их в tail
            u32 rights = 0;
            if (m2.tail_len >= 4) m_memcpy(&rights, req_tail, 4);
            u32 cl_uid = (u32)*ml_h(&m2, 1);
            P("log-srv: lane: клиент uid ");
            print_dec_n(cl_uid);
            P(", права 0x");
            print_hex_flat(rights);
            P("\n");
            if (transport == ML_TR_HELQ && aux.nhooks >= 2) {
                struct mlq_pair *p = mlq_map(aux.hooks[1]);
                if (!p) {
                    P("log-srv: helq-регион: mmap FAIL\n");
                    sys3(SYS_close, aux.hooks[0], 0, 0);
                    sys3(SYS_close, aux.hooks[1], 0, 0);
                    continue;
                }
                serve_queue_lane(p, cl_uid, (u32)*ml_h(&m2, 0), rights);
                sys6(SYS_munmap, (long)p, MLQ_SIZE, 0, 0, 0, 0);
                sys3(SYS_close, aux.hooks[1], 0, 0);
                sys3(SYS_close, aux.hooks[0], 0, 0);
                P("log-srv: helq-lane закрыт\n");
            } else {
                serve_lane(aux.hooks[0], rights);
            }
        }
    }
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
