#!/bin/sh
# бут-тест: qemu грузит ядро; /init после приветствия выходит,
# паника ядра при panic=-1 и -no-reboot завершает qemu без kill'ов
set -e
cd "$(dirname "$0")/.."
mkdir -p build
BIN="qemu-system-x86_64"
EXTRA=""
if [ -x root/usr/bin/qemu-system-x86_64 ]; then
    export LD_LIBRARY_PATH="$PWD/root/usr/lib/x86_64-linux-gnu:$PWD/root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    BIN="$PWD/root/usr/bin/qemu-system-x86_64"
    for d in root/usr/share/qemu-fw root/usr/share/qemu; do
        [ -d "$d" ] && { EXTRA="-L $PWD/$d"; break; }
    done
fi
"$BIN" -m 256M $EXTRA -nographic -no-reboot -vga none \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1" < /dev/null > build/boot.log
if grep -q "Hello from MiLin" build/boot.log; then
    echo "BOOT OK"
    exit 0
fi
echo "BOOT FAILED — смотри build/boot.log"
exit 1
