// MiLin wire protocol: формат сообщений и opcodes, общий для nostdlib
// и glibc-сборок. Инклюдер обязан предоставить typedef u32/u64.
#ifndef MILIN_MLPROTO_H
#define MILIN_MLPROTO_H

// --- формат сообщения (bragi-стиль) ---
// один пакет = одно сообщение: [ struct ml_msg ][ tail: tail_len байт ]
// head — inline-поля фиксированного размера (как head(128) в bragi).
#define ML_HEAD 24
#define ML_MAXTAIL 256
#define ML_MAXHOOKS 4

// имена сущностей: head-слоты 0-1 (16 байт), транспорт — слот 2
#define ML_MAXNAME 16

struct ml_msg {
    u32 proto;
    u32 msg_id;
    u32 err;        // 0 = OK, иначе код ошибки протокола
    u32 tail_len;
    char head[ML_HEAD];
};

// inline-поля head как u64-слоты (bragi хранит скаляры в head)
static inline u64 *ml_h(struct ml_msg *m, int i) { return (u64 *)(m->head + i * 8); }

// коды ошибок — как enum Error в mbus.bragi
#define ML_E_OK              0
#define ML_E_NO_SUCH_ENTITY  1
#define ML_E_PERM            2  // отказ по capability-политике (M7)
#define ML_E_BUSY            3  // имя уже занято (M8)
#define ML_E_IO              4  // транспорт не создался (M8)

// имя обязано быть NUL-terminated в пределах слотов 0-1
static inline int ml_name_ok(const char *s) {
    for (int i = 0; i < ML_MAXNAME; i++) if (!s[i]) return 1;
    return 0;
}

// --- M7: capability-модель на lane'ах ---
#define ML_CAP_WRITE  (1u << 0)  // LOG_WRITE / LOG_WRITEQ
#define ML_CAP_STATS  (1u << 1)  // LOG_STATS
#define ML_CAP_CLOSE  (1u << 2)  // LOG_CLOSE (закрытие helq-сессии)
#define ML_CAP_ADMIN  (1u << 3)  // LOG_RESET (админ-операция)

// --- протокол реестра (ML_PROTO_BUS, идея mbus) ---
#define ML_PROTO_BUS 1
#define BUS_REGISTER   1
#define BUS_REGISTER_R 2
#define BUS_LOOKUP     3
#define BUS_LOOKUP_R   4
#define BUS_LANE_REQ   5
#define BUS_WHEREIS    6
#define BUS_WHEREIS_R  7
#define BUS_REVOKE     8

// --- протокол log-сервера (ML_PROTO_LOG) ---
#define ML_PROTO_LOG 2
#define LOG_WRITE    1
#define LOG_WRITE_R  2
#define LOG_STATS    3
#define LOG_STATS_R  4
#define LOG_CLOSE    5
#define LOG_CLOSE_R  6
#define LOG_WRITEQ   7
#define LOG_RESET    8
#define LOG_RESET_R  9

// --- транспорт lane'а: head[2] в LookupRequest ---
#define ML_TR_SEQPACKET 0    // AF_UNIX SOCK_SEQPACKET (M5)
#define ML_TR_HELQ      1    // асинхронные очереди в разделяемой памяти (M5.1)

#endif
