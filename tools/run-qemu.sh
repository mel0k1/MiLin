#!/bin/sh
# бут-тест: qemu грузит ядро (-smp 4, M16); супервизор читает манифесты
# /etc/milin, порождает сервисы и client; после "M11 OK" /init выходит,
# паника ядра при panic=-1 и -no-reboot завершает qemu без kill'ов.
# pci-testdev — железо для uiod (драйвер в userspace, шина в ядре).
# e1000 + slirp — железо для netd: HTTP-сервер хоста на 8000 отвечает
# клиенту userspace TCP/IP-стека (гейт M12).
# M16: -smp 4 на pc-i440fx-2.5 — MPS legacy-BIOS несёт все CPU; на
# современных типах машин SeaBIOS кладёт в MPS только boot-CPU (AP-
# перечисление переехало в ACPI MADT — это M17). Совместимая машина
# моделирует legacy-BIOS: честный гейт SMP-бутапа без ACPI.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
# initramfs должен существовать: в CI его некому собрать заранее
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh
BIN="qemu-system-x86_64"
EXTRA=""
# offline-тулчейн: рядом с репо (../root) или внутри (root/)
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
# HTTP-репо на хосте: 10.0.2.2:8000 в slirp достигает 127.0.0.1:8000
mkdir -p build/http-root
echo "MiLin M12 http-root" > build/http-root/index.html
python3 -m http.server 8000 --bind 127.0.0.1 -d build/http-root \
    > build/http.log 2>&1 &
HTTP_PID=$!
trap 'kill $HTTP_PID 2>/dev/null' EXIT
# M16: без -cpu max — на pc-i440fx-2.5 "max" роняет загрузчик SeaBIOS
# ("requires an x86-64 CPU"); qemu64 достаточен: энтропия — virtio-rng (M14)
"$BIN" -m 512M $EXTRA -nographic -no-reboot -vga none \
    -machine pc-i440fx-2.5 -smp 4 \
    -device pci-testdev \
    -device virtio-rng-pci \
    -device e1000,netdev=net0 -netdev user,id=net0 \
    -object filter-dump,id=f0,netdev=net0,file=build/net.pcap \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000" < /dev/null > build/boot.log
FAIL=0
if grep -q "M11 OK" build/boot.log && grep -q "M12 OK" build/boot.log \
   && grep -q "M14 OK" build/boot.log \
   && grep -q "M22 BULK OK" build/boot.log \
   && grep -q "M22 GPU E2E OK" build/boot.log \
   && grep -q "DHCP OK" build/boot.log; then
    echo "BOOT OK"
else
    echo "BOOT FAILED — хвост boot.log:"
    tr -d '\000' < build/boot.log | tail -80
    FAIL=1
fi
if grep -q "Brought up 1 node, 4 CPUs" build/boot.log; then
    echo "SMP OK (4 CPUs)"
else
    echo "SMP FAILED — вторичные CPU не поднялись"
    FAIL=1
fi
exit $FAIL
