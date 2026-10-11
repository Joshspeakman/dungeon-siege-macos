# Install and play

Dungeon Siege runs on Apple Silicon as ordinary arm64 code: no Wine, no Rosetta, no Windows
components. At install time it recompiles **your own** `DungeonSiege.exe` (and the game's Miles Sound System DLLs) from
x86 into native code and links it with a macOS implementation of everything the game uses from Windows (windows and
input, files, threads, DirectDraw/Direct3D 7 on Metal, DirectSound on Core Audio). How it works:
[recomp/README.md](../recomp/README.md).

## What you need

- A Mac with **Apple Silicon** (M1 or later) and macOS 13 or newer. The installer stops on an Intel Mac.
- **Xcode Command Line Tools**: `xcode-select --install` (provides `clang`, `git` and `python3`).
- **Dungeon Siege, version 1.11.1, from GOG or Steam**:
  - a GOG game folder (for example from GOG Galaxy on a PC, copied over, or an existing Wine/CrossOver install);
  - the GOG **offline installer** (`setup_dungeon_siege_*.exe`), for which you also need `innoextract`
    (`brew install innoextract`); or
  - the **Steam** edition, downloaded through Steam on the Mac (see *The Steam edition* below). Its executable
    has no DRM.

  The installer checks the executable's SHA-256 and refuses anything else:

  | Edition | `DungeonSiege.exe` SHA-256 |
  |---|---|
  | GOG 1.11.1 | `41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02` |
  | Steam 1.11.1 (app 39190, depot 39191) | `c408ef77b39484d8ad82ba17859cf1e60b24d3baf6d429283a52b886d67f33ab` |

  Disc versions, other patch levels and modified executables are not supported.
- About 2 GB of free space and a few minutes for the build (the recompiler is written in Python and installs its two
  helper packages, `capstone` and `unicorn`, into `recomp/.venv` with pip on first use, so the first install needs an
  internet connection).

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

This builds and installs **`~/Applications/Dungeon Siege Native.app`**. Keep the cloned folder: the app's Updates row
rebuilds from it. Give `--game-dir` an absolute path (for example `"$HOME/Games/Dungeon Siege"`, not a relative one): the app remembers it and reads the
game from it every time it starts.

All options:

| Option | What it does |
|---|---|
| `--game-dir DIR` | An installed GOG or Steam game folder (the one holding `DungeonSiege.exe`). |
| `--gog-installer FILE` | The GOG offline installer, unpacked into `<data>/gog-extract` with `innoextract`. |
| `--steam [DIR]` | The Steam edition (see *The Steam edition* below). |
| `--expansion DIR` | Legends of Aranna from your own copy ([LEGENDS-OF-ARANNA.md](LEGENDS-OF-ARANNA.md)). |
| `--mods DIR` | Every mod archive under `DIR`, added to the launcher's Mods list ([MODS.md](MODS.md)). |
| `--yesterhaven DIR-or-FILE` | Yesterhaven from a folder with its two files, or from its installer. |
| `--benchmark FILE` | Gas Powered Games' benchmark, from `DSBenchmark.EXE` ([BENCHMARK.md](BENCHMARK.md)). |
| `--collection DIR`, `--save-collection DIR` | Set up from, or write, a backup folder (below). |
| `--nightly` | Dungeon Siege Nightly next to the stable app (below). |
| `--app-dir DIR` | Where the app goes (default `~/Applications`). |
| `--data-dir DIR` | Where settings, saves and logs go (default `~/Games/DungeonSiegeNative`). |
| `--no-downloads` | Skip the install-time downloads (also `DS_NO_DOWNLOADS=1`). |

`--expansion`, `--mods`, `--yesterhaven` and `--benchmark` can be run on their own once the game is installed. With no
game option, `install.sh` rebuilds from the game folder the installed app already uses.

Unless `--no-downloads` is given, an install also downloads, for your Mac only: Gas Powered Games' Yesterhaven
installer from the [Internet Archive](https://archive.org/details/DungeonSiegeYesterhaven) (53 MB, checked by its hash;
only its two archives are kept, in the Mods folder), when the Mods folder doesn't have it yet; and the launch window's
banner art from Steam's store.

What goes where:

| Location | Contents |
|---|---|
| Your game folder | Only read, never changed. The app reads the game's data from it every time it starts, so leave it in place (with `--gog-installer` it is unpacked into the data folder). |
| `~/Applications/Dungeon Siege Native.app` | The recompiled game, built on your Mac from your copy. |
| `~/Games/DungeonSiegeNative` | Saves and the game's settings (`drive_c/Users/player/Documents/Dungeon Siege`), the launch window's choices (`launcher.plist`), the log (`DungeonSiegeNative.log`) and crash reports (`CrashReports/`). |

To update, use the launch window's **Updates** row. When the app opens, it asks GitHub whether a newer version of the
app's branch exists: `main` for Dungeon Siege, `nightly` for Dungeon Siege Nightly. When it says **Update available**,
press it:

- The new version is downloaded into `build/stable` (or `build/nightly`) in this folder; your own checkout isn't
  touched.
- The app is rebuilt from your copy of the game, which takes a few minutes, with a progress window.
- The app then restarts.

Your game folder, saves, settings and mods are kept. The output goes to `update.log` in the data folder. This needs
the git clone the app was installed from, still in the same place; otherwise the row says *Not available*. *Couldn't
check* means GitHub wasn't reachable (press it to retry).

The updater runs the new version's `install.sh` without the options you installed with. If you used `--app-dir` or
`--data-dir`, update by hand instead: with a custom `--app-dir` the update can't find the installed app and stops, and
with a custom `--data-dir` the rebuilt app would use the default data folder. Updating by hand always works: `git pull`,
then the same `install.sh` command again.

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

Open **Dungeon Siege Native** from `~/Applications` (or Spotlight). The launch window appears first. The **Game** row is
at the top; the other settings are grouped under four buttons, each showing its current settings: **Display**,
**Graphics**, **Mods** (the list of mods, see [MODS.md](MODS.md)) and **Updates**. Display and Graphics open as a panel
(Back, Escape or a click outside closes it).

Every option of the game's own Options > Video page is here, so it needn't be used: the launcher writes them into the
game's settings (`prefs.gas`; Legends of Aranna keeps its own) when you press Play, and a change made in the game shows
up in the launcher next time.

| Setting | Choices |
|---|---|
| **Game** | Single Player, or Multiplayer: the game opens on its multiplayer screens (see [MULTIPLAYER.md](MULTIPLAYER.md)); the note shows this Mac's address for other players. **Benchmark**, when installed: Gas Powered Games' benchmark demo, run uncapped (see [BENCHMARK.md](BENCHMARK.md)). |
| **Display: Resolution** | Your display's size (recommended), its full Retina size, or a standard size that fits. The game's front-end menus are always 800×600 by design and are shown with bars at the sides; the game world uses the chosen resolution. |
| **Display: Interface Size** | 100% (as the game draws it) up to 300%: the game runs at the chosen resolution divided by this, so its interface (menus, panels, text) is that much larger, while the world is drawn at the full resolution. Only sizes that leave the game at least 800×600 are offered; with the full Retina resolution, 200% gives the original interface size at double sharpness. |
| **Display: Notch** | On a Mac with a notch: **Hide the Notch** (the picture below the camera, a black band beside it) or **Around the Notch** (the picture on the whole display, the notch over a little of its top centre). The resolution's "This display" follows it. |
| **Display: View Distance** | Original, Far (125%), Farther (150%, about what the SeeFar mod gives), Very Far (200%), Horizon (250%), Maximum (300%). Higher settings draw much more of the world; Horizon and Maximum can lower the frame rate in the largest outdoor areas. |
| **Display: Gamma** | The game's gamma correction, 0.5 (darker) to 1.5 (brighter); 1.0 as shipped. |
| **Display: Frame Rate** | Automatic (a steady 120 fps on ProMotion displays, 60 in the heaviest scenes), 120, 60, 30, or Unlimited (no cap, no vertical sync). |
| **Graphics: Texture Filtering** | Bilinear (as shipped) or Trilinear: smoother distant textures. |
| **Graphics: Object Detail** | How much of the small scenery (plants, rocks, clutter) is drawn: Lowest, Low, Medium, High or Full (as shipped). |
| **Graphics: Shadows** | Which characters cast shadows: Off, Simple (round shadows), Party Complex (true shadows for your party; as shipped) or All Complex. |
| **Graphics: Shadow Detail** | The size of each character's shadow silhouette: Original (64 pixels, as shipped), 128, 256 (default), 512, 1024. Larger shadows are sharper, especially at high resolutions, and use a little more GPU memory and time. |
| **Graphics: Shadow Edges** | Original (hard edges, as shipped), Soft (default) or Softer: a light filter that smooths the edges of character shadows. |

The game runs in macOS full screen, so **Game Mode** comes on while you play (the CPU and GPU kept for the game,
lower Bluetooth latency), whichever Notch setting is chosen.

Your choices are remembered: next time just press **Play** (or Return). Arrow keys move between and change settings,
Return opens a group and closes it again; Escape closes it, or quits.

The banner at the top is Dungeon Siege's key art, downloaded by the installer from Steam's store for this window
(`~/Games/DungeonSiegeNative/art`; not part of this project). Without it (or with `--no-downloads`) the window shows
its title plaque instead.

In the game: the mouse or trackpad controls the game's cursor; two-finger click is right click; two-finger scroll
zooms. All keyboard shortcuts work as on Windows. **Cmd+Tab** switches away, **Cmd+Q** quits.

With the SeeFar mod installed: at Original view distance the mod works as before; at any farther setting the game
uses the built-in view distance instead (the mod's file is set aside for that session, not deleted).

## If something goes wrong

- **Crash or freeze:** the app writes a report to `~/Games/DungeonSiegeNative/CrashReports` (also when the game stops
  responding, while it is still frozen) and offers it on the next launch. The report says what the game was doing;
  attach it when reporting a problem.
- **The build stops with "not the original GOG or Steam 1.11.1 DungeonSiege.exe"** (or `--steam` with "is not Steam's
  Dungeon Siege 1.11.1"): the folder holds a different version of the game (disc, another patch level, or a modified
  executable).
- **`innoextract is needed for --gog-installer`:** `brew install innoextract`.
- **`clang` or `python3` missing:** install the Xcode Command Line Tools (`xcode-select --install`).
- **Sound problems:** start it from Terminal with the hand-written sound system instead of the game's own:
  `W32_MILES=native "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- **Skip the launch window** (use the saved settings directly):
  `DS_NO_LAUNCHER=1 "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- The log is `~/Games/DungeonSiegeNative/DungeonSiegeNative.log` (`DungeonSiegeNightly.log` for Nightly); an update's
  output is in `update.log` next to it.

## Legends of Aranna

Add the expansion from your own copy with `./install.sh --expansion "/path/to/Legends of Aranna"` and choose it in the
launch window: [LEGENDS-OF-ARANNA.md](LEGENDS-OF-ARANNA.md).

## Not supported

- Base-game versions other than the supported GOG and Steam 1.11.1 executables.
- Controllers (mouse, trackpad and keyboard only).
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
