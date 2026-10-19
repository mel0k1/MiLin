#!/bin/sh
# audio: aplay тестового wav через ALSA (M27); QEMU пишет гость->host wav
set -u
export PATH=/usr/sbin:/usr/bin:/sbin:/bin
[ -x /usr/bin/aplay ] || { echo "AUDIO FAIL no aplay"; exit 1; }
# QEMU hda-кодек стартует с замьюченным усилителем — снимаем mute
for c in Master Front Line PCM; do
    amixer -c 0 set "$c" 100% unmute >/dev/null 2>&1
done
aplay -D hw:0,0 -q /usr/share/snd/test.wav >/tmp/audio.log 2>&1 \
    && echo "AUDIO E2E OK aplay" \
    || { echo "AUDIO FAIL aplay"; cat /tmp/audio.log; exit 1; }
