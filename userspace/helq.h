// MiLin HelQueue (M5.1): асинхронные очереди сообщений в разделяемой памяти.
//
// Идея подсмотрена у Managarm (hel.h / thor/generic/ipc-queue.cpp / rust
// hel/src/queue.rs) и пересажена с kernel↔user на user↔user:
//   * очередь живёт в памяти, общей для двух процессов: ядро в data path
//     не участвует — ни копирований в skb, ни sendmsg/recvmsg;
//   * сообщения едут чанками (страницами), как HelChunk: слово progress
//     хранит число валидных байт (24 бита) + флаги DONE/FULL;
//   * элементы length-prefixed, как HelElement: {len, opcode, payload} —
//     потребитель читает payload ПРЯМО ИЗ общей памяти (zero copy);
//   * будильник — futex-слово notify с битами, как userNotify/kernelNotify:
//     producer ставит биты через atomic OR, consumer снимает через atomic
//     AND, парковка — настоящий futex (аналог HelFutex). Бит ставится
//     только после публикации данных (release), поэтому гонки будильника
//     нет: consumer перепроверяет данные после снятия бита.
//
// Отличия от Managarm (сознательные упрощения):
//   * оба направления (запросы и ответы) — симметричные SPSC-очереди в
//     одном регионе; у Managarm SQ/CQ асимметричны (ядро — второй конец);
//   * возврат чанков — явный свободный список free (consumer -> producer)
//     вместо retreat/tail-механики; обмен цепочками через atomic exchange;
//   * один чанк = одна партия: flush публикует чанк с флагом DONE.
//
// Режимы ожидания: MLQ_FUTEX — парковка настоящим futex (безbusy-поллинга);
// MLQ_YIELD — поллинг с sched_yield (для однопроцессорной машины, где
// чистый spin означал бы дедлок: крутищийся consumer не пустит producer);
// MLQ_WATCHDOG — futex с таймаутом 50мс: mlq_next возвращает 0, если
// данных нет (M7.1: потребитель проверяет, жив ли сосед по очереди —
// смерть клиента в shm невидима, EOF тут не бывает).
//
// M8 — раздельные пути: мелкие сообщения едут inline в чанках, крупные
// (до MLQ_BLOB) — через отдельные blob-слоты: producer один раз копирует
// payload в слот, элемент несёт только ссылку, consumer читает прямо из
// общей памяти (zero copy) и возвращает слот через mlq_consume. mlq_put
// сам выбирает путь по размеру.
#ifndef MILIN_HELQ_H
#define MILIN_HELQ_H

#include "milin.h"
#include "ipc.h"

// --- геометрия очереди ---
#define MLQ_CHUNK    4096
#define MLQ_NCHUNKS  8
#define MLQ_NBLOBS   8
#define MLQ_BLOB     32768
#define MLQ_MAGIC    0x4D4C5132u   // "MLQ2"

// флаги progress — как в managarm (kHelProgress*)
#define MLQ_P_MASK   0x00FFFFFFu   // 24 бита: валидные байты
#define MLQ_P_DONE   (1u << 25)    // producer закончил писать чанк
#define MLQ_P_FULL   (1u << 26)    // чанк заполнен (резерв, как в managarm)

// биты notify-слова — как kHelUserNotifyCqProgress / kHelKernelNotify*
#define MLQ_N_DATA   (1 << 0)      // в ready есть чанки (данные готовы)
#define MLQ_N_SUPPLY (1 << 1)      // в free есть чанки (producer может писать)
#define MLQ_N_BLOB   (1 << 2)      // освобождён blob-слот (M8)

// элемент: length-prefixed, как HelElement {length, opcode, context}
// blob = 0: payload за заголовком; blob = N: payload в blob-слоте N (M8)
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

// заголовок региона: два направления + ссылки цепочки blob-слотов
// [0] = client -> server (запросы), [1] = server -> client (ответы)
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

#define MLQ_HDR_OFF  64      // заголовок выровнен на 64 (cache line)
#define MLQ_BLOB_OFF (MLQ_HDR_OFF + MLQ_NCHUNKS * MLQ_CHUNK)
#define MLQ_SIZE     (MLQ_BLOB_OFF + MLQ_NBLOBS * MLQ_BLOB)
#define MLQ_INLINE_MAX (MLQ_CHUNK - 8 - (u32)sizeof(struct mlq_elem))

static struct mlq_chunk *mlq_chunk_at(struct mlq_pair *p, int idx) {
    return (struct mlq_chunk *)((char *)p + MLQ_HDR_OFF + (idx - 1) * MLQ_CHUNK);
}

static char *mlq_blob_at(struct mlq_pair *p, int idx) {
    return (char *)p + MLQ_BLOB_OFF + (idx - 1) * MLQ_BLOB;
}

// разметка региона (вызывает ЕДИНСТВЕННЫЙ инициализатор до раздачи fd)
static void mlq_init(struct mlq_pair *p) {
    // именно свой memset: __builtin_memset на 32КБ превращается в вызов
    // libc (ловушка nostdlib), а m_memset компилируется в цикл
    m_memset(p, 0, MLQ_SIZE);
    p->magic = MLQ_MAGIC;
    p->chunk_size = MLQ_CHUNK;
    p->num_chunks = MLQ_NCHUNKS;
    // чанки и blob-слоты делим между направлениями: у КАЖДОГО свой
    // свободный список, иначе первый же ack встрял бы навсегда (нечем
    // ответить — нечем запросить чанки назад: у адресатов ещё нет ни
    // одного чанка q[1])
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

// для адресатов: регион обязан быть уже размечен инициализатором (шиной)
static struct mlq_pair *mlq_map(int shm_fd) {
    struct mlq_pair *p = mlq_map_raw(shm_fd);
    if (!p || p->magic != MLQ_MAGIC) return 0;
    return p;
}

// --- цепочки чанков: push (CAS) и pop (exchange) ---
// у каждой цепочки ровно один писатель и один читатель (SPSC):
//   ready: producer кладёт, consumer забирает
//   free:  consumer возвращает, producer забирает
static void mlq_push(volatile int *head, struct mlq_pair *p, int idx) {
    struct mlq_chunk *c = mlq_chunk_at(p, idx);
    int old = __atomic_load_n(head, __ATOMIC_RELAXED);
    for (;;) {
        __atomic_store_n(&c->next, old, __ATOMIC_RELAXED);
        if (__atomic_compare_exchange_n(head, &old, idx, 0,
                                        __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return;
        // old обновлён CAS'ом текущим значением — повторяем
    }
}

static int mlq_pop(volatile int *head, struct mlq_pair *p) {
    int idx = __atomic_exchange_n(head, 0, __ATOMIC_ACQUIRE);
    return idx; // 0 = цепочка пуста; иначе забрали всю цепочку (next в чанках)
}

// blob-слоты живут своей цепочкой: ссылки — в заголовке региона
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

// --- будильник: атомарный OR + futex_wake только если бит не был установлен
// (та же схема, что wake_kernel_futex в managarm: syscalls не сыплются
//  без необходимости, а снявший бит перепроверяет данные) ---

// режимы ожидания
#define MLQ_FUTEX 0
#define MLQ_YIELD 1
#define MLQ_WATCHDOG 2

static void mlq_notify(volatile int *notify, int bit) {
    int old = __atomic_fetch_or(notify, bit, __ATOMIC_RELEASE);
    if (!(old & bit))
        futex_wake(notify);
}

// ожидание состояния *state != 0 с танцем битов (как wait_progress_futex
// в managarm). Ключевой момент: ПРОТУХШИЙ бит (поставленный ранее, когда
// состояние уже забрали) обязан сниматься при наблюдении пустого состояния,
// иначе спящий будет ждать вечно — producer не будит, если бит уже стоит.
// Поэтому: состояние пусто + бит стоит -> снять бит, перепроверить
// состояние, и только потом парковаться.
static int mlq_wait_state(volatile int *notify, int bit,
                           volatile int *state, int mode) {
    for (;;) {
        if (__atomic_load_n(state, __ATOMIC_ACQUIRE)) return 1;
        int n = __atomic_load_n(notify, __ATOMIC_RELAXED);
        if (n & bit) {
            __atomic_fetch_and(notify, ~bit, __ATOMIC_ACQ_REL);
            if (__atomic_load_n(state, __ATOMIC_ACQUIRE)) return 1;
            continue; // бит снят: можно парковаться
        }
        int val = __atomic_load_n(notify, __ATOMIC_RELAXED);
        if (mode == MLQ_FUTEX) {
            futex_wait(notify, val); // EAGAIN = значение сменилось: цикл
        } else if (mode == MLQ_WATCHDOG) {
            if (futex_wait_to(notify, val, 50) == -110) return 0; // ETIMEDOUT
        } else {
            sys3(SYS_sched_yield, 0, 0, 0);
        }
    }
}

// --- producer: пишет элементы партиями внутри чанка ---

struct mlq_out {
    struct mlq_pair *p;
    int dir;                 // направление: 0 запросы, 1 ответы
    int cur;                 // чанк в работе (0 = нет)
    u32 off;                 // смещение записи в cur->buf
    int enq;                 // сколько элементов в текущей партии (статистика)
    int spare;               // остаток свободной цепочки (процесс-приватный)
};

static void mlq_out_init(struct mlq_out *o, struct mlq_pair *p, int dir) {
    o->p = p; o->dir = dir; o->cur = 0; o->off = 0; o->enq = 0; o->spare = 0;
}

static int mlq_flush(struct mlq_out *o);

// ждём свободный чанк и берём его (flow control: не больше NCHUNKS в полёте).
// pop забирает всю цепочку: голову используем, хвост прячем в spare
// (иначе хвостовые чанки утекли бы — у producer'а больше нет ссылки)
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
        // свободных нет: ждём SUPPLY-бит (манагарм-танец)
        mlq_wait_state(&h->notify, MLQ_N_SUPPLY, &h->free, mode);
    }
}

// положить элемент (без публикации): партия едет одним flush
static int mlq_put_elem(struct mlq_out *o, u32 opcode, const void *payload,
                        u32 len, u32 blob, int mode) {
    if (!o->cur) o->cur = mlq_take_chunk(o, mode);
    struct mlq_chunk *c = mlq_chunk_at(o->p, o->cur);
    u32 need = (u32)sizeof(struct mlq_elem) + (blob ? 0 : len);
    if (o->off + need > sizeof(c->buf)) {
        // чанк кончился: публикуем и берём следующий
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

// резерв blob-слота (flow control: не больше NBLOBS/2 на направление)
static int mlq_take_blob(struct mlq_out *o, int mode) {
    struct mlq_hdr *h = &o->p->q[o->dir];
    for (;;) {
        int idx = mlq_pop(&h->blob_free, o->p);
        if (idx) return idx;
        mlq_wait_state(&h->notify, MLQ_N_BLOB, &h->blob_free, mode);
    }
}

// M8: путь выбирается по размеру — мелкие inline, крупные через blob-слот
static int mlq_put(struct mlq_out *o, u32 opcode, const void *payload, u32 len,
                   int mode) {
    if (len > MLQ_BLOB) return -1;
    if (len > MLQ_INLINE_MAX) {
        int b = mlq_take_blob(o, mode);
        if (b < 0) return b;
        m_memcpy(mlq_blob_at(o->p, b), payload, len);
        int r = mlq_put_elem(o, opcode, 0, len, (u32)b, mode);
        // payload уже в слоте: публикуем немедленно, иначе потребитель не
        // увидит элементы, а слоты кончатся — взаимоблокировка на пачках
        if (r == 0) mlq_flush(o);
        return r;
    }
    return mlq_put_elem(o, opcode, payload, len, 0, mode);
}

// опубликовать партию: progress (release) -> ready -> notify
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

// --- consumer: читает элементы прямо из общей памяти (zero copy) ---
struct mlq_view {
    u32 opcode;
    u32 len;
    const void *payload;     // указатель В ОБЩУЮ память: действует до consume
    u32 blob;                // 0 = inline, иначе номер blob-слота (M8)
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

// вернуть producer'у blob-слот просмотренного элемента (для inline — no-op)
static void mlq_consume(struct mlq_in *in, struct mlq_view *v) {
    if (v->blob) {
        struct mlq_hdr *h = &in->p->q[in->dir];
        mlq_push_blob(in->p, &h->blob_free, (int)v->blob);
        mlq_notify(&h->notify, MLQ_N_BLOB);
        v->blob = 0;
    }
}

// вернуть чанк producer'у (обработан) — flow control в обратную сторону
static void mlq_recycle(struct mlq_in *in) {
    struct mlq_hdr *h = &in->p->q[in->dir];
    mlq_push(&h->free, in->p, in->cur);
    mlq_notify(&h->notify, MLQ_N_SUPPLY);
    in->cur = 0; in->off = 0; in->end = 0;
}

// следующий элемент; ждёт по режиму; 1 = есть view, 0 = нет данных
// (MLQ_YIELD — пусто сейчас, MLQ_WATCHDOG — пусто уже 50мс)
static int mlq_next(struct mlq_in *in, struct mlq_view *v, int mode) {
    struct mlq_hdr *h = &in->p->q[in->dir];
    for (;;) {
        if (!in->cur) {
            int idx = mlq_pop(&h->ready, in->p);
            if (!idx) {
                // данных нет: ждём DATA-бит (манагарм-танец)
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
            // чанк обработан целиком (DONE гарантирует: новых байтов не будет).
            // следующего чанка в цепочке читаем ДО recycle (push затрёт next)
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
        if (e->blob) {
            // M8: payload в blob-слоте; мусорные ссылки пропускаем целиком
            int lo = (in->dir ? MLQ_NBLOBS / 2 : 0) + 1;
            int hi = lo + MLQ_NBLOBS / 2 - 1;
            if (e->blob < (u32)lo || e->blob > (u32)hi || e->len > MLQ_BLOB) {
                in->off = in->end;
                continue;
            }
            v->payload = mlq_blob_at(in->p, (int)e->blob);
            v->blob = e->blob;
            return 1;
        }
        // inline: мусорный len от недобросовестного producer'а не читаем
        if ((u64)eoff + sizeof(struct mlq_elem) + e->len > in->end) {
            in->off = in->end;
            continue;
        }
        v->payload = e + 1;
        return 1;
    }
}

#endif
