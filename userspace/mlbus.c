// /sbin/mlbus (M5 + M5.1): реестр серверов — идея mbus из Managarm.
//
// Единственная работа шины — рандеву:
//   * сервер: RegisterRequest{имя} и остаётся на связи;
//   * клиент: LookupRequest{имя} -> шина создаёт lane и раздаёт концы:
//     клиенту LookupResponse{hooks:[lane]}, серверу LaneRequest.
// Дальше клиент и сервер общаются напрямую, шина данных не видит.
// Хэндлы (fd) передаются SCM_RIGHTS — как hooks в bragi.
//
// Шина — однопоточный event loop на poll(): слушатель + все соединения.
// Так клиентский LOOKUP, пришедший РАНЬШЕ регистрации сервера, честно
// получает NO_SUCH_ENTITY и повторяет попытку, а шина тем временем
// принимает REGISTER. Последовательный accept-цикл здесь давал
// живую блокировку: шина сидела в чтении клиента и не могла принять
// сервер (M5-баг, всплыл на M5.1).
//
// M5.1 (creds): шина слушает с SO_PASSCRED — ядро прикладывает к каждому
// сообщению struct ucred (pid/uid/gid), подделать их нельзя. При LOOKUP
// creds клиента уезжают серверу в LaneRequest — сервер знает, кто к нему
// постучался (в Managarm личность клиента знает ядро, здесь — шина).
// Лane'овые socketpair'ы тоже размечаются: SO_PASSCRED на оба конца ДО
// раздачи, иначе первое сообщение приходит с мусорными creds.
//
// M5.1 (helq): клиент может попросить транспорт ML_TR_HELQ — тогда шина
// дополнительно создаёт файл в /tmp (ramfs), размечает в нём пару
// асинхронных очередей HelQueue (userspace/helq.h), мапит, раздаёт fd
// обоим концам и unlink'ает: объект живёт, пока живы маппинги —
// классические shm-семантики без CONFIG_TMPFS/CONFIG_MEMFD.
//
// M7 (capabilities): шина — точка выдачи прав. Сервер при REGISTER
// объявляет политику (tail: маски world/root), шина сверяет uid клиента
// (kernel-верифицированный, SO_PASSCRED) и выдаёт lane только если
// права непусты; маска уезжает клиенту (LookupResponse) и серверу
// (LaneRequest tail). Нет прав — нет lane'а. Права сервера шина
// считает доверенными: сервер — TCB, его uid шина знает от ядра.
// Мёртвый сервер с непочищенным именем: LaneRequest уходит ПЕРВЫМ и
// EPIPE на нём честно превращается в NO_SUCH_ENTITY клиенту.

#include "milin.h"
#include "ipc.h"
#include "helq.h"
#include "bulk.h"

#define MAXREG    8
#define MAXCLIENT 8

// --- poll: единственный мультиплексор, который остался в ядре (EPOLL
// вырезан ещё в M2); sys_poll живёт в fs/select.c, всегда в сборке ---
struct pollfd {
    int fd;
    short events;
    short revents;
};
#define POLLIN 0x001

struct reg {
    int used;
    char name[24];
    long conn;      // соединение с сервером (его канал к шине)
    u32 pid;        // pid сервера (creds при REGISTER) — для WHEREIS (M7)
    u32 world;      // права, выдаваемые непривилегированным (M7)
    u32 root;       // права, выдаваемые uid 0 (M7)
};

static struct reg regs[MAXREG];

// --- M7.1: журнал выданных capabilities ---
// шина выдала lane клиенту (по conn её LOOKUP) на сервере — пока клиент
// жив, права в силе; умер (EOF на его конне к шине) — шина шлёт серверу
// REVOKE. Ключ — conn: у процесса может быть несколько коннов (форки).
#define MAXCAP 16
struct issued {
    int used;
    long client;    // conn клиента к шине
    u32 pid;        // pid клиента (creds от ядра)
    long server;    // conn сервера к шине
};
static struct issued issued_caps[MAXCAP];

// одна запись на пару (клиент, сервер): сервер отзывает по pid сразу все
// lane'ы этого клиента, дубликаты не нужны
static void cap_add(long client, u32 pid, long server) {
    for (int i = 0; i < MAXCAP; i++)
        if (issued_caps[i].used && issued_caps[i].client == client
            && issued_caps[i].server == server) {
            issued_caps[i].pid = pid;
            return;
        }
    for (int i = 0; i < MAXCAP; i++)
        if (!issued_caps[i].used) {
            issued_caps[i].used = 1;
            issued_caps[i].client = client;
            issued_caps[i].pid = pid;
            issued_caps[i].server = server;
            return;
        }
}

// клиент умер: его права отзывает владелец (шина), сервер только enforced'ит
static void cap_revoke_client(long client) {
    for (int i = 0; i < MAXCAP; i++) {
        if (!issued_caps[i].used || issued_caps[i].client != client) continue;
        P("mlbus: клиент (pid ");
        print_dec_n(issued_caps[i].pid);
        P(") умер — revoke capability -> серверу\n");
        struct ml_msg rv = { ML_PROTO_BUS, BUS_REVOKE, 0, 0, {0} };
        *ml_h(&rv, 0) = issued_caps[i].pid;
        ml_send(issued_caps[i].server, &rv, 0, 0, 0, 0);
        issued_caps[i].used = 0;
    }
}

// сервер умер: его записи в журнале прав больше не имеют силы
static void cap_drop_server(long server) {
    for (int i = 0; i < MAXCAP; i++)
        if (issued_caps[i].used && issued_caps[i].server == server)
            issued_caps[i].used = 0;
}

static struct reg *reg_by_conn(long fd) {
    for (int i = 0; i < MAXREG; i++)
        if (regs[i].used && regs[i].conn == fd)
            return &regs[i];
    return 0;
}

static struct reg *reg_by_name(const char *name) {
    for (int i = 0; i < MAXREG; i++)
        if (regs[i].used && m_strncmp(regs[i].name, name, 24) == 0)
            return &regs[i];
    return 0;
}

// таблица poll: [0] = слушатель, дальше conn'ы клиентов и серверов
static struct pollfd pfds[1 + MAXREG + MAXCLIENT];
static int npfd = 1;

static void pfd_add(long fd) {
    pfds[npfd].fd = (int)fd;
    pfds[npfd].events = POLLIN;
    pfds[npfd].revents = 0;
    npfd++;
}

static void pfd_del(long fd) {
    for (int i = 1; i < npfd; i++) {
        if (pfds[i].fd == (int)fd) {
            pfds[i] = pfds[npfd - 1];
            npfd--;
            return;
        }
    }
}

// создать shm-файл с размеченной парой очередей; вернуть fd (-1 = ошибка)
static int make_helq_region(int seq) {
    char path[32] = "/tmp/mlq-";
    int n = 9;
    char num[8];
    int k = 0;
    if (!seq) num[k++] = '0';
    while (seq) { num[k++] = '0' + seq % 10; seq /= 10; }
    while (k) path[n++] = num[--k];
    m_memcpy(path + n, ".shm", 5);

    long fd = sys3(SYS_open, (long)path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        P("mlbus: ");
        print_str(path);
        P(": open FAIL\n");
        return -1;
    }
    // размер через запись нулей (ftruncate на ramfs не проверяли)
    static char zero[4096];
    m_memset(zero, 0, sizeof(zero));
    long left = MLQ_SIZE;
    while (left > 0) {
        long w = sys3(SYS_write, fd, (long)zero,
                      left < (long)sizeof(zero) ? left : (long)sizeof(zero));
        if (w <= 0) {
            P("mlbus: shm: write FAIL\n");
            sys3(SYS_close, fd, 0, 0);
            return -1;
        }
        left -= w;
    }
    struct mlq_pair *p = mlq_map_raw((int)fd);
    if (!p) {
        P("mlbus: shm: mmap MAP_SHARED FAIL (ramfs без mmap?)\n");
        sys3(SYS_close, fd, 0, 0);
        return -1;
    }
    mlq_init(p);
    // munmap: страница вернётся, fd уже размечен; mmap у адресатов создастся заново
    sys6(SYS_munmap, (long)p, MLQ_SIZE, 0, 0, 0, 0);
    return (int)fd;
}

// суффикс последнего созданного региона (для unlink после раздачи)
static char last_path[32];

// M22: арена bulk-слотов — та же shm-семантика, что у helq-региона:
// файл в /tmp нужен только на момент раздачи fd, дальше объект живёт
// в памяти, пока живы маппинги адресатов
static int make_bulk_region(int seq) {
    char path[32] = "/tmp/mlb-";
    int n = 9;
    char num[8];
    int k = 0;
    if (!seq) num[k++] = '0';
    while (seq) { num[k++] = '0' + seq % 10; seq /= 10; }
    while (k) path[n++] = num[--k];
    m_memcpy(path + n, ".shm", 5);

    long fd = sys3(SYS_open, (long)path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        P("mlbus: ");
        print_str(path);
        P(": open FAIL\n");
        return -1;
    }
    static char zero[4096];
    m_memset(zero, 0, sizeof(zero));
    long left = MLB_SIZE;
    while (left > 0) {
        long w = sys3(SYS_write, fd, (long)zero,
                      left < (long)sizeof(zero) ? left : (long)sizeof(zero));
        if (w <= 0) {
            P("mlbus: bulk shm: write FAIL\n");
            sys3(SYS_close, fd, 0, 0);
            return -1;
        }
        left -= w;
    }
    struct mlb_hdr *h = mlb_map_raw((int)fd);
    if (!h) {
        P("mlbus: bulk shm: mmap MAP_SHARED FAIL\n");
        sys3(SYS_close, fd, 0, 0);
        return -1;
    }
    mlb_init(h);
    sys6(SYS_munmap, (long)h, MLB_SIZE, 0, 0, 0, 0);
    return (int)fd;
}

static void serve_lookup(struct ml_msg *m, long client, struct ml_aux *aux) {
    const char *name = m->head;
    // transport в head-слоте 2 (слот 0-1 занимает имя, см. lookup_log)
    int transport = (int)*ml_h(m, 2);
    if (!ml_name_ok(name)) return;
    struct reg *r = reg_by_name(name);

    if (!r) {
        P("mlbus: lookup ");
        print_str(name);
        P(": нет такой сущности (как NO_SUCH_ENTITY в mbus)\n");
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_NO_SUCH_ENTITY, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }

    // M7: адмиссионный контроль — uid подтверждён ядром (SO_PASSCRED),
    // права выбираются из политики сервера; uid 0 — привилегированный
    u32 uid = aux && aux->has_cred ? aux->cred.uid : 0;
    u32 rights = (uid == 0) ? r->root : r->world;
    if (!rights) {
        P("mlbus: lookup ");
        print_str(name);
        P(": DENIED: uid ");
        print_dec_n(uid);
        P(" не входит в политику сервера\n");
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_PERM, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }

    // socketpair: [0] -> клиенту, [1] -> серверу. После отправки оба
    // конца живут у адресатов, шина свои копии закрывает.
    // SO_PASSCRED ставим ПРЯМО ЗДЕСЬ, на оба конца, до раздачи: флаг
    // принятому через SCM_RIGHTS сокету не «доезжает» сам, а без него
    // первое сообщение сервер получает с мусорными creds (unmapped uid).
    int sv[2];
    if (ml_socketpair(sv) < 0) {
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_IO, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }
    ml_passcred(sv[0]);
    ml_passcred(sv[1]);

    // helq-очереди или bulk-арена: shm-регион, fd -> оба конца
    static int seq = 1;
    int arena = -1;
    if (transport == ML_TR_HELQ) {
        arena = make_helq_region(seq++);
        if (arena >= 0) {
            m_memcpy(last_path, "/tmp/mlq-", 10);
            int n = 9;
            int s = seq - 1;
            char num[8];
            int k = 0;
            if (!s) num[k++] = '0';
            while (s) { num[k++] = '0' + s % 10; s /= 10; }
            while (k) last_path[n++] = num[--k];
            m_memcpy(last_path + n, ".shm", 5);
        }
    } else if (transport == ML_TR_BULK) {
        arena = make_bulk_region(seq++);
        if (arena >= 0) {
            m_memcpy(last_path, "/tmp/mlb-", 10);
            int n = 9;
            int s = seq - 1;
            char num[8];
            int k = 0;
            if (!s) num[k++] = '0';
            while (s) { num[k++] = '0' + s % 10; s /= 10; }
            while (k) last_path[n++] = num[--k];
            m_memcpy(last_path + n, ".shm", 5);
        }
    }
    if (arena < 0 && transport != ML_TR_SEQPACKET) {
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_IO, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        sys3(SYS_close, sv[0], 0, 0);
        sys3(SYS_close, sv[1], 0, 0);
        return;
    }

    // creds клиента: ядро приложило их само (SO_PASSCRED), шина честно
    // пересылает серверу — сервер знает, кто стучится
    u32 pid = aux && aux->has_cred ? aux->cred.pid : 0;

    // M7: LaneRequest уходит ПЕРВЫМ, права серверу — в tail (маска).
    // Порядок важен: если сервер уже мёртв (SIGKILL, имя ещё не
    // подчистено), sendmsg вернёт EPIPE и клиент получит честную ошибку
    // вместо lane'а в никуда.
    struct ml_msg rs = { ML_PROTO_BUS, BUS_LANE_REQ, ML_E_OK, 0, {0} };
    *ml_h(&rs, 0) = pid;
    *ml_h(&rs, 1) = uid;
    *ml_h(&rs, 2) = transport;
    long to_srv = arena >= 0
        ? ml_send(r->conn, &rs, &rights, 4, (int[]){ sv[1], arena }, 2)
        : ml_send(r->conn, &rs, &rights, 4, (int[]){ sv[1] }, 1);
    if (to_srv < 0) {
        P("mlbus: lookup ");
        print_str(name);
        P(": сервер недоступен (EPIPE), имя подчистено\n");
        r->used = 0;
        sys3(SYS_close, r->conn, 0, 0);
        pfd_del(r->conn);
        sys3(SYS_close, sv[0], 0, 0);
        sys3(SYS_close, sv[1], 0, 0);
        if (arena >= 0) sys3(SYS_close, arena, 0, 0);
        struct ml_msg resp = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_NO_SUCH_ENTITY, 0, {0} };
        ml_send(client, &resp, 0, 0, 0, 0);
        return;
    }

    // LookupResponse клиенту: права — в head-слоте 0 (клиент знает,
    // ЧТО ему разрешено на этом lane'е)
    struct ml_msg rc = { ML_PROTO_BUS, BUS_LOOKUP_R, ML_E_OK, 0, {0} };
    *ml_h(&rc, 0) = rights;
    if (arena >= 0)
        ml_send(client, &rc, 0, 0, (int[]){ sv[0], arena }, 2);
    else
        ml_send(client, &rc, 0, 0, (int[]){ sv[0] }, 1);

    sys3(SYS_close, sv[0], 0, 0);
    sys3(SYS_close, sv[1], 0, 0);
    if (arena >= 0) {
        // unlink после раздачи: объект живёт в памяти, пока живы маппинги
        // (в M2 tmpfs вырезан, имя в /tmp нам больше не нужно)
        sys3(SYS_unlink, (long)last_path, 0, 0);
    }
    // M7.1: lane выдан — фиксируем в журнале прав (отзыв — по смерти клиента)
    cap_add(client, pid, r->conn);
    P("mlbus: lookup ");
    print_str(name);
    P(": uid ");
    print_dec_n(uid);
    P(", права 0x");
    print_hex_flat(rights);
    if (arena >= 0)
        P(transport == ML_TR_BULK ? " + bulk-арена\n" : " + helq-регион\n");
    else
        P("\n");
}

// обработать одно сообщение с соединения; 1 = conn жив, 0 = закрыт
static int bus_msg(long c) {
    struct ml_msg m;
    char tail[ML_MAXTAIL];
    struct ml_aux aux;
    long r = ml_recv(c, &m, tail, sizeof(tail), &aux);
    if (r <= 0) return 0;

    if (m.proto == ML_PROTO_BUS && m.msg_id == BUS_REGISTER) {
        // имя обязано быть NUL-terminated в пределах head — иначе не имя
        if (!ml_name_ok(m.head)) return 1;
        if (reg_by_name(m.head)) {
            P("mlbus: register ");
            print_str(m.head);
            P(": имя занято — отказ (ML_E_BUSY)\n");
            struct ml_msg resp = { ML_PROTO_BUS, BUS_REGISTER_R, ML_E_BUSY, 0, {0} };
            ml_send(c, &resp, 0, 0, 0, 0);
            return 1;
        }
        // M7: политика прав обязательна — tail = { world, root } (u32 x2)
        u32 masks[2] = { 0, 0 };
        if (m.tail_len >= sizeof(masks))
            m_memcpy(masks, tail, sizeof(masks));
        if (!masks[0] && !masks[1]) {
            // сервер не объявил права ни для кого — регистрируем, но
            // ни один LOOKUP не пройдёт (fail-closed)
            P("mlbus: register ");
            print_str(m.head);
            P(": БЕЗ политики — lookup'ы будут отклонены\n");
        }
        int slot = -1;
        for (int i = 0; i < MAXREG; i++)
            if (!regs[i].used) { slot = i; break; }
        struct ml_msg resp = { ML_PROTO_BUS, BUS_REGISTER_R,
                               slot < 0 ? 2 : ML_E_OK, 0, {0} };
        ml_send(c, &resp, 0, 0, 0, 0);
        if (slot < 0) return 1;
        regs[slot].used = 1;
        m_memcpy(regs[slot].name, m.head, 24);
        regs[slot].conn = c;
        regs[slot].world = masks[0];
        regs[slot].root = masks[1];
        regs[slot].pid = aux.has_cred ? aux.cred.pid : 0;
        P("mlbus: register \"");
        print_str(m.head);
        P("\" (uid ");
        print_dec_n(aux.has_cred ? aux.cred.uid : 0);
        P(", pid ");
        print_dec_n(regs[slot].pid);
        P(", world=0x");
        print_hex_flat(regs[slot].world);
        P(" root=0x");
        print_hex_flat(regs[slot].root);
        P(")\n");
        return 1; // серверное соединение живёт в реестре
    }
    if (m.proto == ML_PROTO_BUS && m.msg_id == BUS_LOOKUP) {
        serve_lookup(&m, c, &aux);
        return 1;
    }
    if (m.proto == ML_PROTO_BUS && m.msg_id == BUS_WHEREIS) {
        // M7: pid сервера по имени — личность из creds при REGISTER
        if (!ml_name_ok(m.head)) return 1;
        struct reg *r = reg_by_name(m.head);
        if (!r) {
            struct ml_msg resp = { ML_PROTO_BUS, BUS_WHEREIS_R,
                                   ML_E_NO_SUCH_ENTITY, 0, {0} };
            ml_send(c, &resp, 0, 0, 0, 0);
            return 1;
        }
        struct ml_msg resp = { ML_PROTO_BUS, BUS_WHEREIS_R, ML_E_OK, 0, {0} };
        *ml_h(&resp, 0) = r->pid;
        ml_send(c, &resp, 0, 0, 0, 0);
        P("mlbus: whereis \"");
        print_str(m.head);
        P("\" -> pid ");
        print_dec_n(r->pid);
        P("\n");
        return 1;
    }
    return 1;
}

void milin_main(void) {
    // M7.2: наименьшие привилегии — шине не нужен root (bind абстрактный,
    // shm в /tmp с 0777). Заодно клиент того же uid может убить её в демо
    // каскада — kill разрешён между процессами одного uid
    if (sys3(SYS_setuid, 1000, 0, 0) != 0) {
        P("mlbus: setuid(1000): FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    long lane = ml_listen("milin-bus");
    if (lane < 0) {
        P("mlbus: bind @milin-bus: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    // M5.1: ядро прикладывает к сообщениям creds отправителя
    if (ml_passcred(lane) != 0) {
        P("mlbus: SO_PASSCRED: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("mlbus: реестр lane'ов слушает @milin-bus (SO_PASSCRED, poll)\n");

    pfds[0].fd = (int)lane;
    pfds[0].events = POLLIN;
    pfds[0].revents = 0;
    npfd = 1;

    for (;;) {
        long r = sys3(SYS_poll, (long)pfds, npfd, -1);
        if (r <= 0) continue;

        // новые соединения
        if (pfds[0].revents & POLLIN) {
            long c = ml_accept(lane);
            if (c >= 0) {
                if (npfd < (int)(sizeof(pfds) / sizeof(pfds[0]))) {
                    ml_passcred(c); // флаг accepted-сокет не наследует
                    pfd_add(c);
                } else {
                    sys3(SYS_close, c, 0, 0); // таблица полна — отказ, не утечка
                }
            }
        }

        // сообщения с соединений; pfd_del меняет хвост таблицы —
        // поэтому идём с конца, чтобы swap-remove не пропускал элементы
        for (int i = npfd - 1; i >= 1; i--) {
            if (!(pfds[i].revents & POLLIN)) continue;
            long c = pfds[i].fd;
            if (!bus_msg(c)) {
                // соединение закрыто: если это был сервер — убираем из реестра
                struct reg *r = reg_by_conn(c);
                sys3(SYS_close, c, 0, 0);
                pfd_del(c);
                if (r) {
                    r->used = 0;
                    cap_drop_server(c);
                    P("mlbus: сервер \"");
                    print_str(r->name);
                    P("\" отвалился, имя освобождено\n");
                } else {
                    // M7.1: умер не сервер, а клиент — его права отозвать
                    cap_revoke_client(c);
                }
            }
        }
    }
    __builtin_unreachable();
}
