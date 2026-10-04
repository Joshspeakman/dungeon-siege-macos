#!/bin/bash
# build_runtime.sh <outdir> : compile the runtime + Win32 layer and link with the lifted objects into libgame.dylib
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$1"; mkdir -p "$OUT/obj/rt"
python3 "$HERE/tools/gen_imptab.py" "$OUT/imptab.c" "$HERE"/runtime/win32/*.c
for f in "$HERE"/runtime/rt.c "$HERE"/runtime/win32/*.c "$OUT/imptab.c"; do
  clang -c -O1 -g -Wall -Wno-unused-function -I "$HERE/runtime" -I "$HERE/runtime/win32" -o "$OUT/obj/rt/$(basename "$f" .c).o" "$f" &
done; wait
clang -shared -o "$OUT/libgame.dylib" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
