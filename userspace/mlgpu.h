// MiLin mlgpu (M11): wire-протокол фасада amdgpu.
// Зеркало linux/drivers/mlin/mlgpu.h — менять синхронно.
// Формат запроса: [ u8 op ][ payload ], ответ — чистый payload.
// Транспорт: /dev/amdgpu (placement=kernel, kthread домена) или lane
// "gpu.amdgpu" через шину (placement=user, демон /sbin/gpud).
#ifndef MILIN_MLGPU_H
#define MILIN_MLGPU_H

#define MLGPU_API 1

// op-коды
#define MLGPU_INFO   'I'
#define MLGPU_ALLOC  'A'
#define MLGPU_FREE   'F'
#define MLGPU_SUBMIT 'S'
#define MLGPU_WAIT   'W'
#define MLGPU_SUBMITB 'B'  // bulk-submit (M22): командный буфер в арене

// режим фасада: связь с amdgpu
#define MLGPU_MODE_FACADE 0  // amdgpu нет: эмуляция
#define MLGPU_MODE_AMDGPU 1  // драйвер amdgpu в ядре, устройства нет
#define MLGPU_MODE_HW     2  // AMD-устройство найдено (форвардинг в DRM)

struct mlgpu_info_out {
    char name[16];
    u32 api;
    u32 mode;
    u32 nslots;
    u32 pad;
    u64 vram_bytes;
    u64 fences;
    u64 allocs;
};

struct mlgpu_alloc_in { u64 size; };
struct mlgpu_alloc_out { u64 handle; u64 offset; };
struct mlgpu_free_in { u64 handle; };
struct mlgpu_submit_in { u64 handle; u32 ndw; u32 pad; }; // + u32 cmd[ndw]
struct mlgpu_submit_out { u64 fence; };
// bulk-submit (M22): ответ несёт и fence, и сумму dwords — клиент сверяет
// с локальной, байты реально прошли через арену (не только ссылка)
struct mlgpu_submitb_out { u64 fence; u64 sum; };
struct mlgpu_wait_in { u64 fence; };
struct mlgpu_wait_out { u32 signaled; u32 pad; };

// MLGPU_SUBMITB ходит только по lane с ареной (ML_TR_BULK): tail =
// [op][mlgpu_submit_in], dwords в слоте арены; head-слоты запроса:
// h[0] = номер слота, h[1] = число байт (ndw*4) — как LOG_WRITEB у log-сервера.

#endif
