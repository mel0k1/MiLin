// /sbin/gputest (M22): e2e mlkd-фасада amdgpu из независимого клиента.

#include "milin.h"
#include "ipc.h"
#include "bulk.h"
#include "mlgpu.h"

#define BULK_NDW 16384  // 64КБ dwords — на порядок больше ML_MAXTAIL

static char rbuf[512];

static long bus_conn(void) {
    for (int tries = 0;; tries++) {
        long bus = ml_connect("milin-bus");
        if (bus >= 0) return bus;
        if (tries > 400) return -1;
        sleep_ms(25);
    }
}

static long dev_req(long fd, u8 op, const void *payload, u32 plen) {
    static u8 req[1 + sizeof(struct mlgpu_submit_in) + 8 * 4];
    req[0] = op;
    if (plen) m_memcpy(req + 1, payload, plen);
    long w = sys3(SYS_write, fd, (long)req, 1 + plen);
    if (w != (long)(1 + plen)) return w;
    return sys3(SYS_read, fd, (long)rbuf, sizeof(rbuf));
}

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

static int phase_deny(long fd) {
    struct mlgpu_alloc_in a = { 4096 };
    if (dev_req(fd, MLGPU_ALLOC, &a, sizeof(a)) != -19) {
        P("gputest: deny (dev): запрос НЕ отклонён — дыра в политике!\n");
        return 1;
    }
    P("M22 GPU DENYDEV OK\n");
    return 0;
}

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

static int phase_bulk(long bus) {
    struct ml_msg q = { ML_PROTO_BUS, BUS_LOOKUP, 0, 0, {0} };
    m_memcpy(q.head, "gpu.amdgpu", 11);
    *ml_h(&q, 2) = ML_TR_BULK;
    long lane = -1, shmfd = -1;
    for (int tries = 0; tries < 100; tries++) {
        if (ml_send(bus, &q, 0, 0, 0, 0) < 0) return 1;
        struct ml_msg resp;
        struct ml_aux aux;
        if (ml_recv(bus, &resp, 0, 0, &aux) <= 0) return 1;
        if (resp.err == ML_E_OK && aux.nhooks >= 2) {
            lane = aux.hooks[0];
            shmfd = aux.hooks[1];
            break;
        }
        if (resp.err == ML_E_PERM) return 1;
        sleep_ms(20);
    }
    if (lane < 0 || shmfd < 0) return 1;

    struct mlb_hdr *ar = mlb_map(shmfd);
    sys3(SYS_close, shmfd, 0, 0);
    if (!ar) {
        P("gputest: bulk: арена не разметана\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }

    if (!info_ok(-1, lane)) {
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }

    struct mlgpu_alloc_in a = { (u64)BULK_NDW * 4 };
    if (lane_req(lane, MLGPU_ALLOC, &a, sizeof(a))
        != sizeof(struct mlgpu_alloc_out)) {
        P("gputest: bulk: alloc FAIL\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    u64 h;
    m_memcpy(&h, rbuf, sizeof(h));

    int slot = mlb_take(ar, MLB_WAIT);
    if (!slot) {
        P("gputest: bulk: слот не выделен (арена молчит)\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    u32 *cw = mlb_ptr(ar, slot);
    for (u32 i = 0; i < BULK_NDW; i++) cw[i] = i + 1;
    u64 want = (u64)BULK_NDW * (BULK_NDW + 1) / 2;

    static u8 tb[1 + sizeof(struct mlgpu_submit_in)];
    tb[0] = MLGPU_SUBMITB;
    struct mlgpu_submit_in s = { h, BULK_NDW, 0 };
    m_memcpy(tb + 1, &s, sizeof(s));
    struct ml_msg rq = { ML_PROTO_GPU, GPU_REQ, 0, 0, {0} };
    *ml_h(&rq, 0) = (u32)slot;
    *ml_h(&rq, 1) = BULK_NDW * 4;
    if (ml_send(lane, &rq, tb, sizeof(tb), 0, 0) < 0) {
        P("gputest: bulk: send FAIL\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    struct ml_msg rp;
    if (ml_recv(lane, &rp, rbuf, sizeof(rbuf), 0) <= 0
        || rp.msg_id != GPU_REQ_R || rp.err != ML_E_OK
        || rp.tail_len != sizeof(struct mlgpu_submitb_out)) {
        P("gputest: bulk: ack FAIL\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    struct mlgpu_submitb_out so;
    m_memcpy(&so, rbuf, sizeof(so));
    if (!so.fence || so.sum != want) {
        P("gputest: bulk: сумма dwords не сошлась (got ");
        print_dec_n((long)so.sum);
        P(", want ");
        print_dec_n((long)want);
        P(")\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }

    struct mlgpu_wait_in w = { so.fence };
    if (lane_req(lane, MLGPU_WAIT, &w, sizeof(w))
        != sizeof(struct mlgpu_wait_out)) {
        P("gputest: bulk: wait FAIL\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    u32 sig;
    m_memcpy(&sig, rbuf, 4);
    if (sig != 1) {
        P("gputest: bulk: fence не взведён\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }
    struct mlgpu_free_in f = { h };
    if (lane_req(lane, MLGPU_FREE, &f, sizeof(f)) != 0) {
        P("gputest: bulk: free FAIL\n");
        sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
        sys3(SYS_close, lane, 0, 0);
        return 1;
    }

    P("gputest: bulk: submit ");
    print_dec_n(BULK_NDW * 4);
    P(" байт через слот арены, sum сошлась, слот возвращён до ack\n");
    sys6(SYS_munmap, (long)ar, MLB_SIZE, 0, 0, 0, 0);
    sys3(SYS_close, lane, 0, 0);
    P("M22 GPU BULK OK\n");
    return 0;
}

static int phase_return(long fd, long bus) {
    if (gpu_place("kernel\n", 7)) return 1;
    if (!info_ok(fd, 0)) return 1;
    long lane = lookup_gpu_lane(bus);
    if (lane < 0) return 1;
    struct ml_msg q = { ML_PROTO_GPU, GPU_REQ, 0, 0, {0} };
    u8 t = MLGPU_INFO;
    if (ml_send(lane, &q, &t, 1, 0, 0) < 0) return 1;
    struct ml_msg resp;
    if (ml_recv(lane, &resp, 0, 0, 0) <= 0 || resp.msg_id != GPU_REQ_R
        || resp.err != ML_E_PERM) return 1;
    sys3(SYS_close, lane, 0, 0);  // EOF: serve_lane вернётся в главный цикл
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
    sys3(SYS_close, lane, 0, 0);

    for (int attempt = 0;; attempt++) {
        if (!phase_bulk(bus)) break;
        if (attempt > 5) sys3(SYS_exit, 1, 0, 0);
        sys3(SYS_close, bus, 0, 0);
        bus = bus_conn();
        if (bus < 0) sys3(SYS_exit, 1, 0, 0);
        P("gputest: bulk: ретрай на новой шине\n");
    }

    if (phase_return(fd, bus)) {
        P("gputest: return: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    sys3(SYS_close, fd, 0, 0);
    P("M22 GPU E2E OK\n");
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
