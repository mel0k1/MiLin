// /sbin/xbpstest (M14): e2e пакетного менеджера против НАСТОЯЩЕГО репо.
// динамический glibc-бинарь: fork+exec xbps-install (LD_PRELOAD=netsh.so
// уводит сокеты в netd через IPC; getaddrinfo резолвит по TCP:53), ждёт,
// проверяет установленный payload. Репо — repo-default.voidlinux.org по
// https: TLS в самом xbps (openssl из Void), подпись индекса — ключ из
// пакета xbps (60:ae:0c:...), доверие — ca-certificates из rootfs.
// Маркер M14 OK = реальный репо синкнулся, пакет скачан/распакован/запущен.

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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

    // 1) синк + установка из настоящего Void-репо (адрес в /etc/xbps.d):
    //    dash — маленький динамический шелл, glibc-зависимый
    char *env[] = {
        "LD_PRELOAD=/lib/netsh.so",
        "PATH=/usr/bin:/usr/sbin:/bin",
        "HOME=/root",
        "TMPDIR=/tmp",
        "SSL_CA_CERT_FILE=/etc/ssl/certs/ca-certificates.crt",
        0
    };
    char *install[] = { "/usr/bin/xbps-install", "-S", "dash", "-y", 0 };
    // сеть userspace переживает каскад шины (M7.2): попытки xbps-install
    // разнесены, чтобы slirp/TLS-шум реального репо не ронял гейт
    int rc = -1;
    for (int attempt = 0; attempt <= 2; attempt++) {
        if (attempt) sleep(1);
        rc = run(install, env);
        if (rc == 0) break;
    }
    printf("xbpstest: xbps-install rc=%d\n", rc);
    if (rc != 0)
        return 1;

    // 2) payload установлен и исполняем? (динамический, Void glibc)
    if (access("/usr/bin/dash", X_OK) != 0) {
        printf("xbpstest: /usr/bin/dash отсутствует\n");
        return 1;
    }

    // 3) запуск установленного бинаря — вывод уходит в консоль
    char *dash[] = { "/usr/bin/dash", "-c",
                     "echo installed-dash-runs pid=$$ version=$0", 0 };
    rc = run(dash, env);
    printf("xbpstest: dash rc=%d\n", rc);
    if (rc != 0)
        return 1;

    printf("M14 OK: real void repo (https) install + run over userspace network\n");

    // M15: маркер персистентности на реальном корне — persistcheck
    // следующего бута по нему доказывает, что xbps-установка легла на ext4
    mkdir("/var/milin", 0755);
    FILE *mk = fopen("/var/milin/installed", "w");
    if (mk) {
        fputs("dash\n", mk);
        fclose(mk);
        printf("xbpstest: persist marker записан\n");
    }
    return 0;
}
