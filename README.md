# Dungeon Siege for Apple Silicon Macs

Play the original **Dungeon Siege** (GOG or Steam, version 1.11.1) natively on an Apple Silicon Mac: no Wine, no Rosetta, no
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

Own the **Steam** edition instead? `./install.sh --steam` (see the guide for the download).

For the newest experimental work as a second app, **Dungeon Siege Nightly**: `./install.sh --nightly`.

**Full instructions: [docs/INSTALL.md](docs/INSTALL.md)**: requirements, installing, playing, settings, updating and
troubleshooting.

## Features

- **Scales with your Mac.**
  - The frame rate follows your display: 60 Hz, or 120 Hz on ProMotion screens.
  - Unlimited is offered for faster hardware.
  - View distance goes up to 300% of the original.
  - Defaults are chosen from your Mac's GPU: Very Far on Max and Ultra chips, Farther elsewhere.
  - Nothing is capped to suit older Macs.
- **Native performance.** The game's own code runs as optimised arm64 with no translation layer. Its work per frame
  is down by about 40% and the renderer's CPU time is about halved, with frame pacing that stays steady.
- **Multiplayer and cross-play** on LAN and over the internet: Macs with each other, and Macs with PCs running the
  Windows game. Tested in both directions: Mac hosting with a PC joining, and a PC hosting with a Mac joining. Also
  covers the Yesterhaven map, router port forwarding done for you, and quicker movement response for joining players.
- **GOG or Steam.** Either edition of 1.11.1 works. The Steam executable has no DRM, and a Steam copy plays with GOG
  players.
- **A launch window** in the game's style: single player or multiplayer, Legends of Aranna, mods, resolution, view
  distance, shadows and frame rate, all remembered. **Updates** come from it too: it tells you when a newer version is on
  GitHub and installs it.
- **Legends of Aranna**, the expansion, from your own copy's data (`./install.sh --expansion <folder>`).
- **Mods** chosen per game in the launcher (Yesterhaven, ResolutionFix, UberUI and others; credits in
  [docs/MODS.md](docs/MODS.md)).
- **Best graphics by default:** a new installation starts with all complex shadows and trilinear filtering. The 2002
  game picks lower settings for any video card newer than its hardware table. Choices in Options still apply.
- **Sharper, softer character shadows:** Shadow Detail (up to 1024 pixels, 256 by default; the original is 64) and
  Shadow Edges (Soft by default) in the launcher.
- **The game's own sound and video** (Miles, Bink), recompiled as well and playing through Core Audio.
- Mouse, trackpad and keyboard, saving and loading, and the full single-player campaign.
- **Crash and freeze reports** in `~/Games/DungeonSiegeNative/CrashReports`, so problems can be diagnosed.
- **A second app, Dungeon Siege Nightly,** for the newest experimental work (`./install.sh --nightly`).

## How it works

`install.sh` checks that the executable is the GOG or Steam 1.11.1 build, then:

1. analyses its x86 code and translates every function into C (`recomp/tools/analyze.py`, `recomp/tools/lift.py`),
   and does the same for the game's Miles Sound System DLLs;
2. compiles that C for arm64 together with the runtime (`recomp/runtime`): a 32-bit Windows environment implemented on
   macOS, and a Metal renderer for the game's Direct3D 7 drawing (`src/renderer`);
3. builds `Dungeon Siege Native.app`, which reads the game's data from your game folder every time it starts.

The translation is checked against the original code running in an x86 emulator, instruction form by instruction form
and on complete routines. Details: [recomp/README.md](recomp/README.md).

## Status

- **Single-player campaign:** playable.
- **Multiplayer:**
  - LAN and internet games, including Yesterhaven;
  - cross-play tested against the Windows game running on Linux through Proton with Microsoft's own DirectPlay, in
    both directions and over the internet;
  - a Mac built from the Steam copy played with a GOG PC.

  A native Windows PC hasn't been tried yet, but the PC side was the Windows game and Microsoft's DirectPlay
  ([docs/MULTIPLAYER.md](docs/MULTIPLAYER.md)).
- **Legends of Aranna:** its campaign runs on the recompiled engine from your own copy's data, with the engine
  functions it added written anew. Not everything is done yet
  ([docs/LEGENDS-OF-ARANNA.md](docs/LEGENDS-OF-ARANNA.md)).
- **Supported base games:** Dungeon Siege 1.11.1 from GOG or Steam. The disc releases use other executables.
- **Hardened:** the network code, file handling and renderer were security-reviewed and fuzz-tested. The protocol's
  own limits are described in [docs/MULTIPLAYER.md](docs/MULTIPLAYER.md#security).

## Legal

This project contains no game code, data or assets. The game's executable is recompiled on your own Mac from your own
legally obtained copy, and the app reads the game's data and art from that copy at run time; nothing derived from the
game is stored in this repository. Dungeon Siege is a trademark of its respective owners; this project is not affiliated
with them. The project's own code is MIT-licensed ([LICENSE](LICENSE)); credits are in [NOTICE.md](NOTICE.md).
