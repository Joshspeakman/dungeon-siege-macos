# Dungeon Siege for Apple Silicon Macs

Play the original **Dungeon Siege** (GOG, version 1.11.1) natively on an Apple Silicon Mac: no Wine, no Rosetta, no
Windows components. The installer recompiles **your own copy** of the game from x86 into native arm64 code and links it
with a macOS implementation of everything the game uses from Windows (windows and input, files, threads, DirectDraw and
Direct3D 7 on Metal, DirectSound on Core Audio). The result is an ordinary Mac app.

```sh
xcode-select --install                     # once: clang and python3
git clone https://github.com/Joshspeakman/dungeon-siege-macos.git && cd dungeon-siege-macos
./install.sh --game-dir "/path/to/GOG Games/Dungeon Siege"
#   or: ./install.sh --gog-installer ~/Downloads/setup_dungeon_siege_*.exe     (needs: brew install innoextract)
open ~/Applications/"Dungeon Siege Native.app"
```

**Full instructions: [docs/INSTALL.md](docs/INSTALL.md)**: requirements, installing, playing, settings, updating and
troubleshooting.

## Features

- **Native performance** at your display's resolution and refresh rate, with steady frame pacing.
- **A launch window** in the game's style for resolution, **view distance** and frame rate. Your choices are remembered,
  so normally you just press Play.
- **Built-in view distance** from the original up to 300%, without mods (150% matches the community SeeFar mod).
- **The game's own sound system** (Miles), recompiled as well, playing through Core Audio.
- Mouse, trackpad and keyboard, saving and loading, the full single-player campaign.
- **Crash and freeze reports** in `~/Games/DungeonSiegeNative/CrashReports`, so problems can be diagnosed.

## How it works

`install.sh` checks that the executable is the GOG 1.11.1 build, then:

1. analyses its x86 code and translates every function into C (`recomp/tools/analyze.py`, `recomp/tools/lift.py`),
   and does the same for the game's Miles Sound System DLLs;
2. compiles that C for arm64 together with the runtime (`recomp/runtime`): a 32-bit Windows environment implemented on
   macOS, and a Metal renderer for the game's Direct3D 7 drawing (`src/renderer`);
3. builds `Dungeon Siege Native.app`, which reads the game's data from your game folder every time it starts.

The translation is checked against the original code running in an x86 emulator, instruction form by instruction form
and on complete routines. Details: [recomp/README.md](recomp/README.md).

## Status

- Single-player campaign: playable.
- Multiplayer: not yet. The plan, including playing together with Windows players, is in
  [docs/MULTIPLAYER-PLAN.md](docs/MULTIPLAYER-PLAN.md).
- Only the GOG release 1.11.1 is supported; *Legends of Aranna* is not supported yet.

## Legal

This project contains no game code, data or assets. The game's executable is recompiled on your own Mac from your own
legally obtained copy, and the app reads the game's data and art from that copy at run time; nothing derived from the
game is stored in this repository. Dungeon Siege is a trademark of its respective owners; this project is not affiliated
with them. The project's own code is MIT-licensed ([LICENSE](LICENSE)); credits are in [NOTICE.md](NOTICE.md).
