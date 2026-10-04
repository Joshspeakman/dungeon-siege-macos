# Recompiler: Dungeon Siege as native arm64 code

Static recompilation of the user's own GOG 1.11.1 `DungeonSiege.exe` into C, compiled for Apple Silicon. Nothing
derived from the game is committed: `build.sh` regenerates everything locally from the executable. Installing and
playing: [docs/INSTALL.md](../docs/INSTALL.md).

```sh
recomp/build.sh "/path/to/Dungeon Siege/DungeonSiege.exe"     # -> recomp/work/full/libgame.dylib
```

## Status

| Step | State |
|---|---|
| Analysis: function discovery, jump tables, SEH and x87 census (`tools/analyze.py`, `tools/forms.py`) | done: 30,691 functions, 99.7% of `.text` (Miles DLLs 96–98%) |
| Lifter: x86-32 → C (`tools/lift.py`), runtime core (`runtime/`) | done: the whole executable compiles and links (1.4M lines of C, 26 MB arm64) |
| Verification against the original code | every instruction form + four complete routines, see below |
| Win32 platform layer (`runtime/win32`): loader, memory, heaps, threads, SEH, files, registry, USER32 window manager, GDI on Core Text, DbgHelp demangler | working |
| DirectDraw/Direct3D 7 (`runtime/win32/ddraw.c`) + Metal renderer (`../src/renderer`) + macOS host (`host/main.m`) | working |
| Miles Sound System: the game's own `Mss32.dll` 6.1c and the providers it loads (`system/mss/Mssfast.m3d`, `Mp3dec.asi`), recompiled from the user's game folder like the executable, on DirectSound/WINMM implemented over Core Audio (`runtime/win32/dsound.c`, `winmm.c`) | working; output verified against an independent decode (below) |
| Hand-written Miles replacement (`runtime/win32/miles.c`), kept as a fallback: `W32_MILES=native` | working |
| Bink video | the game's own BinkW32.dll, recompiled like Miles (movies are inside Objects.dsres; the expansion's in Expansion.dsres); its sound goes through Miles; frames copied to the primary surface are presented |
| Structured exception handling incl. `__except` blocks (landing pads in the 39 functions that use them) | working |
| App bundle + installer (`../install.sh`), launch window (`host/launcher.m`) | working |
| DirectPlay 8 client/server and TCP/IP provider (`runtime/win32/dpnet.c`, `dp8proto.c`), from Microsoft's protocol specifications; WSOCK32 host name and addresses | working between copies of the app; not yet tested against Windows (`../docs/MULTIPLAYER.md`) |

The recompiled game runs on Apple Silicon without Wine or Rosetta: intro, menus, new games, loading and saving, the
campaign, with no optimisation of the recompiled code yet.

For development: `recomp/build.sh <DungeonSiege.exe> && recomp/tools/build_app.sh recomp/work/full && recomp/run.sh
<game folder>` (borderless full screen, mouse captured; Cmd+Q quits). Test mode: `DS_TEST=1` (background window, no
capture, no launch window),
`DS_SCRIPT="@32:click:400,377;@75:shot:/tmp/a.png"` (scripted input and screenshots; also `move`, `rel`, `rclick`,
`key`, `type`, `wheel`), `DS_ARGS` (extra arguments for the game), `W32_TRACE=<file>` (every Windows
call), `W32_SEHLOG=1` (exceptions; crash addresses resolve to the original x86 instruction with `atos`), Ctrl-T /
`kill -INFO <pid>` (a snapshot report, below).

Resolution: the game's `DungeonSiege.ini` decides it; when it has none, the app writes this screen's size (in points). `DS_RESOLUTION=1920x1080` sets another one at launch. The game's video options list the
standard sizes that fit the screen, the screen's own size and its full Retina pixel size. The front-end menus are
always 800×600 (the game's design) and are shown with bars at the sides.

Mouse: macOS reports relative motion; it is added to the cursor the game re-centres every frame, so trackpads and mice
behave alike. `DS_SCRIPT` action `@t:rel:dx,dy` replays a swipe through the same path.

Launch window (`host/launcher.m`): resolution, view distance and frame rate, remembered in
`~/Games/DungeonSiegeNative/launcher.plist`; Return plays, arrows change settings. It is drawn with the game's own main
menu art (stone wall, leather plaque, wooden buttons), read at launch from the player's `Objects.dsres`, and
Copperplate, the game's UI typeface. `DS_NO_LAUNCHER=1` skips it (saved settings still apply); test mode never shows it;
`DS_LAUNCHER_SHOT=<png>` renders it to an image.

View distance (`runtime/win32/hooks.c`, `DS_DRAW_DISTANCE=<percent>`): the native build's own SeeFar. A hook in the
mood loader (x86 0x59000f; hooks are listed in `tools/lift.py` HOOKS and do nothing at default settings) scales every
area's fog distances and gives it a world frustum of the game's default 45 x 60 m times the setting. 150% matches the
SeeFar mod (same amount drawn in the same view); while the setting is above 100% an installed `sf_SeeFar*.dsres` is
hidden from the game so the two don't stack. The player's files are not changed.

Crash and hang reports (`runtime/win32/crash.c`): plain text files in `~/Games/DungeonSiegeNative/CrashReports`,
written when the game's code faults, the runtime or the macOS side crashes (signals, abort, Objective-C exceptions),
or the game stops responding — no frame and no window message for `DS_HANG_SECONDS` (default 20; 0 = off), or the
macOS UI thread stalls for 10 s. Hang reports are written while the game is still stuck (so they survive a
force-quit) and renamed `hang-recovered-*` if it continues. A report has the reason and x86 location, the game's
registers and stack, every thread's stack (recompiled functions are `f_<x86 address>`), what each thread waits for
(handles, critical sections and their owners), each thread's last 32 Windows calls, the Mac and settings, and the end
of the log. After a crash the app offers the report; a report from a session that ended badly is offered at the next
launch. Ctrl-T (`kill -INFO <pid>`) writes a snapshot report. `tools/symbolize-report.py <report>` maps its host
addresses onto exact x86 instructions (needs the build's object files). Test mode only:
`DS_CRASHTEST=fault|hang|stall|abort|hostcrash|uihang@<seconds>` exercises each path.

Audio switches: `W32_AUDIO_MUTE=1` (play silence, keep everything else), `W32_AUDIOLOG=1` (output level once per second),
`W32_AUDIO_DUMP=<file.wav>` (record the output), `W32_MILES=native` (the hand-written Miles instead of the recompiled
one), `W32_DSOUND=0` (no DirectSound: Miles falls back to waveOut, ~250 ms latency instead of ~100 ms), `W32_DSLOG=1`.

### Recompiled DLLs

`build.sh` also recompiles `Mss32.dll`, `Mssfast.m3d` and `Mp3dec.asi` from the game folder (each DLL's analysis
covers all its code sections; `MSSMIXER` holds Miles' hand-written mixers). Each image keeps its preferred base;
the loader maps them at start, binds imports between images to the exporting image's code, runs `DllMain`, and serves
`LoadLibrary`/`GetProcAddress`/`GetModuleHandle` for them. Instructions a DLL guards with a CPU check (Mp3dec's
3DNow!/MMX paths) raise `STATUS_ILLEGAL_INSTRUCTION`; `cpuid` reports a Pentium without MMX, so the plain x86/x87
paths run. The EAX providers (`Msseax*.m3d`) need hardware DirectSound3D and are left out; Miles skips them.

Check (menu music, `s_m_frontend.mp3`): the recorded output against Apple's own decode of the same file, with
`tests/audiocheck.c` — correlation 1.00000 at every point tested, constant offset (no drift: rate and pitch exact),
residual 61–64 dB below the music (16-bit and MP3-decoder rounding). The game sets one Miles preference, the
DirectSound mix-ahead (12 × 8 ms fragments); Miles mixes on its own 5 ms timer thread, ~5% of one core.

```sh
W32_MILES=native W32_MILES_SAVE=/tmp/streams DS_TEST=1 W32_AUDIO_MUTE=1 <app> ...   # saves s_m_frontend.mp3 as streamed
afconvert -f WAVE -d LEI16@44100 -c 2 /tmp/streams/s_m_frontend.mp3 /tmp/ref.wav     # Apple's decode
W32_AUDIO_DUMP=/tmp/out.wav DS_TEST=1 W32_AUDIO_MUTE=1 <app> ...                       # 40 s at the main menu
clang -O3 -o /tmp/audiocheck tests/audiocheck.c && /tmp/audiocheck /tmp/ref.wav /tmp/out.wav 12 20 28
```

Not covered yet: output devices with large IO buffers (Bluetooth); the mixer accepts slices up to 4096 frames and
moves the write cursor ahead by the largest slice seen. An indirect call into DLL code the analysis missed stops the
game with "indirect call to unknown code in <image>"; `W32_MILES=native` avoids the recompiled DLLs.

## Execution model

- **Memory:** one 4 GB host region; guest address = offset. The image sits at its preferred base `0x400000`, so all
  pointers the game stores (vtables, saves, its own structures) keep their 32-bit values.
- **Functions:** one C function per original function. Registers are C locals, written back to a per-thread context
  around calls. The guest stack is real (`call` pushes the original return address; arguments are read where the
  original reads them), so calling conventions, `this` pointers and stack-walking code behave as before.
- **Flags:** lazy — flag-setting instructions record their operands; consumers (`jcc`, `setcc`, `adc`, `sahf` …)
  compute only the flags they need.
- **x87:** eight doubles + TOP + control/status words, the precision model this game was measured to run with under
  Rosetta: double arithmetic, float rounding at 32-bit stores, `fist(p)` honouring the rounding
  mode. x87 register forms are decoded from the opcode bytes (Intel semantics), not from disassembler text.
- **Indirect calls/jumps:** an address → function table; jump tables become `switch` statements; imports are dispatched
  through `rt_import` (IAT slots hold thunk addresses, so a call through a register still lands there).
- Junk decodes (data read as code) and padding become `rt_unhandled()` traps.

## Verification

`tests/harness.py` runs original x86 code in [Unicorn](https://www.unicorn-engine.org/) and the recompiled C from
identical memory and compares all registers, the x87 stack and every writable byte. Unicorn's x87 runs at 53-bit
precision (control word `0x027f`), so the comparison is **bit-for-bit against the original under the 53-bit x87
model**, on every instruction form and on four complete routines:

- `tests/fuzz_insn.py build && tests/fuzz_insn.py run 200`: every distinct instruction form the game uses (1,424 forms,
  4,513 real instances from the executable), each on 200 random register/flag/x87/memory states — 902,600 runs. All
  integer, flag, string and memory forms and the x87 arithmetic, compare, load/store and conversion forms match.
  Unicorn is not a reference for `fprem1` (QEMU rounds it wrongly; Rosetta without the sidecar matches our
  implementation on 200,000 pairs) or for transcendentals (it uses the host math library) — see below.
- `tests/test_slice.py`: whole routines with their callees — ray/box test, oriented-box frustum culling (+3 helpers),
  quaternion SLERP and the colour accumulator — 0 mismatches in 20,000 runs each, against the full-game build.

Transcendentals (`fsin`, `fcos`, `fpatan`, …) use the host math library, so they can differ from x87 hardware in the
last bits; arithmetic, square roots, rounding and conversions follow the 53-bit model exactly.

## Known gaps

- **Start-up differential harness** (`tests/startup.py`): the game's entry point runs natively and in Unicorn on the
  same Win32 layer; registers and memory hashes are identical at every import call up to the first wait on a
  worker thread (C runtime start-up, static constructors, file/registry/GDI/USER32 work, the whole D3D device setup).
  It found and fixed: functions whose body starts before their entry point, memcpy-style jump tables, missed
  function-pointer seeds.
- **SEH:** access violations and RaiseException are dispatched through the game's handlers; "continue execution",
  "continue search" and "execute the __except block" work for `_except_handler3` frames. C++ `catch` continuations
  (`__CxxFrameHandler`) are not handled yet; the game throws C++ exceptions only in error paths.
- **Not supported:** controllers. DirectPlay sends everything reliably (the game asks for guaranteed delivery anyway),
  and only client/server sessions exist (the game uses no peer-to-peer).
- **x87 tag word** is not modelled: a stack overflow/underflow gives a stale register instead of the NaN real hardware
  produces (the game's code is not expected to do this).
- **Threads** run in parallel on real cores. `lock`-prefixed instructions are atomic; the game is told the real core
  count, which makes it choose its multi-processor locks (with a count of 1 it uses plain increments, which race).
- Indirect calls to addresses outside the function table are fatal with the address logged (then added as seeds);
  12 jump tables are bounded heuristically.
