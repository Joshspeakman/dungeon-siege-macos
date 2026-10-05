#!/bin/bash
# install.sh --game-dir <GOG "Dungeon Siege" folder> [--app-dir DIR] [--data-dir DIR]
# install.sh --gog-installer <setup_dungeon_siege_*.exe>      (GOG offline installer; needs: brew install innoextract)
# install.sh --yesterhaven <folder>    adds Gas Powered Games' Yesterhaven multiplayer map (Yesterhaven.dsmap and
#                                      Yesterhaven.dsres, found anywhere under <folder>); on its own or with the above
# install.sh --expansion <folder>      adds Legends of Aranna from your own copy (Expansion.dsres, Expansion.dsmap and
#                                      ExpVoices.dsres, plus XPRes.dsres/XPMap.dsmap if present, found anywhere under
#                                      <folder>, e.g. the installed game's DSLOA folder or the disc); only the data is
#                                      used, never DSLOA.exe. Choose "Legends of Aranna" in the launcher.
# install.sh --mods <folder>           adds every mod archive (.dsres/.dsmap that is not the game's or the expansion's
#                                      own) found anywhere under <folder> to the Mods list (tick them in the launcher;
#                                      known ones start ticked). Mods this project may redistribute (mods/) are always
#                                      added. Credits: docs/MODS.md.
#
# Builds the natively recompiled Dungeon Siege from your own copy of the GOG 1.11.1 game and installs
# "Dungeon Siege Native.app" (no Wine, no Rosetta). The game folder is only read: settings, saves and logs go to the
# data folder (default ~/Games/DungeonSiegeNative). Needs the Xcode command line tools and python3 (the recompiler
# installs capstone and unicorn with pip into recomp/.venv).
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
GAME=""; GOG_EXE=""; YH=""; LOA=""; MODS=""; APPS="$HOME/Applications"; DATA="$HOME/Games/DungeonSiegeNative"
while [ $# -gt 0 ]; do
  case "$1" in
    --game-dir) GAME="$2"; shift 2;; --gog-installer) GOG_EXE="$2"; shift 2;; --app-dir) APPS="$2"; shift 2;; --data-dir) DATA="$2"; shift 2;;
    --yesterhaven) YH="$2"; shift 2;; --expansion) LOA="$2"; shift 2;; --mods) MODS="$2"; shift 2;;
    *) echo "unknown option $1"; exit 1;;
  esac
done
# Yesterhaven: the map and its resources go to the Mods list (<data>/mods; ticked by default, for both games); the
# launcher links ticked mods into the data folder's view of the game folder, which the game sees as its own Maps and
# Resources folders. The game folder itself is not touched.
yesterhaven() {
  local m r; m="$(find "$YH" -iname Yesterhaven.dsmap -print -quit 2>/dev/null)"; r="$(find "$YH" -iname Yesterhaven.dsres -print -quit 2>/dev/null)"
  [ -n "$m" ] && [ -n "$r" ] || { echo "Yesterhaven.dsmap and Yesterhaven.dsres not found under $YH"; exit 1; }
  for f in "$m" "$r"; do   # Dungeon Siege archives ("DSigTank") made for Yesterhaven
    [ "$(head -c 8 "$f")" = DSigTank ] && head -c 4096 "$f" | LC_ALL=C tr -d '\000' | LC_ALL=C grep -a Yesterhaven >/dev/null || { echo "$f is not a Yesterhaven archive"; exit 1; }
  done
  mkdir -p "$DATA/mods"
  cp "$m" "$DATA/mods/Yesterhaven.dsmap"; cp "$r" "$DATA/mods/Yesterhaven.dsres"
  echo "== Yesterhaven added to the Mods list: host a multiplayer game and choose it under Map Settings"
}
# Mods: every Dungeon Siege archive under the folder that is not part of the game or the expansion goes to <data>/mods
# (the launcher's Mods list); the project's own redistributable mods (mods/ in this repository) too
mods() {
  local src="$1" f n count=0; mkdir -p "$DATA/mods"
  while IFS= read -r -d '' f; do
    n="$(basename "$f")"
    case "$(printf %s "$n" | tr '[:upper:]' '[:lower:]')" in
      logic.dsres|objects.dsres|terrain.dsres|sound.dsres|voices.dsres|devlogic.dsres|world.dsmap|mpworld.dsmap) continue;;
      expansion.dsres|expansion.dsmap|expvoices.dsres|xpres.dsres|xpmap.dsmap) continue;;
    esac
    [ "$(head -c 8 "$f")" = DSigTank ] || continue
    cp "$f" "$DATA/mods/$n"; echo "   mod: $n"; count=$((count + 1))
  done < <(find "$src" \( -iname '*.dsres' -o -iname '*.dsmap' \) -type f -print0 2>/dev/null)
  echo "== $count mod archive(s) from $src added to the Mods list (tick them in the launcher)"
}
bundled_mods() { [ -d "$HERE/mods" ] && find "$HERE/mods" -iname '*.dsres' -o -iname '*.dsmap' | grep -q . && mods "$HERE/mods" >/dev/null && echo "== the project's bundled mods added to the Mods list" || true; }
# Legends of Aranna: its archives go to <data>/expansion, a read-only layer the game sees over its own folder when the
# expansion is chosen in the launcher (other .dsres files next to them, such as mods, are left out)
expansion() {
  local f n src; mkdir -p "$DATA/expansion/Resources" "$DATA/expansion/Maps"
  for n in Expansion.dsres ExpVoices.dsres Expansion.dsmap XPRes.dsres XPMap.dsmap; do
    src="$(find "$LOA" -iname "$n" -print -quit 2>/dev/null)"
    if [ -z "$src" ]; then
      case "$n" in XP*) continue;; *) echo "$n not found under $LOA"; exit 1;; esac
    fi
    [ "$(head -c 8 "$src")" = DSigTank ] || { echo "$src is not a Dungeon Siege archive"; exit 1; }
    case "$n" in *.dsmap) f="$DATA/expansion/Maps/$n";; *) f="$DATA/expansion/Resources/$n";; esac
    cp "$src" "$f"
  done
  echo "== Legends of Aranna installed in $DATA/expansion: choose it in the launcher's Game row"
}
if [ -n "$YH$LOA$MODS" ]; then
  [ -z "$YH" ] || yesterhaven
  [ -z "$LOA" ] || expansion
  [ -z "$MODS" ] || mods "$MODS"
  bundled_mods
  [ -n "$GAME$GOG_EXE" ] || exit 0
fi
if [ -n "$GOG_EXE" ]; then       # unpack the installer into the data folder; the app reads the game from there
  [ -f "$GOG_EXE" ] || { echo "no such file: $GOG_EXE"; exit 1; }
  command -v innoextract >/dev/null || { echo "innoextract is needed for --gog-installer: brew install innoextract"; exit 1; }
  X="$DATA/gog-extract"; rm -rf "$X"; mkdir -p "$X"; echo "== unpacking $GOG_EXE"
  innoextract --gog -s -d "$X" "$GOG_EXE"
  GAME="$(dirname "$(find "$X" -name DungeonSiege.exe -print -quit)")"
  [ -f "$GAME/DungeonSiege.exe" ] || { echo "DungeonSiege.exe not found in the installer"; exit 1; }
fi
[ -n "$GAME" ] || { echo "usage: install.sh --game-dir <Dungeon Siege folder> | --gog-installer <setup_dungeon_siege_*.exe>"; exit 1; }
for tool in clang python3; do command -v $tool >/dev/null || { echo "$tool is missing: install the Xcode command line tools (xcode-select --install)"; exit 1; }; done
[ "$(uname -m)" = arm64 ] || { echo "the native build is for Apple Silicon Macs"; exit 1; }
EXE="$GAME/DungeonSiege.exe"; [ -f "$GAME/DungeonSiege.exe.orig" ] && EXE="$GAME/DungeonSiege.exe.orig"
for cand in "$GAME/DungeonSiege.exe.orig-fpscap" "$GAME/DungeonSiege.exe.orig" "$GAME/DungeonSiege.exe"; do
  [ -f "$cand" ] && [ "$(shasum -a 256 "$cand" | cut -d' ' -f1)" = 41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02 ] && { EXE="$cand"; break; }
done
echo "== recompiling $EXE (a few minutes)"
"$HERE/recomp/build.sh" "$EXE"
"$HERE/recomp/tools/build_app.sh" "$HERE/recomp/work/full" >/dev/null

A="$APPS/Dungeon Siege Native.app"; echo "== installing $A"
mkdir -p "$APPS"; rm -rf "$A"; mkdir -p "$A/Contents/MacOS" "$A/Contents/Resources"
cp "$HERE/recomp/work/full/app/DungeonSiegeNative" "$HERE/recomp/work/full/app/shaders.metal" "$HERE/recomp/work/full/app/dsr_snapshot.bin" "$A/Contents/MacOS/"
cp "$HERE/recomp/work/DungeonSiege.exe" "$A/Contents/Resources/DungeonSiege.exe"
cat > "$A/Contents/MacOS/launcher" <<LAUNCH
#!/bin/bash
# Dungeon Siege Native launcher (generated by install.sh)
GAME="\${DS_GAME_DIR:-$GAME}"; DATA="\${DS_NATIVE_DATA:-$DATA}"; DOCS="\$DATA/drive_c/Users/player/Documents"
mkdir -p "\$DOCS"
mkdir -p "\$DATA"
L="\$DATA/DungeonSiegeNative.log"                                   # keep the log small: previous one as .old
[ -f "\$L" ] && [ "\$(stat -f %z "\$L")" -gt 5000000 ] && mv -f "\$L" "\$DATA/DungeonSiegeNative.old.log"
B="\$(cd "\$(dirname "\$0")" && pwd)"
exec "\$B/DungeonSiegeNative" --exe "\$B/../Resources/DungeonSiege.exe" --game "\$GAME" --data "\$DATA" 2>>"\$DATA/DungeonSiegeNative.log"
LAUNCH
chmod +x "$A/Contents/MacOS/launcher"
cat > "$A/Contents/Info.plist" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
  <key>CFBundleName</key><string>Dungeon Siege Native</string>
  <key>CFBundleDisplayName</key><string>Dungeon Siege</string>
  <key>CFBundleIdentifier</key><string>io.github.dungeon-siege-macos.native</string>
  <key>CFBundleExecutable</key><string>launcher</string>
  <key>CFBundleIconFile</key><string>icon</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>1.11.1</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.role-playing-games</string>
  <key>LSMinimumSystemVersion</key><string>13.0</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSLocalNetworkUsageDescription</key><string>Dungeon Siege looks for and hosts multiplayer games on your local network.</string>
</dict></plist>
PLIST
I="$(mktemp -d)"
if python3 "$HERE/recomp/tools/extract_icon.py" "$EXE" "$I/ds.ico" 2>/dev/null && sips -s format png "$I/ds.ico" --out "$I/ds.png" >/dev/null 2>&1; then
  sips -z 512 512 "$I/ds.png" --out "$I/ds512.png" >/dev/null && sips -s format icns "$I/ds512.png" --out "$A/Contents/Resources/icon.icns" >/dev/null 2>&1 || true
fi
rm -rf "$I"
codesign --force --deep --sign - "$A" >/dev/null 2>&1 || true
/System/Library/Frameworks/CoreServices.framework/Frameworks/LaunchServices.framework/Support/lsregister -f "$A" 2>/dev/null || true
bundled_mods
echo "== done: $A (data in $DATA)"
