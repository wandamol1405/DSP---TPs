#!/bin/bash
# Compila y ejecuta el test de host de la UI (requiere gcc). Uso: bash run.sh
set -e
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$HERE/../../MCXN947_Project_TP2/source"
OUT="${TMPDIR:-/tmp}/ui_host_build"
mkdir -p "$OUT"
cd "$OUT"
# U8g2 se compila una sola vez (solo si cambió)
if [ ! -f libu8g2.a ]; then
  for f in "$SRC"/u8g2/*.c; do
    gcc -O0 -w -c -I"$SRC/u8g2" "$f" -o "$(basename "$f" .c).o" &
  done
  wait
  ar rcs libu8g2.a u8*.o
fi
gcc -O0 -g -Wall -Wextra -Wno-unused-function -I"$HERE/stubs" -I"$SRC" -I"$SRC/u8g2" \
  "$HERE/ui_host_test.c" "$HERE/sim_display.c" libu8g2.a -o ui_host_test
./ui_host_test
cp ui_screens.txt "$HERE/ui_screens.txt"

