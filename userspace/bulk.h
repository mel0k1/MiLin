// bulk (M22): bulk-путь IPC — данные в разделяемой памяти, lane несёт только ссылку.

#ifndef MILIN_BULK_H
#define MILIN_BULK_H

#include "milin.h"
#include "ipc.h"

#define MLB_MAGIC   0x4D4C4231u   // "MLB1"
#define MLB_NSLOTS  4
#define MLB_SLOT    (128 * 1024)
#define MLB_HDR_OFF 128           // заголовок обязан целиком влезть до слотов
#define MLB_SIZE    (MLB_HDR_OFF + MLB_NSLOTS * MLB_SLOT)

#define MLB_N_FREE  (1 << 0)      // есть свободный слот
#define MLB_WAIT    0             // futex с таймаутом 50мс
#define MLB_YIELD   1             // поллинг с sched_yield

struct mlb_hdr {
    u32 magic;
    u32 nslots;
    u32 slot_size;
    volatile int freelist;         // голова freelist (0 = пусто), слоты 1..N
    volatile int notify;           // futex-слово будильника
    int next[MLB_NSLOTS + 1];      // ссылки freelist, next[0] не используется
};

_Static_assert(sizeof(struct mlb_hdr) <= MLB_HDR_OFF, "mlb_hdr overrun");

static struct mlb_hdr *mlb_map_raw(int shm_fd) {
    long r = sys6(SYS_mmap, 0, MLB_SIZE, PROT_READ | PROT_WRITE,
                  MAP_SHARED, shm_fd, 0);
    if (r < 0 && r > -4096L) return 0;
    return (struct mlb_hdr *)r;
}

static struct mlb_hdr *mlb_map(int shm_fd) {
    struct mlb_hdr *h = mlb_map_raw(shm_fd);
    if (!h || h->magic != MLB_MAGIC) return 0;
    return h;
}

static void mlb_init(struct mlb_hdr *h) {
    m_memset(h, 0, MLB_SIZE);
    h->magic = MLB_MAGIC;
    h->nslots = MLB_NSLOTS;
    h->slot_size = MLB_SLOT;
    for (int i = 1; i <= MLB_NSLOTS; i++)
        h->next[i] = (i < MLB_NSLOTS) ? i + 1 : 0;
    h->freelist = 1;
}

static int mlb_wait_free(struct mlb_hdr *h, int mode) {
    for (;;) {
        if (__atomic_load_n(&h->freelist, __ATOMIC_RELAXED)) return 1;
        int val = __atomic_load_n(&h->notify, __ATOMIC_RELAXED);
        if (val & MLB_N_FREE) {
            __atomic_fetch_and(&h->notify, ~MLB_N_FREE, __ATOMIC_ACQ_REL);
            continue; // бит снят: полный перепроверочный круг
        }
        if (__atomic_load_n(&h->freelist, __ATOMIC_RELAXED)) return 1;
        if (mode == MLB_YIELD) {
            sys3(SYS_sched_yield, 0, 0, 0);
            continue;
        }
        if (futex_wait_to(&h->notify, val, 50) == -110) return 0; // ETIMEDOUT
    }
}

static int mlb_take(struct mlb_hdr *h, int mode) {
    for (;;) {
        int head = __atomic_load_n(&h->freelist, __ATOMIC_RELAXED);
        while (head) {
            if (__atomic_compare_exchange_n(&h->freelist, &head,
                                            h->next[head], 0,
                                            __ATOMIC_ACQUIRE,
                                            __ATOMIC_RELAXED))
                return head;
        }
        if (!mlb_wait_free(h, mode)) return 0;
    }
}

static void mlb_give(struct mlb_hdr *h, int slot) {
    int head = __atomic_load_n(&h->freelist, __ATOMIC_RELAXED);
    for (;;) {
        __atomic_store_n(&h->next[slot], head, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(&h->freelist, &head, slot, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            break;
    }
    int old = __atomic_fetch_or(&h->notify, MLB_N_FREE, __ATOMIC_RELEASE);
    if (!(old & MLB_N_FREE))
        futex_wake(&h->notify);
}

static void *mlb_ptr(struct mlb_hdr *h, int slot) {
    return (char *)h + MLB_HDR_OFF + (slot - 1) * MLB_SLOT;
}

#endif
