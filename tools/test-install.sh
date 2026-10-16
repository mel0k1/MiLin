#!/bin/sh
# test-install.sh (M20/M22): MiLini как устанавливаемая ОС — e2e в пять бутов.
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

FAIL=0

in_stage() {
    [ -z "$STAGE" ] && return 0
    case ",$STAGE," in *",$1,"*) return 0 ;; esac
    return 1
}

if [ -f build/install-state.sh ]; then . build/install-state.sh; fi

GU_DISK=4d494c49-4d20-4d32-8000-0000000000d1
GU_ESP=4d494c49-4d20-4d32-8000-0000000000e2
GU_ROOT=4d494c49-4d20-4d32-8000-0000000000a3

if in_stage a; then
    rm -f "$DISK" build/install-state.sh
    truncate -s 512M "$DISK"

echo "== бут A: установка на /dev/sda из live-сессии"
timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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

canonical="4d494c49-4d20-4d32-8000-000000000001"
PUA="$(echo "$LA" | sed -n 's/.*minstall: PARTUUID.*\([0-9a-f]\{8\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{12\}\).*/\1/p' | head -1)"
if [ -z "$PUA" ]; then
    echo "   бут A FAIL: PARTUUID установки не напечатан"
    FAIL=1
elif [ "$PUA" = "$canonical" ]; then
    echo "   бут A FAIL: PARTUUID канонический — установки больше не уникальны"
    FAIL=1
else
    echo "   бут A: PARTUUID установки $PUA (не канонический)"
fi
echo "PUA='$PUA'" > build/install-state.sh
fi  # in_stage a

if in_stage a2; then
    if [ ! -f "$DISK" ]; then
        echo "   бут A2 FAIL: нет $DISK — сначала STAGE=a"
        FAIL=1
    else
echo "== бут A2: переустановка с GUID'ами оператора (uuid_disk/uuid_esp/uuid_root)"
timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -machine q35 -smp 4 \
    -device virtio-rng-pci \
    -device ich9-ahci,id=ahci \
    -drive id=d0,if=none,format=raw,file="$DISK" \
    -device ide-hd,drive=d0,bus=ahci.0 \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 install=/dev/sda uuid_disk=$GU_DISK uuid_esp=$GU_ESP uuid_root=$GU_ROOT" \
    < /dev/null > build/install-a2.log || true
LA2="$(tr -d '\000' < build/install-a2.log)"
PUP="$(echo "$LA2" | sed -n 's/.*minstall: PARTUUID.*\([0-9a-f]\{8\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{4\}-[0-9a-f]\{12\}\).*/\1/p' | head -1)"
chk_a2() {
    if echo "$LA2" | grep -q "$1"; then
        echo "   бут A2: $2"
    else
        echo "   бут A2 FAIL: нет '$1'"
        FAIL=1
    fi
}
chk_a2 "minstall: заданы оператором" "uuid_*= приняты инсталлятором"
chk_a2 "uuid_root=$GU_ROOT" "эхо операторских GUID (round-trip parse->str)"
chk_a2 "minstall: PARTUUID $GU_ROOT" "заданный uuid_root записан в GPT"
chk_a2 "BLKRRPART -> 0" "рескан после переустановки"
if [ "$PUP" = "$GU_ROOT" ]; then
    echo "   бут A2: операторский PARTUUID $PUP вытеснил случайный"
else
    echo "   бут A2 FAIL: PARTUUID $PUP != $GU_ROOT"
    FAIL=1
fi
if [ -n "$PUA" ] && echo "$LA2" | grep -q "$PUA"; then
    echo "   бут A2 FAIL: старый случайный PARTUUID всплыл при переустановке"
    FAIL=1
fi
echo "PUP='$PUP'" >> build/install-state.sh
    fi  # DISK
fi  # in_stage a2

if in_stage b; then
    if [ ! -f "$DISK" ]; then
        echo "   бут B FAIL: нет $DISK — сначала STAGE=a"
        FAIL=1
    else
echo "== бут B: загрузка с диска (SeaBIOS/gptmbr/extlinux), getty + login"
( sleep 25; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf 'echo M19 SHELL OK\n'; sleep 2; printf 'exit\n'; sleep 5;
  printf 'user\n'; sleep 2; printf 'user\n'; sleep 4;
  printf 'echo M19 USER OK\n'; sleep 2; printf 'exit\n'; sleep 5;
  printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf '/sbin/poweroff\n' ) \
| timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
if [ -n "$PUP" ]; then
    chk_b "root=PARTUUID=$PUP" "M22: APPEND переписан на операторский PARTUUID"
    chk_b "PARTUUID $PUP -> /dev/sda2" "резолвер нашёл корень по операторскому PARTUUID"
else
    chk_b "PARTUUID $PUA -> /dev/sda2" "M22: резолвер PARTUUID нашёл корень установки"
fi
chk_b "M19 LOGIN OK user=root" "login root (sha512-crypt) OK"
chk_b "M19 SHELL OK" "root shell OK"
chk_b "M19 LOGIN OK user=user" "login user (setuid 1000) OK"
chk_b "M19 USER OK" "user shell OK"
chk_b "M22 GPU E2E OK" "M22: GPU-фасад e2e в установленной системе"
chk_b "M22 BULK OK" "M22: bulk-путь IPC в установленной системе"
chk_b "reboot: Power down" "poweroff S5 OK"
    fi  # DISK
fi  # in_stage b

if in_stage b2; then
    if [ ! -f "$DISK" ]; then
        echo "   бут B2 FAIL: нет $DISK — сначала STAGE=a"
        FAIL=1
    else
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
  printf '/sbin/who\n'; sleep 3;
  printf '/sbin/last\n'; sleep 5; printf 'exit\n'; sleep 5;
  printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
  printf '/sbin/poweroff\n' ) \
| timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
chk_p "M23 WHO OK" "who: сессии из /var/run/utmp"
chk_p "M21 WTMP OK" "last: wtmp читается, USER_PROCESS есть"
chk_p "7 user console" "USER_PROCESS записи от login в wtmp"
chk_p "8 - console" "DEAD_PROCESS записи от getty в wtmp"
chk_p "reboot: Power down" "poweroff S5 OK"
    fi  # DISK
fi  # in_stage b2

if [ -n "$OVMF_CODE" ]; then
if in_stage c1; then
    echo "== бут C1: UEFI-загрузка (OVMF): fallback -> лоадер создаёт NVRAM entry"
    cp "$OVMF_VARS" build/install-ovmf-vars.fd
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 UEFI C1 OK\n'; sleep 2; printf 'exit\n'; sleep 5;
      printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf '/sbin/poweroff\n' ) \
    | timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
    if tr -d '\000' < build/install-ovmf-vars.fd | grep -q "MiLin" \
       && tr -d '\000' < build/install-ovmf-vars.fd | grep -q "Boot0000" \
       && tr -d '\000' < build/install-ovmf-vars.fd | grep -q "BootOrder"; then
        echo "   бут C1: vars-файл хранит Boot0000 (MiLin) + BootOrder"
    else
        echo "   бут C1 FAIL: Boot0000/BootOrder не дожили в vars-файле"
        FAIL=1
    fi
fi  # in_stage c1

if in_stage c2; then
    echo "== бут C2: UEFI — загрузка прошивкой ПО NVRAM entry (\\EFI\\MILIN\\...)"
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 UEFI C2 OK\n'; sleep 2; printf 'exit\n'; sleep 5;
      printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf '/sbin/poweroff\n' ) \
    | timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
fi  # in_stage c2
else
    echo "== буты C1/C2 ПРОПУЩЕНЫ — нет OVMF.fd в тулчейне"
fi

OVMF_SEC=""
for R in root ../root; do
    if [ -f "$R/usr/share/OVMF/OVMF_CODE_4M.secboot.fd" ]; then
        OVMF_SEC="$PWD/$R/usr/share/OVMF/OVMF_CODE_4M.secboot.fd"
        break
    fi
done
VFW=""
for v in /home/z/my-project/root/usr/bin/virt-fw-vars virt-fw-vars; do
    command -v "$v" >/dev/null 2>&1 && { VFW="$v"; break; }
done
DISTPKG=""
for R in root ../root; do
    [ -d "$R/usr/lib/python3/dist-packages" ] && { DISTPKG="$PWD/$R/usr/lib/python3/dist-packages"; break; }
done
SBV=""
for v in sbverify /home/z/my-project/root/usr/bin/sbverify; do
    command -v "$v" >/dev/null 2>&1 && { SBV="$v"; break; }
done

if [ -n "$OVMF_SEC" ] && [ -n "$VFW" ] && [ -n "$SBV" ] \
   && [ -f build/efiboot.pefi ] && [ -f keys/db.crt ]; then
if in_stage d; then
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
    PYTHONPATH="$DISTPKG" python3 "$VFW" \
        --inplace build/install-sb-vars.fd \
        --set-pk 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/PK.crt \
        --add-kek 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/KEK.crt \
        --add-db 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/db.crt \
        > /dev/null
    ( sleep 45; printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf 'echo M22 SECUREBOOT GUEST OK\n'; sleep 2; printf 'exit\n'; sleep 5;
      printf 'root\n'; sleep 3; printf 'milin\n'; sleep 4;
      printf '/sbin/poweroff\n' ) \
    | timeout 420 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
fi  # in_stage d

if in_stage d2; then
    echo "== бут D2: негативный — испорченный лоадер, прошивка обязана отказать"
    cp "$DISK" build/install-disk-bad.img
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
    PYTHONPATH="$DISTPKG" python3 "$VFW" \
        --inplace build/install-sb-vars-bad.fd \
        --set-pk 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/PK.crt \
        --add-kek 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/KEK.crt \
        --add-db 8be4df61-93ca-11d2-aa0d-00e098032b8c keys/db.crt \
        > /dev/null
    timeout 180 "$BIN" -m 512M $EXTRA -nographic -no-reboot \
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
fi  # in_stage d2
else
    echo "== буты D/D2 ПРОПУЩЕНЫ — нет secboot-OVMF/sbsign/ключей"
fi

[ "$FAIL" = 0 ] && echo "INSTALL GATES OK"
exit "$FAIL"
