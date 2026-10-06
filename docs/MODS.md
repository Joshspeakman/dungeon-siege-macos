# Mods

Dungeon Siege mods are archives (`.dsres`, and `.dsmap` for maps) that the game loads next to its own. The app keeps
them in one place and lets each game have its own selection.

## Using them

- **Adding:** put the files in the Mods folder, `~/Games/DungeonSiegeNative/mods` (the launcher's Mods panel has an
  *Open Mods Folder* button), or let the installer collect them:

  ```sh
  ./install.sh --mods "/path/to/a/folder/with/mods"
  ```

  It takes every Dungeon Siege archive it finds under that folder except the game's and the expansion's own, so an
  old Windows installation or a backup folder can be given as it is.
- **Choosing:** the launcher's **Mods** row shows how many are on; click it for the list and tick what you want. The
  selection is separate for Dungeon Siege and for Legends of Aranna (the Game row decides which one you are editing).
  The mods below start ticked for the games they belong to; others start unticked.
- **Multiplayer:** everyone in a game needs the same archives, mods included; a player who is missing one is told
  which. See [MULTIPLAYER.md](MULTIPLAYER.md).

The game folder is never changed: ticked mods are linked into the data folder's view of it when the game starts.

## Known mods and credits

These are free mods by their authors, credited here. Only the ones whose authors allow redistribution are included in
this repository (`mods/`); the others are added from your own copies with `--mods`.

| Mod | File | Author | Games | Notes |
|---|---|---|---|---|
| Yesterhaven | `Yesterhaven.dsmap`, `Yesterhaven.dsres` | Gas Powered Games | both | Gas Powered Games' free multiplayer adventure. Not in this repository: the installer downloads GPG's own installer for it from the [Internet Archive](https://archive.org/details/DungeonSiegeYesterhaven) (checked by its hash) and takes the two archives out, unless it's already in the Mods folder (`--no-downloads` skips it). `--yesterhaven` also takes your own copy (a folder, or the installer). |
| Multiplayer Quest Save (beta 6) | `ikkyo_mpsave_beta_6.dsres` | Jason "Ikkyo" Gripp, 2003 | both | Keeps quest progress on the multiplayer maps between sessions. **Included** (unaltered, with its readme): the author permits copying and transmission free of charge, unaltered. |
| SeeFar2020 Resolution Fix | `sf_ResolutionFix.dsres` | antonior (SeeFar2020, 2020), after SeeFar by Jeff Kretz and Irwin Ryan (camera code by Ikkyo) | both | Interface layout for wide resolutions. Not included (no redistribution terms from its author); add your copy with `--mods`. |
| Fairy Fix | `fairyfix.dsres` | unknown | both | A small fix commonly installed with Yesterhaven. Not included (author and terms unknown). |
| UberUI for Legends of Aranna (0.02) | `UberUI_LoA_v0.02.dsres` | unknown | Legends of Aranna | Extended character screen. Not included (author and terms unknown). |
| DS1 Difficulty Patch | `DS1_Difficulty_Patch.dsres` | unknown | Legends of Aranna | Rebalanced monsters. Not included (author and terms unknown). |

If you know the author of one marked unknown, or where it was published with terms that allow redistribution, it can
be credited properly and included.

Legends of Aranna itself is a commercial expansion and is never part of this repository: `install.sh --expansion`
copies the data from your own copy.
