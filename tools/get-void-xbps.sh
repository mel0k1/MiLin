#!/bin/sh
# M13: мир Void в госте — динамический xbps + библиотеки + glibc.
# качает .xbps из repo-default.voidlinux.org (current, glibc), распаковывает
# в build/void/ex/<pkg>. Кэш: повторный запуск ничего не качает.
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
V="$MI/build/void"
EX="$V/ex"
mkdir -p "$V" "$EX"
Z=../../root/usr/bin/zstd
[ -x /home/z/my-project/root/usr/bin/zstd ] && Z=/home/z/my-project/root/usr/bin/zstd
command -v zstd >/dev/null && Z="$(command -v zstd)"

BASE=https://repo-default.voidlinux.org/current
PKGS="xbps-0.60.7_1 libxbps-0.60.7_1 libarchive-3.8.9_1 libcrypto3-3.6.5_1 \
libssl3-3.6.5_1 openssl-3.6.5_1 zlib-1.3.2_1 libzstd-1.5.7_1 liblzma-5.8.3_1 \
liblz4-1.10.0_1 bzip2-1.0.8_2 acl-2.4.0_1 attr-2.6.0_1 glibc-2.41_1"

for p in $PKGS; do
    f="$V/$p.x86_64.xbps"
    [ -f "$f" ] || { echo "get-void: $p"; curl -sfLo "$f" "$BASE/$p.x86_64.xbps"; }
    [ -d "$EX/$p" ] || {
        mkdir -p "$EX/$p"
        "$Z" -dc "$f" | tar -x -C "$EX/$p"
    }
done
echo "get-void: ex/ готово"
