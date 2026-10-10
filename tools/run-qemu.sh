#!/bin/sh
# бут-тест: qemu грузит ядро, в логе должно появиться Hello from MiLin
set -e
cd "$(dirname "$0")/.."
mkdir -p build
BIN="qemu-system-x86_64"
EXTRA=""
if [ -x root/usr/bin/qemu-system-x86_64 ]; then
    export LD_LIBRARY_PATH="$PWD/root/usr/lib/x86_64-linux-gnu:$PWD/root/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    BIN="$PWD/root/usr/bin/qemu-system-x86_64"
    EXTRA="-L $PWD/root/usr/share/qemu"
fi
timeout 90 "$BIN" -m 256M $EXTRA -nographic -no-reboot \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1" > build/boot.log || true
if grep -q "Hello from MiLin" build/boot.log; then
    echo "BOOT OK"
    exit 0
fi
echo "BOOT FAILED — смотри build/boot.log"
exit 1
