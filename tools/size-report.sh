#!/bin/sh
# метрики размера ядра: markdown-таблица + сравнение с последним baseline из docs/metrics/
set -e
cd "$(dirname "$0")/.."
V="$(cat tools/kernel-version)"
B="linux-$V"
BZ="build/bzImage"
VM="$B/vmlinux"
CFG="$B/.config"

[ -f "$BZ" ] || { echo "нет $BZ — сначала tools/build.sh"; exit 1; }
[ -f "$VM" ] || VM="build/vmlinux"

BZ_BYTES=$(wc -c < "$BZ")
VM_BYTES=$(wc -c < "$VM")
GZ_BYTES=$(gzip -9 -c "$BZ" | wc -c)
CFG_Y=$(grep -c '=y\|=m' "$CFG" 2>/dev/null || echo 0)
SIZES=$(size "$VM" 2>/dev/null | tail -1)
TEXT=$(echo "$SIZES" | awk '{print $1}')
DATA=$(echo "$SIZES" | awk '{print $2}')
BSS=$(echo "$SIZES" | awk '{print $3}')

echo "## MiLin — метрики размера (Linux $V)"
echo
echo "| метрика | значение |"
echo "|---------|----------|"
echo "| bzImage | $BZ_BYTES Б ($(awk "BEGIN{printf \"%.2f\", $BZ_BYTES/1048576}") МиБ) |"
echo "| bzImage gzip -9 | $GZ_BYTES Б |"
echo "| vmlinux | $VM_BYTES Б |"
echo "| text | $TEXT Б |"
echo "| data | $DATA Б |"
echo "| bss | $BSS Б |"
echo "| CONFIG =y/=m | $CFG_Y |"

BASE=$(ls docs/metrics/m*.json 2>/dev/null | sort -V | tail -1)
if [ -n "$BASE" ] && command -v python3 >/dev/null 2>&1; then
    python3 - "$BASE" "$BZ_BYTES" "$VM_BYTES" "$TEXT" "$CFG_Y" <<'EOF'
import json, sys
base = json.load(open(sys.argv[1]))
cur = {"bzImage_bytes": int(sys.argv[2]), "vmlinux_bytes": int(sys.argv[3]),
       "text_bytes": int(sys.argv[4]), "config_y": int(sys.argv[5])}
print()
print(f"Сравнение с baseline **{base['milestone']}** (docs/metrics/{sys.argv[1].split('/')[-1]}):")
print()
print("| метрика | baseline | сейчас | Δ |")
print("|---------|----------|--------|---|")
for k in ["bzImage_bytes", "vmlinux_bytes", "text_bytes", "config_y"]:
    b, c = base.get(k, 0), cur[k]
    if b:
        d = (c - b) / b * 100
        print(f"| {k} | {b} | {c} | {d:+.1f}% |")
    else:
        print(f"| {k} | — | {c} | — |")
EOF
fi
