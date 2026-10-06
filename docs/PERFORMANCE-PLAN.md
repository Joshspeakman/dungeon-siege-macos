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

## Done so far (branch `netcode-perf`)

- **Measuring:** test mode's once-a-second line gives GPU time per frame (average and max), the longest frame and the
  frames over 25 ms; `DSR_PSOLOG=1` logs pipeline compiles. Forest scene, 1024x768, M1 Pro: GPU 2.75 ms per frame.
- **Renderer CPU:** C hash tables instead of Objective-C dictionaries, uniforms copied once, unchanged encoder state
  skipped: render-command time about halved.
- **Renderer GPU:** draws without alpha testing use a shader without `discard`, so hidden-surface removal works for
  them: GPU time -13% (3.15 -> 2.75 ms), peaks 5.7 -> 4.1 ms; images identical.
- **Hitches:** pipeline compiles measured at 0.2 ms or less with the system shader cache: not a hitch source, so no
  binary archive is needed.
- **Game thread (step 6):** game-code CPU per frame in the forest benchmark ~6.0 -> ~3.4 ms:
  - the host's rounding mode follows the game's x87 rounding control (the game mostly rounds toward zero), which made
    the float rounding exact and removed per-operation corrections and library calls (`nextafterf`, `fesetround`);
  - `flags_set` (after every x87 compare) computes only the flags it keeps and is inlined;
  - the quaternion rotation at the heart of character animation (a quarter of the game thread) runs natively, 10x
    cheaper, checked bit for bit against the original (`DS_NATIVE_CHECK=1`).
  Every change was checked: random-operation tests against the hardware, the instruction fuzzer, the differential
  check, and the multiplayer digest (still equal to the PC's). Next candidates: the rest of character skinning
  (`0x693f6f`, `0x695c99`, slerp `0x694970`), or keeping the x87 stack in registers in the lifter.
- **Benchmark hitches (2026-10-06, GPG's benchmark demo, 1728x1117 uncapped, M1 Pro):** two kinds, both in the game
  thread (the GPU stays at 2-5 ms per frame throughout):
  - *One-off stalls* when the demo reaches new ground (about 35 s in: 170-280 ms; smaller ones at 129, 142 and
    147 s). A CPU sample at that moment shows the game reading the new content: its gas/Fuel parser (a recursive
    walk, `0x448f59`) and the archive data it decompresses on first touch, which arrives as page faults. Part of each
    fault's cost was our own check that the fault came from recompiled code (`dladdr`, a symbol search, twice per
    fault: more than the decompression itself); the answer is now cached per code address (`seh.c`). The rest is the
    original engine's own loading work on its main thread.
  - *The final battle* (the last 30 s, 25-35 ms frames, which sets the 1% low): about half the game thread is the world
    update and half the render pass; character skinning is the largest single part (`0x695c99` 13%, `0x693f6f` with
    the native slerp `0x694970` about 10%). Inside `0x695c99` the time is its two vertex-lighting loops (each vertex's
    normal against a light, its colour added or taken away): these now run natively (`native.c`, hooks `0x6960b1`,
    `0x696139`), bit for bit (`DS_NATIVE_CHECK=1`: 1.76 million loop runs, 300 million vertices over the whole demo,
    no difference; the colour routines also against the original x86 code in an emulator, 400,000 random cases).
    Final battle 42 -> 49 fps (slowest second 34 -> 42); whole demo 139 -> 151 fps average, 1% low 38 -> 45.
- **Dynamic resolution (step 5): not needed.** At this M1 Pro's native 1728x1117 the GPU takes 4.3 ms per frame
  (forest, 150% or 300% view distance; single-frame peaks 7-13 ms). A base M1 has ~2.3x less GPU and a smaller
  screen (1440x900), so about 7 ms against a 60 Hz display's 16.7 ms; the 120 Hz (ProMotion) Macs have Pro/Max/Ultra
  GPUs. Revisit only if a scene measures over budget.
- **Defaults:** view distance starts at Very Far (200%) on Max and Ultra chips, Farther (150%) elsewhere; the frame
  rate already follows the display.

## Reinstalling

Everything here is code in this repository; settings it adds are defaults the app chooses itself. After a reinstall,
`install.sh` (see [INSTALL.md](INSTALL.md)) rebuilds the same app; nothing needs redoing by hand.
