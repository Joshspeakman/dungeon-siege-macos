# Action Mode

Dungeon Siege played like an action RPG: you steer one hero directly with the keyboard or a controller, fight with
abilities on a hotbar, and the rest of the party follows and fights on its own. It is a launcher setting, **Play
Style**:

- **Action**: everything below, with bigger monster groups.
- **Action, Original Packs**: the same, with the monster groups as the game made them.
- **Classic**: the original point-and-click game (none of this runs).

It changes how the game is played, not its data: saves, maps and mods are the same in every style, and a save can be
carried between them (the only things Action Mode adds to a world are ordinary ones: the extra monsters of a pack,
dropped potions).

The development app is **Dungeon Siege Action** (`./install.sh --as-action`, branch `action-mode`). It starts in
Action and updates from the `action-mode` branch.

## Controls

| | Keyboard and mouse | Controller |
|---|---|---|
| Walk | W A S D (relative to the camera) | Left stick |
| Attack, talk, pick up | Left click (as in the original) | A: the nearest enemy ahead, else the nearest person or thing ahead; right after you moved the pointer, a click at the pointer |
| Dodge | Space | B |
| Ability 1 | 1 | X |
| Ability 2 | 2 | Y |
| Ability 3 | 3 | RB |
| Ability 4 | 4 | RT |
| Health / mana potion | H / M (as in the original) | LT / LB |
| Previous / next hero | , and . (as in the original) or click a portrait | D-pad left / right |
| Quick weapon select | Q | D-pad up |
| Collect loot | Z | D-pad down |
| Inventory | I | View |
| Map | Tab | Left stick click |
| Pointer | Mouse | Right stick |
| Camera | Arrow keys, mouse wheel (as in the original) | Right stick click switches the right stick to the camera (turn, zoom) and back |
| Options | Esc | Menu |
| Pause | P (or Pause) | |

In menus, the inventory, shops and the front end, a controller moves the pointer with either stick, A clicks, X
right-clicks and B goes back. Keys held with Ctrl or Alt keep the game's own meaning (Alt+W A S D for orders, Ctrl+S to
save, Ctrl+1-0 to store weapon sets); weapon sets 5-0 are still recalled with their number keys. Text boxes (chat, a
save's name) get every key as usual.

## The party

You play the **selected** hero (with several selected, the first one; as soon as you start walking, only that one).
It goes where you send it and nowhere else: the engine's own jobs for it that would walk it somewhere or chase an
enemy are turned down. While you stand still it still defends itself against an enemy already in reach (about 3 m
with a melee weapon, its range with a bow or a spell); while you walk it doesn't stop to fight. Its other jobs go on as
usual (potions, getting up, dying), and nothing about the hero itself changes, so saves are the same as in Classic.

Every other party member gets the game's **Guard** order on you: they follow you and fight what attacks you, with
their own AI. Select another hero (a portrait, `,` `.`, or the D-pad) and you play that one instead; the others guard
the new hero.

## Abilities

What slots 1 and 3 do depends on what the hero holds:

| Slot | Melee weapon | Ranged weapon | Spell | Cooldown |
|---|---|---|---|---|
| Dodge | dash 4.5 m the way you're steering (else away from the nearest enemy) | | | 0.9 s |
| 1 | **Cleave**: every enemy in an arc within reach, 110% damage | **Volley**: up to five enemies ahead, 85% each | **Nova**: every enemy within 7 m, 100% (mana) | 3-4.5 s |
| 2 | **Second Wind**: heal 35% of maximum life | | | 30 s |
| 3 | **Charge**: rush the first enemy ahead, 180% | **Power Shot**: the first enemy ahead, 240% | **Chain Lightning**: up to six enemies, 90% each (mana) | 6-7 s |
| 4 | **Rally**: the whole party heals 25% of life and 20% of mana | | | 60 s |

Damage is the hero's own damage with what is in hand (the game's damage range for that weapon or spell, with the
hero's skills and bonuses), dealt through the game's own damage rules, so experience, kills and loot work as usual.
The visual effects are the game's own effect scripts. The hotbar along the bottom of the screen shows the slots, their
keys (or controller buttons when one is connected), and their cooldowns; it hides while a game window is open.
Abilities work in single player and for the host of a multiplayer game.

## Pace

- **Bigger packs** (Play Style **Action**): when a group of two or more of the same monster comes into view, it gets
  one more of its kind half of the time. A lone monster (a boss, a quest character) is never copied.
- **Drops**: a fallen enemy leaves a small health potion 18% of the time, a small mana potion 8% of the time.
- **Gold and potions** lying within a few metres are picked up when your hero stands idle with no enemy near (other
  loot stays where it is: Z collects all of it, as in the original).

## How it works

All of it is in `recomp/runtime/win32/action.c` (game side) and `recomp/host/ds_pad.h`, `ds_hud.h` (controller and
overlay), at functions `recomp/tools/lift.py` already lets the runtime take over, so nothing was recompiled for it.
With Action Mode off every hook returns at once: the Classic game, its saves and multiplayer digests are unchanged.

- **Steering.** The engine only knows "move to a place", so while a direction is held the hero is given a place 4.5 m
  ahead (`GoMind::RSMove`, the same order a click gives), and a new one when the direction asked for changes, when
  that leg is nearly walked, or every half second. Measured: a steered walk is as even as a walk on a click (4.3-4.5
  m/s, fewer stalled frames). Positions in Dungeon Siege belong to terrain nodes; the camera's direction is turned into
  the hero's node space with the engine's own conversion (`GetSiegeDifference`) and the point is put on the ground
  with `SiegeEngine::AdjustPointToTerrain` (nearer points are tried where there is no ground).
- **Keys.** The game's in-game input binder hands Action Mode W A S D, Space, 1-4 and P first (only while the game's
  keys are live, so never while typing).
- **Once a frame** (at `WorldTime::Update`, not while paused or while a game window is open): pick the hero, steer,
  use abilities, keep the party guarding, pick up gold, watch fights for drops and packs, fill the overlay's numbers.
- **Your orders only.** `GoMind::SDoJob` is asked first: for the hero you play, a move, follow, patrol, flee, guard or
  attack job its own brain (`ao_reflex`) asks for is turned down (attacks in reach while you stand still excepted).
  Orders from you, from scripts and cutscenes, and from the party go through.
- **Loading.** Leaving the world (a load, the main menu) forgets every object of it.

## Testing

Test mode (`DS_TEST=1`) commands for a scripted controller: `stick:x,y` (left stick in percent, held until the next
one) and `pad:bits,frames` (buttons as `PAD_*` bits of `runtime/win32/w32.h`). `hudshot:<png>` saves the overlay.
`DS_ACTIONLOG=1` logs every order, hit, ability and message; `=2` also every job the hero is given and how evenly it
walks. Test mode only: `DS_ACTION_TESTPARTY=gyorn,ulora` clones those characters next to the hero and adds them to the
party (for trying the party with a solo save), `DS_ACTION_TESTKIND=melee|ranged|magic` sets which abilities slots 1
and 3 are. `DS_ACTION_FLIPX=1` mirrors left and right of the steering.

## Not yet

- Multiplayer: Action Mode is made for single player. Hosting works (your hero, your party); a joining player can
  steer, but its moves go through the host (about 0.2 s) and its abilities are off (their damage is the host's).
- The abilities have no new animations of their own: the hero swings or shoots its normal attack at the first target.
- Legends of Aranna runs on the same engine and should play the same, but hasn't been tried in Action Mode yet.
