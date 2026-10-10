#!/bin/sh
# carve.sh — хирургия исходников Linux: проект MiLin, этап M3.
# Физически удаляет подсистемы из дерева и чинит Kbuild/Kconfig так,
# чтобы сборка не замечала пропажу. Коммиты делает вызывающий.
#
# usage: carve.sh <step>       (TREE_DIR=<путь к клону linux>)
# steps: arch | deadtop | subsys | drivers | fs | kernel | all
#
# Принцип: каждый шаг = один коммит в форке; после шага дерево обязано
# собираться и грузиться (проверяет вызывающий через build.sh + run-qemu.sh).

set -e
TREE="${TREE_DIR:?установи TREE_DIR=<путь к клону linux>}"
HERE="$(cd "$(dirname "$0")" && pwd)"

say() { printf '[carve] %s\n' "$1"; }

fix() {
    say "чистка мёртвых ссылок Kbuild/Kconfig"
    python3 "$HERE/fix-dead-refs.py" "$TREE"
}

# --- шаг 1: все архитектуры кроме x86 -------------------------------------
# arch/Kconfig (файл) — общий заголовок arch-опций, его оставляем.
step_arch() {
    say "arch: удаляю все архитектуры кроме x86"
    find "$TREE/arch" -mindepth 1 -maxdepth 1 -type d ! -name x86 -exec rm -rf {} +
    # hostfs — файловая система UML (чужая архитектура); ванильный fs/Makefile
    # тянет её безусловно, а она инклудит arch/um/scripts/Makefile.rules
    rm -rf "$TREE/fs/hostfs"
    fix
}

# --- шаг 2: заведомо мёртвый груз верхнего уровня --------------------------
# Documentation/samples — не участвуют в vmlinux; rust/certs — выключены;
# usr/ — built-in initramfs (мы грузим внешний initrd); io_uring выключен.
# Швы: sched (io_wq-стабы), initramfs (__initramfs_size), init/Kconfig
# (меню RD_* переезжает из usr/Kconfig), console_on_rootfs (узел /dev/console).
step_deadtop() {
    say "deadtop: Documentation, samples, rust, certs, usr, io_uring"
    rm -rf "$TREE/Documentation" "$TREE/samples" "$TREE/rust" \
           "$TREE/certs" "$TREE/usr" "$TREE/io_uring"
    fix
    python3 - "$TREE" <<'PYEOF'
import sys
tree = sys.argv[1]

def patch(path, old, new):
    p = f"{tree}/{path}"
    s = open(p).read()
    if new in s:
        return  # уже нашито (идемпотентность)
    assert old in s, f"{path}: pattern not found"
    open(p, 'w').write(s.replace(old, new, 1))
    print(f"  stitch: {path}")

patch('kernel/sched/core.c',
      '#include "../../io_uring/io-wq.h"',
      '#ifdef CONFIG_IO_URING\n'
      '#include "../../io_uring/io-wq.h"\n'
      '#else\n'
      '/* MiLin: io_uring is carved away, PF_IO_WORKER cannot exist;\n'
      ' * stubs only keep the generic scheduler paths compilable */\n'
      'static inline void io_wq_worker_sleeping(struct task_struct *tsk) {}\n'
      'static inline void io_wq_worker_running(struct task_struct *tsk) {}\n'
      '#endif')

patch('init/initramfs.c',
      'extern unsigned long __initramfs_size;',
      '/* MiLin: usr/ (built-in initramfs blob) removed from the tree;\n'
      ' * the blob is always empty, we boot from an external initrd\n'
      ' * unpacked by the very same code path */\n'
      'unsigned long __initramfs_size = 0;')

patch('init/main.c',
      'void __init console_on_rootfs(void)\n'
      '{\n'
      '\tstruct file *file = filp_open("/dev/console", O_RDWR, 0);\n'
      '\n'
      '\tif (IS_ERR(file)) {\n'
      '\t\tpr_err("Warning: unable to open an initial console.\\n");\n'
      '\t\treturn;\n'
      '\t}',
      'void __init console_on_rootfs(void)\n'
      '{\n'
      '\tstruct file *file;\n'
      '\n'
      '\t/* MiLin: the initramfs is built unprivileged and carries no device\n'
      '\t * nodes (the vanilla default-cpio blob from usr/ is carved away);\n'
      '\t * create the initial console node ourselves if it is missing */\n'
      '\tfile = filp_open("/dev/console", O_RDWR, 0);\n'
      '\tif (IS_ERR(file) && PTR_ERR(file) == -ENOENT) {\n'
      '\t\tinit_mkdir("/dev", 0755);\n'
      '\t\tinit_mknod("/dev/console", S_IFCHR | 0600,\n'
      '\t\t\t   new_encode_dev(MKDEV(5, 1)));\n'
      '\t\tfile = filp_open("/dev/console", O_RDWR, 0);\n'
      '\t}\n'
      '\n'
      '\tif (IS_ERR(file)) {\n'
      '\t\tpr_err("Warning: unable to open an initial console. err=%ld\\n",\n'
      '\t\t       PTR_ERR(file));\n'
      '\t\treturn;\n'
      '\t}')
PYEOF
    # меню декомпрессоров внешнего initrd: из usr/Kconfig в init/Kconfig
    python3 - "$TREE" <<'PYEOF'
import sys, subprocess
tree = sys.argv[1]
k = f"{tree}/init/Kconfig"
s = open(k).read()
if 'config RD_GZIP' in s:
    print("  stitch: init/Kconfig (RD_* already present)")
    sys.exit(0)
rd = subprocess.run(
    ['git', '-C', tree, 'show', 'HEAD:usr/Kconfig'],
    capture_output=True, text=True).stdout
assert rd, "usr/Kconfig not in HEAD history? carve шаг уже применён?"
out, in_choice = [], False
for line in rd.splitlines()[rd.splitlines().index('config RD_GZIP'):]:
    if line.startswith('choice'):
        in_choice = True          # choice-блок = built-in blob, он удалён
    if not in_choice:
        out.append(line)
block = ('# ------------------------------------------------------------------\n'
         '# MiLin: внешний initrd и его декомпрессоры живут здесь (init/),\n'
         '# потому что usr/ (built-in initramfs) вырезан из дерева. Блоки\n'
         '# RD_* перенесены дословно из usr/Kconfig (v7.2).\n'
         '# ------------------------------------------------------------------\n'
         'menu "External initrd decompression"\n\n'
         + '\n'.join(out) + '\nendmenu\n')
open(k, 'a').write('\n' + block)
print("  stitch: init/Kconfig (RD_* menu appended)")
PYEOF
}

# --- шаг 3: подсистемы, выключенные ещё в M2 --------------------------------
# sound, net, block, crypto, virt(+kvm), ipc — всё это за пределами
# «судьбоносного треугольника» (sched/mm/IPC-ABI), вернётся как userspace.
# security/ не удаляем, а ужимаем до двух безусловно-линкуемых файлов:
# commoncap.o (cap_capable/cap_ptrace_* — kernel/capability.c и ptrace)
# и min_addr.o (mmap_min_addr) — это клей, а не подсистема.
step_subsys() {
    say "subsys: sound, net, block, crypto, virt, x86/kvm; security -> 2 файла"
    rm -rf "$TREE/sound" "$TREE/net" "$TREE/block" "$TREE/crypto" \
           "$TREE/virt" "$TREE/arch/x86/kvm"
    for f in "$TREE"/security/*; do
        b="$(basename "$f")"
        case " commoncap.c min_addr.c Makefile Kconfig " in
            *" $b "*) ;;
            *) rm -rf "$f" ;;
        esac
    done
    cat > "$TREE/security/Makefile" <<'EOF'
# SPDX-License-Identifier: GPL-2.0
# MiLin: only unconditionally-linked security glue survives the carve
obj-y                           += commoncap.o min_addr.o
EOF
    fix
}

# --- шаг 4: drivers — оставляем минимум -------------------------------------
# base (модель драйверов), tty (serial-консоль), char (misc/random),
# virtio (транспорт будущих userspace-серверов), firmware, uio (M4).
# clocksource/rtc возвращены после первой попытки: arch/x86 безусловно
# линкуется с i8253 PIT (clockevent_i8253_*) и CMOS RTC (mc146818_*).
# M4: pci — шина для userspace-драйверов (uio_pci_generic); video —
# screen_info_pci.o, на который ссылается vgaarb из drivers/pci.
DRIVERS_KEEP="base tty char virtio firmware uio clocksource rtc pci video"
step_drivers() {
    say "drivers: оставляю только: $DRIVERS_KEEP"
    for d in "$TREE"/drivers/*/; do
        d="$(basename "$d")"
        keep=0
        for k in $DRIVERS_KEEP; do
            [ "$d" = "$k" ] && keep=1
        done
        [ "$keep" = 0 ] && rm -rf "$TREE/drivers/$d"
    done
    fix
}

# --- шаг 5: fs — ядро ВФС + ramfs (rootfs) + fuse + контракт серверов -------
# M4: iomap (select у FUSE_FS), sysfs+kernfs (discovery-интерфейс серверов:
# привязка device-id к uio_pci_generic идёт через /sys/bus/.../new_id)
FS_KEEP="ramfs fuse iomap sysfs kernfs"
step_fs() {
    say "fs: из подкаталогов оставляю только: $FS_KEEP"
    for d in "$TREE"/fs/*/; do
        d="$(basename "$d")"
        keep=0
        for k in $FS_KEEP; do
            [ "$d" = "$k" ] && keep=1
        done
        [ "$keep" = 0 ] && rm -rf "$TREE/fs/$d"
    done
    fix
}

# --- шаг 6: внутренности kernel/ и хост-утилиты -----------------------------
# bpf/trace/cgroup/livepatch/power/kexec/gcov выключены конфигом или
# переехали; kernel/events/ НЕ режем: в v7.2 PERF_EVENTS без промпта
# (always-on), arch/x86/events — obj-y, perf-ядро — часть скелета.
# tools/: в v7.2 CONFIG_OBJTOOL=y (mainline-дефолт), objtool нужен сборке —
# оставляем его машинерию, хост-утилиты (perf, selftests и пр.) — под нож.
TOOLS_KEEP="objtool build include scripts lib arch"
step_kernel() {
    say "kernel: bpf, trace, cgroup, livepatch, power, kexec, gcov"
    rm -rf "$TREE/kernel/bpf" "$TREE/kernel/trace" \
           "$TREE/kernel/cgroup" "$TREE/kernel/livepatch" "$TREE/kernel/power" \
           "$TREE/kernel/kexec" "$TREE/kernel/gcov"
    say "tools: оставляю $TOOLS_KEEP (objtool жив: CONFIG_OBJTOOL=y)"
    for d in "$TREE"/tools/*/; do
        d="$(basename "$d")"
        keep=0
        for k in $TOOLS_KEEP; do
            [ "$d" = "$k" ] && keep=1
        done
        [ "$keep" = 0 ] && rm -rf "$TREE/tools/$d"
    done
    fix
    # шов: syscall-трейспойнты (CONFIG_HAVE_SYSCALL_TRACEPOINTS жил в
    # вырезанном kernel/trace/Kconfig; в v7.2 определения уже здесь)
    python3 - "$TREE" <<'PYEOF'
import sys
p = sys.argv[1] + '/kernel/entry/syscall-common.c'
s = open(p).read()
new = ('/* MiLin: kernel/trace/ is carved away, so CONFIG_HAVE_SYSCALL_TRACEPOINTS\n'
       ' * does not exist and the syscall tracepoints cannot be defined; local\n'
       ' * no-op stubs keep the generic entry paths compilable */\n'
       '#ifdef CONFIG_HAVE_SYSCALL_TRACEPOINTS\n'
       '#define CREATE_TRACE_POINTS\n'
       '#include <trace/events/syscalls.h>\n'
       '#else\n'
       'static inline void trace_sys_enter(struct pt_regs *regs, long syscall) {}\n'
       'static inline void trace_sys_exit(struct pt_regs *regs, long ret) {}\n'
       '#endif')
if new in s:
    print("  stitch: kernel/entry/syscall-common.c (already)")
    sys.exit(0)
old = '#define CREATE_TRACE_POINTS\n#include <trace/events/syscalls.h>\n'
assert old in s, "syscall-common.c: pattern not found"
open(p, 'w').write(s.replace(old, new, 1))
print("  stitch: kernel/entry/syscall-common.c")
PYEOF
}

usage() {
    echo "usage: TREE_DIR=<clone> carve.sh <arch|deadtop|subsys|drivers|fs|kernel|all>"
    exit 1
}

case "${1:-}" in
    arch)    step_arch ;;
    deadtop) step_deadtop ;;
    subsys)  step_subsys ;;
    drivers) step_drivers ;;
    fs)      step_fs ;;
    kernel)  step_kernel ;;
    all)     step_arch; step_deadtop; step_subsys; step_drivers; step_fs; step_kernel ;;
    *)       usage ;;
esac

say "готово: $1"
