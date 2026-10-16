#!/bin/sh
# test-install.sh (M20/M22): MiLini как устанавливаемая ОС — e2e в пять бутов.
# бут A (live, -kernel): cmdline install=/dev/sda — GPT-таблица (ESP 64M +
#   корень), BLKRRPART-рескан без ребута (ноды sda1/sda2 в devtmpfs),
#   mkfs.ext4 на sda2, копия мира, extlinux.conf (root=/dev/sda2),
#   extlinux --install, gptmbr.bin на LBA0, ESP = FAT16 с
#   /EFI/BOOT/BOOTX64.EFI (наш EFI-лоадер) -> MINSTALL OK;
# бут B (BIOS): SeaBIOS -> gptmbr -> extlinux (sda2) -> ядро + мини-initramfs
#   -> switch_root -> login root/milin (sha512-crypt) -> root shell ->
#   login user/user (setuid 1000) -> user shell -> poweroff (S5);
# бут B2 (BIOS): passwd в госте — root меняет user без текущего пароля,
#   вход user с новым паролем, self-смена через setuid (текущий -> новый x2),
#   вход с паролем после self-смены -> poweroff (S5);
# бут C1 (UEFI): OVMF, свежий vars — fallback-бут \EFI\BOOT\BOOTX64.EFI,
#   лоадер создаёт boot-entry Boot0000 ("MiLin" -> \EFI\MILIN\BOOTX64.EFI)
#   и первым номером в BootOrder, проверяет чтением назад -> NVRAM-маркер;
# бут C2 (UEFI): тот же vars — прошивка грузит ЛОАДЕР ПО NVRAM-ENTRY
#   (\EFI\MILIN\BOOTX64.EFI — лоадер узнаёт это по собственному device path);
# бут D (UEFI secure boot): OVMF.secboot + PK/KEK/db в vars (virt-fw-vars),
#   лоадер и ядро подписаны db (sbsign) — SecureBoot=1, всё грузится;
# бут D2 (негативный): испорченный байт подписанного лоадера на ESP —
#   прошивка обязана ОТКАЗАТЬ загрузку (в логе нет баннера лоадера).
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
chk_a "ESP OK — BOOTX64.EFI + MILIN/BOOTX64.EFI + VMLINUZ.EFI + CMDLINE" "UEFI-путь: ESP FAT16 (лоадер x2 + ядро + cmdline)"
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
    echo "== бут C1: UEFI-загрузка (OVMF): fallback -> лоадер создаёт NVRAM entry"
    cp "$OVMF_VARS" build/install-ovmf-vars.fd
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 UEFI C1 OK\n'; sleep 2; printf 'exit\n'; sleep 5;
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
        > build/install-c1.log || true
    LC="$(tr -d '\000' < build/install-c1.log)"
    chk_c() {
        if echo "$LC" | grep -q "$1"; then
            echo "   бут C1: $2"
        else
            echo "   бут C1 FAIL: нет '$1'"
            FAIL=1
        fi
    }
    chk_c "MILEFI: boot manager" "OVMF -> наш EFI-лоадер"
    chk_c "M22 NVRAM ENTRY OK" "лоадер создал Boot0000 и прочитал назад"
    chk_c "MILEFI: boot entry in BootOrder" "лоадер поставил entry в BootOrder"
    chk_c "Mounted root (ext4 filesystem)" "прямой root-mount по PARTUUID OK"
    chk_c "M19 LOGIN OK user=root" "login root после UEFI-загрузки OK"
    chk_c "M22 UEFI C1 OK" "UEFI root shell OK"
    chk_c "reboot: Power down" "poweroff S5 OK"
    # NVRAM-перистентность на хосте: vars-файл обязан содержать entry
    if tr -d '\000' < build/install-ovmf-vars.fd | grep -q "MiLin" \
       && tr -d '\000' < build/install-ovmf-vars.fd | grep -q "Boot0000" \
       && tr -d '\000' < build/install-ovmf-vars.fd | grep -q "BootOrder"; then
        echo "   бут C1: vars-файл хранит Boot0000 (MiLin) + BootOrder"
    else
        echo "   бут C1 FAIL: Boot0000/BootOrder не дожили в vars-файле"
        FAIL=1
    fi

    echo "== бут C2: UEFI — загрузка прошивкой ПО NVRAM entry (\\EFI\\MILIN\\...)"
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 UEFI C2 OK\n'; sleep 2; printf 'exit\n'; sleep 5;
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
        > build/install-c2.log || true
    LC="$(tr -d '\000' < build/install-c2.log)"
    chk_c2() {
        if echo "$LC" | grep -q "$1"; then
            echo "   бут C2: $2"
        else
            echo "   бут C2 FAIL: нет '$1'"
            FAIL=1
        fi
    }
    chk_c2 "M22 NVRAM BOOT OK" "прошивка загрузила лоадер по NVRAM entry"
    chk_c2 "Mounted root (ext4 filesystem)" "root-mount по PARTUUID (через entry) OK"
    chk_c2 "M19 LOGIN OK user=root" "login root OK"
    chk_c2 "M22 UEFI C2 OK" "UEFI root shell OK"
    chk_c2 "reboot: Power down" "poweroff S5 OK"
else
    echo "== буты C1/C2 ПРОПУЩЕНЫ — нет OVMF.fd в тулчейне"
fi

# --- M22: secure boot (бут D) + негативный тест подписи (бут D2) ---
OVMF_SEC=""
for R in root ../root; do
    if [ -f "$R/usr/share/OVMF/OVMF_CODE_4M.secboot.fd" ]; then
        OVMF_SEC="$PWD/$R/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"
        break
    fi
done
VFW=""
for v in virt-fw-vars /home/z/my-project/root/usr/bin/virt-fw-vars; do
    command -v "$v" >/dev/null 2>&1 && { VFW="$v"; break; }
done
SBV=""
for v in sbverify /home/z/my-project/root/usr/bin/sbverify; do
    command -v "$v" >/dev/null 2>&1 && { SBV="$v"; break; }
done

if [ -n "$OVMF_SEC" ] && [ -n "$VFW" ] && [ -n "$SBV" ] \
   && [ -f build/efiboot.pefi ] && [ -f keys/db.crt ]; then
    echo "== бут D: UEFI secure boot — подписанные лоадер и ядро, PK/KEK/db в vars"
    if ! "$SBV" --list build/efiboot.pefi | grep -q "signature 1"; then
        echo "   бут D FAIL: лоадер не подписан"
        FAIL=1
    fi
    if ! "$SBV" --list build/bzImage | grep -q "signature 1"; then
        echo "   бут D FAIL: ядро не подписано"
        FAIL=1
    fi
    cp "$OVMF_VARS" build/install-sb-vars.fd
    PYTHONPATH="$PWD/root/usr/lib/python3/dist-packages" python3 "$VFW" \
        --inplace build/install-sb-vars.fd \
        --set-pk 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/PK.crt \
        --add-kek 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/KEK.crt \
        --add-db 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/db.crt \
        > /dev/null
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 SECUREBOOT GUEST OK\n'; sleep 2; printf 'exit\n'; sleep 5;
      printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf '/sbin/poweroff\n' ) \
    | timeout 240 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
        -machine q35 -smp 4 \
        -device virtio-rng-pci \
        -device ich9-ahci,id=ahci \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_SEC" \
        -drive if=pflash,format=raw,file=build/install-sb-vars.fd \
        -drive id=d0,if=none,format=raw,file="$DISK" \
        -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
        > build/install-d.log || true
    LD="$(tr -d '\000' < build/install-d.log)"
    chk_d() {
        if echo "$LD" | grep -q "$1"; then
            echo "   бут D: $2"
        else
            echo "   бут D FAIL: нет '$1'"
            FAIL=1
        fi
    }
    chk_d "MILEFI: secure boot on" "лоадер увидел SecureBoot=1"
    chk_d "M22 SECUREBOOT OK" "маркер secure boot"
    chk_d "Mounted root (ext4 filesystem)" "подписанное ядро загрузилось (root-mount)"
    chk_d "M19 LOGIN OK user=root" "login root OK"
    chk_d "reboot: Power down" "poweroff S5 OK"

    echo "== бут D2: негативный — испорченный лоадер, прошивка обязана отказать"
    cp "$DISK" build/install-disk-bad.img
    # XOR байта внутри .flat лоадера (первый кластер файла = сектор 2193,
    # смещение 0x1000) — подпись больше не совпадает
    python3 -c "
f = open('build/install-disk-bad.img', 'r+b')
f.seek(2193 * 512 + 0x1000)
b = f.read(1)
f.seek(2193 * 512 + 0x1000)
f.write(bytes([b[0] ^ 0xFF]))
f.close()
print('   бут D2: байт лоадера испорчен (0x%02X -> 0x%02X)' % (b[0], b[0] ^ 0xFF))
"
    cp "$OVMF_VARS" build/install-sb-vars-bad.fd
    PYTHONPATH="$PWD/root/usr/lib/python3/dist-packages" python3 "$VFW" \
        --inplace build/install-sb-vars-bad.fd \
        --set-pk 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/PK.crt \
        --add-kek 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/KEK.crt \
        --add-db 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/db.crt \
        > /dev/null
    timeout 120 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
        -machine q35 -smp 4 \
        -device virtio-rng-pci \
        -device ich9-ahci,id=ahci \
        -drive if=pflash,format=raw,readonly=on,file="$OVMF_SEC" \
        -drive if=pflash,format=raw,file=build/install-sb-vars-bad.fd \
        -drive id=d0,if=none,format=raw,file=build/install-disk-bad.img \
        -device ide-hd,drive=d0,bus=ahci.0,bootindex=0 \
        < /dev/null > build/install-d2.log || true
    LD2="$(tr -d '\000' < build/install-d2.log)"
    if echo "$LD2" | grep -q "MILEFI: boot manager"; then
        echo "   бут D2 FAIL: испорченный лоадер ЗАГРУЗИЛСЯ — secure boot не работает"
        FAIL=1
    else
        echo "   бут D2: прошивка отказала испорченному лоадеру (баннера нет)"
    fi
else
    echo "== буты D/D2 ПРОПУЩЕНЫ — нет secboot-OVMF/sbsign/ключей"
fi

[ "$FAIL" = 0 ] && echo "INSTALL GATES OK"
exit "$FAIL"
