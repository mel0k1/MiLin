#!/bin/sh
# качает и распаковывает исходники Linux (версия из tools/kernel-version или $1)
set -e
cd "$(dirname "$0")/.."
V="${1:-$(cat tools/kernel-version)}"
F="linux-$V.tar.xz"
B="https://cdn.kernel.org/pub/linux/kernel/v${V%%.*}.x"
[ -d "linux-$V" ] && { echo "linux-$V: уже распакован"; exit 0; }
[ -f "$F" ] || { echo "fetch: $B/$F"; curl -fL --retry 3 -o "$F" "$B/$F"; }
echo "unpack: $F"
tar -xf "$F"
echo "ok: linux-$V"
