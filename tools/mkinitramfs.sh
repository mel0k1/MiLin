#!/bin/sh
# initramfs: статический /init без libc -> build/initramfs.cpio.gz
set -e
cd "$(dirname "$0")/.."
mkdir -p build/rootfs
gcc -static -nostdlib -O2 -o build/rootfs/init init/init.c
( cd build/rootfs && find . | cpio -o -H newc --quiet | gzip -9 ) > build/initramfs.cpio.gz
ls -lh build/initramfs.cpio.gz
