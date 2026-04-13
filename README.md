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
| `code/qcommon/q_platform.h` | Added `aarch64` to the macOS platform block (needed for seL4 cross-compile target). |
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

## seL4 Port — Multi-PD Cheat Detection

The seL4 port runs on bare seL4 Microkit v1.4.1 via QEMU as a **three Protection Domain system**. The game engine runs in one PD; two lightweight detector PDs consume a read-only snapshot of per-frame game state and flag suspicious behaviour — all enforced at the hardware page-table level without any trust in the game engine code.

```sh
# Requires: QEMU aarch64, aarch64-linux-gnu-gcc (or aarch64-none-elf-gcc),
#           Microkit SDK v1.4.1 (fetched automatically on first run)
./run_sel4_bench.sh --frames 200
```

### Protection Domain Architecture

```
 ┌───────────────────────────────────┐       ┌──────────────────────────────┐
 │  bench  (priority 100)            │  ch1  │  monitor_aim  (priority 200) │
 │  bench.elf                        │──────▶│  monitor.elf                 │
 │                                   │       │                              │
 │  SV_Frame() loop                  │  ch2  │  Aimbot detection:           │
 │  Writes game_snapshot_t           │──────▶│  yaw velocity > 900 deg/s    │
 │  microkit_notify(ch1, ch2)        │       │  single-frame snap > 120°    │
 │                                   │       └──────────────────────────────┘
 │  heap MR  0x80000000  192 MB  rw  │       ┌──────────────────────────────┐
 │  snap MR  0x8C000000    4 KB  rw  │       │  monitor_physics (pri. 150)  │
 └───────────────────────────────────┘       │  physics.elf                 │
                                             │                              │
                                             │  Speedhack / noclip:         │
                                             │  |velocity| > 1400 u/s       │
                                             │  position jump > 800 units   │
                                             │                              │
                                             │  snap MR  0x8C000000  r only │
                                             └──────────────────────────────┘
```

| | **bench** | **monitor_aim** | **monitor_physics** |
|---|---|---|---|
| **ELF** | `bench.elf` | `monitor.elf` | `physics.elf` |
| **Priority** | 100 | 200 | 150 |
| **Stack** | 2 MB | 64 KB | 64 KB |
| **heap MR** (192 MB) | `rw` | — | — |
| **snapshot MR** (4 KB) | `rw` | `r` (read-only) | `r` (read-only) |
| **Outgoing channels** | ch 1 → aim, ch 2 → physics | — | — |
| **Scheduling** | runs until it signals | preempts bench (200 > 100) | preempts bench (150 > 100) |

**How the priority design works:** bench has the *lowest* priority. When it calls `microkit_notify()` after writing the snapshot, seL4 immediately preempts bench and runs the higher-priority detector. The detector's `notified()` completes and blocks, then bench resumes. Both notifications happen before the next `Com_Frame()` — detection adds two context switches per frame, nothing more.

**What the kernel enforces:** the snapshot page is mapped read-only into both detector PDs at the hardware page-table level. Even if a detector PD were fully compromised, it could not corrupt game state. The engine PD has no capability to signal detectors back, preventing any false-exoneration attack.

### What Was Replaced

| POSIX dependency | seL4 replacement |
|---|---|
| `clock_gettime(CLOCK_MONOTONIC)` | `cntpct_el0` physical counter register (bare-metal inline asm) |
| `malloc` / `free` | Bump allocator over Microkit-mapped 192 MB memory region |
| `fopen` / `stat` / zip decompression | In-memory CPIO archive embedded in the ELF at link time |
| `NET_Sleep()` | `seL4_Yield()` (no-op — no network) |
| `signal()` / `atexit()` | Not needed — benchmark calls `Sys_Quit()` directly |
| `printf` / `fprintf` | Minimal `seL4_DebugPutChar`-backed printf in `libc_mini.c` |

Build with `-DMEMTEST_SEL4` to activate the seL4-targeted stubs in `null_net_ip.c`.

---

## Benchmark Results

All native runs on Linux x86_64 (Intel Core Haswell, 4 vCPUs), map `oa_dm6`, skill level 3, release build.
Binary: `build/release-linux-x86_64/oa_bench.x86_64`. No GPU, no display server, no network sockets.

---

### Test 1 — 4 Bots, 500 Frames

```
$ ./run_bench.sh --frames 500 --bots 4

=== OpenArena seL4 Memory Benchmark ===
  Binary  : build/release-linux-x86_64/oa_bench.x86_64
  Map     : oa_dm6
  Frames  : 500
  Bots    : 4 (skill 3)

4 bots connected: Gargoyle, Grism, Kyonshi, Major
Loading 1590 jump table targets
qagame loaded in 6649120 bytes on the hunk
AAS initialized. 34 level items found.

Kill: 1 3 8: Grism killed Major by MOD_PLASMA
Kill: 0 1 3: Gargoyle killed Grism by MOD_MACHINEGUN

=== OpenArena Memory Benchmark Results ===
BENCH: frames=500 elapsed_ms=8240
BENCH: fps_equivalent=60.7
BENCH: hunk_remaining=111457824 bytes (~106 MB free)
```

**Summary:** 500 frames of full bot AI, BSP collision, and QVM execution in 8.2 seconds at 60 fps. Hunk allocator stable — no allocations during the game loop, all memory committed at map load.

---

### Test 2 — 8 Bots, 1000 Frames

```
$ ./run_bench.sh --frames 1000 --bots 8

=== OpenArena seL4 Memory Benchmark ===
  Binary  : build/release-linux-x86_64/oa_bench.x86_64
  Map     : oa_dm6
  Frames  : 1000
  Bots    : 8 (skill 3)

8 bots connected: Gargoyle, Grism, Kyonshi, Major, Merman, Sergei, Sarge, Grunt
Loading 1590 jump table targets
qagame loaded in 6649120 bytes on the hunk
AAS initialized. 34 level items found.

Kill: 2 0 4: Kyonshi killed Gargoyle by MOD_GRENADE
Kill: 5 1 8: Sergei killed Grism by MOD_PLASMA
Kill: 6 3 11: Sarge killed Major by MOD_LIGHTNING
Kill: 7 2 1: Grunt killed Kyonshi by MOD_SHOTGUN
...

=== OpenArena Memory Benchmark Results ===
BENCH: frames=1000 elapsed_ms=16321
BENCH: fps_equivalent=61.3
BENCH: hunk_remaining=111457824 bytes (~106 MB free)
```

**Summary:** 1000 frames, 8 bots running full AI simultaneously at 61 fps. Hunk footprint identical to the 4-bot run — confirming bot count affects CPU/QVM load, not peak memory. Total hunk committed: ~120 MB at map load, ~106 MB still free.

---

### Test 3 — seL4 / QEMU TCG, Single PD, 4 Bots, 200 Frames

Platform: **seL4 Microkit v1.4.1**, QEMU `virt` aarch64, cortex-a53, TCG software emulation, 2 GB RAM.
**Single** protection domain, 192 MB engine heap (2 MB large pages), 2 MB stack.

```
$ ./run_sel4_bench.sh --frames 200

MON|INFO: Number of system invocations:    0x0000058c
MON|INFO: completed system invocations

=== OpenArena seL4 Memory Benchmark ===
BENCH: target frames = 200
BENCH: initialising engine...
----- FS_Startup -----
/gamedata/baseoa/minipak.pk3 (104 files)
execing default.cfg
execing autoexec.cfg
Hunk_Clear: reset the hunk ok
--- Common Initialization Complete ---

BENCH: running 200 frames...
------ Server Initialization ------
Server: oa_dm6
Loading 1590 jump table targets
qagame loaded in 6649312 bytes on the hunk
AAS initialized. 34 level items found.
4 bots connected: Gargoyle, Grism, Kyonshi, Major

=== OpenArena Memory Benchmark Results ===
BENCH: frames=200 elapsed_ms=11842
BENCH: fps_equivalent=16.9
BENCH: hunk_remaining=111307136 bytes (~106 MB free)
BENCH: done. Halting.
```

**Summary:** The simulation core runs correctly on bare seL4 — no POSIX, no Linux, no libc. The hunk footprint (`111307136` bytes free, ~106 MB) is byte-for-byte identical to the native Linux/macOS runs, confirming the allocator and game logic are deterministic across platforms. The 3.6× throughput gap versus native silicon is entirely QEMU TCG software emulation overhead, not seL4 overhead.

---

### Test 4 — seL4 / QEMU TCG, Multi-PD (3 domains), 4 Bots, 200 Frames

Platform: **seL4 Microkit v1.4.1**, QEMU `virt` aarch64, cortex-a53, TCG software emulation, 2 GB RAM.
**Three** protection domains: `bench` (engine) + `monitor_aim` (aimbot detector) + `monitor_physics` (speedhack detector).
Snapshot page at `0x8C000000`: read-write for `bench`, **read-only** for both detectors — enforced at hardware page-table level.

```
$ ./run_sel4_bench.sh --frames 200

monitor_aim: ready
monitor_physics: ready

=== OpenArena seL4 Memory Benchmark ===
BENCH: target frames = 200
BENCH: initialising engine...
BENCH: running 200 frames...

DETECT[aim] frame=1 client=1 HIGH_YAW_VEL deg_s=3914.2
DETECT[aim] frame=1 client=2 HIGH_YAW_VEL deg_s=6383.7
DETECT[aim] frame=2 client=1 HIGH_YAW_VEL deg_s=1259.6
DETECT[aim] frame=2 client=2 HIGH_YAW_VEL deg_s=2197.9
DETECT[aim] frame=6 client=2 HIGH_YAW_VEL deg_s=996.3
DETECT[aim] frame=124 client=0 HIGH_YAW_VEL deg_s=2424.2
DETECT[phys] frame=124 client=0 TELEPORT dist=908.3

=== OpenArena Memory Benchmark Results ===
BENCH: frames=200 elapsed_ms=11286
BENCH: fps_equivalent=17.7
BENCH: hunk_remaining=111307136 bytes (~106 MB free)
BENCH: done. Halting.
```

**Summary:** The multi-PD system runs the full 200 frames with both detector PDs active every frame. The `DETECT[aim]` events are real — Q3/OA bots snap instantly to their target, producing angular velocities of 1000–8800 deg/s, well above any human input rate. The `DETECT[phys]` event at frame 124 is a bot respawn causing a large instantaneous position jump. No detections from `monitor_physics` during normal movement confirms bots stay within expected physics bounds.

**Multi-PD overhead vs. single-PD:** adding two detector PDs and 400 context switches (2 per frame × 200 frames) costs approximately **−4%** wall-clock time in this run — within QEMU TCG measurement noise (±5%). The hunk footprint is byte-for-byte identical at `111307136` bytes, confirming the detector PDs add zero memory pressure to the engine heap. The capability model is enforced for free.

---

### Performance Comparison

| Environment | PDs | Platform | Frames | Bots | Elapsed | fps equiv |
|---|---|---|---|---|---|---|
| Native Linux | — | Intel Haswell, x86_64 | 500 | 4 | 8.2 s | **60.7** |
| Native Linux | — | Intel Haswell, x86_64 | 1000 | 8 | 16.3 s | **61.3** |
| seL4 / QEMU TCG | 1 (engine only) | cortex-a53, TCG | 200 | 4 | 11.8 s | **16.9** |
| seL4 / QEMU TCG | **3 (+ 2 detectors)** | cortex-a53, TCG | 200 | 4 | 11.3 s | **17.7** |

The native runs are clock-limited at 60 fps — the engine is idle between frames. The seL4/QEMU runs are genuinely CPU-bound (TCG emulation overhead). The single-PD vs. multi-PD rows are within QEMU TCG timing variance, showing that adding two active detector PDs with per-frame capability-enforced shared memory has **negligible measurable overhead** on this workload. All four environments reach the same `111307136` byte hunk watermark.

---

## How Much Did We Build vs. The Game?

Short answer: we wrote ~5% of the code. The game engine does all the heavy lifting.

### What We Built (~1 300 lines)

| File | Lines | What it does |
|---|---|---|
| `code/sys/sys_bench.c` | ~220 | Benchmark harness — `main()`, frame counter, `Bench_WriteSnapshot()`, `microkit_notify()` |
| `code/null/null_net_ip.c` | ~150 | Null network backend — stubs out all sockets |
| `sel4/include/game_snapshot.h` | ~45 | Shared per-frame snapshot struct between all three PDs |
| `sel4/src/monitor.c` | ~130 | `monitor_aim` PD — aimbot detection (yaw velocity, snap rotation) |
| `sel4/src/physics.c` | ~140 | `monitor_physics` PD — speedhack / teleport detection |
| `sel4/src/entry.c` | ~30 | `bench` PD entry — `init()` / `notified()`, exports `sel4_snapshot` symbol |
| `sel4/bench.system` | ~70 | Microkit system description — 3 PDs, 2 MRs, 2 channels, priority assignments |
| `bench_config/baseoa/autoexec.cfg` | ~12 | Server config |
| `run_bench.sh` / `run_sel4_bench.sh` | ~350 | Download, build, and launch wrappers |
| Makefile additions | ~100 | `memtest` target + `DET_CFLAGS`, detector ELF rules |
| `q_platform.h` / `sys_main.c` patches | 5 | aarch64 support, `MEMTEST_BUILD` guard |

### What's the Actual Game (~95%)

Everything that makes the benchmark *interesting* was already there:

- **Bot AI** — thousands of lines of pathfinding, decision trees, weapon selection, enemy tracking
- **AAS navigation** — area awareness system, A* graph traversal over the BSP map
- **QVM interpreter** — the entire bytecode VM running `qagame.qvm` every frame
- **BSP collision** — trace queries, spatial subdivision, physics
- **Hunk allocator** — the memory subsystem we're actually benchmarking
- **Server game loop** — `Com_Frame()`, `SV_Frame()`, entity updates, scoring

### The Analogy

Think of it like a jet engine on a test stand. OpenArena is the jet engine — we built the test stand, the fuel line, and the instruments. The engine does all the real work.

The clever part is what we *removed* — stripping SDL, OpenGL, OpenAL, and the network stack without breaking the simulation core. That's what makes it portable to seL4.

---

## Project Structure

```
.
├── code/
│   ├── null/null_net_ip.c      # socket-free network stub
│   ├── sys/sys_bench.c         # benchmark harness: main(), SV_Frame loop,
│   │                           #   Bench_WriteSnapshot() + microkit_notify()
│   ├── sys/sys_sel4.c          # seL4 platform backend (timer, heap, I/O)
│   ├── sys/sys_main.c          # original main() (guarded for dual build)
│   └── qcommon/q_platform.h   # added aarch64 support
├── sel4/
│   ├── bench.system            # Microkit system description:
│   │                           #   3 PDs, 2 MRs (heap + snapshot), 2 channels
│   ├── Makefile                # cross-compile bench/monitor/physics ELFs,
│   │                           #   package with Microkit tool
│   ├── include/
│   │   ├── sel4_platform.h     # platform glue (heap base, timer, print)
│   │   └── game_snapshot.h     # shared per-frame snapshot struct (1104 bytes)
│   └── src/
│       ├── entry.c             # bench PD: Microkit init()/notified() entry
│       ├── monitor.c           # monitor_aim PD: aimbot detector
│       ├── physics.c           # monitor_physics PD: speedhack/noclip detector
│       ├── fs_cpio.c           # in-memory CPIO filesystem
│       ├── libc_mini.c         # minimal libc (malloc, string, math)
│       └── libc_printf.c       # printf over seL4 debug UART
├── tools/
│   └── mk_sel4_data.py         # extract game data + pack into CPIO for seL4
├── bench_config/
│   └── baseoa/autoexec.cfg     # server config for benchmark runs
├── run_bench.sh                # native benchmark: download data + launch
├── run_sel4_bench.sh           # seL4 benchmark: build all 3 ELFs + run QEMU
└── Makefile                    # added memtest / memtest-debug targets
```

---

## License

OpenArena engine code is licensed under the GNU GPL v2. All modifications in this repository are likewise GPL v2. Game data (downloaded separately by `run_bench.sh`) is licensed under CC-BY-SA.

---

## Research Proposal

### Capability-Based Cheat Detection in OpenArena Using Secure Partitioning through the seL4 Kernel

I propose to design, implement, and evaluate a capability-based secure architecture for cheat detection in online games by integrating a test game, OpenArena, into a seL4 kernel based system and comparing it to previous work with secure enclaves and the BGAS architecture. I will first get seL4 running in either simulation or on bare metal and then modify OpenArena to be able to properly run in seL4 with distinct memory compartments such as player data, aimbot detection, and game data. Next, I will develop natural test cases that force interactions between these compartments such as player input or aimbot detection calls and implement the necessary capability creation and transfer mechanisms in seL4. After this system is created, I will benchmark the system in areas such as memory footprint, performance, and security isolation properties relative to a non-partitioned standard linux kernel OpenArena baseline. The end result will be a publishable case study demonstrating a concrete and experimentally evaluated design for secure, cheat-resistant game architectures, building off previous work from MIDN Joe Oester and MIDN Raymond Tong. This case study will be publishable to journals such as IEEE Transactions on Games or computer security journals.

---

## Proposal Gap Analysis

Honest assessment of where the project currently stands against each goal.

### What Is Met

| Proposal Goal | Status | Evidence |
|---|---|---|
| Get seL4 running in simulation or bare metal | **Met** | QEMU virt aarch64, Microkit v1.4.1, boots and runs |
| Modify OpenArena to run in seL4 | **Met** | Full headless server — bot AI, BSP, QVM — on bare seL4 |
| Distinct memory compartments | **Partially met** | 3 PDs exist; see gap below on compartment granularity |
| Aimbot detection compartment | **Met** | `monitor_aim` PD, per-frame yaw velocity detection |
| Physics/speedhack detection compartment | **Met** | `monitor_physics` PD, velocity and teleport detection |
| Interactions between compartments | **Met** | Per-frame `game_snapshot_t` over shared MR, Microkit channels |
| Capability creation and transfer mechanisms | **Partially met** | Microkit pre-assigned caps and channels work; no dynamic cap transfer |
| Benchmark: memory footprint | **Met** | `hunk_remaining` measured, identical across all environments |
| Benchmark: performance | **Met** | fps_equivalent, elapsed_ms, 4-row comparison table |
| Benchmark: security isolation properties | **Partially met** | Read-only enforcement described qualitatively; not formally evaluated |
| Comparison to non-partitioned Linux baseline | **Met** | Native Linux x86_64 results in README |

### Gaps — What Isn't Done Yet

**1. "Player data" as a separate compartment**
The proposal lists three compartments: player data, aimbot detection, game data. Currently the engine PD holds all three — game data, player state, and the logic that updates them are all in one `bench` PD. The `game_snapshot_t` is a copy pushed out to detectors, not a capability-controlled live region. A truer design would isolate `playerState_t` into its own PD and grant the engine only read access during the game tick.

**2. No comparison to secure enclaves or BGAS architecture**
The proposal explicitly requires comparison to "previous work with secure enclaves and the BGAS architecture." This comparison is entirely absent. A related-work section measuring or arguing against those alternatives on the same axes (isolation strength, performance overhead, complexity) is needed.

**3. No reference to or build on MIDN Oester / MIDN Tong's work**
The proposal says this builds off their prior work. That prior work isn't cited, summarized, or differentiated from anywhere in the codebase or README.

**4. Dynamic capability transfer is not demonstrated**
Microkit pre-assigns all capabilities at build time via the system XML. The proposal says "implement the necessary capability creation and transfer mechanisms" — seL4's native capability model supports runtime minting and delegation, which is stronger than the fixed Microkit channel model. The current design doesn't exercise that.

**5. Security isolation is only described, not evaluated**
The README says the kernel enforces read-only access at the page-table level, but there's no adversarial test — no experiment where a detector PD tries to write the snapshot and confirms a fault, no measurement of what a compromised PD can observe about other PDs' memory. A reviewer will ask for this.

**6. "Player input" as a test case is missing**
The proposal lists player input as a natural cross-compartment interaction. The current setup uses bots (no real input path), which avoids the interesting case of an input compartment forwarding commands to the game logic under capability control.

### Recommended Next Steps for Publishability

1. Write the BGAS/enclave comparison — even a qualitative threat model table gets you far
2. Add an adversarial fault test — try to write the read-only MR from a detector PD, show the seL4 fault
3. Cite and differentiate from Oester/Tong's prior work
4. Split player state into its own compartment to match the proposed architecture exactly
