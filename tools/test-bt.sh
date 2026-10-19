#!/bin/sh
# test-bt.sh (M27): BT e2e — /dev/vhci в госте, стек поднимает hci0,
# Read_Local_Version_Information проходит полный транспортный круг.
set -e
cd "$(dirname "$0")/.."
export PATH=/home/z/my-project/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/z/my-project/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh
BIN=qemu-system-x86_64
EXTRA=""
for R in root ../root; do
    if [ -x "$R/usr/bin/qemu-system-x86_64" ]; then
        export LD_LIBRARY_PATH="$PWD/$R/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
        BIN="$PWD/$R/usr/bin/qemu-system-x86_64"
        for d in "$R/usr/share/qemu-fw" "$R/usr/share/qemu"; do
            [ -d "$d" ] && { EXTRA="-L $PWD/$d"; break; }
        done
        break
    fi
done

"$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -device virtio-rng-pci \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 bt" \
    < /dev/null > build/bt.log || true

if tr -d '\000' < build/bt.log | grep -q "BT E2E OK"; then
    tr -d '\000' < build/bt.log | grep -E "btvhid|bttest|BT E2E OK"
    echo "BT GATES OK"
else
    echo "BT GATES FAILED — хвост лога:"
    tr -d '\000' < build/bt.log | tail -30
    exit 1
fi
