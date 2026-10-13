#!/bin/sh
# test-install.sh (M19): MiLini как устанавливаемая ОС — e2e в два бута.
# бут A (live, -kernel): cmdline install=/dev/vda — супервизор держит
#   minstall в очереди, запуск после завершения остальных oneshot'ов:
#   MBR-таблица, mkfs.ext4, копия мира, extlinux.conf, extlinux --install,
#   mbr.bin -> MINSTALL OK; panic=-1 + -no-reboot закрывают qemu.
# бут B (установленный, без -kernel): SeaBIOS -> mbr.bin -> extlinux ->
#   ядро + мини-initramfs (только /init) -> switch_root на /dev/vda1 ->
#   супервизор видит /etc/milin/.installed и живёт после oneshot'ов;
#   getty -> login root/milin (sha512-crypt из /etc/shadow) -> root shell;
#   exit -> login user/user (setuid 1000) -> user shell -> poweroff (S5)
#   -> "reboot: Power down" закрывает qemu.
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

DISK=build/install-disk.img
rm -f "$DISK"
truncate -s 512M "$DISK"

FAIL=0

echo "== бут A: установка на /dev/sda из live-сессии"
"$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -machine q35 -smp 4 \
    -device virtio-rng-pci \
    -device ich9-ahci,id=ahci \
    -drive id=d0,if=none,format=raw,file="$DISK" \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 install=/dev/sda" \
    < /dev/null > build/install-a.log || true
tr -d '\000' < build/install-a.log | grep -q "MINSTALL OK /dev/sda" \
    && echo "   бут A: MINSTALL OK (мир + extlinux на /dev/sda)" \
    || { echo "   бут A FAIL — хвост лога:"; tr -d '\000' < build/install-a.log | tail -25; FAIL=1; }

echo "== бут B: загрузка с диска (SeaBIOS/extlinux), getty + login"
( sleep 35; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf 'echo M19 SHELL OK\n'; sleep 2; printf 'exit\n'; sleep 5;
  printf 'user\n'; sleep 2; printf 'user\n'; sleep 4;
  printf 'echo M19 USER OK\n'; sleep 2; printf 'exit\n'; sleep 5;
  printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf '/sbin/poweroff\n' ) \
| timeout 240 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -machine q35 -smp 4 \
    -device virtio-rng-pci \
    -device ich9-ahci,id=ahci \
    -drive id=d0,if=none,format=raw,file="$DISK" \
    -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
    > build/install-b.log || true
LOG="$(tr -d '\000' < build/install-b.log)"

chk() {
    if echo "$LOG" | grep -q "$1"; then
        echo "   бут B: $2"
    else
        echo "   бут B FAIL: нет '$1'"
        FAIL=1
    fi
}
chk "switch_root OK" "загрузка с диска (switch_root) OK"
chk "M19 LOGIN OK user=root" "login root (sha512-crypt) OK"
chk "M19 SHELL OK" "root shell OK"
chk "M19 LOGIN OK user=user" "login user (setuid 1000) OK"
chk "M19 USER OK" "user shell OK"
chk "reboot: Power down" "poweroff S5 OK"

[ "$FAIL" = 0 ] && echo "INSTALL GATES OK"
exit "$FAIL"
