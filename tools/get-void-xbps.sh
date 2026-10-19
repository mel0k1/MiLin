#!/bin/sh
# M14: мир Void в госте — версии из ЖИВОГО репо, а не хардкод.
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
V="$MI/build/void"
EX="$V/ex"
mkdir -p "$V" "$EX"
Z=/home/z/my-project/root/usr/bin/zstd
command -v zstd >/dev/null && Z="$(command -v zstd)"

BASE=https://repo-default.voidlinux.org/current
ARCH=x86_64
PKGS="xbps libxbps libarchive libcrypto3 libssl3 openssl zlib libzstd \
liblzma liblz4 bzip2 acl attr glibc ca-certificates xbps-triggers \
e2fsprogs e2fsprogs-libs syslinux libblkid libuuid util-linux-common libsmartcols \
wpa_supplicant libnl3 dbus-libs libreadline8 ncurses-libs busybox iw dnsmasq \
libidn2 nettle gmp libunistring \
alsa-lib alsa-utils"

IDX="$V/repodata-$ARCH.cache"
need=1
[ -f "$IDX" ] && [ $(( $(date +%s) - $(stat -c %Y "$IDX") )) -lt 600 ] && need=0
if [ "$need" = 1 ]; then
    echo "get-void: индекс $BASE/$ARCH-repodata"
    curl -sfL "$BASE/$ARCH-repodata" | "$Z" -dc > "$IDX.cache"
    tar -xf "$IDX.cache" -O index.plist > "$IDX" 2>/dev/null
    rm -f "$IDX.cache"
fi

getver() {
    python3 - "$IDX" "$1" <<'PYEOF'
import plistlib, sys
d = plistlib.load(open(sys.argv[1], 'rb'))
idx = d.get('repository-index', d)
p = idx.get(sys.argv[2])
if not p:
    sys.exit(f"пакета {sys.argv[2]} нет в индексе")
print(p['pkgver'])
PYEOF
}

for name in $PKGS; do
    ver="$(getver "$name")"
    f="$V/$ver.$ARCH.xbps"
    [ -f "$f" ] || { echo "get-void: $ver"; curl -sfL --retry 5 --retry-all-errors -C - -o "$f" "$BASE/$ver.$ARCH.xbps"; }
    [ -d "$EX/$ver" ] || {
        mkdir -p "$EX/$ver"
        "$Z" -dc "$f" | tar -x -C "$EX/$ver"
    }
done
echo "get-void: ex/ готово"
