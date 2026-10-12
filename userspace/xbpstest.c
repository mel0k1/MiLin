// /sbin/xbpstest (M13): e2e пакетного менеджера в госте.
// динамический glibc-бинарь: fork+exec xbps-install (LD_PRELOAD=netsh.so
// уводит сокеты в netd через IPC), ждёт, проверяет установленный payload.
// Маркер M13 OK = репо синкнулся, пакет скачан/распакован, бинарь запущен.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static int run(char **argv, char **env) {
    pid_t p = fork();
    if (p < 0) return -1;
    if (p == 0) {
        execve(argv[0], argv, env);
        _exit(127);
    }
    int st = 0;
    waitpid(p, &st, 0);
    fprintf(stderr, "xbpstest: waitpid st=%#x sig=%d exit=%d\n",
            st, WIFSIGNALED(st) ? WTERMSIG(st) : 0,
            WIFEXITED(st) ? WEXITSTATUS(st) : -1);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int main(void) {
    setvbuf(stdout, 0, _IONBF, 0);

    // 1) синк + установка: репо из /etc/xbps.d (10.0.2.2:8000/repo)
    char *env[] = {
        "LD_PRELOAD=/lib/netsh.so",
        "PATH=/usr/bin:/usr/sbin:/bin",
        "HOME=/root",
        "TMPDIR=/tmp",
        0
    };
    char *install[] = { "/usr/bin/xbps-install", "-Syy", "hello", "-y", 0 };
    int rc = run(install, env);
    printf("xbpstest: xbps-install rc=%d\n", rc);
    if (rc != 0)
        return 1;

    // 2) payload установлен и исполняем?
    if (access("/usr/bin/hello", X_OK) != 0) {
        printf("xbpstest: /usr/bin/hello отсутствует\n");
        return 1;
    }

    // 3) запуск установленного бинаря — вывод уходит в консоль
    char *hello[] = { "/usr/bin/hello", 0 };
    rc = run(hello, env);
    printf("xbpstest: hello rc=%d\n", rc);
    if (rc != 0)
        return 1;

    printf("M13 OK: xbps install + run over userspace network\n");
    return 0;
}
