#!/bin/sh
# собирает ядро MiLin -> build/bzImage
#
# источник дерева:
#   TREE_DIR=<путь>  — обрезанное дерево (форк mel0k1/linux, ветка milin);
#   иначе            — pristine tarball с kernel.org (baseline M1/M2,
#                      версия из tools/kernel-version).
# сборка всегда через O=, чтобы дерево исходников оставалось чистым.
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
cd "$MI"
HOST_ARG=""
[ -n "$HOSTPKG_CONFIG" ] && HOST_ARG="HOSTPKG_CONFIG=$HOSTPKG_CONFIG"

if [ -n "$TREE_DIR" ]; then
    SRCTREE="$(cd "$TREE_DIR" && pwd -P)"
else
    V="$(cat tools/kernel-version)"
    [ -d "linux-$V" ] || tools/fetch-linux.sh
    SRCTREE="$(cd "linux-$V" && pwd -P)"
fi
echo "build: tree=$SRCTREE"

OUT="$MI/build"
mkdir -p "$OUT"
cd "$OUT"
make -C "$SRCTREE" O="$OUT" tinyconfig
"$SRCTREE/scripts/kconfig/merge_config.sh" -m .config "$MI/configs/milin.config"
# KDRV=1: добавка configs/milin-kdrv.config — настоящие драйверы (amdgpu и др.)
if [ -n "$KDRV" ]; then
    echo "build: kdrv fragment (configs/milin-kdrv.config)"
    "$SRCTREE/scripts/kconfig/merge_config.sh" -m .config "$MI/configs/milin-kdrv.config"
fi
make -C "$SRCTREE" O="$OUT" olddefconfig
make -C "$SRCTREE" O="$OUT" -j"$(nproc)" $HOST_ARG
cp "$OUT/arch/x86/boot/bzImage" "$OUT/bzImage"
echo "ok: build/bzImage"
ls -lh "$OUT/bzImage"
