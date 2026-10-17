#!/bin/sh
# M25: прошивки iwlwifi/rtw88 — из Void-пакета linux-firmware-network.
set -e
MI="$(cd "$(dirname "$0")/.." && pwd)"
V="$MI/build/void"
F="$MI/build/firmware"
Z=/home/z/my-project/root/usr/bin/zstd
command -v zstd >/dev/null && Z="$(command -v zstd)"
NETPKG=linux-firmware-network-20260810_1
X="$V/$NETPKG.x86_64.xbps"
# iwlwifi: 7260/7265 (DVM), 8000/9000 (MVM), AX200/201 (cc-a0), AX210 (so-a0)
IWL="iwlwifi-7260-17 iwlwifi-7265D-29 iwlwifi-8000C-36 \
iwlwifi-9000-pu-b0-jf-b0-46 iwlwifi-cc-a0-77 iwlwifi-so-a0-gf-a0-89"
# rtw88: 8723D/8821C/8822B/8822C PCIe
RTW="rtw8723d_fw.bin rtw8821c_fw.bin rtw8822b_fw.bin rtw8822c_fw.bin"
need=0
for f in $IWL; do [ -s "$F/$f.ucode" ] || need=1; done
for f in $RTW; do [ -s "$F/rtw88/$f" ] || need=1; done
[ $need = 0 ] && { echo "fw: кэш готов"; exit 0; }
mkdir -p "$V/fwnet" "$F/rtw88"
[ -s "$X" ] || curl -sfL --retry 5 --retry-all-errors -C - -o "$X" \
    "https://repo-default.voidlinux.org/current/$NETPKG.x86_64.xbps"
EXTRACT=""
for f in $IWL; do
    EXTRACT="$EXTRACT ./usr/lib/firmware/$f.ucode.zst"
    EXTRACT="$EXTRACT ./usr/lib/firmware/intel/iwlwifi/$f.ucode.zst"
done
for f in $RTW; do EXTRACT="$EXTRACT ./usr/lib/firmware/rtw88/$f.zst"; done
$Z -dc "$X" | tar -x -C "$V/fwnet" $EXTRACT
for f in $IWL; do $Z -dc < "$V/fwnet/usr/lib/firmware/$f.ucode.zst" > "$F/$f.ucode"; done
for f in $RTW; do $Z -dc "$V/fwnet/usr/lib/firmware/rtw88/$f.zst" > "$F/rtw88/$f"; done
echo "fw: $(ls $F/*.ucode $F/rtw88/*.bin | wc -l) файлов в $F"
