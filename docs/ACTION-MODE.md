# Action Mode

Dungeon Siege played like an action RPG: you steer one hero directly with the keyboard or a controller, fight with
abilities on a hotbar, and the rest of the party follows and fights on its own. It is a launcher setting, **Play
Style: Action** (the original game is **Classic**). It changes how you control the game, not the game's data: saves,
maps and mods are the same in both styles, and a save can be carried between them.

The development app is **Dungeon Siege Action** (`./install.sh --as-action`, branch `action-mode`). It starts in
Action, has its own data folder if you give it one (`--data-dir`), and updates from the `action-mode` branch.

## Controls

| | Keyboard and mouse | Controller |
|---|---|---|
| Walk | W A S D (relative to the camera) | Left stick |
| Attack, talk, pick up | Left click (as in the original) | A: the nearest enemy ahead, else the nearest person or thing ahead |
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
| Pointer | Mouse | Right stick (click it to click) |
| Options | Esc | Menu |
| Pause | P (or Pause) | |

In menus, the inventory, shops and the front end, a controller moves the pointer with either stick, A clicks, X
right-clicks and B goes back. Keys held with Ctrl or Alt keep the game's own meaning (Alt+W A S D for orders, Ctrl+S to
save, Ctrl+1-0 to store weapon sets); weapon sets 5-0 are still recalled with their number keys.

## The party

You play the **selected** hero (with several selected, the first one; as soon as you start walking, only that one).
It acts only on your orders: the engine's own jobs for it that would walk it somewhere or start a fight on its own
(chasing an enemy it saw, picking a target) are turned down, so nothing happens unless you do it. Its other jobs go
on as usual (it still drinks potions, gets up, dies), and nothing about the hero itself changes, so saves are the same
as in Classic. Every other party member gets the game's **Guard** order on you: they follow you and fight what attacks
you, with their own AI. Select another hero (a portrait, `,` `.`, or the D-pad) and you play that one instead; the
others guard the new hero.

Gold and potions lying within a few metres are picked up when your hero stands idle with no enemy near (other loot
stays where it is: Z collects all of it, as in the original).

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

## How it works

All of it is in `recomp/runtime/win32/action.c` (game side) and `recomp/host/ds_pad.h`, `ds_hud.h` (controller and
overlay). Nothing runs with Action Mode off: every hook returns at once, so the Classic game, its saves and multiplayer
digests are unchanged.

- **Steering.** The engine only knows "move to a place", so while a direction is held the hero is given a place 2.5 m
  ahead, again every 0.15 s or when the direction changes (`GoMind::RSMove`, the same order a click gives). Positions
  in Dungeon Siege belong to terrain nodes; the camera's direction is turned into the hero's node space with the
  engine's own conversion (`GetSiegeDifference`) and the point is put on the ground with
  `SiegeEngine::AdjustPointToTerrain` (nearer points are tried where there is no ground).
- **Keys.** The game's in-game input binder hands Action Mode W A S D, Space, 1-4 and P first (only while the game's
  keys are live: never while typing in the chat box).
- **Once a frame** (at `WorldTime::Update`): pick the hero, steer, use abilities, keep the party guarding, pick up gold,
  fill the overlay's numbers.
- **Only your orders.** `GoMind::SDoJob` is asked first: for the hero you play, a move, follow, patrol, flee, guard or
  attack job that does not come from you (`ao_human`) is turned down.

## Testing

Test mode (`DS_TEST=1`) commands for a scripted controller: `stick:x,y` (left stick in percent, held until the next
one) and `pad:bits,frames` (buttons as `PAD_*` bits of `runtime/win32/w32.h`). `hudshot:<png>` saves the overlay.
`DS_ACTIONLOG=1` logs every order, hit, ability and message. `DS_ACTION_TESTPARTY=gyorn,ulora` (test mode) clones
those characters next to the hero and adds them to the party, for trying the party with a solo save. `DS_ACTION_FLIPX=1` mirrors left and right of the steering.

## Not yet

- Multiplayer: Action Mode is made for single player. Hosting works (your hero, your party); a joining player can
  steer, but its moves go through the host (about 0.2 s) and its abilities are off (their damage is the host's to
  deal).
- The abilities have no new animations of their own: the hero swings or shoots its normal attack at the first target.
