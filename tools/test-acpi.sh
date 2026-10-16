#!/bin/sh
# M17-гейт: ACPI на СОВРЕМЕННОЙ машине. run-qemu.sh гейтит legacy
set -e
cd "$(dirname "$0")/.."
mkdir -p build
[ -d build/rootfs ] || tools/mkinitramfs.sh
BIN="qemu-system-x86_64"
EXTRA=""
for R in root ../root; do
    if [ -x "$R/usr/bin/qemu-system-x86_64" ]; then
        export LD_LIBRARY_PATH="$PWD/$R/usr/lib/x86_64-linux-gnu:$PWD/$R/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
        BIN="$PWD/$R/usr/bin/qemu-system-x86_64"
        export PATH="$PWD/$R/usr/bin:$PWD/$R/bin:$PATH"
        for d in "$R/usr/share/qemu-fw" "$R/usr/share/qemu"; do
            [ -d "$d" ] && { EXTRA="-L $PWD/$d"; break; }
        done
        break
    fi
done
mkdir -p build/http-root
echo "MiLin M12 http-root" > build/http-root/index.html
python3 -m http.server 8000 --bind 127.0.0.1 -d build/http-root \
    > build/http.log 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT

QARGS="-m 512M $EXTRA -nographic -vga none -machine q35 -smp 4 \
    -device pci-testdev -device virtio-rng-pci \
    -device e1000,netdev=net0 -netdev user,id=net0 \
    -kernel build/bzImage"
APP="console=ttyS0 panic=-1 memmap=4M\$0x8000000"

"$BIN" $QARGS -no-reboot \
    -initrd build/initramfs.cpio.gz -append "$APP" \
    < /dev/null > build/boot-acpi.log
FAIL=0
for m in "M11 OK" "M12 OK" "M14 OK" "DHCP OK" \
         "M22 BULK OK" "M22 GPU E2E OK" \
         "Interpreter enabled" \
         "Using ACPI (MADT) for SMP configuration information" \
         "Brought up 1 node, 4 CPUs" \
         "pnp: PnP ACPI: found" \
         "Using ACPI for IRQ routing"; do
    if ! grep -q "$m" build/boot-acpi.log; then
        echo "ACPI GATE FAILED: нет '$m'"
        FAIL=1
    fi
done
if [ "$FAIL" = 0 ]; then
    echo "ACPI GATES OK (q35 -smp 4: MADT 4 CPUs + IRQ routing + chain M16)"
fi

rm -rf build/rootfs-pm
cp -a build/rootfs build/rootfs-pm
gcc -static -nostdlib -ffreestanding -O2 -o build/rootfs-pm/sbin/poweroff userspace/poweroff.c
gcc -static -nostdlib -ffreestanding -O2 -DML_REBOOT=1 -o build/rootfs-pm/sbin/reboot userspace/poweroff.c

mk_pm_initrd() {
    rm -f build/rootfs-pm/etc/milin/poweroff.svc build/rootfs-pm/etc/milin/reboot.svc
    printf 'name=%s\nexec=/sbin/%s\ntype=oneshot\norder=99\n' "$1" "$1" \
        > build/rootfs-pm/etc/milin/$1.svc
    ( cd build/rootfs-pm && find . | cpio -o -H newc --quiet | gzip -9 ) \
        > build/initramfs-pm.cpio.gz
}

mk_pm_initrd poweroff
set +e
"$BIN" $QARGS -no-reboot \
    -initrd build/initramfs-pm.cpio.gz -append "$APP" \
    < /dev/null > build/boot-poweroff.log
RC=$?
set -e
if [ "$RC" = 0 ] && grep -q "reboot: Power down" build/boot-poweroff.log; then
    echo "POWEROFF OK (ACPI S5)"
else
    echo "POWEROFF FAILED (rc=$RC) — хвост boot.log:"
    tr -d '\000' < build/boot-poweroff.log | tail -40
    FAIL=1
fi

mk_pm_initrd reboot
set +e
"$BIN" $QARGS -no-reboot \
    -initrd build/initramfs-pm.cpio.gz -append "$APP" \
    < /dev/null > build/boot-reboot.log
RC=$?
set -e
if [ "$RC" = 0 ] && grep -q "reboot: Restarting system" build/boot-reboot.log; then
    echo "REBOOT OK"
else
    echo "REBOOT FAILED (rc=$RC) — хвост boot.log:"
    tr -d '\000' < build/boot-reboot.log | tail -40
    FAIL=1
fi

exit $FAIL
