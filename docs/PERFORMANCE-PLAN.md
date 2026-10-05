# Performance plan: the flattest maximum frame rate, from the slowest supported Mac up

Branch: `netcode-perf`. Floor: a base M1 MacBook Air (8 GB, 7-core GPU), which has the same CPU cores as an M1 Pro
but half the GPU and less memory bandwidth. Goal: a flat frame rate (no spikes) at the display's best rate, whatever
is on screen.

## Rule for every change: scale up, never cap

Newer and faster Macs must simply get more:
- frame-rate targets follow the display (60, 120 or whatever ProMotion offers), never a fixed number;
- thread counts and work splitting follow the cores present;
- quality reductions (render scale, view distance) are a **safety net** that switches on only when a target is missed,
  so a fast Mac runs at full quality;
- defaults are chosen from the hardware at first launch and can be changed in the launcher.

## What was measured (forest scene, M1 Pro, `Farther` view distance, uncapped)

- game thread busy ~3.6 ms per frame, render thread ~5 ms (much of it per-draw state lookups through Objective-C
  dictionaries), ~650 draw calls per frame;
- in test mode the background window is paced by the window system (~65 fps), so GPU time is still unknown: step 1
  measures it.

## Steps

| # | Work | Effect | Chance |
|---|---|---|---|
| 1 | **Measure:** GPU time per frame (Metal timestamps), game/render thread times, a frame-time histogram and spike log, a foreground benchmark mode, fixed scenes (forest, town, a large fight, Yesterhaven). | decides which steps below matter on low-end Macs | 95% |
| 2 | **Hitches:** shader variants compiled ahead (Metal binary archive), texture uploads off the critical path, world streaming checked for stalls. | "flat": no spikes | 70% |
| 3 | **Renderer CPU:** fixed tables instead of dictionary lookups, fewer state changes. | render thread time roughly halved | 85% |
| 4 | **Renderer GPU:** specialised shaders instead of one general one; alpha-tested foliage drawn so Apple's hidden-surface removal still works (depth pre-pass); compressed textures. | large gains in foliage-heavy scenes on weak GPUs | 60–75% each |
| 5 | **Dynamic resolution** with MetalFX upscaling, only when the target is missed. | the safety net for weak GPUs | 85% |
| 6 | **Game thread in big fights:** profile large battles; hottest maths routines rewritten natively and checked against the original with differential tests; recompiler improvements that speed up all game code. | large fights stop dragging the frame rate | 70% (for −20–40%) |
| 7 | **Hardware-based defaults** in the launcher (render scale, view distance, frame cap). | good results without tinkering | 90% |

Overall: a flat 60 fps on a base M1 everywhere, big fights included, about 65% at native resolution after steps 2–4
and 6, about 85% with step 5. A flat 120 on an M1 Pro: about 50%, limited mostly by the game thread in large fights.

## Reinstalling

Everything here is code in this repository; settings it adds are defaults the app chooses itself. After a reinstall,
`install.sh` (see [INSTALL.md](INSTALL.md)) rebuilds the same app; nothing needs redoing by hand.
