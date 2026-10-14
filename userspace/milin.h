// MiLin userspace shim: raw syscalls, no libc.
// общий каркас всех binaries: супервизор и серверы.
#ifndef MILIN_H
#define MILIN_H

typedef unsigned long u64;
typedef long i64;
typedef unsigned int u32;
typedef int i32;
typedef unsigned short u16;
typedef unsigned char u8;

static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

static long sys5(long n, long a, long b, long c, long d, long e) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8)
        : "rcx", "r11", "memory");
    return r;
}

static long sys6(long n, long a, long b, long c, long d, long e, long f) {
    long r;
    register long r10 __asm__("r10") = d;
    register long r8  __asm__("r8")  = e;
    register long r9  __asm__("r9")  = f;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
        : "rcx", "r11", "memory");
    return r;
}

// --- номера системных вызовов x86_64 ---
#define SYS_read      0
#define SYS_write     1
#define SYS_open      2
#define SYS_close     3
#define SYS_stat      4
#define SYS_lstat     6
#define SYS_lseek     8
#define SYS_mmap      9
#define SYS_munmap    11
#define SYS_nanosleep 35
#define SYS_getdents64 217
#define SYS_fork      57
#define SYS_execve    59
#define SYS_exit      60
#define SYS_wait4     61
#define SYS_kill      62
#define SYS_getpid    39
#define SYS_getuid    102
#define SYS_setuid    105
#define SYS_mkdir     83
#define SYS_chdir     80
#define SYS_symlink   88
#define SYS_readlink  89
#define SYS_rename    82
#define SYS_unlink    87
#define SYS_fchmod    91
#define SYS_chown     92
#define SYS_chroot    161
#define SYS_mknod     133
#define SYS_mount     165
#define SYS_umount2   166
#define SYS_ioctl     16
#define SYS_dup2      33
#define SYS_umask     95
#define SYS_gettimeofday 96
#define SYS_setgid    106
#define SYS_setsid    112
#define SYS_fsync     74

#define MS_MOVE (1 << 13)

#define O_RDONLY   0
#define O_WRONLY   1
#define O_RDWR     2
#define O_CREAT    0100
#define O_TRUNC    01000
#define O_APPEND   02000
#define O_DIRECTORY 0200000
#define PROT_READ  1
#define PROT_WRITE 2
#define MAP_SHARED 1
#define MNT_DETACH 2
#define SEEK_SET   0

// getdents64: struct linux_dirent64 живёт не в uapi, но ABI стабилен;
// имена в буфере уже NUL-terminированы ядром
struct ldirent64 {
    u64 d_ino;
    i64 d_off;
    u16 d_reclen;
    u8 d_type;
    char d_name[];
};

// gettimeofday/gettimespec-стиль: long-поля, усечение до 32-бит — в utmp.h
struct timeval_k {
    long sec, usec;
};

// --- мини-строки/память: builtins при переменной длине зовут libc ---
static void *m_memcpy(void *d, const void *s, unsigned n) {
    unsigned char *dd = d; const unsigned char *ss = s;
    while (n--) *dd++ = *ss++;
    return d;
}
static void *m_memset(void *d, int c, unsigned n) {
    unsigned char *dd = d;
    while (n--) *dd++ = (unsigned char)c;
    return d;
}
static unsigned m_strlen(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    return n;
}
static int m_strncmp(const char *a, const char *b, unsigned n) {
    while (n && *a && *a == *b) { a++; b++; n--; }
    return n ? (u8)*a - (u8)*b : 0;
}

// --- печать ---
#define P(s) sys3(SYS_write, 1, (long)(s), sizeof(s) - 1)

static void print_str(const char *s) {
    long n = 0;
    while (s[n]) n++;
    sys3(SYS_write, 1, (long)s, n);
}

static void print_dec_n(long v) {
    char buf[24];
    int i = 24;
    int neg = v < 0;
    if (neg) v = -v;
    do { buf[--i] = '0' + (v % 10); v /= 10; } while (v);
    if (neg) buf[--i] = '-';
    sys3(SYS_write, 1, (long)(buf + i), 24 - i);
}

static void print_dec(long v) {
    print_dec_n(v);
    sys3(SYS_write, 1, (long)"\n", 1);
}

// плоский hex (без 0x и перевода строки): маски прав в лог-строках
static void print_hex_flat(u32 v) {
    char buf[8];
    for (int i = 7; i >= 0; i--) {
        int d = v & 15;
        buf[i] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    }
    sys3(SYS_write, 1, (long)buf, 8);
}

static void print_hex(u32 v) {
    char buf[12];
    int i = 12;
    buf[--i] = '\n';
    do {
        int d = v & 15;
        buf[--i] = d < 10 ? '0' + d : 'a' + d - 10;
        v >>= 4;
    } while (v);
    buf[--i] = 'x';
    buf[--i] = '0';
    sys3(SYS_write, 1, (long)(buf + i), 12 - i);
}

static void print_oct(u32 v) {
    char buf[16];
    int i = 16;
    buf[--i] = '\n';
    do { buf[--i] = '0' + (v & 7); v >>= 3; } while (v);
    sys3(SYS_write, 1, (long)(buf + i), 16 - i);
}

static void sleep_ms(long ms) {
    struct { long sec; long nsec; } t = { 0, ms * 1000000L };
    sys3(SYS_nanosleep, (long)&t, 0, 0);
}

// Точка входа: ядро передаёт управление с rsp, выровненным на 16, и БЕЗ
// return address, а ABI gcc ожидает rsp%16==8 (адрес возврата в стеке).
// Без этого trampлина любая movaps в теле ловит general protection fault.
// argc/argv снимаются со стека ДО выравнивания (M19: login читает argv[1]).
void milin_main(void) __attribute__((noreturn));
static int milin_argc __attribute__((used));
static char **milin_argv __attribute__((used));
__attribute__((naked, noreturn)) void _start(void) {
    __asm__ volatile (
        "mov (%rsp), %rax\n"
        "lea 8(%rsp), %rdx\n"
        "mov %rax, milin_argc(%rip)\n"
        "mov %rdx, milin_argv(%rip)\n"
        "and $-16, %rsp\n"
        "call milin_main\n"
        "hlt\n");
}

#endif
