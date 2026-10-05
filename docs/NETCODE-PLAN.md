# Netcode plan: making multiplayer feel like single player for joining players

Branch: `netcode-perf`. The biggest pain point, and the first target: **a joining player clicks to move and the hero
starts about half a second later**, worse over the internet. In single player it is instant.

## How Dungeon Siege does it (read from the GOG 1.11.1 code)

- The host simulates everything. Movement exists only as short planned segments (about 0.2 s each) that the host
  sends to clients (`GoFollower::RCSendPackedUpdateToFollowers`, `…PositionUpdate…`, `…Clip…`); that is the ~5 updates
  a second measured on the wire. The host sends its clock once a second (`WorldTime::RCSetServerTime`).
- A joiner's click is only a request: `GoMind::RSMove` → `RSDoJob` packs a job request and sends it (0x5D26EB);
  nothing happens locally. The hero moves when the host's plan comes back: round trip + host frame + the plan's start
  time.
- A joiner plays segments by its own clock. Segments that arrive after their start are applied at once and the
  position **snaps** to the newest one; there is no blending (playback in `GoFollower::Update`, 0x5FBB3C; hard
  placement 0x537065).
- The joiner's clock (`RCSetServerTime` body, 0x574789): error = host − local; over 2 s it is set outright, otherwise
  clamped to ±0.5 s and a third of it applied **in one step**, with **no allowance for network delay**. So the joiner
  runs about one one-way delay behind the host (segments arrive late) and the world can jolt by up to ~0.17 s once a
  second.

## Rule for every change

Nothing changes on the wire: Windows (and Linux/Proton) players must stay compatible. Everything below changes how
the Mac *handles* the same messages, or, when the Mac hosts, *when* it sends them.

## Steps, in order

| # | Change | Who benefits | Chance | Effort |
|---|---|---|---|---|
| 0 | **Tools.** Lag simulator in the DirectPlay layer (`DP8_LAG=<ms>[,<jitter>]`, both directions); counters for click-to-move delay, late segments and snaps (`DP8_STATS`); fix the Mac-to-Mac joiner black world and the test tool that stops reading commands once a game starts. | testing | 95% | small |
| 1 | **Instant own-hero movement on Mac joiners** (the pain point): when the joiner orders a move, start walking locally at once toward the target, at the hero's own speed; when the host's plan arrives, merge into it with step 2. Movement orders only at first; attacks and other jobs keep waiting for the host. | Mac joiners | 55–65% | large |
| 2 | **Smooth corrections on Mac joiners:** when a segment is late, a position jumps, or prediction (1) differs from the host, blend the visible difference away over ~100–200 ms. Game logic keeps the true position. | Mac joiners | 85% | medium |
| 3 | **Latency-aware clock on Mac joiners:** add half the measured round trip to the host's time and slew gently every frame instead of one jolt a second. | Mac joiners | 80% | small–medium |
| 4 | **Mac host serves better time:** sync each joiner several times a second, each with its own delay added. | all joiners, Windows too | 70% | medium |
| 5 | **Host plans further ahead** (research first): segments generated earlier so they reach joiners before they start. | all joiners | 40–50% | large |

Steps 1–3 belong together: prediction needs smoothing to merge cleanly, and an accurate clock makes the merge small.

## How success is measured

With the lag simulator at 0, 50, 100 and 200 ms (and over a real internet path):
- **click-to-move delay** on a Mac joiner: today about a round trip plus ~0.3–0.5 s; goal: the next frame;
- **snaps** per minute of play and their size: goal near zero visible snaps;
- **world jolts** from clock corrections: goal none;
- Windows joiners on a Mac host (step 4): fewer late segments than with a Windows host.

## Scaling

Nothing here depends on the Mac's speed: blending and prediction are per frame and scale with the frame rate (a
120 Hz Mac gets smoother blends, not different behaviour).
