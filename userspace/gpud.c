// /sbin/gpud (M11/M22): user-placement GPU-демон — вторая инкарнация фасада.

#include "milin.h"
#include "ipc.h"
#include "bulk.h"
#include "mlgpu.h"

#define GPU_WORLD 0
#define GPU_ROOT  ML_CAP_WRITE

#define FAC_SLOTS 8
#define FAC_SLOT_SIZE (1u << 20)
#define FAC_HANDLE_MAGIC 0xfac00000ull
#define FAC_MAX_DW 64
#define FAC_MAX_DW_BULK (MLB_SLOT / 4)  // 32К dwords = слот арены целиком

struct fac_state {
    u32 used[FAC_SLOTS];
    u32 size[FAC_SLOTS];
    u64 fences;
    u64 allocs;
};

static struct fac_state fst;

static int fac_slot_of(u64 handle) {
    u32 h = (u32)handle;
    if (handle >> 32 || (h & 0xffff0000) != FAC_HANDLE_MAGIC) return -1;
    h &= 0xffff;
    return h < FAC_SLOTS ? (int)h : -1;
}

static long gpu_serve(const u8 *in, u32 inlen, const u8 *cmd, u32 cmdlen,
                      u8 *out, u32 outmax) {
    if (inlen < 1) return -1;

    switch (in[0]) {
    case MLGPU_INFO: {
        struct mlgpu_info_out o;
        if (outmax < sizeof(o)) return -1;
        m_memset(&o, 0, sizeof(o));
        m_memcpy(o.name, "amdgpu", 7);
        o.api = MLGPU_API;
        o.mode = MLGPU_MODE_FACADE; // user-placement: amdgpu-домен уступил
        o.nslots = FAC_SLOTS;
        o.vram_bytes = (u64)FAC_SLOTS * FAC_SLOT_SIZE;
        o.fences = fst.fences;
        o.allocs = fst.allocs;
        m_memcpy(out, &o, sizeof(o));
        return sizeof(o);
    }
    case MLGPU_ALLOC: {
        struct mlgpu_alloc_in a;
        struct mlgpu_alloc_out o;
        if (inlen != 1 + sizeof(a) || outmax < sizeof(o)) return -1;
        m_memcpy(&a, in + 1, sizeof(a));
        if (!a.size || a.size > FAC_SLOT_SIZE) return -1;
        for (int i = 0; i < FAC_SLOTS; i++) {
            if (fst.used[i]) continue;
            fst.used[i] = 1;
            fst.size[i] = (u32)a.size;
            fst.allocs++;
            o.handle = FAC_HANDLE_MAGIC | (u32)i;
            o.offset = (u64)i * FAC_SLOT_SIZE;
            m_memcpy(out, &o, sizeof(o));
            return sizeof(o);
        }
        return -1;
    }
    case MLGPU_FREE: {
        struct mlgpu_free_in f;
        if (inlen != 1 + sizeof(f)) return -1;
        m_memcpy(&f, in + 1, sizeof(f));
        int s = fac_slot_of(f.handle);
        if (s < 0 || !fst.used[s]) return -1;
        fst.used[s] = 0;
        return 0;
    }
    case MLGPU_SUBMIT: {
        struct mlgpu_submit_in s;
        struct mlgpu_submit_out o;
        if (cmd) return -1;  // 'S' не несёт слота — это признак сломанного клиента
        if (inlen < 1 + sizeof(s) || outmax < sizeof(o)) return -1;
        m_memcpy(&s, in + 1, sizeof(s));
        u32 ndw = s.ndw < FAC_MAX_DW ? s.ndw : FAC_MAX_DW;
        if (s.ndw > FAC_MAX_DW || inlen != 1 + sizeof(s) + ndw * 4) return -1;
        int slot = fac_slot_of(s.handle);
        if (slot < 0 || !fst.used[slot]) return -1;
        u32 sum = 0;
        for (u32 i = 0; i < ndw; i++) {
            u32 dw;
            m_memcpy(&dw, in + 1 + sizeof(s) + i * 4, 4);
            sum += dw;
        }
        fst.fences++;
        o.fence = fst.fences;
        m_memcpy(out, &o, sizeof(o));
        return sizeof(o);
    }
    case MLGPU_SUBMITB: {
        struct mlgpu_submit_in s;
        struct mlgpu_submitb_out o;
        if (!cmd) return -1;
        if (inlen != 1 + sizeof(s) || outmax < sizeof(o)) return -1;
        m_memcpy(&s, in + 1, sizeof(s));
        if (!s.ndw || s.ndw > FAC_MAX_DW_BULK || cmdlen != s.ndw * 4) return -1;
        int slot = fac_slot_of(s.handle);
        if (slot < 0 || !fst.used[slot]) return -1;
        u64 sum = 0;
        for (u32 i = 0; i < s.ndw; i++) {
            u32 dw;
            m_memcpy(&dw, cmd + i * 4, 4);
            sum += dw;
        }
        fst.fences++;
        o.fence = fst.fences;
        o.sum = sum;
        m_memcpy(out, &o, sizeof(o));
        return sizeof(o);
    }
    case MLGPU_WAIT: {
        struct mlgpu_wait_in w;
        struct mlgpu_wait_out o;
        if (inlen != 1 + sizeof(w) || outmax < sizeof(o)) return -1;
        m_memcpy(&w, in + 1, sizeof(w));
        o.signaled = w.fence && w.fence <= fst.fences;
        o.pad = 0;
        m_memcpy(out, &o, sizeof(o));
        return sizeof(o);
    }
    default:
        return -1;
    }
}

static int placement_is_user(void) {
    long fd = sys3(SYS_open, (long)"/sys/class/mlkd/amdgpu/state", O_RDONLY, 0);
    if (fd < 0) return 0;
    char b[16];
    long n = sys3(SYS_read, fd, (long)b, sizeof(b) - 1);
    sys3(SYS_close, fd, 0, 0);
    if (n <= 0) return 0;
    b[n] = 0;
    return m_strncmp(b, "user", 4) == 0;
}

static void serve_lane(long lane, u32 rights, struct mlb_hdr *ar) {
    ml_passcred(lane);
    for (;;) {
        struct ml_msg m;
        static char tail[ML_MAXTAIL];
        struct ml_aux aux;
        long r = ml_recv(lane, &m, tail, sizeof(tail), &aux);
        if (r <= 0) {
            P("gpud: lane: клиент ушёл (EOF), capability отозвана\n");
            sys3(SYS_close, lane, 0, 0);
            return;
        }
        if (m.proto != ML_PROTO_GPU || m.msg_id != GPU_REQ) continue;

        u32 slot = (u32)*ml_h(&m, 0);
        u32 blen = (u32)*ml_h(&m, 1);
        const u8 *cmd = 0;
        if (slot) {
            if (!ar || slot > MLB_NSLOTS || blen > MLB_SLOT) {
                P("gpud: bulk: плохая ссылка (слот ");
                print_dec_n(slot);
                P(", len ");
                print_dec_n(blen);
                P(")\n");
                struct ml_msg bad = { ML_PROTO_GPU, GPU_REQ_R, ML_E_IO, 0, {0} };
                ml_send(lane, &bad, 0, 0, 0, 0);
                continue;
            }
            cmd = mlb_ptr(ar, (int)slot);
        }

        struct ml_msg resp = { ML_PROTO_GPU, GPU_REQ_R, ML_E_OK, 0, {0} };
        if (!(rights & ML_CAP_WRITE)) {
            resp.err = ML_E_PERM;
        } else if (!placement_is_user()) {
            P("gpud: запрос отклонён: фасад в домене ядра (placement=kernel)\n");
            resp.err = ML_E_PERM;
        } else {
            static u8 reply[ML_MAXTAIL];
            long n = gpu_serve((u8 *)tail, m.tail_len, cmd, cmd ? blen : 0,
                               reply, sizeof(reply));
            if (n >= 0) {
                resp.tail_len = (u32)n;
                m_memcpy(tail, reply, (u32)n);
            } else {
                resp.err = ML_E_IO;
            }
        }
        if (cmd) mlb_give(ar, (int)slot);
        ml_send(lane, &resp, tail, resp.err == ML_E_OK ? resp.tail_len : 0, 0, 0);
    }
}

static long bus_register(void) {
    long bus = -1;
    for (int tries = 0; ; tries++) {
        bus = ml_connect("milin-bus");
        if (bus >= 0) break;
        if (tries > 2000) return -1;
        sleep_ms(10);
    }
    struct ml_msg m = { ML_PROTO_BUS, BUS_REGISTER, 0, 0, {0} };
    m_memcpy(m.head, "gpu.amdgpu", 11);
    u32 masks[2] = { GPU_WORLD, GPU_ROOT };
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
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("gpud: setuid(1000): FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    long bus = bus_register();
    if (bus < 0) {
        P("gpud: шина не отвечает\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("gpud: зарегистрирован в шине как \"gpu.amdgpu\" (user-placement)\n");

    for (;;) {
        struct ml_msg m2;
        char req_tail[8];
        struct ml_aux aux;
        if (ml_recv(bus, &m2, req_tail, sizeof(req_tail), &aux) <= 0) {
            P("gpud: шина умерла — перерегистрация (state пережил: ");
            print_dec_n(fst.fences);
            P(")\n");
            sys3(SYS_close, bus, 0, 0);
            bus = bus_register();
            if (bus < 0) {
                P("gpud: шина не вернулась\n");
                sys3(SYS_exit, 1, 0, 0);
            }
            P("gpud: перерегистрирован в новой шине\n");
            continue;
        }
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_REVOKE) continue;
        if (m2.proto == ML_PROTO_BUS && m2.msg_id == BUS_LANE_REQ && aux.nhooks >= 1) {
            u32 rights = 0;
            if (m2.tail_len >= 4) m_memcpy(&rights, req_tail, 4);
            struct mlb_hdr *ar = 0;
            if (aux.nhooks >= 2 && aux.hooks[1] >= 0)
                ar = mlb_map(aux.hooks[1]);
            serve_lane(aux.hooks[0], rights, ar);
        }
    }
    sys3(SYS_exit, 0, 0, 0);
    __builtin_unreachable();
}
