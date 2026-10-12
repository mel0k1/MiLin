#!/bin/sh
# test-realhw.sh (M14): бут-гейты реального железа.
# 1) virtio-blk + ext4 (быстрый QEMU-путь, тот же контракт что и SATA/NVMe)
# 2) AHCI/SATA (q35 — как SATA-контроллер реальной машины)
# 3) NVMe (как NVMe-SSD реальной машины)
# 4) VGA-консоль (реальное железо без serial)
# каждый гейт: ядро + initramfs грузятся, mnttest монтирует ext4 и гоняет
# roundtrip; маркер M14HW OK в логе — гейт пройден.
set -e
cd "$(dirname "$0")/.."
export PATH=/home/z/my-project/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/z/my-project/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh
BIN=qemu-system-x86_64
EXTRA=""
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

# тестовый ext4-диск: пустой raw, форматим хостовым mkfs.ext4
DISK=build/realhw-disk.img
rm -f "$DISK"
truncate -s 64M "$DISK"
[ -x ../root/usr/sbin/mkfs.ext4 ] && MKFS=../root/usr/sbin/mkfs.ext4 || MKFS=mkfs.ext4
"$MKFS" -q -F "$DISK"

run_boot() {
    name="$1"; shift
    echo "== гейт: $name"
    "$BIN" -m 512M $EXTRA -nographic -no-reboot \
        -kernel build/bzImage -initrd build/initramfs.cpio.gz \
        -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000" \
        "$@" < /dev/null > "build/realhw-$name.log" || true
    if grep -q "M14HW OK" "build/realhw-$name.log"; then
        echo "   OK"
        return 0
    fi
    echo "   FAIL — хвост лога:"
    tr -d '\000' < "build/realhw-$name.log" | tail -12
    return 1
}

FAIL=0
run_boot virtio \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" || FAIL=1

run_boot ahci \
    -machine q35 -device virtio-rng-pci \
    -device ich9-ahci,id=ahci -device ide-hd,drive=d1,bus=ahci.0 -drive id=d1,if=none,format=raw,file="$DISK" || FAIL=1

run_boot nvme \
    -device virtio-rng-pci \
    -drive id=d2,if=none,format=raw,file="$DISK" \
    -device nvme,drive=d2,serial=MILIN01 || FAIL=1

# e1000e (82574L — как Intel-NIC реальной машины): DHCP + сеть через netd
run_boot nic-e1000e \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device e1000e,netdev=net0 -netdev user,id=net0 || FAIL=1
if grep -q "DHCP OK" build/realhw-nic-e1000e.log; then
    echo "   e1000e: DHCP OK"
else
    echo "   e1000e FAIL — DHCP не поднялся"
    FAIL=1
fi

# USB-клавиатуры (M15): xHCI (USB3-машины) и UHCI (старые Intel) —
# usbcore + HCD + usbhid должны перечислить usb-kbd как input-устройство
run_boot usb-xhci \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device qemu-xhci,id=xhci -device usb-kbd || FAIL=1
if grep -q "input: QEMU QEMU USB Keyboard" build/realhw-usb-xhci.log; then
    echo "   usb-xhci: клавиатура зарегистрирована"
else
    echo "   usb-xhci FAIL — usbhid не поднял клавиатуру"
    FAIL=1
fi

run_boot usb-uhci \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device piix3-usb-uhci,id=u -device usb-kbd || FAIL=1
if grep -q "input: QEMU QEMU USB Keyboard" build/realhw-usb-uhci.log; then
    echo "   usb-uhci: клавиатура зарегистрирована"
else
    echo "   usb-uhci FAIL — usbhid не поднял клавиатуру"
    FAIL=1
fi

# VGA-консоль: без -nographic, дисплей пустой — ядро должно поднять tty0
"$BIN" -m 512M $EXTRA -no-reboot -vga std -display none -serial stdio \
    -device virtio-rng-pci \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000" \
    < /dev/null > build/realhw-vga.log || true
if grep -aq "Console: colour" build/realhw-vga.log; then
    echo "== гейт: vga"
    echo "   OK"
else
    echo "== гейт: vga FAIL (консоль не поднялась)"
    FAIL=1
fi

[ "$FAIL" = 0 ] && echo "REALHW GATES OK"
exit "$FAIL"
