#!/bin/sh
# собирает ядро: tinyconfig + наш минимальный набор -> build/bzImage
set -e
cd "$(dirname "$0")/.."
V="$(cat tools/kernel-version)"
cd "linux-$V"
# ядро = tinyconfig + наша конституция configs/milin.config
make tinyconfig
scripts/kconfig/merge_config.sh -m .config ../configs/milin.config
make olddefconfig
HOST_ARG=""
[ -n "$HOSTPKG_CONFIG" ] && HOST_ARG="HOSTPKG_CONFIG=$HOSTPKG_CONFIG"
make -j"$(nproc)" $HOST_ARG
mkdir -p ../build
cp arch/x86/boot/bzImage ../build/bzImage
echo "ok: ../build/bzImage"
ls -lh ../build/bzImage
