# The Dungeon Siege Benchmark

Gas Powered Games released the **Dungeon Siege Benchmark** in 2001: a self-playing, three-minute rolling demo on its
own map (`benchmark_demo`) with AI active and the world streaming in, which writes every frame's time to a log. The
native build runs it as an option in the launch window, and as a tool for tuning the recompiled game and its renderer.

## Install

You need GPG's installer, `DSBenchmark.EXE` (5,319,358 bytes), a free download from the time (it's still offered by
hardware sites such as [Guru3D](https://www.guru3d.com/download/dungeon-siege-pc-benchmark/)). Nothing from it is in this
repository, and since no site allows automated downloads of it, the installer can't fetch it for you.

```sh
./install.sh --benchmark ~/Downloads/DSBenchmark.EXE
```

Only its two archives are taken out (`recomp/tools/extract_installer.py`; the installer is not run):
`Resources/Benchmark.dsres` and `Maps/BenchmarkMap.dsmap`. They go to `~/Games/DungeonSiegeNative/benchmark`, apart
from the mods: they change the game's content, and so its identity in multiplayer, so they are linked into the game's
view only for a benchmark run and removed afterwards.

## Run it

Choose **Benchmark** in the launch window's Game row and press Play. The demo plays itself and the game quits when it
ends (Escape ends it early, without results). It runs:

- with your Resolution, View Distance, Shadow Detail and Shadow Edges settings;
- without mods, frame cap or vertical sync, so it shows what the Mac can do.

A summary appears afterwards: the average, median and 1% low frame rate (the frame rate that 99% of frames beat) and
the slowest frame. Each run is kept in `~/Games/DungeonSiegeNative/Benchmarks`, with the settings it ran with and
every frame's time.

## For tuning

`recomp/tools/benchmark.sh` runs it in the background with the build in `recomp/work/full`, as often as asked, and
prints each run's summary; settings to compare are given as environment:

```sh
cd recomp
tools/benchmark.sh --runs 3
DS_RESOLUTION=1728x1117 DS_DRAW_DISTANCE=200 DS_SHADOW_RESOLUTION=1024 DSR_SHADOW_FILTER=softer tools/benchmark.sh
```

Full results go to `recomp/work/benchmarks`. The 1% low and the slowest frame show hitches (the world streaming in)
that an average hides.
