#!/bin/sh
# test-install.sh (M20): MiLini как устанавливаемая ОС — e2e в три бута.
# бут A (live, -kernel): cmdline install=/dev/sda — GPT-таблица (ESP 64M +
#   корень), BLKRRPART-рескан без ребута (ноды sda1/sda2 в devtmpfs),
#   mkfs.ext4 на sda2, копия мира, extlinux.conf (root=/dev/sda2),
#   extlinux --install, gptmbr.bin на LBA0, ESP = FAT16 с
#   /EFI/BOOT/BOOTX64.EFI (ядро с EFI stub) -> MINSTALL OK;
# бут B (BIOS): SeaBIOS -> gptmbr -> extlinux (sda2) -> ядро + мини-initramfs
#   -> switch_root -> login root/milin (sha512-crypt) -> root shell ->
#   login user/user (setuid 1000) -> user shell -> poweroff (S5);
# бут B2 (BIOS): passwd в госте — root меняет user без текущего пароля,
#   вход user с новым паролем, self-смена через setuid (текущий -> новый x2),
#   вход с паролем после self-смены -> poweroff (S5);
# бут C (UEFI): OVMF -> прошивка читает ESP -> BOOTX64.EFI (EFI stub без
#   LoadOptions) -> built-in cmdline (root=PARTUUID корня) -> прямой
#   mount ext4 без initramfs -> /sbin/init -> login -> poweroff (S5).
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
OVMF_CODE=""
OVMF_VARS=""
for R in root ../root; do
    if [ -f "$R/usr/share/OVMF/OVMF_CODE_4M.fd" ]; then
        OVMF_CODE="$PWD/$R/usr/share/OVMF/OVMF_CODE_4M.fd"
        OVMF_VARS="$PWD/$R/usr/share/OVMF/OVMF_VARS_4M.fd"
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
LA="$(tr -d '\000' < build/install-a.log)"
chk_a() {
    if echo "$LA" | grep -q "$1"; then
        echo "   бут A: $2"
    else
        echo "   бут A FAIL: нет '$1'"
        FAIL=1
    fi
}
chk_a "MINSTALL OK /dev/sda" "MINSTALL OK (GPT: ESP + корень)"
chk_a "BLKRRPART -> 0" "BLKRRPART-рескан принял новую таблицу"
chk_a "в devtmpfs (рескан без ребута)" "ноды sda1/sda2 в devtmpfs без ребута"
chk_a "BIOS-загрузчик gptmbr OK" "BIOS-путь: gptmbr.bin на LBA0"
chk_a "ESP OK — BOOTX64.EFI + VMLINUZ.EFI + CMDLINE" "UEFI-путь: ESP FAT16 (лоадер + ядро + cmdline)"
chk_a "RESIZE OK" "resize2fs вырастил ext4 до края p2"

# M22: уникальный PARTUUID установки — не канонический, форма 8-4-4-4-12
canonical="4d494c49-4d20-4d32-8000-000000000001"
PUA="$(echo "$LA" | sed -n 's/.*minstall: PARTUUID \([0-9a-f-]\{36\}\).*/\1/p' | head -1)"
if [ -z "$PUA" ]; then
    echo "   бут A FAIL: PARTUUID установки не напечатан"
    FAIL=1
elif [ "$PUA" = "$canonical" ]; then
    echo "   бут A FAIL: PARTUUID канонический — установки больше не уникальны"
    FAIL=1
else
    echo "   бут A: PARTUUID установки $PUA (не канонический)"
fi

echo "== бут B: загрузка с диска (SeaBIOS/gptmbr/extlinux), getty + login"
( sleep 25; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
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
LB="$(tr -d '\000' < build/install-b.log)"
chk_b() {
    if echo "$LB" | grep -q "$1"; then
        echo "   бут B: $2"
    else
        echo "   бут B FAIL: нет '$1'"
        FAIL=1
    fi
}
chk_b "switch_root OK" "загрузка с диска (switch_root) OK"
chk_b "PARTUUID $PUA -> /dev/sda2" "M22: резолвер PARTUUID нашёл корень установки"
chk_b "M19 LOGIN OK user=root" "login root (sha512-crypt) OK"
chk_b "M19 SHELL OK" "root shell OK"
chk_b "M19 LOGIN OK user=user" "login user (setuid 1000) OK"
chk_b "M19 USER OK" "user shell OK"
chk_b "M22 GPU E2E OK" "M22: GPU-фасад e2e в установленной системе"
chk_b "M22 BULK OK" "M22: bulk-путь IPC в установленной системе"
chk_b "reboot: Power down" "poweroff S5 OK"

echo "== бут B2: passwd в госте (root-смена + self-смена через setuid)"
( sleep 22; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf 'passwd user\n'; sleep 2; printf 'newpass\n'; sleep 2; printf 'newpass\n'; sleep 5;
  printf 'exit\n'; sleep 5;
  printf 'user\n'; sleep 2; printf 'newpass\n'; sleep 4;
  printf 'echo M20 PASSWD LOGIN1 OK\n'; sleep 2;
  printf 'passwd\n'; sleep 2; printf 'newpass\n'; sleep 2; printf 'pass1234\n'; sleep 2; printf 'pass1234\n'; sleep 6;
  printf 'echo M20 PASSWD SELF OK\n'; sleep 2; printf 'exit\n'; sleep 5;
  printf 'user\n'; sleep 2; printf 'pass1234\n'; sleep 4;
  printf 'echo M20 PASSWD LOGIN2 OK\n'; sleep 2;
  printf '/sbin/last\n'; sleep 5; printf 'exit\n'; sleep 5;
  printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf '/sbin/poweroff\n' ) \
| timeout 240 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -machine q35 -smp 4 \
    -device virtio-rng-pci \
    -device ich9-ahci,id=ahci \
    -drive id=d0,if=none,format=raw,file="$DISK" \
    -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
    > build/install-pw.log || true
LP="$(tr -d '\000' < build/install-pw.log)"
chk_p() {
    if echo "$LP" | grep -q "$1"; then
        echo "   бут B2: $2"
    else
        echo "   бут B2 FAIL: нет '$1'"
        FAIL=1
    fi
}
NP="$(echo "$LP" | grep -c 'M20 PASSWD OK user=user' || true)"
if [ "$NP" -ge 2 ]; then
    echo "   бут B2: passwd root+self ($NP смены)"
else
    echo "   бут B2 FAIL: passwd-маркеров $NP/2"
    FAIL=1
fi
chk_p "M20 PASSWD LOGIN1 OK" "вход user с паролем от root-смены OK"
chk_p "M20 PASSWD SELF OK" "self-смена с подтверждением текущего OK"
chk_p "M20 PASSWD LOGIN2 OK" "вход user с паролем после self-смены OK"
chk_p "M21 WTMP OK" "last: wtmp читается, USER_PROCESS есть"
chk_p "7 user console" "USER_PROCESS записи от login в wtmp"
chk_p "8 - console" "DEAD_PROCESS записи от getty в wtmp"
chk_p "reboot: Power down" "poweroff S5 OK"

if [ -n "$OVMF_CODE" ]; then
    echo "== бут C: UEFI-загрузка установленного диска (OVMF -> ESP -> EFI stub)"
    cp "$OVMF_VARS" build/install-ovmf-vars.fd
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M20 UEFI SHELL OK\n'; sleep 2; printf 'exit\n'; sleep 5;
      printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf '/sbin/poweroff\n' ) \
    | timeout 240 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
        -machine q35 -smp 4 \
        -device virtio-rng-pci \
        -device ich9-ahci,id=ahci \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_CODE" \
        -drive if=pflash,format=raw,file=build/install-ovmf-vars.fd \
        -drive id=d0,if=none,format=raw,file="$DISK" \
        -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
        > build/install-c.log || true
    LC="$(tr -d '\000' < build/install-c.log)"
    chk_c() {
        if echo "$LC" | grep -q "$1"; then
            echo "   бут C: $2"
        else
            echo "   бут C FAIL: нет '$1'"
            FAIL=1
        fi
    }
    chk_c "Mounted root (ext4 filesystem)" "прямой root-mount по PARTUUID OK"
    chk_c "M19 LOGIN OK user=root" "login root после UEFI-загрузки OK"
    chk_c "M20 UEFI SHELL OK" "UEFI root shell OK"
    chk_c "reboot: Power down" "poweroff S5 OK"
else
    echo "== бут C ПРОПУЩЕН — нет OVMF.fd в тулчейне"
fi

[ "$FAIL" = 0 ] && echo "INSTALL GATES OK"
exit "$FAIL"
