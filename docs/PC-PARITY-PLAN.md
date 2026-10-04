# PC parity: one setup per machine for Mac, Windows and Linux multiplayer

Goal: after a fresh install, running one setup on each computer is enough for the Mac app and Windows/Linux copies
of Dungeon Siege to play together. Nothing should need to be worked out by hand again.

## What the game checks (found 2026-10-04, Mac port vs. the original under Proton)

Dungeon Siege compares these before it lets a player join; all must be identical on every machine:

| Check | What it covers | Consequence |
|---|---|---|
| Version block | version 1.11.1.1486 and build date | GOG and Steam both say 1.11.1, but... |
| PE checksum | the executable's header checksum | Steam's exe has 0, GOG's 0x003B4D58: they refuse each other |
| FuBi sync digest | the exported functions plus the executable's **file name** (lower case) | the exe must be named `DungeonSiege.exe` |
| Archive list | the GUIDs of every loaded `.dsres` | every player needs the same archives |
| ContentDb digest | compiled skrit from the archives (needs x87-exact maths; fixed in the Mac port) | same archives, same maths |

Transport: DirectPlay 8 over UDP 6073 (enumeration) and 2302-2400. Wine/Proton's built-in DirectPlay cannot host or
join; Microsoft's must be installed into the prefix.

## Deliverables

1. **A shared manifest** (`parity/manifest.txt`): the required game build (sha256 of `DungeonSiege.exe`), and the
   list of archives every machine must have (name, size, sha256): the base archives, Yesterhaven and the companion
   mods in use (today: `fairyfix.dsres`, `ikkyo_mpsave_beta_6.dsres`, `sf_ResolutionFix.dsres`). All three setups
   read it, so they always agree.
2. **Mac (`install.sh`)**: copy the extra archives from the user's folders into the data folder. Base-game ones go to
   `<data>/game` (seen by both games), Legends-of-Aranna-only ones (`UberUI_LoA`, `DS1_Difficulty_Patch`) to
   `<data>/expansion`. Then check the result against the manifest.
3. **Windows (`parity/setup-windows.ps1`)**, run from a copy of this repository:
   - find the game folder (GOG registry key, or ask); check `DungeonSiege.exe` against the manifest;
   - turn on DirectPlay (`DISM /Online /Enable-Feature /FeatureName:DirectPlay /All`, needs an elevated prompt;
     the script asks and explains);
   - copy missing archives from a folder the user points at, and report extras that would break the match;
   - create a desktop shortcut with `zonematch=true`;
   - optionally open the firewall for UDP 6073 and 2302-2400 (asks first).
4. **Linux (`parity/setup-linux.sh`)**, for the game under Steam/Proton:
   - find the Steam library and the game's compatdata prefix;
   - install Microsoft DirectPlay into that prefix (`protontricks <appid> directplay`);
   - if the installed exe is not the manifest's build: set up a side-by-side folder holding the right
     `DungeonSiege.exe` and links to the rest, without touching Steam's own folder;
   - put the archives in place, check them against the manifest, and set the launch argument;
   - print the firewall rule needed (ufw/firewalld), applying it only if asked.
5. **A check mode** in all three (`--check`): compare this machine with the manifest and print exactly what differs.
   That's the first thing to run when a join is refused.

## Goal: Steam copies too

Today every player needs the GOG build, because the Steam executable is a different build: different header
checksum and sync digest, plus 18 small code differences, the same exported functions and the same data. Plan:

- **Mac:** let `build.sh` also accept the Steam 1.11.1 executable (sha256 `c408ef77…`). The recompiler is not
  specific to one build: the Steam exe is recompiled as is, and the Mac app then *is* the Steam build and plays with
  Steam players. The handful of places the runtime refers to fixed addresses must be checked against the Steam build.
  The project's own patches (`patch_exe.py`) need Steam variants or are skipped.
- **PC:** Steam players keep Steam's exe; the manifest gains a "build" choice (GOG or Steam), and everyone in a game
  picks the same one.
- Mixed GOG/Steam games stay impossible: that is the game's own rule, on Windows too.

## Not in scope (yet)

- Legends of Aranna across machines: the PC runs the expansion's own `DSLOA.exe`, the Mac runs the expansion on the
  Dungeon Siege 1 engine. They are different programs, and the game only lets identical programs play together.
- The Mac-hosted joiner on another Mac stayed black in automated tests (a PC joiner works); recheck after the test
  tool fix.

## Testing

The Mac and a Linux PC (Proton) over Tailscale, both directions: join, start, both heroes visible, chat, play for a
few minutes. A Windows PC when available. Each setup is run from scratch on a clean install before it counts as done.
