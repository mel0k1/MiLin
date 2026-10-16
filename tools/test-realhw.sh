#!/bin/sh
# test-realhw.sh (M14/M16/M18): бут-гейты реального железа.
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
        -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 ${APPEND_EXTRA:-}" \
        "$@" < /dev/null > "build/realhw-$name.log" || true
    APPEND_EXTRA=""
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
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" || FAIL=1

run_boot ahci \
    -machine pc-q35-2.9 -smp 4 -device virtio-rng-pci \
    -device ich9-ahci,id=ahci -device ide-hd,drive=d1,bus=ahci.0 -drive id=d1,if=none,format=raw,file="$DISK" || FAIL=1

run_boot nvme \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -drive id=d2,if=none,format=raw,file="$DISK" \
    -device nvme,drive=d2,serial=MILIN01 || FAIL=1

run_boot nic-e1000e \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device e1000e,netdev=net0 -netdev user,id=net0 || FAIL=1
if grep -q "DHCP OK" build/realhw-nic-e1000e.log; then
    echo "   e1000e: DHCP OK"
else
    echo "   e1000e FAIL — DHCP не поднялся"
    FAIL=1
fi

run_boot usb-xhci \
    -machine pc-i440fx-2.5 -smp 4 \
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
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device piix3-usb-uhci,id=u -device usb-kbd || FAIL=1
if grep -q "input: QEMU QEMU USB Keyboard" build/realhw-usb-uhci.log; then
    echo "   usb-uhci: клавиатура зарегистрирована"
else
    echo "   usb-uhci FAIL — usbhid не поднял клавиатуру"
    FAIL=1
fi

APPEND_EXTRA="no=uiod" run_boot net-mltap \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-net-pci,netdev=net0 -netdev user,id=net0 || FAIL=1
if grep -q "DHCP OK" build/realhw-net-mltap.log; then
    echo "   net-mltap: DHCP через /dev/mltap (kernel-netdev) OK"
else
    echo "   net-mltap FAIL — DHCP не поднялся"
    FAIL=1
fi

run_boot wifi-stack \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci || FAIL=1
if grep -q "cfg80211" build/realhw-wifi-stack.log && grep -q "mltap: готов" build/realhw-wifi-stack.log; then
    echo "   wifi-stack: cfg80211+mac80211+ath9k в ядре, /dev/mltap готов"
else
    echo "   wifi-stack FAIL — wireless-стек не поднялся"
    FAIL=1
fi

"$BIN" -m 512M $EXTRA -no-reboot -vga std -display none -serial stdio \
    -machine pc-i440fx-2.5 -smp 4 \
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

OVMF_CODE=""
OVMF_VARS=""
for R in root ../root; do
    if [ -f "$R/usr/share/OVMF/OVMF_CODE_4M.fd" ] && [ -f "$R/usr/share/OVMF/OVMF_VARS_4M.fd" ]; then
        OVMF_CODE="$PWD/$R/usr/share/OVMF/OVMF_CODE_4M.fd"
        OVMF_VARS="$PWD/$R/usr/share/OVMF/OVMF_VARS_4M.fd"
        break
    fi
done
if [ -n "$OVMF_CODE" ]; then
    echo "== гейт: efifb (OVMF, UEFI Class 3)"
    cp "$OVMF_VARS" build/realhw-efifb-vars.fd
    "$BIN" -m 512M $EXTRA -no-reboot \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file=build/realhw-efifb-vars.fd \
        -vga std -display none -serial stdio \
        -machine pc-i440fx-2.5 -smp 4 \
        -device virtio-rng-pci \
        -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
        -device e1000e,netdev=net0 -netdev user,id=net0 \
        -kernel build/bzImage -initrd build/initramfs.cpio.gz \
        -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000" \
        < /dev/null > build/realhw-efifb.log || true
    EFIFB_FAIL=0
    grep -q "M14HW OK" build/realhw-efifb.log || { echo "   FAIL — сервисный след под UEFI"; EFIFB_FAIL=1; }
    grep -q "DHCP OK" build/realhw-efifb.log || { echo "   FAIL — DHCP под UEFI"; EFIFB_FAIL=1; }
    grep -aq "Console: switching to colour frame buffer device" build/realhw-efifb.log || { echo "   FAIL — fbcon не взял tty0"; EFIFB_FAIL=1; }
    grep -aq "frame buffer device" build/realhw-efifb.log || { echo "   FAIL — fb-устройство не зарегистрировано"; EFIFB_FAIL=1; }
    if [ "$EFIFB_FAIL" = 0 ]; then
        echo "   OK (fbcon + EFI framebuffer + полный след)"
    else
        tr -d '\000' < build/realhw-efifb.log | tail -12
        FAIL=1
    fi
else
    echo "== гейт: efifb ПРОПУЩЕН — нет OVMF.fd в тулчейне"
fi

run_boot psmouse \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" || FAIL=1
if grep -aqi "input:.*mouse" build/realhw-psmouse.log; then
    echo "   psmouse: мышь зарегистрирована"
else
    echo "   psmouse FAIL — AUX-мышь не поднялась"
    FAIL=1
fi

UDISK=build/realhw-usbdisk.img
rm -f "$UDISK"
truncate -s 32M "$UDISK"
run_boot usb-storage \
    -machine pc-i440fx-2.5 -smp 4 \
    -device virtio-rng-pci \
    -device virtio-blk,drive=d0 -drive id=d0,if=none,format=raw,file="$DISK" \
    -device qemu-xhci,id=xhci -device usb-storage,drive=u1 \
    -drive id=u1,if=none,format=raw,file="$UDISK" || FAIL=1
if grep -q "USB Mass Storage device detected" build/realhw-usb-storage.log \
   && grep -aq "Attached SCSI disk" build/realhw-usb-storage.log; then
    echo "   usb-storage: mass-storage -> sd привязан"
else
    echo "   usb-storage FAIL — хвост лога:"
    tr -d '\000' < build/realhw-usb-storage.log | tail -8
    FAIL=1
fi

[ "$FAIL" = 0 ] && echo "REALHW GATES OK"
exit "$FAIL"
