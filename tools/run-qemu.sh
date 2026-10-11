#!/bin/sh
# бут-тест M5.2: qemu грузит ядро; супервизор читает манифесты /etc/milin,
# порождает сервисы и client; после "M9 OK" /init выходит,
# паника ядра при panic=-1 и -no-reboot завершает qemu без kill'ов.
# pci-testdev — железо для uiod (драйвер в userspace, шина в ядре).
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
"$BIN" -m 256M $EXTRA -nographic -no-reboot -vga none \
    -device pci-testdev \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1" < /dev/null > build/boot.log
if grep -q "M9 OK" build/boot.log; then
    echo "BOOT OK"
    exit 0
fi
echo "BOOT FAILED — смотри build/boot.log"
exit 1
