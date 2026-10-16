// MiLin HelQueue (M5.1): асинхронные очереди сообщений в разделяемой памяти.
#ifndef MILIN_HELQ_H
#define MILIN_HELQ_H

#ifdef MILIN_GLIBC
#include <string.h>
#include <errno.h>
#include <time.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <linux/futex.h>
typedef unsigned long u64;
typedef unsigned int u32;
typedef unsigned char u8;
static long ml_ret(long r) { return r < 0 ? -(long)errno : r; }
static long sys3(long n, long a, long b, long c) { return ml_ret(syscall(n, a, b, c)); }
static long sys5(long n, long a, long b, long c, long d, long e) { return ml_ret(syscall(n, a, b, c, d, e)); }
static long sys6(long n, long a, long b, long c, long d, long e, long f) { return ml_ret(syscall(n, a, b, c, d, e, f)); }
static void *m_memcpy(void *d, const void *s, unsigned n) { return memcpy(d, s, n); }
static void *m_memset(void *d, int c, unsigned n) { return memset(d, c, n); }
static int futex_wait(volatile int *u, int v) { return (int)ml_ret(syscall(SYS_futex, u, FUTEX_WAIT, v, 0, 0, 0)); }
static int futex_wait_to(volatile int *u, int v, long ms) {
    struct timespec t = { ms / 1000, (ms % 1000) * 1000000 };
    return (int)ml_ret(syscall(SYS_futex, u, FUTEX_WAIT, v, &t, 0, 0));
}
static int futex_wake(volatile int *u) { return (int)ml_ret(syscall(SYS_futex, u, FUTEX_WAKE, 1, 0, 0, 0)); }
#else
#include "milin.h"
#include "ipc.h"
#endif

#define MLQ_CHUNK    4096
#define MLQ_NCHUNKS  8
#define MLQ_NBLOBS   8
#define MLQ_BLOB     32768
#define MLQ_MAXCHAIN (MLQ_NBLOBS / 2)         // M8.1: слотов в цепочке на направление
#define MLQ_BULK_MAX (MLQ_MAXCHAIN * MLQ_BLOB) // 128КБ: потолок bulk-сообщения
#define MLQ_MAGIC    0x4D4C5132u   // "MLQ2"

#define MLQ_P_MASK   0x00FFFFFFu   // 24 бита: валидные байты
#define MLQ_P_DONE   (1u << 25)    // producer закончил писать чанк
#define MLQ_P_FULL   (1u << 26)    // чанк заполнен (резерв, как в managarm)

#define MLQ_N_DATA   (1 << 0)      // в ready есть чанки (данные готовы)
#define MLQ_N_SUPPLY (1 << 1)      // в free есть чанки (producer может писать)
#define MLQ_N_BLOB   (1 << 2)      // освобождён blob-слот (M8)

struct mlq_elem {
    u32 len;
    u32 opcode;
    u32 blob;
};

struct mlq_hdr {
    int notify;              // futex-слово (биты MLQ_N_*)
    int ready;               // цепочка чанков с данными (producer -> consumer)
    int free;                // цепочка свободных чанков (consumer -> producer)
    int blob_free;           // цепочка свободных blob-слотов (M8)
};

struct mlq_pair {
    u32 magic;
    u32 chunk_size;
    u32 num_chunks;
    u32 pad;
    struct mlq_hdr q[2];
    int blob_next[MLQ_NBLOBS + 1];   // next в цепочке blob-слотов (1..N)
};

struct mlq_chunk {
    int next;                // индекс следующего чанка в цепочке (0 = конец)
    int progress;            // MLQ_P_MASK | MLQ_P_DONE
    char buf[MLQ_CHUNK - 8];
};

#define MLQ_HDR_OFF  128     // заголовок обязан целиком влезть ДО чанков:
_Static_assert(sizeof(struct mlq_pair) <= MLQ_HDR_OFF, "mlq_pair overrun chunks");
#define MLQ_BLOB_OFF (MLQ_HDR_OFF + MLQ_NCHUNKS * MLQ_CHUNK)
#define MLQ_SIZE     (MLQ_BLOB_OFF + MLQ_NBLOBS * MLQ_BLOB)
#define MLQ_INLINE_MAX (MLQ_CHUNK - 8 - (u32)sizeof(struct mlq_elem))

static struct mlq_chunk *mlq_chunk_at(struct mlq_pair *p, int idx) {
    return (struct mlq_chunk *)((char *)p + MLQ_HDR_OFF + (idx - 1) * MLQ_CHUNK);
}

static char *mlq_blob_at(struct mlq_pair *p, int idx) {
    return (char *)p + MLQ_BLOB_OFF + (idx - 1) * MLQ_BLOB;
}

static void mlq_init(struct mlq_pair *p) {
    m_memset(p, 0, MLQ_SIZE);
    p->magic = MLQ_MAGIC;
    p->chunk_size = MLQ_CHUNK;
    p->num_chunks = MLQ_NCHUNKS;
    for (int i = 1; i <= MLQ_NCHUNKS / 2; i++)
        mlq_chunk_at(p, i)->next = (i < MLQ_NCHUNKS / 2) ? i + 1 : 0;
    p->q[0].free = 1;
    for (int i = MLQ_NCHUNKS / 2 + 1; i < MLQ_NCHUNKS; i++)
        mlq_chunk_at(p, i)->next = i + 1;
    p->q[1].free = MLQ_NCHUNKS / 2 + 1;
    int hb = MLQ_NBLOBS / 2;
    for (int i = 1; i <= hb; i++)
        p->blob_next[i] = (i < hb) ? i + 1 : 0;
    p->q[0].blob_free = 1;
    for (int i = hb + 1; i <= MLQ_NBLOBS; i++)
        p->blob_next[i] = (i < MLQ_NBLOBS) ? i + 1 : 0;
    p->q[1].blob_free = hb + 1;
}

static struct mlq_pair *mlq_map_raw(int shm_fd) {
    long r = sys6(SYS_mmap, 0, MLQ_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, shm_fd, 0);
    if (r < 0 && r > -4096L) return 0;
    return (struct mlq_pair *)r;
}

static struct mlq_pair *mlq_map(int shm_fd) {
    struct mlq_pair *p = mlq_map_raw(shm_fd);
    if (!p || p->magic != MLQ_MAGIC) return 0;
    return p;
}

static void mlq_push(volatile int *head, struct mlq_pair *p, int idx) {
    struct mlq_chunk *c = mlq_chunk_at(p, idx);
    int old = __atomic_load_n(head, __ATOMIC_RELAXED);
    for (;;) {
        __atomic_store_n(&c->next, old, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(head, &old, idx, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
}

static int mlq_pop(volatile int *head, struct mlq_pair *p) {
    int idx = __atomic_exchange_n(head, 0, __ATOMIC_ACQUIRE);
    return idx; // 0 = цепочка пуста; иначе забрали всю цепочку (next в чанках)
}

static void mlq_push_blob(struct mlq_pair *p, volatile int *head, int idx) {
    volatile int *next = &p->blob_next[idx];
    int old = __atomic_load_n(head, __ATOMIC_RELAXED);
    for (;;) {
        __atomic_store_n(next, old, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(head, &old, idx, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
    }
}


#define MLQ_FUTEX 0
#define MLQ_YIELD 1
#define MLQ_WATCHDOG 2

static void mlq_notify(volatile int *notify, int bit) {
    int old = __atomic_fetch_or(notify, bit, __ATOMIC_RELEASE);
    if (!(old & bit))
        futex_wake(notify);
}

static int mlq_wait_state(volatile int *notify, int bit,
                           volatile int *state, int mode) {
    for (;;) {
        if (__atomic_load_n(state, __ATOMIC_ACQUIRE)) return 1;
        int val = __atomic_load_n(notify, __ATOMIC_RELAXED);
        if (val & bit) {
            __atomic_fetch_and(notify, ~bit, __ATOMIC_ACQ_REL);
            continue; // бит снят: полный перепроверочный круг
        }
        if (__atomic_load_n(state, __ATOMIC_ACQUIRE)) return 1;
        if (mode == MLQ_FUTEX) {
            futex_wait(notify, val); // EAGAIN = значение сменилось: цикл
        } else if (mode == MLQ_WATCHDOG) {
            if (futex_wait_to(notify, val, 50) == -110) return 0; // ETIMEDOUT
        } else {
            sys3(SYS_sched_yield, 0, 0, 0);
        }
    }
}


struct mlq_out {
    struct mlq_pair *p;
    int dir;                 // направление: 0 запросы, 1 ответы
    int cur;                 // чанк в работе (0 = нет)
    u32 off;                 // смещение записи в cur->buf
    int enq;                 // сколько элементов в текущей партии (статистика)
    int spare;               // остаток свободной цепочки чанков (процесс-приватный)
    int bspare;              // остаток свободной цепочки blob-слотов (M8.1)
};

static void mlq_out_init(struct mlq_out *o, struct mlq_pair *p, int dir) {
    o->p = p; o->dir = dir; o->cur = 0; o->off = 0; o->enq = 0;
    o->spare = 0; o->bspare = 0;
}

static int mlq_flush(struct mlq_out *o);

static int mlq_take_chunk(struct mlq_out *o, int mode) {
    struct mlq_hdr *h = &o->p->q[o->dir];
    for (;;) {
        int idx = o->spare;
        if (idx) {
            o->spare = __atomic_load_n(&mlq_chunk_at(o->p, idx)->next,
                                       __ATOMIC_RELAXED);
            return idx;
        }
        idx = mlq_pop(&h->free, o->p);
        if (idx) {
            o->spare = __atomic_load_n(&mlq_chunk_at(o->p, idx)->next,
                                       __ATOMIC_RELAXED);
            return idx;
        }
        mlq_wait_state(&h->notify, MLQ_N_SUPPLY, &h->free, mode);
    }
}

static int mlq_put_elem(struct mlq_out *o, u32 opcode, const void *payload,
                        u32 len, u32 blob, int mode) {
    if (!o->cur) o->cur = mlq_take_chunk(o, mode);
    struct mlq_chunk *c = mlq_chunk_at(o->p, o->cur);
    u32 need = (u32)sizeof(struct mlq_elem) + (blob ? 0 : len);
    if (o->off + need > sizeof(c->buf)) {
        int r = mlq_flush(o);
        if (r < 0) return r;
        o->cur = mlq_take_chunk(o, mode);
        c = mlq_chunk_at(o->p, o->cur);
    }
    struct mlq_elem *e = (struct mlq_elem *)(c->buf + o->off);
    e->len = len;
    e->opcode = opcode;
    e->blob = blob;
    if (!blob && len) m_memcpy(e + 1, payload, len);
    o->off += need;
    o->enq++;
    return 0;
}

static void mlq_free_blobs(struct mlq_pair *p, struct mlq_hdr *h, int head,
                           int n) {
    int idx = head;
    for (int i = 0; i < n && idx; i++) {
        int nxt = p->blob_next[idx];
        mlq_push_blob(p, &h->blob_free, idx);
        idx = nxt;
    }
    mlq_notify(&h->notify, MLQ_N_BLOB);
}

static int mlq_take_blob(struct mlq_out *o, int mode) {
    struct mlq_hdr *h = &o->p->q[o->dir];
    for (;;) {
        int idx = o->bspare;
        if (idx) {
            o->bspare = o->p->blob_next[idx];
            return idx;
        }
        idx = mlq_pop(&h->blob_free, o->p);
        if (idx) {
            o->bspare = o->p->blob_next[idx];
            return idx;
        }
        mlq_wait_state(&h->notify, MLQ_N_BLOB, &h->blob_free, mode);
    }
}

static int mlq_put(struct mlq_out *o, u32 opcode, const void *payload, u32 len,
                   int mode) {
    if (len > MLQ_BULK_MAX) return -1;
    if (len > MLQ_INLINE_MAX) {
        int ns = (int)((len + MLQ_BLOB - 1) / MLQ_BLOB);
        int chain[MLQ_MAXCHAIN];
        for (int i = 0; i < ns; i++) {
            int b = mlq_take_blob(o, mode);
            if (b < 0) {
                for (int j = 0; j < i; j++)
                    mlq_push_blob(o->p, &o->p->q[o->dir].blob_free, chain[j]);
                if (i) mlq_notify(&o->p->q[o->dir].notify, MLQ_N_BLOB);
                return b;
            }
            chain[i] = b;
        }
        u32 off = 0;
        for (int i = 0; i < ns; i++) {
            u32 n = len - off;
            if (n > MLQ_BLOB) n = MLQ_BLOB;
            m_memcpy(mlq_blob_at(o->p, chain[i]), (const char *)payload + off, n);
            off += n;
            o->p->blob_next[chain[i]] = (i + 1 < ns) ? chain[i + 1] : 0;
        }
        int r = mlq_put_elem(o, opcode, 0, len, (u32)chain[0], mode);
        if (r < 0) {
            mlq_free_blobs(o->p, &o->p->q[o->dir], chain[0], ns);
            return r;
        }
        if (r == 0) mlq_flush(o);
        return r;
    }
    return mlq_put_elem(o, opcode, payload, len, 0, mode);
}

static int mlq_flush(struct mlq_out *o) {
    struct mlq_hdr *h = &o->p->q[o->dir];
    if (!o->cur) return 0;
    struct mlq_chunk *c = mlq_chunk_at(o->p, o->cur);
    __atomic_store_n(&c->progress, (int)(o->off | MLQ_P_DONE), __ATOMIC_RELEASE);
    mlq_push(&h->ready, o->p, o->cur);
    mlq_notify(&h->notify, MLQ_N_DATA);
    o->cur = 0;
    o->off = 0;
    int n = o->enq; o->enq = 0;
    return n;
}

struct mlq_view {
    u32 opcode;
    u32 len;
    const void *payload;     // указатель В ОБЩУЮ память: действует до consume
    u32 blob;                // 0 = inline, иначе головной blob-слот (M8)
    u32 nslots;              // слотов в цепочке (M8.1): 1 = обычный blob
};

struct mlq_in {
    struct mlq_pair *p;
    int dir;
    int cur;                 // обрабатываемый чанк
    u32 off;                 // смещение чтения внутри cur->buf
    u32 end;                 // граница валидных байт
};

static void mlq_in_init(struct mlq_in *in, struct mlq_pair *p, int dir) {
    in->p = p; in->dir = dir; in->cur = 0; in->off = 0; in->end = 0;
}

static void mlq_consume(struct mlq_in *in, struct mlq_view *v) {
    if (v->blob) {
        mlq_free_blobs(in->p, &in->p->q[in->dir], (int)v->blob, (int)v->nslots);
        v->blob = 0;
    }
}

static long mlq_gather(struct mlq_in *in, struct mlq_view *v, void *dst,
                       u32 cap) {
    if (!v->blob) {
        u32 n = v->len < cap ? v->len : cap;
        m_memcpy(dst, v->payload, n);
        return n;
    }
    if (v->len > cap) return -1;
    u32 off = 0;
    int idx = (int)v->blob;
    for (u32 i = 0; i < v->nslots && off < v->len; i++) {
        u32 n = v->len - off;
        if (n > MLQ_BLOB) n = MLQ_BLOB;
        m_memcpy((char *)dst + off, mlq_blob_at(in->p, idx), n);
        off += n;
        idx = in->p->blob_next[idx];
    }
    return (long)off;
}

static void mlq_recycle(struct mlq_in *in) {
    struct mlq_hdr *h = &in->p->q[in->dir];
    mlq_push(&h->free, in->p, in->cur);
    mlq_notify(&h->notify, MLQ_N_SUPPLY);
    in->cur = 0; in->off = 0; in->end = 0;
}

static int mlq_next(struct mlq_in *in, struct mlq_view *v, int mode) {
    struct mlq_hdr *h = &in->p->q[in->dir];
    for (;;) {
        if (!in->cur) {
            int idx = mlq_pop(&h->ready, in->p);
            if (!idx) {
                if (!mlq_wait_state(&h->notify, MLQ_N_DATA, &h->ready, mode))
                    return 0;
                continue;
            }
            in->cur = idx;
            struct mlq_chunk *c = mlq_chunk_at(in->p, idx);
            in->off = 0;
            in->end = __atomic_load_n(&c->progress, __ATOMIC_ACQUIRE) & MLQ_P_MASK;
        }
        struct mlq_chunk *c = mlq_chunk_at(in->p, in->cur);
        if (in->off >= in->end) {
            int nxt = __atomic_load_n(&c->next, __ATOMIC_ACQUIRE);
            mlq_recycle(in);
            if (nxt) {
                in->cur = nxt;
                struct mlq_chunk *c2 = mlq_chunk_at(in->p, nxt);
                in->off = 0;
                in->end = __atomic_load_n(&c2->progress, __ATOMIC_ACQUIRE) & MLQ_P_MASK;
            }
            continue;
        }
        struct mlq_elem *e = (struct mlq_elem *)(c->buf + in->off);
        u32 eoff = in->off;
        in->off += (u32)sizeof(struct mlq_elem) + e->len;
        v->opcode = e->opcode;
        v->len = e->len;
        v->blob = 0;
        v->nslots = 0;
        if (e->blob) {
            int lo = (in->dir ? MLQ_NBLOBS / 2 : 0) + 1;
            int hi = lo + MLQ_NBLOBS / 2 - 1;
            if (e->blob < (u32)lo || e->blob > (u32)hi ||
                e->len > MLQ_MAXCHAIN * MLQ_BLOB) {
                in->off = in->end;
                continue;
            }
            int ns = (int)((e->len + MLQ_BLOB - 1) / MLQ_BLOB);
            int idx = (int)e->blob;
            for (int i = 1; i < ns; i++) {
                idx = in->p->blob_next[idx];
                if (idx < lo || idx > hi) {
                    idx = 0;
                    break;
                }
            }
            if (ns > 1 && idx == 0) {
                in->off = in->end;
                continue;
            }
            v->payload = mlq_blob_at(in->p, (int)e->blob);
            v->blob = e->blob;
            v->nslots = (u32)ns;
            return 1;
        }
        if ((u64)eoff + sizeof(struct mlq_elem) + e->len > in->end) {
            in->off = in->end;
            continue;
        }
        v->payload = e + 1;
        v->nslots = 0;
        return 1;
    }
}

#endif
