# Install and play

Dungeon Siege runs on Apple Silicon as ordinary arm64 code: no Wine, no Rosetta, no Windows
components. At install time it recompiles **your own** `DungeonSiege.exe` (and the game's Miles Sound System DLLs) from
x86 into native code and links it with a macOS implementation of everything the game uses from Windows (windows and
input, files, threads, DirectDraw/Direct3D 7 on Metal, DirectSound on Core Audio). How it works:
[recomp/README.md](../recomp/README.md).

## What you need

- A Mac with **Apple Silicon** and macOS 13 or newer.
- **Xcode Command Line Tools**: `xcode-select --install` (provides `clang` and `python3`).
- **Dungeon Siege from GOG, version 1.11.1**, either
  - an installed game folder (for example from GOG Galaxy on a PC, copied over, or an existing Wine/CrossOver install), or
  - the GOG **offline installer** (`setup_dungeon_siege_*.exe`); for this also install `innoextract`
    (`brew install innoextract`).

  The installer checks the executable's SHA-256 and refuses anything else. Steam and disc versions use a different
  executable and are not supported.
- About 2 GB of free space and a few minutes for the build (the recompiler is written in Python and installs its two
  helper packages, `capstone` and `unicorn`, into `recomp/.venv` on first use).

Optional: the community mods SeeFar and ResolutionFix work if they are in the game's `Resources` folder, but the app
has its own view-distance setting, so SeeFar is no longer needed (see below).

## Install

```sh
git clone https://github.com/Joshspeakman/dungeon-siege-macos.git
cd dungeon-siege-macos

# either from an installed game folder:
./install.sh --game-dir "/path/to/GOG Games/Dungeon Siege"

# or from the GOG offline installer:
./install.sh --gog-installer ~/Downloads/setup_dungeon_siege_*.exe
```

This builds and installs **`~/Applications/Dungeon Siege Native.app`**. Options: `--app-dir DIR` (where the app goes),
`--data-dir DIR` (where settings and saves go, default `~/Games/DungeonSiegeNative`).

What goes where:

| Location | Contents |
|---|---|
| Your game folder | Only read, never changed. The app reads the game's data from it every time it starts, so leave it in place (with `--gog-installer` it is unpacked into the data folder). |
| `~/Applications/Dungeon Siege Native.app` | The recompiled game, built on your Mac from your copy. |
| `~/Games/DungeonSiegeNative` | Saves and the game's settings (`drive_c/Users/player/Documents/Dungeon Siege`), the launch window's choices (`launcher.plist`), the log (`DungeonSiegeNative.log`) and crash reports (`CrashReports/`). |

To update: `git pull`, then run the same `install.sh` command again (saves are kept).

### The Steam edition

Steam's Dungeon Siege (app 39190) works too. Steam on the Mac doesn't install Windows games, but its console downloads
them: open `steam://open/console` (or Steam's console) and enter

```
download_depot 39190 39191
```

then run

```sh
./install.sh --steam              # finds Steam's download; or: ./install.sh --steam "/path/to/the/downloaded/folder"
```

The download is copied into the data folder (`steam-game`), so Steam may clean up its own copy afterwards. Steam's
`DungeonSiege.exe` is the same 1.11.1 build as GOG's (no DRM), minus GOG's few small fixes. Windows refuses to mix
Steam and GOG copies in multiplayer, but this build presents itself as GOG 1.11.1, so you can play with GOG players and
with PCs running the GOG executable. To play with unmodified Steam copies instead, start it with
`DS_STEAM_IDENTITY=steam`. Everything else (the launcher, mods, Legends of Aranna, `--nightly`) works the same way.

### Backing up and setting up again

Keep one backup folder with everything, and a new or reinstalled Mac is set up with one command (a Steam copy works in
the collection too):

```sh
./install.sh --save-collection "/Volumes/Backup/Dungeon Siege collection"    # write the backup
./install.sh --collection "/Volumes/Backup/Dungeon Siege collection"         # set up from it
```

The collection holds the GOG game folder, Legends of Aranna's archives, Yesterhaven and other mods, your saves and the
launcher's settings (a GOG offline installer can be put in it instead of the game folder). Setting up copies the game
into the data folder, restores saves and settings when the Mac has none yet, adds the expansion and mods, and builds
the app; the backup can be unplugged afterwards. Keep the collection private: it holds your own copies of the games.

### Nightly: the newest experimental work

```sh
./install.sh --nightly          # install or update "Dungeon Siege Nightly.app"
```

Work in progress lands on the repository's `nightly` branch first and reaches `main` (the stable app) once it has
proven itself. `--nightly` fetches the newest `nightly` from GitHub, builds it in `build/nightly` (your checkout stays
as it is) and installs **Dungeon Siege Nightly.app** next to the stable **Dungeon Siege Native.app**, so you can play
either. Both use the same data folder: saves, settings and mods are shared, and saves work in both; each keeps its own
log (`DungeonSiegeNative.log` / `DungeonSiegeNightly.log`). Run the same command again to update it. It uses the game
folder of the app you already have (or give `--game-dir` as usual). Expect rough edges: that is what it is for.

To uninstall: delete `~/Applications/Dungeon Siege Native.app` (and `Dungeon Siege Nightly.app`), and
`~/Games/DungeonSiegeNative` if you no longer want your saves.

## Playing

Open **Dungeon Siege Native** from `~/Applications` (or Spotlight). The launch window appears first:

| Setting | Choices |
|---|---|
| **Game** | Single Player, or Multiplayer: the game opens on its multiplayer screens (see [MULTIPLAYER.md](MULTIPLAYER.md)); the note shows this Mac's address for other players. |
| **Resolution** | Your display's size (recommended), its full Retina size, or a standard size that fits. The game's front-end menus are always 800×600 by design and are shown with bars at the sides; the game world uses the chosen resolution. |
| **View Distance** | Original, Far (125%), Farther (150%, about what the SeeFar mod gives), Very Far (200%), Horizon (250%), Maximum (300%). Higher settings draw much more of the world; Horizon and Maximum can lower the frame rate in the largest outdoor areas. |
| **Frame Rate** | Automatic (a steady 120 fps on ProMotion displays, 60 in the heaviest scenes), 120, 60, 30, or Unlimited (no cap, no vertical sync). |

Your choices are remembered: next time just press **Play** (or Return). Arrow keys move between and change settings;
Escape quits.

In the game: the mouse or trackpad controls the game's cursor; two-finger click is right click; two-finger scroll
zooms. All keyboard shortcuts work as on Windows. **Cmd+Tab** switches away, **Cmd+Q** quits.

With the SeeFar mod installed: at Original view distance the mod works as before; at any farther setting the game
uses the built-in view distance instead (the mod's file is set aside for that session, not deleted).

## If something goes wrong

- **Crash or freeze:** the app writes a report to `~/Games/DungeonSiegeNative/CrashReports` (also when the game stops
  responding, while it is still frozen) and offers it on the next launch. The report says what the game was doing;
  attach it when reporting a problem.
- **The build stops with "not the original GOG 1.11.1 DungeonSiege.exe":** the folder holds a different version of the
  game (Steam, disc, or a modified executable).
- **`clang` or `python3` missing:** install the Xcode Command Line Tools (`xcode-select --install`).
- **Sound problems:** start it from Terminal with the hand-written sound system instead of the game's own:
  `W32_MILES=native "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- **Skip the launch window** (use the saved settings directly):
  `DS_NO_LAUNCHER=1 "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- The log is `~/Games/DungeonSiegeNative/DungeonSiegeNative.log`.

## Legends of Aranna

Add the expansion from your own copy with `./install.sh --expansion "/path/to/Legends of Aranna"` and choose it in the
launch window: [LEGENDS-OF-ARANNA.md](LEGENDS-OF-ARANNA.md).

## Not supported

- The Steam and disc versions of Dungeon Siege as the base game (the recompiler needs the GOG 1.11.1 executable).
- `DSLOA.exe` itself: the expansion's data runs on the GOG engine instead.

## For developers

Building by hand, test mode, scripted input, crash-report tools and the verification suite are described in
[recomp/README.md](../recomp/README.md). In short:

```sh
recomp/build.sh "/path/to/Dungeon Siege/DungeonSiege.exe"      # recompile -> recomp/work/full
recomp/tools/build_app.sh recomp/work/full                      # link the app binary
DS_TEST=1 recomp/run.sh "/path/to/Dungeon Siege"                # background window, no mouse capture
```

Everything generated from the game stays in `recomp/work/` and is never committed.
