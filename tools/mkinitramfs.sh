#!/bin/sh
# initramfs M5.2: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
# + серверы устройств (/sbin/uiod, /sbin/fused) + демо-клиент (/sbin/client)
# + сервисные манифесты (/etc/milin/*.svc — супервизор читает их при старте).
# базовые binaries — статические, без libc (raw syscalls из userspace/milin.h)
# M8: /sbin/gclient — динамический glibc-клиент (ld.so + libc.so.6 в rootfs)
# M13: мир Void — динамический xbps + библиотеки + glibc (tools/get-void-xbps.sh),
#      подписанный репо с ключом предоверия (tools/mkrepo.sh)
set -e
cd "$(dirname "$0")/.."
R=build/rootfs
mkdir -p $R/sbin $R/dev $R/mnt $R/etc/milin $R/tmp $R/root $R/var/log $R/var/run
# -ffreestanding: не только -fno-builtin, но и запрет превращать наши
# циклы в вызовы memset/strlen (M5.1: m_memset(нулями 4096) -> memset)
gcc -static -nostdlib -ffreestanding -O2 -o $R/init userspace/supervisor.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/mlbus userspace/mlbus.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/logsrv userspace/logsrv.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/uiod userspace/uiod.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/fused userspace/fused.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/client userspace/client.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/gpud userspace/gpud.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/netd userspace/netd.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/netclient userspace/netclient.c
cp userspace/manifests/*.svc $R/etc/milin/
cp userspace/manifests/pci.ids $R/etc/milin/
# M8: glibc-клиент (динамический) — helq.h общий с nostdlib-миром
gcc -O2 -DMILIN_GLIBC -Iuserspace -o $R/sbin/gclient userspace/gclient.c
# M12: socket-шима для динамических glibc-бинарей (LD_PRELOAD) —
# сокеты уезжают в netd через ML_PROTO_NET
mkdir -p $R/lib
gcc -shared -fPIC -O2 -o $R/lib/netsh.so userspace/netsh.c -lpthread -ldl
# M13: e2e пакетного менеджера
gcc -O2 -o $R/sbin/xbpstest userspace/xbpstest.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/mnttest userspace/mnttest.c
# M15: проверка персистентности switch_root
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/persistcheck userspace/persistcheck.c
# M19: видимая система — getty/login, инсталлятор, poweroff/reboot
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/getty userspace/getty.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/login userspace/login.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/minstall userspace/minstall.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/poweroff userspace/poweroff.c
gcc -static -nostdlib -ffreestanding -O2 -DML_REBOOT=1 -o $R/sbin/reboot userspace/poweroff.c
# M20: passwd — setuid root (bprm_fill_uid жив в форке), пишет shadow под uid 1000
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/passwd userspace/passwd.c
chmod 4755 $R/sbin/passwd
# M21: last — чтение wtmp (utmp/wtmp пишут login/getty через userspace/utmp.h)
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/last userspace/last.c
# M22: gputest — e2e mlkd-фасада amdgpu из независимого клиента
# (kernel/user placement + отказ-пути; вердикт M11/M22 — по нему)
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/gputest userspace/gputest.c

# --- мир Void (tools/get-void-xbps.sh качает и распаковывает в build/void/ex) ---
sh tools/get-void-xbps.sh
for d in build/void/ex/*/; do
    [ -f "$d/props.plist" ] || continue
    cp -a "$d/." $R/
    rm -f $R/props.plist $R/files.plist
done
# seed-shell: INSTALL-скрипты пакетов (xbps-triggers) исполняются chroot'ом
# в rootfs и требуют /bin/sh ВНУТРИ root. Хостовый dash динамический, но его
# GLIBC_* символы (<= 2.36) перекрываются миром Void (glibc 2.41).
mkdir -p $R/bin
cp /bin/sh $R/bin/sh
# интерпретатор PT_INTERP=/lib64/ld-linux-x86-64.so.2; библиотеки живут в /usr/lib
rm -rf $R/lib64 $R/lib
ln -sfn usr/lib $R/lib64
ln -sfn usr/lib $R/lib
# ld.so.cache: в госте нет /lib/x86_64-linux-gnu (дефолт-пути host-ld.so),
# кэш — единственный надёжный резолвер NEEDED
build/void/ex/glibc-2.41_1/usr/bin/ldconfig -r "$PWD/$R" 2>/dev/null || true
ls -la $R/etc/ld.so.cache
# минимальный /etc: passwd/group (xbps спрашивает uid)
printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n' > $R/etc/passwd
printf 'root:x:0:\nuser:x:1000:\n' > $R/etc/group
# M19: пароли sha512-crypt (root/milin, user/user — см. realhw.md) + mtab
mkdir -p $R/home/user
printf 'root:%s:19800:0:99999:7:::\nuser:%s:19800:0:99999:7:::\n' \
    "$(openssl passwd -6 -salt MiLinR00t milin)" \
    "$(openssl passwd -6 -salt MiLinUser user)" > $R/etc/shadow
chmod 600 $R/etc/shadow
ln -sfn /proc/mounts $R/etc/mtab
# M14: TLS-доверие — бандл + hash-ссылки (openssl rehash из мира Void),
# дефолтные пути openssl: CAfile=/etc/ssl/cert.pem, CApath=/etc/ssl/certs
mkdir -p $R/etc/ssl/certs
cp -a build/void/ex/ca-certificates-*/usr/share/ca-certificates $R/usr/share/
cat $R/usr/share/ca-certificates/mozilla/*.crt > $R/etc/ssl/certs/ca-certificates.crt
cp $R/usr/share/ca-certificates/mozilla/*.crt $R/etc/ssl/certs/
ln -sfn certs/ca-certificates.crt $R/etc/ssl/cert.pem
# hash-ссылки: хостовой openssl (алгоритм subject-hash стандартный);
# Void-openssl на хосте не стартует (OPENSSL_3.6.0 vs host libcrypto)
openssl rehash "$PWD/$R/etc/ssl/certs" 2>/dev/null || true
ls $R/etc/ssl/certs | head -3
# M14: DNS (TCP:53 через netd) + НАСТОЯЩИЙ Void-репо (glibc-мир)
printf 'nameserver 1.1.1.1\n' > $R/etc/resolv.conf
mkdir -p $R/etc/xbps.d
echo "repository=https://repo-default.voidlinux.org/current" > $R/etc/xbps.d/00-repository-main.conf
echo "repository=https://repo-default.voidlinux.org/current" > $R/etc/xbps.d/00-repo.conf
# openssl: cert-пути + потолок TLS 1.2 (TLS 1.3 в связке с netd дал
# загадочный mismatch сертификата — 1.2 оставляет cert на wire видимым)
cat > $R/etc/ssl/openssl.cnf <<'EOF'
openssl_conf = openssl_init

[openssl_init]
ssl_conf = ssl_sect

[ssl_sect]
system_default = system_default_sect

[system_default_sect]
MaxProtocol = TLSv1.2
Groups = prime256v1
EOF
# M19: payload инсталлятора — ядро + мини-initramfs (только /init) в /boot;
# установленный корень получает мир с диска, полный initramfs на диск не пишется
# M22: подпись secure boot — ядро и лоадер подписываются db-ключом
# (tools/sb-keys.sh генерит тестовые PK/KEK/db). sbsign в PATH нет (CI) —
# уходят неподписанные образы, secure boot-гейт (бут D) тогда пропускается.
SB=""
if command -v sbsign >/dev/null 2>&1; then
    SB=sbsign
elif [ -x /home/z/my-project/root/usr/bin/sbsign ]; then
    PATH="/home/z/my-project/root/usr/bin:$PATH"
    export PATH
    LD_LIBRARY_PATH="/home/z/my-project/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    export LD_LIBRARY_PATH
    SB=sbsign
fi
RI=build/rootfs-install
rm -rf $RI
mkdir -p $RI/dev $RI/newroot $RI/proc $RI/sys $RI/tmp
gcc -static -nostdlib -ffreestanding -O2 -o $RI/init userspace/supervisor.c
( cd $RI && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs-install.cpio.gz
mkdir -p $R/boot
if [ -n "$SB" ]; then
    sh tools/sb-keys.sh
    # подпись bzImage: сертификат дописывается в конец, real-mode заголовок
    # не трогается (objcopy переписал бы весь PE — BIOS-путь ломается)
    cp build/bzImage build/bzImage-unsigned
    $SB --key keys/db.key --cert keys/db.crt \
        --output build/bzImage build/bzImage-unsigned
fi
cp build/bzImage $R/boot/vmlinuz
cp build/initramfs-install.cpio.gz $R/boot/initramfs.cpio.gz
# M22: EFI boot manager — лоадер BOOTX64.EFI (PE32+ без relocs, фиксированная
# геометрия efiboot.ld). Прошивка грузит его, он читает с ESP CMDLINE и
# стартует VMLINUZ.EFI с LoadOptions — per-install root=PARTUUID
EFCF="-Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
-mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -fno-ident"
gcc $EFCF -c userspace/efiboot.S -o build/efiboot-s.o
gcc $EFCF -Wall -c userspace/efiboot.c -o build/efiboot.o
ld -nostdlib -T userspace/efiboot.ld -o build/efiboot.elf \
   build/efiboot-s.o build/efiboot.o 2>/dev/null
objcopy -O binary build/efiboot.elf build/efiboot.pefi
# M22: подпись лоадера (SBAT-секция уже в образе) — Certificate Table
# заполнит sbsign, геометрия не меняется
if [ -n "$SB" ]; then
    cp build/efiboot.pefi build/efiboot-unsigned.pefi
    $SB --key keys/db.key --cert keys/db.crt \
        --output build/efiboot.pefi build/efiboot-unsigned.pefi
fi
cp build/efiboot.pefi $R/boot/efiboot.pefi
ls -lh $R/boot
( cd $R && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz build/initramfs-install.cpio.gz
