#!/bin/sh
# initramfs M5.2: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
# + серверы устройств (/sbin/uiod, /sbin/fused) + демо-клиент (/sbin/client)
# + сервисные манифесты (/etc/milin/*.svc — супервизор читает их при старте).
# базовые binaries — статические, без libc (raw syscalls из userspace/milin.h)
# M8: /sbin/gclient — динамический glibc-клиент (ld.so + libc.so.6 в rootfs)
# M13: мир Void — динамический xbps + библиотеки + glibc (tools/get-void-xbps.sh),
#      подписанный репо с ключом предоверия (tools/mkrepo.sh)
set -e
cd "$(dirname "$0")/.."
R=build/rootfs
mkdir -p $R/sbin $R/dev $R/mnt $R/etc/milin $R/tmp $R/root
# -ffreestanding: не только -fno-builtin, но и запрет превращать наши
# циклы в вызовы memset/strlen (M5.1: m_memset(нулями 4096) -> memset)
gcc -static -nostdlib -ffreestanding -O2 -o $R/init userspace/supervisor.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/mlbus userspace/mlbus.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/logsrv userspace/logsrv.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/uiod userspace/uiod.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/fused userspace/fused.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/client userspace/client.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/gpud userspace/gpud.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/netd userspace/netd.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/netclient userspace/netclient.c
cp userspace/manifests/*.svc $R/etc/milin/
cp userspace/manifests/pci.ids $R/etc/milin/
# M8: glibc-клиент (динамический) — helq.h общий с nostdlib-миром
gcc -O2 -DMILIN_GLIBC -Iuserspace -o $R/sbin/gclient userspace/gclient.c
# M12: socket-шима для динамических glibc-бинарей (LD_PRELOAD) —
# сокеты уезжают в netd через ML_PROTO_NET
mkdir -p $R/lib
gcc -shared -fPIC -O2 -o $R/lib/netsh.so userspace/netsh.c -lpthread -ldl
# M13: e2e пакетного менеджера
gcc -O2 -o $R/sbin/xbpstest userspace/xbpstest.c

# --- мир Void (tools/get-void-xbps.sh качает и распаковывает в build/void/ex) ---
sh tools/get-void-xbps.sh
for p in xbps-0.60.7_1 libxbps-0.60.7_1 libarchive-3.8.9_1 libcrypto3-3.6.5_1 \
         libssl3-3.6.5_1 openssl-3.6.5_1 zlib-1.3.2_1 libzstd-1.5.7_1 \
         liblzma-5.8.3_1 liblz4-1.10.0_1 bzip2-1.0.8_2 acl-2.4.0_1 \
         attr-2.6.0_1 glibc-2.41_1; do
    cp -a build/void/ex/$p/. $R/
done
# интерпретатор PT_INTERP=/lib64/ld-linux-x86-64.so.2; библиотеки живут в /usr/lib
rm -rf $R/lib64 $R/lib
ln -sfn usr/lib $R/lib64
ln -sfn usr/lib $R/lib
# ld.so.cache: в госте нет /lib/x86_64-linux-gnu (дефолт-пути host-ld.so),
# кэш — единственный надёжный резолвер NEEDED
build/void/ex/glibc-2.41_1/usr/bin/ldconfig -r "$PWD/$R" 2>/dev/null || true
ls -la $R/etc/ld.so.cache
# минимальный /etc: passwd/group (xbps спрашивает uid), xbps.d — наш репо
printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home:/bin/sh\n' > $R/etc/passwd
printf 'root:x:0:\nuser:x:1000:\n' > $R/etc/group
mkdir -p $R/etc/xbps.d
: > $R/etc/xbps.d/00-repository-main.conf     # тень Void-репо (в госте недостижим)
echo "repository=http://10.0.2.2:8000/repo" > $R/etc/xbps.d/00-repo.conf
# репо + предовренный ключ (tools/mkrepo.sh, соберёт и провалидирует репо)
sh tools/mkrepo.sh
( cd $R && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz
