#!/bin/sh
# бут-тест: qemu грузит ядро; супервизор читает манифесты /etc/milin,
# порождает сервисы и client; после "M11 OK" /init выходит,
# паника ядра при panic=-1 и -no-reboot завершает qemu без kill'ов.
# pci-testdev — железо для uiod (драйвер в userspace, шина в ядре).
# e1000 + slirp — железо для netd: HTTP-сервер хоста на 8000 отвечает
# клиенту userspace TCP/IP-стека (гейт M12).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
# initramfs должен существовать: в CI его некому собрать заранее
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh
BIN="qemu-system-x86_64"
EXTRA=""
# offline-тулчейн: рядом с репо (../root) или внутри (root/)
for R in root ../root; do
    if [ -x "$R/usr/bin/qemu-system-x86_64" ]; then
        export LD_LIBRARY_PATH="$PWD/$R/usr/lib/x86_64-linux-gnu:$PWD/$R/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        BIN="$PWD/$R/usr/bin/qemu-system-x86_64"
        for d in "$R/usr/share/qemu-fw" "$R/usr/share/qemu"; do
            [ -d "$d" ] && { EXTRA="-L $PWD/$d"; break; }
        done
        break
    fi
done
# HTTP-репо на хосте: 10.0.2.2:8000 в slirp достигает 127.0.0.1:8000
mkdir -p build/http-root
echo "MiLin M12 http-root" > build/http-root/index.html
python3 -m http.server 8000 --bind 127.0.0.1 -d build/http-root \
    > build/http.log 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
"$BIN" -m 256M $EXTRA -nographic -no-reboot -vga none \
    -device pci-testdev \
    -device e1000,netdev=net0 -netdev user,id=net0 \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000" < /dev/null > build/boot.log
if grep -q "M11 OK" build/boot.log && grep -q "M12 OK" build/boot.log; then
    echo "BOOT OK"
    exit 0
fi
echo "BOOT FAILED — хвост boot.log:"
tr -d '\000' < build/boot.log | tail -80
exit 1
