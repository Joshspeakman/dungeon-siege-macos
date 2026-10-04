#!/bin/bash
# build.sh <DungeonSiege.exe> [outdir]
# Recompile the user's own GOG 1.11.1 DungeonSiege.exe to arm64: analysis -> C -> libgame.dylib.
# Everything generated is derived from the executable and stays local (work/ is git-ignored).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
EXE="${1:?usage: build.sh <DungeonSiege.exe> [outdir]}"; OUT="${2:-$HERE/work/full}"
SHA=41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02
[ "$(shasum -a 256 "$EXE" | cut -d' ' -f1)" = "$SHA" ] || { echo "not the original GOG 1.11.1 DungeonSiege.exe (sha256 $SHA)"; exit 1; }
PY="$HERE/.venv/bin/python"
[ -x "$PY" ] || { python3 -m venv "$HERE/.venv" && "$HERE/.venv/bin/pip" install -q capstone==5.0.7 unicorn==2.1.4; }
mkdir -p "$HERE/work"
# three small executable patches (uncapped frame rate, cursor handling; tools/patch_exe.py)
cp "$EXE" "$HERE/work/DungeonSiege.exe" && python3 "$HERE/tools/patch_exe.py" "$HERE/work/DungeonSiege.exe" >/dev/null
EXE="$HERE/work/DungeonSiege.exe"
echo "== analysis"; "$PY" "$HERE/tools/analyze.py" "$EXE" "$HERE/work/analysis.json" | head -6
echo "== lifting";  "$PY" "$HERE/tools/lift.py" "$EXE" "$HERE/work/analysis.json" "$OUT"
# the Miles Sound System the game ships (Mss32.dll and the providers it loads), recompiled the same way; game folder
# paths, so the build needs the folder the executable came from
GAME="$(cd "$(dirname "$1")" && pwd)"; IMGS=("=")
if [ -f "$GAME/Mss32.dll" ]; then
  mkdir -p "$HERE/work/miles"; NIMP=$("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/image.json")
  for rel in Mss32.dll system/mss/Mssfast.m3d system/mss/Mp3dec.asi BinkW32.dll; do
    tag=$(basename "$rel" | tr . _); [ -f "$GAME/$rel" ] || { echo "missing $GAME/$rel"; exit 1; }
    "$PY" "$HERE/tools/analyze.py" "$GAME/$rel" "$HERE/work/miles/$tag.json" | head -1
    "$PY" "$HERE/tools/lift.py" "$GAME/$rel" "$HERE/work/miles/$tag.json" "$OUT" --tag "$tag" --imp-base "$NIMP" | grep -E 'lifted|internal'
    NIMP=$((NIMP + $("$PY" -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["imports"]))' "$OUT/${tag}_image.json")))
    IMGS+=("$tag=$rel")
  done
fi
"$PY" "$HERE/tools/link_tables.py" "$OUT" "${IMGS[@]}"
echo "== compiling"
rm -rf "$OUT/obj"; mkdir -p "$OUT/obj"
printf '%s\n' "$OUT"/lift_*.c "$OUT"/*_lift_*.c "$OUT/fntab.c" | xargs -n 1 -P "$(sysctl -n hw.ncpu)" "$HERE/tools/cc1.sh" "$OUT"
rm -f "$OUT/obj/rt.o"
"$HERE/tools/build_runtime.sh" "$OUT"            # runtime + Win32 layer, then the link
echo "== $OUT/libgame.dylib"
