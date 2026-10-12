#!/bin/sh
# M13: локальный подписанный XBPS-репозиторий для бут-теста.
#   build/http-root/repo/  — репо (его раздаёт run-qemu.sh как 10.0.2.2:8000/repo)
#   build/rootfs/var/db/xbps/keys/<fp>.plist — предовренный ключ гостя
# сборка репо — тем же динамическим xbps, что едет в гостя.
# проверка e2e на хосте: установка в build/mkrepo/trustroot (файл:// не
# поддержан — репо поднимается на http://127.0.0.1:8100).
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
V="$MI/build/void"
EX="$V/ex"
REPO="$MI/build/http-root/repo"
PAY="$V/mkrepo/hello-1.0"
TRUST="$V/mkrepo/trustroot"
KEY="$V/repo-key.pem"

L="ex/libxbps-0.60.7_1/usr/lib:ex/libarchive-3.8.9_1/usr/lib:ex/openssl-3.6.5_1/usr/lib:ex/zlib-1.3.2_1/usr/lib:ex/libzstd-1.5.7_1/usr/lib:ex/liblzma-5.8.3_1/usr/lib:ex/liblz4-1.10.0_1/usr/lib:ex/bzip2-1.0.8_2/usr/lib:ex/acl-2.4.0_1/usr/lib:ex/attr-2.6.0_1/usr/lib:ex/glibc-2.41_1/usr/lib"
export LD_LIBRARY_PATH="$MI/build/void/$L"
XBPS="$EX/xbps-0.60.7_1/usr/bin"

# payload: статический hello — гостю не нужны свои libc-зависимости
mkdir -p "$PAY/usr/bin"
gcc -static -O2 -o "$PAY/usr/bin/hello" -x c - <<'EOF'
#include <stdio.h>
int main(void) { printf("hello from XBPS-installed package\n"); return 0; }
EOF

# ключ репо: один и тот же между сборками — отпечаток стабилен
[ -f "$KEY" ] || openssl genrsa -out "$KEY" 2048 2>/dev/null

rm -rf "$REPO" "$TRUST"
mkdir -p "$REPO" "$TRUST"
(cd "$V/mkrepo" && "$XBPS/xbps-create" -A x86_64 -n hello-1.0_1 -s "hello test package" "$PAY")
mv -f "$V/mkrepo/hello-1.0_1.x86_64.xbps" "$REPO/"
"$XBPS/xbps-rindex" -a "$REPO"/hello-1.0_1.x86_64.xbps
"$XBPS/xbps-rindex" --sign --signedby "MiLin CI" --privkey "$KEY" "$REPO"
"$XBPS/xbps-rindex" --sign-pkg --privkey "$KEY" "$REPO"/hello-1.0_1.x86_64.xbps

# e2e на хосте (валидация репо) + добыча plist доверенного ключа
mkdir -p "$V/mkrepo/conf"
echo "repository=http://127.0.0.1:8100" > "$V/mkrepo/conf/00-repo.conf"
pkill -f "http.server 8100" 2>/dev/null || true
sleep 0.3
python3 -m http.server 8100 --bind 127.0.0.1 -d "$REPO" >/dev/null 2>&1 &
SRV=$!
trap 'kill $SRV 2>/dev/null || true' EXIT
sleep 1
echo y | "$XBPS/xbps-install" -C "$V/mkrepo/conf" --rootdir "$TRUST" -Syy hello -y \
    >/dev/null 2>&1 || { echo "mkrepo: хост-e2e упал"; exit 1; }
"$TRUST/usr/bin/hello"
mkdir -p "$MI/build/rootfs/var/db/xbps/keys"
cp "$TRUST"/var/db/xbps/keys/*.plist "$MI/build/rootfs/var/db/xbps/keys/"
echo "mkrepo: репо готов, ключ предоврен гостю"
exit 0
