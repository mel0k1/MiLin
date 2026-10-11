#!/bin/sh
# initramfs M5: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
# + серверы устройств (/sbin/uiod, /sbin/fused).
# базовые binaries — статические, без libc (raw syscalls из userspace/milin.h)
# M8: /sbin/gclient — динамический glibc-клиент (ld.so + libc.so.6 в rootfs)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/rootfs/sbin build/rootfs/dev build/rootfs/mnt
# -ffreestanding: не только -fno-builtin, но и запрет превращать наши
# циклы в вызовы memset/strlen (M5.1: m_memset(нулями 4096) -> memset)
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/init userspace/supervisor.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/mlbus userspace/mlbus.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/logsrv userspace/logsrv.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/uiod userspace/uiod.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/fused userspace/fused.c
# M8: glibc-клиент (динамический) + интерпретатор и libc в rootfs
gcc -O2 -Iuserspace -o build/rootfs/sbin/gclient userspace/gclient.c
mkdir -p build/rootfs/lib64 build/rootfs/lib/x86_64-linux-gnu
for L in /usr/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2 /lib64/ld-linux-x86-64.so.2; do
    [ -f "$L" ] && cp -L "$L" build/rootfs/lib64/ && break
done
for L in /usr/lib/x86_64-linux-gnu/libc.so.6 /lib/x86_64-linux-gnu/libc.so.6; do
    [ -f "$L" ] && cp -L "$L" build/rootfs/lib/x86_64-linux-gnu/ && break
done
[ -f build/rootfs/lib64/ld-linux-x86-64.so.2 ] || { echo "ld.so не найден"; exit 1; }
[ -f build/rootfs/lib/x86_64-linux-gnu/libc.so.6 ] || { echo "libc.so.6 не найдена"; exit 1; }
( cd build/rootfs && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz
