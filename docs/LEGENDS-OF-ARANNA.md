# Legends of Aranna

*Dungeon Siege: Legends of Aranna* (Mad Doc Software, 2003) is the official expansion: a new campaign in Aranna, new
character options, spells, monsters, pack animals, set items, a world map and new multiplayer maps.

The expansion shipped with its own executable, `DSLOA.exe`. That executable is not used here (the disc version is
copy-protected, and this project never removes copy protection). Instead, the expansion's **data** runs on the
recompiled GOG Dungeon Siege 1.11.1 engine, and the engine functions the expansion added are written anew for that
engine in `recomp/runtime/win32/loa.c`, from what the expansion's own scripts and interface files ask of them. When the
expansion is chosen, they are added to the table the game's script system reads its functions from, so the expansion's
scripts find them like any of the engine's own.

## Installing

You need your own copy of Legends of Aranna (disc, or an installed copy from another computer). The installer copies
three archives, plus the two multiplayer ones if present, from wherever they are under the folder you give it:

```sh
./install.sh --expansion "/path/to/Legends of Aranna"
```

| File | Contents |
|---|---|
| `Expansion.dsres` | the expansion's content: templates, scripts, interface, art |
| `ExpVoices.dsres` | voice acting |
| `Expansion.dsmap` | the Aranna campaign map |
| `XPRes.dsres`, `XPMap.dsmap` | the expansion's multiplayer map (optional) |

They go to `~/Games/DungeonSiegeNative/expansion`; other `.dsres` files in the same folder (mods) are left out, and your
Dungeon Siege game folder is not changed. This works on its own or together with `--game-dir`/`--gog-installer`.

## Playing

In the launch window, set **Game** to **Legends of Aranna** (or **Aranna Multiplayer**) and press Play. The expansion
keeps its own settings, characters and saves, in
`~/Games/DungeonSiegeNative/drive_c/Users/player/Documents/Legends of Aranna`, separate from the Kingdom of Ehb.

The expansion's new keys work as in the original (its default bindings come from its own data):

| Key | Command |
|---|---|
| Shift+Tab | World map |
| R | Redistribute potions across the party |
| U | Unsummon party creatures |
| Y | Untransform party members |
| Shift | Attack Area |

## Status

Working: the main menu, character creation, the campaign start and its quests and dialogue, the expansion's inventory
and journal screens, the world map (pieces revealed as you explore, the current area marked), potion redistribution,
the expansion's AI (new monster behaviour, "approach" jobs, damage transfer), transformation spells, and the
Unsummon/Untransform commands, backpacks (right-click one in the inventory to open it; items drag in and out; Arrange
sorts it), set-item bonuses and the expansion's new item bonuses. While transformed, a character cannot pick up items or talk, as in the original.

Not done yet, or simplified:

- Transformation spells: the character becomes the creature (its model, skins and animations) and changes back when
  the spell ends or with Untransform. A game saved while transformed stores the character in their own form; after
  loading, they stay in their own form until the spell's time runs out.
- Set items: the bonuses for wearing several pieces of a set apply, and are recounted whenever a piece changes hands.
- The expansion's new kinds of item bonus: spell cost and spell damage (for the spell groups they name, such as fire or
  healing spells), weapon range, and the chance to cast a spell at an attacker when hit (special defense) work, from
  items, their random prefixes and suffixes, and sets.
- Shops' Sell All: written to the expansion's description (sells everything not equipped; its list offers Sell All,
  and Sell All but Potions / Spells / Unique Spells / Magic Items; spellbooks, gold, backpacks and unsellable items
  are kept) but not yet tried in a shop.
- Options: the expansion's extra pages work (Enable Selection Rings, Voice Overs, Voice Over Volume) and are
  remembered, but Voice Overs and Voice Over Volume do not yet change how dialogue is played (it plays at the
  Voice Volume).
- Multiplayer: hosting a Legends of Aranna game, creating or importing a character and starting in Arhok works
  (tested on one Mac); playing together with other players has not been tested yet. In the staging area,
  "Import DS Character..." offers your Dungeon Siege characters and "Import DS LOA Character..." your Legends of
  Aranna ones.
- The "Attack Area" key (Shift) attacks what is under the pointer, or the ground there, like the base game's Attack key;
  the original expansion's exact behaviour may differ.
- Not yet done: the Voice Overs options' effect.
- `RSSetGold` is called by one expansion script but does not exist in any version of the engine; the original
  expansion reports the same script error.

Report anything that differs from the original on Windows; the crash and freeze reports in
`~/Games/DungeonSiegeNative/CrashReports` help.
