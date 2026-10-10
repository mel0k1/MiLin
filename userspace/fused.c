// /sbin/fused (M4): файловая система в userspace через FUSE.
// Ядро оставляет себе только ВФС и transport (/dev/fuse); семантика
// файловой системы — целиком здесь: FUSE_INIT, LOOKUP, GETATTR, ACCESS.
// Содержимое: /mnt/README — статический файл.
//
// ABI сверен с include/uapi/linux/fuse.h v7.2 (FUSE_KERNEL 7.45).

#include "milin.h"

// --- fuse ABI ---
#define FUSE_LOOKUP  1
#define FUSE_GETATTR 3
#define FUSE_INIT    26
#define FUSE_ACCESS  34

#define FUSE_ROOT_ID 1
#define README_INO   2

struct fuse_in_header {
    u32 len;
    u32 opcode;
    u64 unique;
    u64 nodeid;
    u32 uid, gid, pid;
    u16 total_extlen;
    u16 padding;
};

struct fuse_out_header {
    u32 len;
    i32 error;
    u64 unique;
};

struct fuse_attr {
    u64 ino, size, blocks, atime, mtime, ctime;
    u32 atimensec, mtimensec, ctimensec;
    u32 mode, nlink, uid, gid, rdev, blksize, flags;
};

struct fuse_init_out {
    u32 major, minor, max_readahead, flags;
    u16 max_background, congestion_threshold;
    u32 max_write, time_gran;
    u16 max_pages, map_alignment;
    u32 flags2, max_stack_depth;
    u16 request_timeout, unused[11];
};

struct fuse_entry_out {
    u64 nodeid, generation, entry_valid, attr_valid;
    u32 entry_valid_nsec, attr_valid_nsec;
    struct fuse_attr attr;
};

struct fuse_attr_out {
    u64 attr_valid;
    u32 attr_valid_nsec, dummy;
    struct fuse_attr attr;
};

static struct fuse_attr *fill_attr(struct fuse_attr *a, u64 ino) {
    int dir = (ino == FUSE_ROOT_ID);
    a->ino = ino;
    a->size = dir ? 4096 : 13;
    a->blocks = 1;
    a->atime = a->mtime = a->ctime = 0;
    a->atimensec = a->mtimensec = a->ctimensec = 0;
    a->mode = dir ? 0040755 : 0100644;
    a->nlink = dir ? 3 : 1;
    a->uid = 0;
    a->gid = 0;
    a->rdev = 0;
    a->blksize = 4096;
    a->flags = 0;
    return a;
}

static u8 req_buf[8192];
static u8 out_buf[512];

static long reply(u64 unique, i32 error, const void *payload, u32 plen) {
    struct fuse_out_header *h = (struct fuse_out_header *)out_buf;
    h->len = sizeof(*h) + plen;
    h->error = error;
    h->unique = unique;
    for (u32 i = 0; i < plen; i++)
        out_buf[sizeof(*h) + i] = ((const u8 *)payload)[i];
    return sys3(SYS_write, 3, (long)out_buf, h->len);
}

void milin_main(void) {
    long fd = sys3(SYS_open, (long)"/dev/fuse", O_RDWR, 0);
    if (fd < 0) {
        P("fused: /dev/fuse: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }

    sys3(SYS_mkdir, (long)"/mnt", 0755, 0);

    // fd сервера передаётся ядру опцией mount
    char opts[64];
    char *p = opts;
    const char *pre = "fd=";
    for (int i = 0; pre[i]; i++) *p++ = pre[i];
    // fd обычно 3, но не полагаемся: печатаем реальный номер
    long n = fd;
    char tmp[12];
    int ti = 0;
    do { tmp[ti++] = '0' + (n % 10); n /= 10; } while (n);
    while (ti) *p++ = tmp[--ti];
    const char *post = ",rootmode=040000,user_id=0,group_id=0,allow_other";
    for (int i = 0; post[i]; i++) *p++ = post[i];
    *p = 0;

    if (sys5(SYS_mount, (long)"milin-fs", (long)"/mnt", (long)"fuse", 0, (long)opts) != 0) {
        P("fused: mount /mnt: FAIL\n");
        sys3(SYS_exit, 1, 0, 0);
    }
    P("fused: /mnt смонтирован (fuse), жду запросы ядра\n");

    for (;;) {
        long rd = sys3(SYS_read, fd, (long)req_buf, sizeof(req_buf));
        if (rd <= 0) {
            // umount от супервизора -> EOF: штатное завершение сервера
            P("fused: соединение закрыто, выхожу\n");
            sys3(SYS_exit, 0, 0, 0);
        }

        struct fuse_in_header *ih = (struct fuse_in_header *)req_buf;
        const char *name = (const char *)(req_buf + sizeof(*ih));

        switch (ih->opcode) {
        case FUSE_INIT: {
            struct fuse_init_out o = { 0 };
            o.major = 7;
            o.minor = 45; // echo: FUSE_KERNEL_MINOR_VERSION v7.2
            o.max_background = 12;
            o.congestion_threshold = 6;
            o.max_write = 4096;
            reply(ih->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_LOOKUP: {
            // имя после in-header; знаем только README
            int match = 1;
            const char *want = "README";
            for (int i = 0; want[i] || name[i]; i++)
                if (name[i] != want[i]) { match = 0; break; }
            if (!match) {
                reply(ih->unique, -2 /*ENOENT*/, 0, 0);
                break;
            }
            struct fuse_entry_out e = { 0 };
            e.nodeid = README_INO;
            e.entry_valid = 86400;
            e.attr_valid = 86400;
            fill_attr(&e.attr, README_INO);
            reply(ih->unique, 0, &e, sizeof(e));
            break;
        }
        case FUSE_GETATTR: {
            if (ih->nodeid != FUSE_ROOT_ID && ih->nodeid != README_INO) {
                reply(ih->unique, -2 /*ENOENT*/, 0, 0);
                break;
            }
            struct fuse_attr_out a = { 0 };
            a.attr_valid = 86400;
            fill_attr(&a.attr, ih->nodeid);
            reply(ih->unique, 0, &a, sizeof(a));
            break;
        }
        case FUSE_ACCESS:
            // сервер разрешает всем (allow_other, default_permissions off)
            reply(ih->unique, 0, 0, 0);
            break;
        default:
            reply(ih->unique, -38 /*ENOSYS*/, 0, 0);
        }
    }
}
