#!/bin/sh
# собирает ядро: tinyconfig + наш минимальный набор -> build/bzImage
set -e
cd "$(dirname "$0")/.."
V="$(cat tools/kernel-version)"
cd "linux-$V"
if [ -f ../configs/milin_defconfig ]; then
    cp ../configs/milin_defconfig .config
else
    make tinyconfig
    ./scripts/config -e 64BIT -e PRINTK -e TTY -e SERIAL_8250 -e SERIAL_8250_CONSOLE \
        -e BINFMT_ELF -e BLK_DEV_INITRD -e RD_GZIP -e PROC_FS -e SYSFS -e SHMEM -e TMPFS
    make olddefconfig
    cp .config ../configs/milin_defconfig
    echo "configs/milin_defconfig создан"
fi
HOST_ARG=""
[ -n "$HOSTPKG_CONFIG" ] && HOST_ARG="HOSTPKG_CONFIG=$HOSTPKG_CONFIG"
make -j"$(nproc)" $HOST_ARG
mkdir -p ../build
cp arch/x86/boot/bzImage ../build/bzImage
echo "ok: ../build/bzImage"
ls -lh ../build/bzImage
