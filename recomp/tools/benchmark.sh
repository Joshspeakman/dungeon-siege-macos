#!/bin/bash
# benchmark.sh [--runs N] [--game <folder>] [--bench <folder>] [--out <folder>]
# Runs Gas Powered Games' Dungeon Siege Benchmark (installed with ./install.sh --benchmark) with the build in work/full,
# in the background (test mode, sound muted), with no frame cap or vertical sync, in a scratch data folder, and prints
# each run's summary (average, median and 1% low fps, worst frame). Settings to compare are given as environment, e.g.
#   tools/benchmark.sh --runs 3
#   DS_RESOLUTION=1728x1117 DS_DRAW_DISTANCE=200 DS_SHADOW_RESOLUTION=1024 DSR_SHADOW_FILTER=softer tools/benchmark.sh
# The full results (with every frame's time) are kept in --out (default work/benchmarks).
set -euo pipefail
HERE="$(cd "$(dirname "$0")/.." && pwd)"
DATA="$HOME/Games/DungeonSiegeNative"; RUNS=1; GAME=""; BENCH="$DATA/benchmark"; OUT="$HERE/work/benchmarks"
while [ $# -gt 0 ]; do
  case "$1" in
    --runs) RUNS="$2"; shift 2;; --game) GAME="$2"; shift 2;; --bench) BENCH="$2"; shift 2;; --out) OUT="$2"; shift 2;;
    *) echo "unknown option $1"; exit 1;;
  esac
done
[ -n "$GAME" ] || GAME="$DATA/game"
[ -f "$GAME/DungeonSiege.exe" ] || { echo "no game in $GAME (--game <folder>)"; exit 1; }
[ -f "$BENCH/Resources/Benchmark.dsres" ] && [ -f "$BENCH/Maps/BenchmarkMap.dsmap" ] || { echo "the benchmark isn't in $BENCH: ./install.sh --benchmark DSBenchmark.EXE"; exit 1; }
APP="$HERE/work/full/app/DungeonSiegeNative"; [ -x "$APP" ] || { echo "no build in work/full (./build.sh)"; exit 1; }
mkdir -p "$OUT"
for k in $(seq 1 "$RUNS"); do
  S="$(mktemp -d "${TMPDIR:-/tmp}/dsbench.XXXXXX")"; ln -s "$BENCH" "$S/benchmark"
  DS_BENCHMARK=1 DS_MODS="" DS_ARGS="${DS_ARGS:+$DS_ARGS }demo=true map=benchmark_demo teleport=island fpslog=true minfps=0" \
    DSR_FPSCAP=0 DSR_VSYNC=0 DS_TEST=1 W32_AUDIO_MUTE=1 "$APP" --exe "$HERE/work/DungeonSiege.exe" --game "$GAME" --data "$S" > "$S/run.log" 2>&1 || true
  echo "run $k: $(grep -a -A1 'benchmark:' "$S/run.log" | head -2 | sed 's/^DungeonSiegeNative: benchmark: //' | tr '\n' ' ')"
  for f in "$S"/Benchmarks/*.txt; do [ -f "$f" ] && cp "$f" "$OUT/"; done
  rm -rf "$S"
done
echo "results: $OUT"
