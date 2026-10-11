// /sbin/fused (M4): файловая система в userspace через FUSE.
// Ядро оставляет себе только ВФС и transport (/dev/fuse); семантика
// файловой системы — целиком здесь: FUSE_INIT, LOOKUP, GETATTR, ACCESS.
// M9.1: OPEN/READ — файл не только виден (stat), но и читается:
// /mnt/README отдаёт содержимое непривилегированному клиенту.
// M5.2: WRITE/READ на /mnt/note.txt (DIRECT_IO, мимо page cache),
// OPENDIR/READDIR — каталог перечисляется getdents64'ом. Права решает
// сервер: default_permissions выключен, README на запись не открывается.
//
// ABI сверен с include/uapi/linux/fuse.h v7.2 (FUSE_KERNEL 7.45).

#include "milin.h"

// --- fuse ABI ---
#define FUSE_LOOKUP  1
#define FUSE_GETATTR 3
#define FUSE_OPEN    14
#define FUSE_READ    15
#define FUSE_WRITE   16
#define FUSE_RELEASE 18
#define FUSE_FLUSH   25
#define FUSE_INIT    26
#define FUSE_OPENDIR 27
#define FUSE_READDIR 28
#define FUSE_RELEASEDIR 29
#define FUSE_ACCESS  34

#define FOPEN_DIRECT_IO 1

#define FUSE_ROOT_ID 1
#define README_INO   2
#define NOTE_INO     3

#define DT_DIR 4
#define DT_REG 8

static const char README_TEXT[] = "MiLin FUSE OK\n";
#define README_SIZE (sizeof(README_TEXT) - 1)

static char note_buf[4096];
static u64 note_size;

static const struct {
    u64 ino;
    const char *name;
    u8 type;
} dir_entries[] = {
    { FUSE_ROOT_ID, ".", DT_DIR },
    { FUSE_ROOT_ID, "..", DT_DIR },
    { README_INO, "README", DT_REG },
    { NOTE_INO, "note.txt", DT_REG },
};
#define NDIR 4

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

struct fuse_open_in {
    u32 flags, open_flags;
};

struct fuse_open_out {
    u64 fh;
    u32 open_flags;
};

struct fuse_read_in {
    u64 fh, offset;
    u32 size, read_flags;
    u64 lock_owner;
    u32 flags;
};

struct fuse_write_in {
    u64 fh, offset;
    u32 size, write_flags;
    u64 lock_owner;
    u32 flags;
};

struct fuse_write_out {
    u32 size, padding;
};

struct fuse_dirent {
    u64 ino, off;
    u32 namelen, type;
    char name[];
};

#define FUSE_NAME_OFFSET 24 // offsetof(fuse_dirent, name)

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
    a->size = dir ? 4096 : ino == README_INO ? README_SIZE : note_size;
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
static u8 out_buf[8192];

static long reply(u64 unique, i32 error, const void *payload, u32 plen) {
    struct fuse_out_header *h = (struct fuse_out_header *)out_buf;
    h->len = sizeof(*h) + plen;
    h->error = error;
    h->unique = unique;
    if (plen) m_memcpy(out_buf + sizeof(*h), payload, plen);
    return sys3(SYS_write, 3, (long)out_buf, h->len);
}

static int lookup_idx(const char *name) {
    for (int i = 0; i < NDIR; i++) {
        const char *want = dir_entries[i].name;
        int match = 1;
        for (int j = 0; want[j] || name[j]; j++)
            if (name[j] != want[j]) { match = 0; break; }
        if (match) return i;
    }
    return -1;
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
    P("fused: /mnt смонтирован (fuse): README + note.txt, жду запросы ядра\n");

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
            // имя после in-header
            int idx = lookup_idx(name);
            if (idx < 0) {
                reply(ih->unique, -2 /*ENOENT*/, 0, 0);
                break;
            }
            struct fuse_entry_out e = { 0 };
            e.nodeid = dir_entries[idx].ino;
            // dentry-кэш только для неизменяемого README: note.txt
            // меняется, таймаут 0 заставляет ядро переспрашивать
            e.entry_valid = e.nodeid == README_INO ? 86400 : 0;
            e.attr_valid = e.entry_valid;
            fill_attr(&e.attr, e.nodeid);
            reply(ih->unique, 0, &e, sizeof(e));
            break;
        }
        case FUSE_GETATTR: {
            if (ih->nodeid != FUSE_ROOT_ID &&
                ih->nodeid != README_INO && ih->nodeid != NOTE_INO) {
                reply(ih->unique, -2 /*ENOENT*/, 0, 0);
                break;
            }
            struct fuse_attr_out a = { 0 };
            a.attr_valid = ih->nodeid == README_INO ? 86400 : 0;
            fill_attr(&a.attr, ih->nodeid);
            reply(ih->unique, 0, &a, sizeof(a));
            break;
        }
        case FUSE_OPEN: {
            struct fuse_open_in *oi = (struct fuse_open_in *)(req_buf + sizeof(*ih));
            if (ih->nodeid == FUSE_ROOT_ID) {
                reply(ih->unique, -21/*EISDIR*/, 0, 0);
                break;
            }
            if (ih->nodeid != README_INO && ih->nodeid != NOTE_INO) {
                reply(ih->unique, -2/*ENOENT*/, 0, 0);
                break;
            }
            // README только читается: запрет на запись — забота сервера
            if (ih->nodeid == README_INO && (oi->flags & 3) != O_RDONLY) {
                reply(ih->unique, -13/*EACCES*/, 0, 0);
                break;
            }
            struct fuse_open_out o = { 0 };
            o.fh = 0; // хэндлы не ведём: READ/WRITE идут по nodeid
            o.open_flags = FOPEN_DIRECT_IO; // READ/WRITE доезжают до сервера, не садясь в page cache
            reply(ih->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_OPENDIR: {
            struct fuse_open_out o = { 0 };
            o.fh = FUSE_ROOT_ID; // без FOPEN_CACHE_DIR: readdir всегда с сервера
            reply(ih->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_READ: {
            struct fuse_read_in *ri = (struct fuse_read_in *)(req_buf + sizeof(*ih));
            const char *src = 0;
            u64 fsize = 0;
            if (ih->nodeid == README_INO) { src = README_TEXT; fsize = README_SIZE; }
            else if (ih->nodeid == NOTE_INO) { src = note_buf; fsize = note_size; }
            else { reply(ih->unique, -2 /*ENOENT*/, 0, 0); break; }
            if (ri->offset >= fsize) {
                reply(ih->unique, 0, 0, 0); // EOF
                break;
            }
            u32 n = fsize - ri->offset;
            if (n > ri->size) n = ri->size;
            reply(ih->unique, 0, src + ri->offset, n);
            break;
        }
        case FUSE_WRITE: {
            struct fuse_write_in *wi = (struct fuse_write_in *)(req_buf + sizeof(*ih));
            const u8 *data = req_buf + sizeof(*ih) + sizeof(*wi);
            if (ih->nodeid != NOTE_INO) {
                reply(ih->unique, -13 /*EACCES*/, 0, 0);
                break;
            }
            if (wi->offset + wi->size > sizeof(note_buf)) {
                reply(ih->unique, -28 /*ENOSPC*/, 0, 0);
                break;
            }
            m_memcpy(note_buf + wi->offset, data, wi->size);
            if (wi->offset + wi->size > note_size) note_size = wi->offset + wi->size;
            struct fuse_write_out o = { 0 };
            o.size = wi->size;
            reply(ih->unique, 0, &o, sizeof(o));
            break;
        }
        case FUSE_FLUSH:
        case FUSE_RELEASE:
        case FUSE_RELEASEDIR:
            reply(ih->unique, 0, 0, 0);
            break;
        case FUSE_READDIR: {
            struct fuse_read_in *ri = (struct fuse_read_in *)(req_buf + sizeof(*ih));
            // off в dirent = cookie позиции: ядро вернёт его в следующем
            // READDIR (ctx->pos), пропускаем всё до этого cookie
            u8 *p = out_buf + sizeof(struct fuse_out_header);
            for (int i = 0; i < NDIR; i++) {
                u64 cookie = i + 1;
                if (cookie <= ri->offset) continue;
                u32 nl = 0;
                while (dir_entries[i].name[nl]) nl++;
                struct fuse_dirent *d = (struct fuse_dirent *)p;
                d->ino = dir_entries[i].ino;
                d->off = cookie;
                d->namelen = nl;
                d->type = dir_entries[i].type;
                m_memcpy(p + FUSE_NAME_OFFSET, dir_entries[i].name, nl);
                p += (FUSE_NAME_OFFSET + nl + 7) & ~(u64)7; // рекорды выровнены на 8
            }
            reply(ih->unique, 0, out_buf + sizeof(struct fuse_out_header),
                  (u32)(p - (out_buf + sizeof(struct fuse_out_header))));
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
