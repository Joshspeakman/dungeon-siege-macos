# Notices and credits

**Dungeon Siege** © its respective rights holders. This project contains no part of the game. The installer recompiles
the executable of a copy the user owns, after verifying its checksum, and the app reads the game's data from that copy.

Used at build time (installed into `recomp/.venv` with pip, not redistributed here):
- **Capstone** (BSD) — disassembly for the recompiler. https://www.capstone-engine.org/
- **Unicorn** (GPL-2.0) — x86 emulation, used only by the test suite to compare the recompiled code with the original.
  https://www.unicorn-engine.org/

Specifications and prior work this project learned from:
- Microsoft's published DirectPlay 8 protocol specifications ([MC-DPL8CS], [MC-DPL8R]) for the planned multiplayer.
- **SiegeFX** (GPL-3.0) and **OpenSiege** (GPL-3.0), open-source reimplementations of the engine; no code was taken
  from them.
- `src/dsr/dsr_snapshot.bin` records the device capabilities reported by Wine's DirectDraw implementation (Wine,
  LGPL), so that the game sees a consistent Direct3D 7 device.
- The launch window uses Copperplate, a font that ships with macOS (not distributed here), and draws its artwork from
  the player's own game files at run time.
