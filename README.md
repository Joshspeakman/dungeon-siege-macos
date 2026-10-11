# Dungeon Siege for Apple Silicon Macs

Play the original **Dungeon Siege** (GOG or Steam, version 1.11.1) natively on an Apple Silicon Mac: no Wine, no Rosetta, no
Windows components. The installer recompiles **your own copy** of the game from x86 into native arm64 code and links it
with a macOS implementation of everything the game uses from Windows (windows and input, files, threads, DirectDraw and
Direct3D 7 on Metal, DirectSound on Core Audio). The result is an ordinary Mac app.

Nothing from the game is in this repository. You need your own copy, and the installer checks that it is the exact
build it supports.

**Status: beta.** The single-player campaign, multiplayer (with Mac and Windows players) and the Legends of Aranna
expansion all play; see [Known issues](#known-issues). Releases: [Beta 1](https://github.com/Joshspeakman/dungeon-siege-macos/releases/tag/beta-1)
and later on the [releases page](https://github.com/Joshspeakman/dungeon-siege-macos/releases).

```sh
xcode-select --install                     # once: clang, git and python3
git clone https://github.com/Joshspeakman/dungeon-siege-macos.git && cd dungeon-siege-macos
./install.sh --game-dir "/path/to/GOG Games/Dungeon Siege"
#   or: ./install.sh --gog-installer ~/Downloads/setup_dungeon_siege_*.exe     (needs: brew install innoextract)
#   or: ./install.sh --steam                                                   (the Steam edition, see below)
open ~/Applications/"Dungeon Siege Native.app"
```

Keep the clone in a path without spaces: the current compile scripts split source paths on whitespace.

The step-by-step guide is **[docs/INSTALL.md](docs/INSTALL.md)**. The sections below summarise it.

## Requirements

- A Mac with **Apple Silicon** (M1 or later). The installer stops on an Intel Mac. The app needs **macOS 13** or newer.
- The **Xcode Command Line Tools**: `xcode-select --install` (installs `clang`, `git` and `python3`). The full Xcode
  isn't needed.
- On the first build the recompiler makes a Python environment in `recomp/.venv` and installs two helper packages into
  it with pip (`capstone` and `unicorn`), so the first install needs an internet connection.
- Build time and peak disk use have not been verified for a clean install. Allow room for your game files,
  generated source and object files, the app, and additional update worktrees; time varies with the Mac. The GOG
  installer, Steam and collection options also copy or unpack the game into the data folder.
- Optional: [Homebrew](https://brew.sh) and `brew install innoextract`, only for installing from the GOG offline
  installer.
- **Dungeon Siege 1.11.1 from GOG or Steam.** The installer compares the SHA-256 of `DungeonSiege.exe` with the two
  builds it supports and refuses anything else:

  | Edition | `DungeonSiege.exe` SHA-256 |
  |---|---|
  | GOG 1.11.1 | `41f14b145e030f2decd95e9f434ccd1de0729ba13d1c5628c4bd9536ee938a02` |
  | Steam 1.11.1 (app 39190, depot 39191) | `c408ef77b39484d8ad82ba17859cf1e60b24d3baf6d429283a52b886d67f33ab` |

  The disc releases, other patch levels and modified executables are not supported. The Steam executable has no DRM.
  The Legends of Aranna expansion's own executable, `DSLOA.exe`, is never used (see below).

## Getting the game files onto the Mac

Pick one; each is a single `install.sh` command.

| You have | Command |
|---|---|
| The GOG game installed somewhere (copied from a PC, GOG Galaxy, an old CrossOver or Wine install) | `./install.sh --game-dir "/path/to/Dungeon Siege"` (the folder that holds `DungeonSiege.exe`) |
| The GOG offline installer, `setup_dungeon_siege_*.exe`, from your GOG library | `./install.sh --gog-installer ~/Downloads/setup_dungeon_siege_*.exe` (needs `innoextract`; the game is unpacked into the data folder) |
| The Steam edition | In Steam on the Mac, open the console (`steam://open/console`) and run `download_depot 39190 39191`; then `./install.sh --steam` (or `./install.sh --steam "/path/to/the/downloaded/folder"`). The download is copied into the data folder. |
| A backup made with `--save-collection` | `./install.sh --collection "/path/to/the/collection"` |

Give `--game-dir` an absolute path (for example `"$HOME/Games/Dungeon Siege"`, not a relative one): the app remembers the folder and reads the game's data
from it every time it starts, so leave that folder where it is. The game folder is only ever read.

## Installing

```sh
git clone https://github.com/Joshspeakman/dungeon-siege-macos.git
cd dungeon-siege-macos
./install.sh --game-dir "/path/to/Dungeon Siege"
```

This recompiles the game and installs **`~/Applications/Dungeon Siege Native.app`** (shown as *Dungeon Siege*). Keep
the cloned folder: the app's Updates row rebuilds from it.

Every `install.sh` option:

| Option | What it does |
|---|---|
| `--game-dir DIR` | Build from an installed GOG or Steam game folder. |
| `--gog-installer FILE` | Unpack the GOG offline installer into `<data>/gog-extract` (with `innoextract`) and build from that. |
| `--steam [DIR]` | The Steam edition: Steam's own download location, or `DIR`. Copied into `<data>/steam-game`. |
| `--expansion DIR` | Add Legends of Aranna from your own copy: `Expansion.dsres`, `ExpVoices.dsres` and `Expansion.dsmap` (plus `XPRes.dsres` and `XPMap.dsmap` if present) are found anywhere under `DIR`. Only the data is used. |
| `--mods DIR` | Add valid Dungeon Siege mod archives (`.dsres`, `.dsmap`) found under `DIR`, excluding the base game and expansion archives, to the launcher's Mods list. |
| `--yesterhaven DIR-or-FILE` | Add Gas Powered Games' free Yesterhaven adventure from a folder holding its two files, or from its installer. |
| `--benchmark FILE` | Add Gas Powered Games' Dungeon Siege Benchmark from its installer, `DSBenchmark.EXE` ([docs/BENCHMARK.md](docs/BENCHMARK.md)). |
| `--collection DIR` | Set up everything from one backup folder: the game, the expansion, mods, saves and launcher settings. |
| `--save-collection DIR` | Write that backup folder from this Mac's current setup. |
| `--nightly` | Install **Dungeon Siege Nightly.app** next to the stable app, built from the `nightly` branch (see *Updating*). |
| `--app-dir DIR` | Where the app goes (default `~/Applications`). See the updater limitation below. |
| `--data-dir DIR` | Where saves, settings and logs go (default `~/Games/DungeonSiegeNative`). See the updater limitation below. |
| `--no-downloads` | Skip the two downloads an install makes by default (also `DS_NO_DOWNLOADS=1`). |

`--expansion`, `--mods`, `--yesterhaven` and `--benchmark` work on their own (after the game is installed) or together
with a game option. With no game option at all, `install.sh` rebuilds from the game folder the installed app already
uses.

**Downloads during an install.** Unless `--no-downloads` is given, the installer fetches two things that are not part
of this project, only for your Mac:

- Yesterhaven: Gas Powered Games' own installer from the [Internet Archive](https://archive.org/details/DungeonSiegeYesterhaven)
  (53 MB, checked by its hash), when the Mods folder doesn't have it yet. Only its two archives are taken out.
- The launch window's banner: Dungeon Siege's key art from Steam's store. Without it the window shows its title plaque.

### What goes where

| Location | Contents |
|---|---|
| Your game folder | Only read, never changed. |
| `~/Applications/Dungeon Siege Native.app` | The recompiled game, built on your Mac from your copy. |
| `~/Games/DungeonSiegeNative` | The data folder: saves and the game's settings (`drive_c/Users/player/Documents/Dungeon Siege`, and `Dungeon Siege LOA` for the expansion), the launch window's choices (`launcher.plist`), mods (`mods/`), the expansion (`expansion/`), the log (`DungeonSiegeNative.log`), update output (`update.log`), crash reports (`CrashReports/`), benchmark results (`Benchmarks/`), and the unpacked or copied game for `--gog-installer`, `--steam` and `--collection`. |
| The cloned repository | The recompiler, its Python environment (`recomp/.venv`), the generated code (`recomp/work`, never committed) and the update builds (`build/stable`, `build/nightly`). |

## First launch

Open **Dungeon Siege** from `~/Applications` or Spotlight. The launch window comes first, drawn with the game's own
menu art. Choose and press **Play** (or Return); every choice is remembered.

- **Game:** Single Player (the Kingdom of Ehb campaign) or Multiplayer. **Legends of Aranna** and **Aranna
  Multiplayer** appear once the expansion is installed, **Benchmark** once the benchmark is.
- **Display:** Resolution (your display's size is recommended), Interface Size (100% to 300%: a larger interface while
  the world stays sharp), Notch (on a Mac with a notch: **Hide the Notch** or **Around the Notch**), View Distance
  (Original up to Maximum, 300%), Gamma, and Frame Rate (Automatic, 120 on displays reporting at least 120 Hz, 60, 30 or Unlimited).
- **Graphics:** the game's own Options > Video settings (Texture Filtering, Object Detail, Shadows) plus Shadow Detail
  and Shadow Edges for sharper, softer character shadows. They're written into the game's settings when you press
  Play, so the in-game Video page needn't be used.
- **Mods:** a tick list per game, and an *Open Mods Folder* button ([docs/MODS.md](docs/MODS.md)).
- **Updates:** see below.

The full table of settings and their choices is in [docs/INSTALL.md](docs/INSTALL.md#playing).

The first time you host or look for a multiplayer game, macOS asks whether Dungeon Siege may find devices on your local
network, and the firewall (if it is on) whether it may accept incoming connections. Allow both.

In the game the mouse or trackpad controls the cursor, a two-finger click is a right click, and two-finger scrolling
zooms. The game's keyboard shortcuts work as on Windows; **Cmd+Tab** switches away and **Cmd+Q** quits. Controllers are
not supported.

## Updating

**From the app:** the launch window's **Updates** row asks GitHub for the newest commit of the app's branch: `main` for
*Dungeon Siege*, `nightly` for *Dungeon Siege Nightly*. When it says **Update available**, press it. The new version
is checked out into `build/stable` (or `build/nightly`) inside your clone, the app is rebuilt from your copy of the
game (with a progress window), and offers a Restart button when it finishes.
Your own checkout, game folder, saves, settings and mods
are left as they are. The output goes to `update.log` in the data folder.

Keep the clone in the same place. *Not available* means the app has no usable updater script; *Couldn't check*
means the script failed to read the remote branch, for example because the clone moved, GitHub was unreachable or
git needed a sign-in. Press the row to retry.

**Known limitation:** the updater runs the new `install.sh` without the options you first installed with. If you used
`--app-dir` or `--data-dir`, update by hand with the same options. The updater searches the default app folder,
so it may fail to find your app or use another installed copy, and it rebuilds with the default data folder.

**By hand:** `git pull`, then run the same `install.sh` command again (with the same `--app-dir`/`--data-dir`, if
any, and `--nightly` for the Nightly app).

**Stable and Nightly.** New work lands on the `nightly` branch first and reaches `main` once it has proven itself.
`./install.sh --nightly` builds the newest `nightly` in `build/nightly` (your checkout stays as it is) and installs
**Dungeon Siege Nightly.app** next to the stable app, with its own icon. Both share the data folder, so saves, settings
and mods work in either; each keeps its own log. Run it again, or use its Updates row, to update it.

## Uninstalling

Delete `~/Applications/Dungeon Siege Native.app` (and `Dungeon Siege Nightly.app`), the cloned folder, and
`~/Games/DungeonSiegeNative` if you no longer want your saves. Your game folder was never changed.

## Features

- **Native performance.** The game's own code runs as optimised arm64 with no translation layer. The frame rate follows
  your display (60 Hz, or 120 Hz on ProMotion screens), with Unlimited for faster hardware, and frame pacing stays
  steady. The heaviest calculation in big battles (vertex lighting) runs as native code with identical results.
- **Game Mode.** [Game Mode](https://support.apple.com/en-us/105118) requires macOS 14 or later and macOS full screen.
  Around the Notch uses a borderless window and does not enable Game Mode.
- **Scales with your Mac.** View distance up to 300% of the original (the built-in equivalent of the SeeFar mod),
  Interface Size for high resolutions, and defaults chosen from your Mac's GPU (Very Far on Max and Ultra chips,
  Farther elsewhere). A new installation starts with all complex shadows and trilinear filtering.
- **Sharper, softer character shadows:** Shadow Detail (up to 1024 pixels, 256 by default; the original is 64) and
  Shadow Edges (Soft by default).
- **Multiplayer and cross-play** on LAN and over the internet, using the game's own DirectPlay 8 protocol
  (implemented from Microsoft's published specifications): Macs with each other, and Macs with PCs running the Windows
  game. A hosting Mac asks the router to forward the game's ports (NAT-PMP or UPnP). A Mac built from either edition
  presents itself as GOG 1.11.1, so it plays with GOG PCs. PC players need the GOG 1.11.1 executable named
  `DungeonSiege.exe`, the same resource files and Microsoft's DirectPlay. Details: [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md).
- **Legends of Aranna**, the expansion, from your own copy's data (`./install.sh --expansion <folder>`), with its own
  saves in the original's folder name, so they can be copied to and from Windows.
- **Mods** chosen per game in the launcher (Yesterhaven, ResolutionFix, UberUI and others; credits in
  [docs/MODS.md](docs/MODS.md)).
- **The Dungeon Siege Benchmark** as a launcher option and tuning tool ([docs/BENCHMARK.md](docs/BENCHMARK.md)).
- **The game's own sound and video** (Miles, Bink), recompiled from your game folder as well and playing through Core
  Audio.
- **Crash and freeze reports** in `~/Games/DungeonSiegeNative/CrashReports`.
- **Backups:** `--save-collection` and `--collection` keep a whole setup (game, expansion, mods, saves, settings) in
  one folder and restore it with one command.

## Legends of Aranna

You need your own copy of the expansion (the disc, or an installed copy from another
computer). Its executable is not used: the disc version is copy-protected, and this project never removes copy
protection. Instead its **data** runs on the recompiled 1.11.1 engine, with the engine functions the expansion added
written anew (`recomp/runtime/win32/loa.c`).

```sh
./install.sh --expansion "/path/to/Legends of Aranna"
```

Then choose **Legends of Aranna** in the Game row. The campaign, its new screens, the world map, transformations,
backpacks, set items and the new item bonuses work; a few parts are simplified or not yet tried. The full list is in
[docs/LEGENDS-OF-ARANNA.md](docs/LEGENDS-OF-ARANNA.md). Because the Mac runs the expansion on the base game's engine,
playing it with a PC running `DSLOA.exe` hasn't been tested and isn't expected to work: the game only lets identical
builds play together ([docs/MULTIPLAYER.md](docs/MULTIPLAYER.md#windows-and-linux-players)).

## Known issues

- **Blank HUD portrait in old saves** ([#1](https://github.com/Joshspeakman/dungeon-siege-macos/issues/1)): characters
  first created under Wine or CrossOver can have an entirely black portrait stored in their save. New characters are
  fine. `recomp/tools/repair_portrait.py` can copy the portrait from a native save of a character with the same
  appearance ([docs/PORTRAIT-REPAIR.md](docs/PORTRAIT-REPAIR.md)).
- **Multiplayer:**
  - Cross-play was tested against the Windows game running on Linux through Proton with Microsoft's own DirectPlay, in
    both directions and over the internet; a native Windows PC hasn't been tried yet.
  - Two copies of the app on the *same* Mac can join each other, but the joiner's world may not load. Use two machines.
  - When a player joins, the host's chat shows "... has entered the game" followed by "... has left the game",
    although the player stays.
  - Legends of Aranna multiplayer: hosting and starting a game work on one Mac; playing together hasn't been tested.
- **Around the Notch** uses a borderless window rather than macOS full screen.
- **The front-end menus** are 800×600 by the game's design and are shown with bars at the sides; the game world uses
  the chosen resolution.
- **Not supported:** controllers, the disc releases, and `DSLOA.exe` itself.

Open issues: [github.com/Joshspeakman/dungeon-siege-macos/issues](https://github.com/Joshspeakman/dungeon-siege-macos/issues).

## Troubleshooting

- **"not the original GOG or Steam 1.11.1 DungeonSiege.exe"** (or, with `--steam`, "is not Steam's Dungeon Siege
  1.11.1"): the folder holds another version of the game (disc, another patch level, or a modified executable). Point
  the installer at an unmodified GOG or Steam 1.11.1 copy.
- **"clang is missing" or "python3 is missing":** run `xcode-select --install`.
- **"innoextract is needed for --gog-installer":** `brew install innoextract`, or unpack the game another way and use
  `--game-dir`.
- **"the native build is for Apple Silicon Macs":** the build needs an M-series Mac.
- **The usage message when running `./install.sh` with no options:** no app is installed yet, so give a game option.
- **A crash or freeze:** the app writes a report to `~/Games/DungeonSiegeNative/CrashReports` (also while a frozen
  game is still stuck) and offers it on the next launch. Attach it, and the log
  (`~/Games/DungeonSiegeNative/DungeonSiegeNative.log`), when reporting a problem.
- **Sound problems:** start it from Terminal with the hand-written sound system instead of the game's own:
  `W32_MILES=native "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- **Skip the launch window** (play with the saved settings):
  `DS_NO_LAUNCHER=1 "$HOME/Applications/Dungeon Siege Native.app/Contents/MacOS/launcher"`.
- **An update fails:** read `update.log` in the data folder; updating by hand (`git pull` and the same `install.sh`
  command) lets you retry with the original options; it can still fail if the build fails.

## How it works

`install.sh` checks that the executable is the GOG or Steam 1.11.1 build, then:

1. analyses its x86 code and translates every function into C (`recomp/tools/analyze.py`, `recomp/tools/lift.py`),
   and does the same for the game's Miles Sound System and Bink DLLs;
2. compiles that C for arm64 together with the runtime (`recomp/runtime`): a 32-bit Windows environment implemented on
   macOS, and a Metal renderer for the game's Direct3D 7 drawing (`src/renderer`);
3. builds `Dungeon Siege Native.app`, which reads the game's data from your game folder every time it starts.

The translation is checked against the original code running in an x86 emulator, instruction form by instruction form
and on complete routines. Everything generated from the game stays in `recomp/work/` and is never committed. Details,
developer builds and test switches: [recomp/README.md](recomp/README.md).

## Documentation

| Document | Contents |
|---|---|
| [docs/INSTALL.md](docs/INSTALL.md) | Installing, the Steam edition, backups, Nightly, every launcher setting, troubleshooting |
| [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md) | LAN and internet games, router ports, playing with Windows and Linux PCs, security |
| [docs/LEGENDS-OF-ARANNA.md](docs/LEGENDS-OF-ARANNA.md) | The expansion: installing, keys, what works |
| [docs/MODS.md](docs/MODS.md) | Adding and choosing mods, credits |
| [docs/BENCHMARK.md](docs/BENCHMARK.md) | Gas Powered Games' benchmark demo |
| [docs/PORTRAIT-REPAIR.md](docs/PORTRAIT-REPAIR.md) | Repairing a black portrait in an old save |
| [recomp/README.md](recomp/README.md) | How the recompiler works, verification, developer switches |
| [docs/NETCODE-PLAN.md](docs/NETCODE-PLAN.md), [docs/PC-PARITY-PLAN.md](docs/PC-PARITY-PLAN.md), [docs/PERFORMANCE-PLAN.md](docs/PERFORMANCE-PLAN.md) | Development plans and findings |

## Legal

This project contains no game code, data or assets. The game's executable is recompiled on your own Mac from your own
legally obtained copy, and the app reads the game's data and art from that copy at run time; nothing derived from the
game is stored in this repository. It does not remove copy protection. Dungeon Siege is a trademark of its respective
owners; this project is not affiliated with them. The project's own code is MIT-licensed ([LICENSE](LICENSE)); credits
are in [NOTICE.md](NOTICE.md).
