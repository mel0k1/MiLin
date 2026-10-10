// /init MiLin: первый userspace-процесс, печатает приветствие и спит
static long sys3(long n, long a, long b, long c) {
    long r;
    __asm__ volatile ("syscall"
        : "=a"(r)
        : "a"(n), "D"(a), "S"(b), "d"(c)
        : "rcx", "r11", "memory");
    return r;
}

#define P(s) sys3(1, 1, (long)(s), sizeof(s) - 1)

void _start(void) {
    P("Hello from MiLin! The kernel is alive.\n");
    for (;;) sys3(34, 0, 0, 0); // pause
}
