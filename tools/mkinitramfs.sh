#!/bin/sh
# initramfs M5.2: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
# + серверы устройств (/sbin/uiod, /sbin/fused) + демо-клиент (/sbin/client)
# + сервисные манифесты (/etc/milin/*.svc — супервизор читает их при старте).
# базовые binaries — статические, без libc (raw syscalls из userspace/milin.h)
# M8: /sbin/gclient — динамический glibc-клиент (ld.so + libc.so.6 в rootfs)
set -e
cd "$(dirname "$0")/.."
mkdir -p build/rootfs/sbin build/rootfs/dev build/rootfs/mnt build/rootfs/etc/milin
# -ffreestanding: не только -fno-builtin, но и запрет превращать наши
# циклы в вызовы memset/strlen (M5.1: m_memset(нулями 4096) -> memset)
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/init userspace/supervisor.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/mlbus userspace/mlbus.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/logsrv userspace/logsrv.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/uiod userspace/uiod.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/fused userspace/fused.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/client userspace/client.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/gpud userspace/gpud.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/netd userspace/netd.c
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs/sbin/netclient userspace/netclient.c
cp userspace/manifests/*.svc build/rootfs/etc/milin/
cp userspace/manifests/pci.ids build/rootfs/etc/milin/
# M8: glibc-клиент (динамический) + интерпретатор и libc в rootfs
# M8.2: helq.h общий с nostdlib-миром — -DMILIN_GLIBC включает libc-шиму
gcc -O2 -DMILIN_GLIBC -Iuserspace -o build/rootfs/sbin/gclient userspace/gclient.c
# M12: socket-шима для динамического xbps (LD_PRELOAD) — сокеты уезжают
# в netd через ML_PROTO_NET; против той же libc, что едет в гостя
mkdir -p build/rootfs/lib
gcc -shared -fPIC -O2 -o build/rootfs/lib/netsh.so userspace/netsh.c -lpthread -ldl
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
