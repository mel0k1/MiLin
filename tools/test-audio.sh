#!/bin/sh
# test-audio.sh (M27): HDA e2e — intel-hda+hda-duplex в QEMU, aplay в госте,
# звук уходит в wav на хосте; RMS проверяет, что DMA-путь реально жил.
set -e
cd "$(dirname "$0")/.."
export PATH=/home/z/my-project/root/usr/bin:$PATH
export LD_LIBRARY_PATH=/home/z/my-project/root/usr/lib/x86_64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
[ -f build/initramfs.cpio.gz ] || tools/mkinitramfs.sh
BIN=qemu-system-x86_64
EXTRA=""
for R in root ../root; do
    if [ -x "$R/usr/bin/qemu-system-x86_64" ]; then
        export LD_LIBRARY_PATH="$PWD/$R/usr/lib/x86_64-linux-gnu:$LD_LIBRARY_PATH"
        BIN="$PWD/$R/usr/bin/qemu-system-x86_64"
        for d in "$R/usr/share/qemu-fw" "$R/usr/share/qemu"; do
            [ -d "$d" ] && { EXTRA="-L $PWD/$d"; break; }
        done
        break
    fi
done

rm -f build/hda-out.wav
"$BIN" -m 512M $EXTRA -nographic -no-reboot \
    -audiodev driver=wav,id=a0,path=build/hda-out.wav \
    -device intel-hda -device hda-duplex,audiodev=a0 \
    -device virtio-rng-pci \
    -kernel build/bzImage -initrd build/initramfs.cpio.gz \
    -append "console=ttyS0 panic=-1 memmap=4M\$0x8000000 audio" \
    < /dev/null > build/audio.log || true

FAIL=0
if tr -d '\000' < build/audio.log | grep -q "AUDIO E2E OK aplay"; then
    echo "   aplay в госте: OK"
else
    echo "   aplay FAIL — хвост лога:"
    tr -d '\000' < build/audio.log | tail -30
    FAIL=1
fi

python3 - <<'PYEOF' || FAIL=1
import wave, struct
try:
    w = wave.open("build/hda-out.wav")
    n = w.getnframes()
    data = w.readframes(n)
    w.close()
except Exception as e:
    print("   wav FAIL: нет файла hda-out.wav:", e)
    raise SystemExit(1)
samples = struct.unpack(f"<{len(data)//2}h", data)
if len(samples) < 1000:
    print(f"   wav FAIL: слишком короткий ({len(samples)} сэмплов)")
    raise SystemExit(1)
rms = (sum(s*s for s in samples) / len(samples)) ** 0.5
peak = max(abs(s) for s in samples)
if rms < 1000:
    print(f"   wav FAIL: тишина (rms={rms:.0f}, peak={peak})")
    raise SystemExit(1)
print(f"   wav на хосте: OK ({len(samples)} сэмплов, rms={rms:.0f}, peak={peak})")
PYEOF

[ "$FAIL" = 0 ] && echo "AUDIO GATES OK"
exit "$FAIL"
