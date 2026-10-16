#!/bin/sh
# test-persist.sh (M15): switch_root на постоянный ext4 — двухбутовый e2e.
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

DISK=build/persist-disk.img
rm -f "$DISK"
truncate -s 256M "$DISK"
[ -x ../root/usr/sbin/mkfs.ext4 ] && MKFS=../root/usr/sbin/mkfs.ext4 || MKFS=mkfs.ext4
"$MKFS" -q -F "$DISK"

run_boot() {
    name="$1"; shift
    echo "== бут: $name"
    "$BIN" -m 512M $EXTRA -nographic -no-reboot \
        -device virtio-rng-pci \
        -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
        -kernel build/bzImage -initrd build/initramfs.cpio.gz \
        -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 root=/dev/vda" \
        "$@" < /dev/null > "build/persist-$name.log" || true
}

FAIL=0

run_boot seed
tr -d '\000' < build/persist-seed.log | grep -E "SEED OK|switch_root OK|M14 OK|persist marker" \
    && echo "   бут A: SEED + switch_root + установка на ext4 — OK" \
    || { echo "   бут A FAIL — хвост лога:"; tr -d '\000' < build/persist-seed.log | tail -25; FAIL=1; }

run_boot persist
tr -d '\000' < build/persist-persist.log | grep -E "switch_root OK|PERSIST OK" > /dev/null \
    && tr -d '\000' < build/persist-persist.log | grep -vE "SEED OK" | grep -q "switch_root OK" \
    && echo "   бут B: PERSIST OK (маркер пришёл с диска) — OK" \
    || { echo "   бут B FAIL — хвост лога:"; tr -d '\000' < build/persist-persist.log | tail -25; FAIL=1; }

[ "$FAIL" = 0 ] && echo "PERSIST GATES OK"
exit "$FAIL"
