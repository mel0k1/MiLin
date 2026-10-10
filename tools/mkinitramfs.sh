#!/bin/sh
# initramfs M5: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
# + серверы устройств (/sbin/uiod, /sbin/fused).
# все binaries — статические, без libc (raw syscalls из userspace/milin.h)
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
( cd build/rootfs && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz
