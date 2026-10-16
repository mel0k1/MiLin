#!/bin/sh
# mkimage.sh (M14): загрузочный образ MiLini для реального железа.
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
cd "$MI"
OUT="${1:-build/milini-x86_64.iso}"
G=/home/z/my-project/root/opt/grub
export PATH="$G/usr/bin:$PATH"
export LD_LIBRARY_PATH="$G/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
[ -f build/bzImage ] || { echo "mkimage: нет build/bzImage — сначала build.sh"; exit 1; }
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh

cat > build/grub.cfg <<'EOF'
set default=0
set timeout=2
search --set=root --file /boot/bzImage
menuentry "MiLini" {
    linux ($root)/boot/bzImage console=tty0 console=ttyS0,115200 panic=-1 memmap=4M\$0x8000000
    initrd ($root)/boot/initramfs.cpio.gz
}
menuentry "MiLini (serial only)" {
    linux ($root)/boot/bzImage console=ttyS0,115200 panic=-1 memmap=4M\$0x8000000
    initrd ($root)/boot/initramfs.cpio.gz
}
EOF

PMOD="$G/usr/lib/grub/i386-pc"
grub-mkimage --directory="$PMOD" -O i386-pc -p '(cd)/boot/grub' \
    -o build/core.img \
    iso9660 biosdisk part_msdos part_gpt gzio all_video linux normal echo \
    ls test gfxterm serial font terminal configfile boot fat search \
    search_fs_file search_label search_fs_uuid
cat "$PMOD/cdboot.img" build/core.img > build/eltorito.img

MOD="$G/usr/lib/grub/x86_64-efi"
mformat -C -i build/efi-cd/mem.img -f 2880 ::
mmd -i build/efi-cd/mem.img ::/boot
mmd -i build/efi-cd/mem.img ::/boot/grub
mcopy -i build/efi-cd/mem.img build/grub.cfg ::/boot/grub/grub.cfg
grub-mkimage --directory="$MOD" -O x86_64-efi \
    -m build/efi-cd/mem.img -p '(memdisk)/boot/grub' \
    -o build/bootx64.efi \
    memdisk iso9660 gzio all_video linux normal echo ls test gfxterm serial \
    font terminal configfile boot fat search search_fs_file \
    search_label search_fs_uuid part_gpt part_msdos
mformat -C -i build/efi-cd/efi.img -T 65536 ::
mmd -i build/efi-cd/efi.img ::/EFI
mmd -i build/efi-cd/efi.img ::/EFI/boot
mmd -i build/efi-cd/efi.img ::/boot
mmd -i build/efi-cd/efi.img ::/boot/grub
mcopy -i build/efi-cd/efi.img build/bootx64.efi ::/EFI/boot/BOOTX64.EFI
mcopy -i build/efi-cd/efi.img build/grub.cfg ::/boot/grub/grub.cfg
cp build/efi-cd/efi.img build/efi.img

ROOT=build/iso-root
rm -rf "$ROOT"
mkdir -p "$ROOT/boot/grub/i386-pc"
cp build/bzImage build/initramfs.cpio.gz "$ROOT/boot/"
cp build/grub.cfg "$ROOT/boot/grub/"
cp "$G/usr/lib/grub/i386-pc"/*.mod "$G/usr/lib/grub/i386-pc"/*.lst \
   "$ROOT/boot/grub/i386-pc/" 2>/dev/null || true
cp "$G/usr/lib/grub/i386-pc/cdboot.img" "$ROOT/boot/"
cp build/efi.img "$ROOT/"
cp build/eltorito.img "$ROOT/boot/"

echo "mkimage: xorrisofs -> $OUT"
rm -f "$OUT"
xorrisofs -o "$OUT" \
    -isohybrid-mbr "$G/usr/lib/grub/i386-pc/boot_hybrid.img" \
    -b /boot/eltorito.img -no-emul-boot -boot-load-size 4 -boot-info-table \
    -eltorito-alt-boot -b efi.img -no-emul-boot \
    -isohybrid-gpt-basdat \
    -volid MILINI -joliet -rock "$ROOT"
ls -lh "$OUT"
echo "mkimage: готово. Реальная машина: dd if=$OUT of=/dev/sdX bs=4M status=progress (BIOS и UEFI)"
