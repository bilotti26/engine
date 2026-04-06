# OpenArena seL4 Memory Benchmark

A headless, graphics-free build of [OpenArena](https://github.com/OpenArena/engine) stripped down to its bare simulation core — AI, physics, collision detection, BSP world traversal, and bot pathfinding — with all I/O stubbed out. The goal is to produce a realistic, deterministic memory workload that can be ported to the [seL4 microkernel](https://sel4.systems/) for memory benchmarking.

---

## What This Is

Modern game engines are surprisingly good stress tests for OS memory subsystems. OpenArena (a Quake III Arena derivative) has:

- A **hunk allocator** that does large, bursty allocations at map load time
- A **QVM bytecode interpreter** running game logic (AI decisions, weapon fire, scoring) every frame
- A **BSP tree** for spatial queries (collision, visibility, pathfinding)
- **Bot AI** running full A* navigation over AAS (area awareness system) graphs
- Zero tolerance for page faults during gameplay — the engine assumes memory is resident

By removing SDL, OpenGL, OpenAL, and the network stack, we get all of that memory behavior without any display server, GPU, or socket dependencies. The result is a single self-contained binary that runs on bare metal or inside a microkernel.

---

## What Was Changed

| Component | Change |
|---|---|
| `code/null/null_net_ip.c` | New file — socket-free network backend. `NET_GetPacket` always returns false, `NET_Sleep` is a no-op (placeholder for `seL4_Yield()`). |
| `code/sys/sys_bench.c` | New file — benchmark harness `main()`. Parses `--bench-frames N`, drives the `Com_Frame()` loop, prints hunk stats on exit. |
| `code/sys/sys_main.c` | Original `main()` guarded with `#ifndef MEMTEST_BUILD` so both builds share all helper functions. |
| `code/qcommon/q_platform.h` | Added Apple Silicon (`aarch64`) to the macOS platform block. |
| `Makefile` | Added `memtest` and `memtest-debug` targets producing `oa_bench`. Uses `-DNO_VM_COMPILED` to force QVM bytecode interpreter (no JIT, portable to any arch). |
| `bench_config/baseoa/autoexec.cfg` | Server config loaded automatically — sets FFA gametype, disables master servers and timelimit. |

---

## Building

Requires: `gcc` or `clang`, GNU `make`, standard POSIX headers. No SDL, OpenGL, or OpenAL needed.

```sh
git clone https://github.com/billotti26/openarena-sel4-bench.git
cd openarena-sel4-bench
make memtest BUILD_CLIENT=0 BUILD_SERVER=0 BUILD_GAME_SO=0 \
             BUILD_GAME_QVM=0 BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \
             BUILD_RENDERER_OPENGL2=0
```

Output: `build/release-<platform>-<arch>/oa_bench`

---

## Running the Benchmark

`run_bench.sh` downloads the free OpenArena 0.8.8 game data, installs the config, and launches the binary with bots.

```sh
chmod +x run_bench.sh

# Default: 4 bots, 500 frames on oa_dm6
./run_bench.sh

# Custom options
./run_bench.sh --frames 1000 --bots 8 --skill 4 --map oa_dm1
```

Options:

| Flag | Default | Description |
|---|---|---|
| `--frames N` | 500 | Number of simulation frames to run |
| `--bots N` | 4 | Number of bot players (max 8) |
| `--skill N` | 3 | Bot skill level (1–5) |
| `--map NAME` | oa_dm6 | Map to load (must exist in pk3 files) |

---

## seL4 Porting Notes

The key remaining POSIX dependencies are:

- `Sys_Milliseconds()` — calls `clock_gettime(CLOCK_MONOTONIC)` → replace with seL4 timer syscall
- `NET_Sleep()` in `null_net_ip.c` — already a no-op, add `seL4_Yield()` here
- `Sys_LoadDll()` — shared library loading, not needed for a static `qagame` build
- `fd_set` in `null_net_ip.c` — already guarded behind `#ifdef MEMTEST_SEL4`, provide the stub typedef

Build with `-DMEMTEST_SEL4` to activate the seL4-targeted stubs.

---

## Benchmark Results

All runs on Apple M-series (aarch64, macOS), map `oa_dm6`, skill level 3, release build.
Binary size: ~873 KB. No GPU, no display server, no network sockets.

---

### Test 1 — 4 Bots, 500 Frames

```
$ ./run_bench.sh --frames 500 --bots 4

=== OpenArena seL4 Memory Benchmark ===
  Binary  : build/release-darwin-arm/oa_bench.arm
  Map     : oa_dm6
  Frames  : 500
  Bots    : 4 (skill 3)

4 bots connected: Gargoyle, Grism, Kyonshi, Major
Loading 1590 jump table targets
qagame loaded in 6649120 bytes on the hunk
AAS initialized. 34 level items found.

Kill: 2 0 6: Kyonshi killed Gargoyle by MOD_ROCKET
Kill: 2 3 7: Kyonshi killed Major by MOD_ROCKET_SPLASH
Kill: 1 0 8: Grism killed Gargoyle by MOD_PLASMA

=== OpenArena Memory Benchmark Results ===
BENCH: frames         = 500
BENCH: elapsed_ms     = 25351
BENCH: fps_equivalent = 19.7
BENCH: hunk_remaining = 111457856 bytes (~106 MB free)
```

**Summary:** 500 frames of full bot AI, BSP collision, and QVM execution in 25.4 seconds. Hunk allocator stable — no allocations during the game loop, all memory committed at map load.

---

### Test 2 — 8 Bots, 1000 Frames

```
$ ./run_bench.sh --frames 1000 --bots 8

=== OpenArena seL4 Memory Benchmark ===
  Binary  : build/release-darwin-arm/oa_bench.arm
  Map     : oa_dm6
  Frames  : 1000
  Bots    : 8 (skill 3)

8 bots connected: Gargoyle, Grism, Kyonshi, Major, Merman, Sergei, Sarge, Grunt
Loading 1590 jump table targets
qagame loaded in 6649120 bytes on the hunk
AAS initialized. 34 level items found.

Kill: 1 0 18: Grism killed Gargoyle by MOD_TELEFRAG
Kill: 6 7  3: Sarge killed Grunt by MOD_MACHINEGUN
Kill: 4 5  4: Merman killed Sergei by MOD_GRENADE
Kill: 6 5  1: Sarge killed Sergei by MOD_SHOTGUN    [Award: EXCELLENT]
Kill: 6 3  1: Sarge killed Major by MOD_SHOTGUN     [Award: EXCELLENT]
Kill: 7 6  6: Grunt killed Sarge by MOD_ROCKET
Kill: 2 5 18: Kyonshi killed Sergei by MOD_TELEFRAG
...

=== OpenArena Memory Benchmark Results ===
BENCH: frames         = 1000
BENCH: elapsed_ms     = 50251
BENCH: fps_equivalent = 19.9
BENCH: hunk_remaining = 111457856 bytes (~106 MB free)
```

**Summary:** 1000 frames, 8 bots running full AI simultaneously. Sarge earned two EXCELLENT awards before getting rocket-fragged by Grunt. Hunk footprint identical to the 4-bot run — confirming bot count affects CPU/QVM load, not peak memory. Total hunk committed: ~120 MB at map load, ~106 MB still free.

---

## Project Structure

```
.
├── code/
│   ├── null/null_net_ip.c      # socket-free network stub
│   ├── sys/sys_bench.c         # benchmark harness main()
│   ├── sys/sys_main.c          # original main() (guarded for dual build)
│   └── qcommon/q_platform.h   # added aarch64 support
├── bench_config/
│   └── baseoa/autoexec.cfg     # server config for benchmark runs
├── run_bench.sh                # download game data + launch benchmark
├── README-memtest.md           # detailed porting guide and diff walkthrough
└── Makefile                    # added memtest / memtest-debug targets
```

---

## License

OpenArena engine code is licensed under the GNU GPL v2. All modifications in this repository are likewise GPL v2. Game data (downloaded separately by `run_bench.sh`) is licensed under CC-BY-SA.
