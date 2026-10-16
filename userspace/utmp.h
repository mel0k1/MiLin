// utmp/wtmp (M21): glibc-совместимый struct utmp x86_64 (384 байта).

#ifndef MILIN_UTMP_H
#define MILIN_UTMP_H

#include "milin.h"

#define UT_LINESIZE 32
#define UT_NAMESIZE 32
#define UT_HOSTSIZE 256

#define UTMP_EMPTY 0
#define UTMP_BOOT  2
#define UTMP_USER  7
#define UTMP_DEAD  8

struct utmp_rec {
    short type;
    short pad;
    int pid;
    char line[UT_LINESIZE];
    char id[4];
    char user[UT_NAMESIZE];
    char host[UT_HOSTSIZE];
    short term, ex;
    int session;
    int tv_sec;
    int tv_usec;
    int addr[4];
    char reserved[20];
};

typedef char utmp_size_check[sizeof(struct utmp_rec) == 384 ? 1 : -1];

static int utmp_line_eq(const char *a, const char *b) {
    for (int i = 0; i < UT_LINESIZE; i++)
        if (a[i] != b[i]) return 0;
    return 1;
}

static void utmp_slot(const char *path, struct utmp_rec *r) {
    static char buf[16 * 384];
    long fd = sys3(SYS_open, (long)path, O_RDWR, 0);
    if (fd < 0) {
        fd = sys3(SYS_open, (long)path, O_RDWR | O_CREAT, 0644);
        if (fd < 0) return;
    }
    long n = sys3(SYS_read, fd, (long)buf, sizeof(buf));
    if (n < 0) n = 0;
    long slot = -1, free = -1;
    for (long off = 0; off + 384 <= n; off += 384) {
        struct utmp_rec *u = (struct utmp_rec *)(buf + off);
        if (utmp_line_eq(u->line, r->line) &&
            (u->type == UTMP_USER || u->type == UTMP_DEAD))
            slot = off;
        if (u->type == UTMP_EMPTY && free < 0) free = off;
    }
    if (slot < 0) slot = (free >= 0) ? free : ((n / 384) < 16 ? n : 15 * 384);
    sys3(SYS_lseek, fd, slot, SEEK_SET);
    sys3(SYS_write, fd, (long)r, 384);
    sys3(SYS_fsync, fd, 0, 0);
    sys3(SYS_close, fd, 0, 0);
}

static void utmp_append(const char *path, struct utmp_rec *r) {
    long fd = sys3(SYS_open, (long)path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    sys3(SYS_write, fd, (long)r, 384);
    sys3(SYS_fsync, fd, 0, 0);
    sys3(SYS_close, fd, 0, 0);
}

static void utmp_write(short type, int pid, const char *line, const char *user) {
    struct timeval_k tv;
    if (sys3(SYS_gettimeofday, (long)&tv, 0, 0) != 0) {
        tv.sec = 0;
        tv.usec = 0;
    }
    struct utmp_rec r;
    m_memset(&r, 0, sizeof(r));
    r.type = type;
    r.pid = pid;
    for (int i = 0; line[i] && i < UT_LINESIZE - 1; i++) r.line[i] = line[i];
    r.id[0] = 'c';
    r.id[1] = 'o';
    for (int i = 0; user[i] && i < UT_NAMESIZE - 1; i++) r.user[i] = user[i];
    r.session = pid;
    r.tv_sec = (int)tv.sec;
    r.tv_usec = (int)tv.usec;
    utmp_slot("/var/run/utmp", &r);
    utmp_append("/var/log/wtmp", &r);
}

#endif
