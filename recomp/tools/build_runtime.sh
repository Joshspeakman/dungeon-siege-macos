#!/bin/bash
# build_runtime.sh <outdir> : compile the runtime + Win32 layer and link with the lifted objects into libgame.dylib
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"; OUT="$1"; mkdir -p "$OUT/obj/rt"
# Check every compiler status before linking; bound parallel work on macOS.
PIDS=(); JOBS="${NATIVE_BUILD_JOBS:-6}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo "invalid NATIVE_BUILD_JOBS" >&2; exit 1; }
wait_jobs() {
  local failed=0 pid
  for pid in "${PIDS[@]:-}"; do [ -z "$pid" ] || wait "$pid" || failed=1; done
  PIDS=(); [ "$failed" -eq 0 ]
}
queue_job() { PIDS+=("$!"); if [ "${#PIDS[@]}" -ge "$JOBS" ]; then wait_jobs; fi; }
trap 'for pid in "${PIDS[@]:-}"; do [ -z "$pid" ] || wait "$pid" || true; done' EXIT
python3 "$HERE/tools/gen_imptab.py" "$OUT/imptab.c" "$HERE"/runtime/win32/*.c
for f in "$HERE"/runtime/rt.c "$HERE"/runtime/win32/*.c "$OUT/imptab.c"; do
  clang -c -O1 -g -Wall -Wno-unused-function -I "$HERE/runtime" -I "$HERE/runtime/win32" -o "$OUT/obj/rt/$(basename "$f" .c).o" "$f" &
  queue_job
done; wait_jobs
clang -shared -o "$OUT/libgame.dylib" "$OUT"/obj/*.o "$OUT"/obj/rt/*.o -framework CoreText -framework CoreGraphics -framework CoreFoundation -framework AudioToolbox -lz
