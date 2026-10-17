#!/bin/sh
# initramfs M5.2: супервизор (/init) + IPC-серверы (/sbin/mlbus, /sbin/logsrv)
set -e
cd "$(dirname "$0")/.."
R=build/rootfs
mkdir -p $R/sbin $R/dev $R/mnt $R/etc/milin $R/tmp $R/root $R/var/log $R/var/run
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
gcc -O2 -DMILIN_GLIBC -Iuserspace -o $R/sbin/gclient userspace/gclient.c
mkdir -p $R/lib
gcc -shared -fPIC -O2 -o $R/lib/netsh.so userspace/netsh.c -lpthread -ldl
gcc -O2 -o $R/sbin/xbpstest userspace/xbpstest.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/mnttest userspace/mnttest.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/persistcheck userspace/persistcheck.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/getty userspace/getty.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/login userspace/login.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/minstall userspace/minstall.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/poweroff userspace/poweroff.c
gcc -static -nostdlib -ffreestanding -O2 -DML_REBOOT=1 -o $R/sbin/reboot userspace/poweroff.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/passwd userspace/passwd.c
chmod 4755 $R/sbin/passwd
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/last userspace/last.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/who userspace/who.c
gcc -static -nostdlib -ffreestanding -O2 -o $R/sbin/gputest userspace/gputest.c

sh tools/get-void-xbps.sh
for d in build/void/ex/*/; do
    [ -f "$d/props.plist" ] || continue
    cp -a "$d/." $R/
    rm -f $R/props.plist $R/files.plist
done
# M25: прошивки iwlwifi/rtw88 -> usr/lib/firmware (lib позже симлинк на usr/lib)
sh tools/get-firmware.sh
mkdir -p $R/usr/lib/firmware
cp -a build/firmware/. $R/usr/lib/firmware/
# M25: wifi-оверлей — STA-конфиг, /sbin/wifi, udhcpc-хук
mkdir -p $R/etc/wpa_supplicant
printf 'ctrl_interface=/run/wpa_supplicant\nnetwork={\n  ssid="MiLin-AP"\n  psk="milinpass"\n}\n' > $R/etc/wpa_supplicant/wpa_supplicant.conf
cp userspace/wifi.sh $R/sbin/wifi
cp userspace/udhcpc.script $R/sbin/udhcpc.script
chmod 755 $R/sbin/wifi $R/sbin/udhcpc.script
# M25: busybox-апплеты для shell-скриптов гостя (wifi, udhcpc-хук)
for a in mkdir ifconfig ip ping cut sleep basename grep cat tr ln rm cp tail head wc ls; do
    ln -sf busybox $R/usr/bin/$a
done
mkdir -p $R/bin
cp /bin/sh $R/bin/sh
rm -rf $R/lib64 $R/lib
ln -sfn usr/lib $R/lib64
ln -sfn usr/lib $R/lib
build/void/ex/glibc-2.41_1/usr/bin/ldconfig -r "$PWD/$R" 2>/dev/null || true
ls -la $R/etc/ld.so.cache
printf 'root:x:0:0:root:/root:/bin/sh\nuser:x:1000:1000:user:/home/user:/bin/sh\n' > $R/etc/passwd
printf 'root:x:0:\nuser:x:1000:\n' > $R/etc/group
# nobody: dnsmasq роняет права на него по умолчанию
printf 'nobody:x:65534:65534:nobody:/var/empty:/sbin/nologin\n' >> $R/etc/passwd
printf 'nobody:x:65534:\n' >> $R/etc/group
mkdir -p $R/home/user
printf 'root:%s:19800:0:99999:7:::\nuser:%s:19800:0:99999:7:::\n' \
    "$(openssl passwd -6 -salt MiLinR00t milin)" \
    "$(openssl passwd -6 -salt MiLinUser user)" > $R/etc/shadow
chmod 600 $R/etc/shadow
ln -sfn /proc/mounts $R/etc/mtab
mkdir -p $R/etc/ssl/certs
cp -a build/void/ex/ca-certificates-*/usr/share/ca-certificates $R/usr/share/
cat $R/usr/share/ca-certificates/mozilla/*.crt > $R/etc/ssl/certs/ca-certificates.crt
cp $R/usr/share/ca-certificates/mozilla/*.crt $R/etc/ssl/certs/
ln -sfn certs/ca-certificates.crt $R/etc/ssl/cert.pem
openssl rehash "$PWD/$R/etc/ssl/certs" 2>/dev/null || true
ls $R/etc/ssl/certs | head -3
# DNS гостя = DNS хоста (slirp-пасsthrough; в изолированных средах 1.1.1.1 мёртв)
cp /etc/resolv.conf $R/etc/resolv.conf 2>/dev/null || printf 'nameserver 1.1.1.1\n' > $R/etc/resolv.conf
mkdir -p $R/etc/xbps.d
echo "repository=https://repo-default.voidlinux.org/current" > $R/etc/xbps.d/00-repository-main.conf
echo "repository=https://repo-default.voidlinux.org/current" > $R/etc/xbps.d/00-repo.conf
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
    cp build/bzImage build/bzImage-unsigned
    $SB --key keys/db.key --cert keys/db.crt \
        --output build/bzImage build/bzImage-unsigned
fi
cp build/bzImage $R/boot/vmlinuz
cp build/initramfs-install.cpio.gz $R/boot/initramfs.cpio.gz
EFCF="-Os -ffreestanding -fno-builtin -fno-stack-protector -fno-pic -fno-pie \
-mno-red-zone -mgeneral-regs-only -fno-asynchronous-unwind-tables -fno-ident"
gcc $EFCF -c userspace/efiboot.S -o build/efiboot-s.o
gcc $EFCF -Wall -c userspace/efiboot.c -o build/efiboot.o
ld -nostdlib -T userspace/efiboot.ld -o build/efiboot.elf \
   build/efiboot-s.o build/efiboot.o 2>/dev/null
objcopy -O binary build/efiboot.elf build/efiboot.pefi
if [ -n "$SB" ]; then
    cp build/efiboot.pefi build/efiboot-unsigned.pefi
    $SB --key keys/db.key --cert keys/db.crt \
        --output build/efiboot.pefi build/efiboot-unsigned.pefi
fi
cp build/efiboot.pefi $R/boot/efiboot.pefi
ls -lh $R/boot
( cd $R && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz build/initramfs-install.cpio.gz
