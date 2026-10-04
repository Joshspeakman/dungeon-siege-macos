#!/bin/bash
# run.sh <game folder> : play the recompiled game straight from the build (after build.sh + tools/build_app.sh).
# The game folder is only read; everything the game writes (settings, saves, logs) goes to the data folder,
# DS_NATIVE_DATA or ~/Games/DungeonSiegeNative. Add DS_TEST=1 for a background window without mouse capture.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME="${1:?usage: run.sh <Dungeon Siege game folder>}"
DATA="${DS_NATIVE_DATA:-$HOME/Games/DungeonSiegeNative}"
mkdir -p "$DATA/drive_c/Users/player/Documents"
exec "$HERE/work/full/app/DungeonSiegeNative" --exe "$HERE/work/DungeonSiege.exe" --game "$GAME" --data "$DATA"
