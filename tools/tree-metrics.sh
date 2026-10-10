#!/bin/sh
# метрики обрезанного дерева MiLin: размер исходников, файлы, строки,
# плюс размер собранного ядра. Печатает markdown-таблицу в stdout.
# использование: tree-metrics.sh <tree-dir> [path/to/bzImage]
set -e
T="${1:?usage: tree-metrics.sh <tree-dir> [bzImage]}"
B="$2"

SIZE="$(du -sh --exclude=.git "$T" | cut -f1)"
FILES="$(find "$T" -path "$T/.git" -prune -o -type f -print | wc -l)"
LOC="$(find "$T" -path "$T/.git" -prune -o -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 cat 2>/dev/null | wc -l)"

echo "| метрика | значение |"
echo "|---------|----------|"
echo "| исходники дерева | $SIZE |"
echo "| файлов | $FILES |"
echo "| строк в .c/.h | $LOC |"
if [ -n "$B" ] && [ -f "$B" ]; then
    BZ="$(wc -c < "$B")"
    echo "| bzImage | $BZ Б |"
fi
