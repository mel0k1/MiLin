// /sbin/gputest (M22): e2e mlkd-фасада amdgpu из независимого клиента.
// Раньше фазу kernel/user крутил /init (gpu_demo в супервизоре) — placement
// переехал сюда: у фасада один владелец, гонок с демо шины нет. Фазы:
//   kernel : /dev/amdgpu INFO -> ALLOC -> SUBMIT -> WAIT -> FREE (kthread)
//   deny   : placement=user -> /dev ALLOC = -ENODEV (домен уступил gpud)
//   caps   : fork+setuid(1000): LOOKUP "gpu.amdgpu" = ML_E_PERM (world=0)
//   user   : LOOKUP от root -> INFO/ALLOC/SUBMIT/WAIT/FREE у gpud
//   return : placement=kernel -> /dev ожил, gpud уступил (ML_E_PERM)
// Шину клиент.c убивает в демо каскада M7.2 — каждая шинная фаза умеет
// переподключиться и начаться заново.
// Маркеры: M22 GPU DEV/DENYDEV/CAPDENY/LANE/RETURN OK, итог M22 GPU E2E OK.

#include "milin.h"
#include "ipc.h"
#include "mlgpu.h"

static char rbuf[512];

static long bus_conn(void) {
    for (int tries = 0;; tries++) {
        long bus = ml_connect("milin-bus");
        if (bus >= 0) return bus;
        if (tries > 400) return -1;
        sleep_ms(25);
    }
}

// запрос через /dev: [op][payload] -> ответ (длина или -код ядра)
static long dev_req(long fd, u8 op, const void *payload, u32 plen) {
    static u8 req[1 + sizeof(struct mlgpu_submit_in) + 8 * 4];
    req[0] = op;
    if (plen) m_memcpy(req + 1, payload, plen);
    long w = sys3(SYS_write, fd, (long)req, 1 + plen);
    if (w != (long)(1 + plen)) return w;
    return sys3(SYS_read, fd, (long)rbuf, sizeof(rbuf));
}

// запрос через шину: GPU_REQ(tail=[op][payload]) -> GPU_REQ_R
static long lane_err;

static long lane_req(long lane, u8 op, const void *payload, u32 plen) {
    static u8 tbuf[512];
    struct ml_msg q = { ML_PROTO_GPU, GPU_REQ, 0, 0, {0} };
    tbuf[0] = (u8)op;
    if (plen) m_memcpy(tbuf + 1, payload, plen);
    if (ml_send(lane, &q, tbuf, 1 + plen, 0, 0) < 0) return -1;
    struct ml_msg resp;
    struct ml_aux aux;
    long r = ml_recv(lane, &resp, rbuf, sizeof(rbuf), &aux);
    if (r <= 0 || resp.proto != ML_PROTO_GPU || resp.msg_id != GPU_REQ_R) {
        lane_err = -1;
        return -1;
    }
    lane_err = (long)resp.err;
    return resp.err == ML_E_OK ? (long)resp.tail_len : -1;
}

static long lookup_gpu_lane(long bus) {
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "gpu.amdgpu", 11);
    *ml_h(&q, 2) = ML_TR_SEQPACKET;
    for (int tries = 0; tries < 100; tries++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) return -1;
        struct ml_msg resp;
        struct ml_aux aux;
        if (ml_recv(bus, &resp, 0, 0, &aux) <= 0) return -1;
        if (resp.err == ML_E_OK && aux.nhooks >= 1) return aux.hooks[0];
        if (resp.err == ML_E_PERM) return -2; // прав нет — ретраи не помогут
        sleep_ms(20);
    }
    return -1;
}

static long gpu_place(const char *pl, long plen) {
    long pf = sys3(SYS_open, (long)"/sys/class/mlkd/amdgpu/placement",
                   O_WRONLY, 0);
    if (pf < 0) return -1;
    long r = sys3(SYS_write, pf, (long)pl, plen);
    sys3(SYS_close, pf, 0, 0);
    return r == plen ? 0 : -1;
}

// полный цикл ALLOC -> SUBMIT -> WAIT -> FREE через указанный транспорт
static int gpu_cycle(long fd, long lane, u32 alloc_size) {
    struct mlgpu_alloc_in a = { alloc_size };
    long r = lane ? lane_req(lane, MLGPU_ALLOC, &a, sizeof(a))
                  : dev_req(fd, MLGPU_ALLOC, &a, sizeof(a));
    if (r != sizeof(struct mlgpu_alloc_out)) return 1;
    u64 h;
    m_memcpy(&h, rbuf, sizeof(h));

    struct { struct mlgpu_submit_in s; u32 cmd[4]; } sp;
    sp.s.handle = h;
    sp.s.ndw = 4;
    sp.s.pad = 0;
    u32 cmd[4] = { 1, 2, 3, 4 };
    m_memcpy(sp.cmd, cmd, sizeof(cmd));
    r = lane ? lane_req(lane, MLGPU_SUBMIT, &sp, sizeof(sp))
             : dev_req(fd, MLGPU_SUBMIT, &sp, sizeof(sp));
    if (r != sizeof(struct mlgpu_submit_out)) return 1;
    u64 fence;
    m_memcpy(&fence, rbuf, sizeof(fence));
    if (!fence) return 1;

    struct mlgpu_wait_in w = { fence };
    r = lane ? lane_req(lane, MLGPU_WAIT, &w, sizeof(w))
             : dev_req(fd, MLGPU_WAIT, &w, sizeof(w));
    if (r != sizeof(struct mlgpu_wait_out)) return 1;
    u32 sig;
    m_memcpy(&sig, rbuf, 4);
    if (sig != 1) return 1;

    struct mlgpu_free_in f = { h };
    r = lane ? lane_req(lane, MLGPU_FREE, &f, sizeof(f))
             : dev_req(fd, MLGPU_FREE, &f, sizeof(f));
    return r != 0;
}

static int info_ok(long fd, long lane) {
    long r = lane ? lane_req(lane, MLGPU_INFO, 0, 0) : dev_req(fd, MLGPU_INFO, 0, 0);
    if (r != sizeof(struct mlgpu_info_out)) return 0;
    struct mlgpu_info_out o;
    m_memcpy(&o, rbuf, sizeof(o));
    return m_strncmp(o.name, "amdgpu", 7) == 0 && o.api == MLGPU_API;
}

// фаза kernel: полный цикл через kthread домена; 0 = OK
static int phase_kernel(long fd) {
    if (!info_ok(fd, 0)) {
        P("gputest: info (dev): FAIL\n");
        return 1;
    }
    if (gpu_cycle(fd, 0, 8192)) {
        P("gputest: cycle (dev): FAIL\n");
        return 1;
    }
    P("M22 GPU DEV OK\n");
    return 0;
}

// фаза deny: домен в 'user' отклоняет /dev-запросы (-ENODEV = 19)
static int phase_deny(long fd) {
    struct mlgpu_alloc_in a = { 4096 };
    if (dev_req(fd, MLGPU_ALLOC, &a, sizeof(a)) != -19) {
        P("gputest: deny (dev): запрос НЕ отклонён — дыра в политике!\n");
        return 1;
    }
    P("M22 GPU DENYDEV OK\n");
    return 0;
}

// фаза caps: uid 1000 lane не получает (политика gpud: world=0)
// 0 = OK (ребёнок подтвердил отказ); шинные проблемы ретраятся снаружи
static int phase_caps(long bus) {
    int pid = sys3(SYS_fork, 0, 0, 0);
    if (pid == 0) {
        sys3(SYS_setuid, 1000, 0, 0);
        long l = lookup_gpu_lane(bus);
        sys3(SYS_exit, l == -2 ? 0 : 1, 0, 0);
    }
    int st = 0;
    sys5(SYS_wait4, pid, (long)&st, 0, 0, 0);
    if ((st & 0x7f) || ((st >> 8) & 0xff)) {
        P("gputest: caps: uid 1000 получил lane — дыра в capability!\n");
        return 1;
    }
    P("M22 GPU CAPDENY OK\n");
    return 0;
}

// фаза user: lane до gpud, полный цикл, mode обязан быть FACADE
static int phase_lane(long bus, long *lane_out) {
    long lane = lookup_gpu_lane(bus);
    if (lane < 0) return 1;
    if (!info_ok(-1, lane)) {
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    struct mlgpu_info_out o;
    m_memcpy(&o, rbuf, sizeof(o));
    if (o.mode != MLGPU_MODE_FACADE) {
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    if (gpu_cycle(-1, lane, 4096)) {
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    *lane_out = lane;
    P("M22 GPU LANE OK\n");
    return 0;
}

// фаза return: домен вернул фасад — /dev ожил, gpud уступил (ML_E_PERM)
static int phase_return(long fd, long lane) {
    if (gpu_place("kernel\n", 7)) return 1;
    if (!info_ok(fd, 0)) return 1;
    struct ml_msg q = { ML_PROTO_GPU, GPU_REQ, 0, 0, {0} };
    u8 t = MLGPU_INFO;
    if (ml_send(lane, &q, &t, 1, 0, 0) < 0) return 1;
    struct ml_msg resp;
    if (ml_recv(lane, &resp, 0, 0, 0) <= 0 || resp.msg_id != GPU_REQ_R
        || resp.err != ML_E_PERM) return 1;
    P("M22 GPU RETURN OK\n");
    return 0;
}

void milin_main(void) {
    long bus = bus_conn();
    if (bus < 0) {
        P("gputest: шина не отвечает\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    long fd = sys3(SYS_open, (long)"/dev/amdgpu", O_RDWR, 0);
    if (fd < 0) {
        P("gputest: /dev/amdgpu: FAIL (фасад не зарегистрирован)\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    if (phase_kernel(fd)) sys3(SYS_exit, 1, 0, 0);
    if (gpu_place("user\n", 5)) {
        P("gputest: placement=user: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    if (phase_deny(fd)) sys3(SYS_exit, 1, 0, 0);

    // caps: шину могли прибить в демо M7.2 — переподключаемся и заново
    for (int attempt = 0;; attempt++) {
        if (!phase_caps(bus)) break;
        if (attempt > 5) sys3(SYS_exit, 1, 0, 0);
        sys3(SYS_close, bus, 0, 0);
        bus = bus_conn();
        if (bus < 0) sys3(SYS_exit, 1, 0, 0);
        P("gputest: caps: ретрай на новой шине\n");
    }

    long lane = -1;
    for (int attempt = 0;; attempt++) {
        if (!phase_lane(bus, &lane)) break;
        if (attempt > 5) sys3(SYS_exit, 1, 0, 0);
        sys3(SYS_close, bus, 0, 0);
        bus = bus_conn();
        if (bus < 0) sys3(SYS_exit, 1, 0, 0);
        P("gputest: lane: ретрай на новой шине\n");
    }

    if (phase_return(fd, lane)) {
        P("gputest: return: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    sys3(SYS_close, lane, 0, 0);
    sys3(SYS_close, fd, 0, 0);
    P("M22 GPU E2E OK\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
