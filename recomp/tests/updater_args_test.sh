#!/bin/bash
# The updater install.sh generates must hand the new installer the paths this app was installed with, spaces included.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"; T="$(mktemp -d)"; trap 'rm -rf "$T"' EXIT
mkdir -p "$T/bin" "$T/My Repo/build/stable/.git" "$T/My Repo/build/nightly/.git" "$T/My Repo/build/stable/recomp" "$T/My Repo/build/nightly/recomp" "$T/My Repo/recomp"
printf '#!/bin/bash\nexit 0\n' > "$T/bin/git"; chmod +x "$T/bin/git"
for w in stable nightly; do printf '#!/bin/bash\nprintf "%%s\\n" "$@" > "%s/args"\n' "$T" > "$T/My Repo/build/$w/install.sh"; chmod +x "$T/My Repo/build/$w/install.sh"; done
TEMPLATE="$(sed -n '/<<UPDATE$/,/^UPDATE$/p' "$ROOT/install.sh" | sed '1d;$d')"
[ -n "$TEMPLATE" ] || { echo "updater template not found"; exit 1; }
check() {  # nightly? in-place?
  local AS_NIGHTLY="$1" IN_PLACE="$2" U_BRANCH=main U_DIR=stable APP_NAME=Test U_BUILT=abc123
  [ -n "$AS_NIGHTLY" ] && { U_BRANCH=nightly; U_DIR=nightly; }
  local U_REPO="$T/My Repo" GAME="/Volumes/G a/Game Folder" DATA="/Volumes/G a/My Saves" APPS="/Volumes/G a/My Apps"
  local Q_GAME Q_DATA Q_APPS; Q_GAME=$(printf '%q' "$GAME"); Q_DATA=$(printf '%q' "$DATA"); Q_APPS=$(printf '%q' "$APPS")
  eval "cat > \"\$T/update\" <<UPDATE
$TEMPLATE
UPDATE"
  rm -f "$T/args"; PATH="$T/bin:$PATH" bash "$T/update" >/dev/null
  local want="--game-dir"$'\n'"$GAME"$'\n'"--data-dir"$'\n'"$DATA"$'\n'"--app-dir"$'\n'"$APPS"
  if grep -q 'IN_PLACE' "$ROOT/install.sh" && [ -n "$IN_PLACE" ]; then want+=$'\n'"--in-place"; fi
  [ -n "$AS_NIGHTLY" ] && want+=$'\n'"--as-nightly"
  [ "$(cat "$T/args")" = "$want" ] || { echo "FAIL (nightly='$AS_NIGHTLY' in_place='$IN_PLACE'):"; cat "$T/args"; exit 1; }
}
check "" ""; check 1 ""; check "" 1; check 1 1
echo "updater: stable and nightly keep custom game, data and app folders"
