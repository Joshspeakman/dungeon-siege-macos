#!/usr/bin/env bash
# macOS host lock and Metal upload regressions; no game files required.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
OUT=$(mktemp -d "${TMPDIR:-/tmp}/ds1-runtime.XXXXXX")
trap 'rm -rf "$OUT"' EXIT
for sanitizer in address,undefined thread; do
    clang -O1 -g "-fsanitize=$sanitizer" "$ROOT/recomp/tests/critical_sections_test.c" -o "$OUT/locks"
    "$OUT/locks"
done
clang -O1 -g -fobjc-arc -fsanitize=address,undefined \
    -framework Metal -framework Foundation -framework QuartzCore \
    "$ROOT/recomp/tests/renderer_ring_test.m" -o "$OUT/ring"
(cd "$ROOT/src/renderer" && "$OUT/ring")
