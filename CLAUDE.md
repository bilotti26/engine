# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What This Repository Is

A fork of the OpenArena engine (ioquake3 derivative) modified to produce a **headless memory benchmark** (`oa_bench`) for eventual porting to the seL4 microkernel. All graphics (SDL/OpenGL), audio (OpenAL), input, and networking have been stubbed out. The simulation core — bot AI, BSP collision, QVM interpreter, hunk allocator — runs intact.

## Build Commands

### Benchmark binary (primary target)
```bash
make memtest BUILD_CLIENT=0 BUILD_SERVER=0 BUILD_GAME_SO=0 \
             BUILD_GAME_QVM=0 BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \
             BUILD_RENDERER_OPENGL2=0
```
Output: `build/release-<platform>-<arch>/oa_bench[.arm]`

### Debug build
```bash
make memtest-debug BUILD_CLIENT=0 BUILD_SERVER=0 BUILD_GAME_SO=0 \
                   BUILD_GAME_QVM=0 BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \
                   BUILD_RENDERER_OPENGL2=0
```

### Force recompile a single object (e.g. after a define change)
```bash
rm build/release-darwin-arm/bench/vm.o && make memtest ...same flags...
```

### Run the benchmark
```bash
./run_bench.sh --frames 500 --bots 4 --skill 3 --map oa_dm6
```
`run_bench.sh` downloads OpenArena 0.8.8 game data on first run, installs `bench_config/baseoa/autoexec.cfg`, and launches the binary.

## Key Compiler Defines

| Define | Effect |
|---|---|
| `-DDEDICATED` | Excludes client/graphics/audio code |
| `-DMEMTEST_BUILD` | Activates `sys_bench.c` main(); guards original `main()` in `sys_main.c` |
| `-DNO_VM_COMPILED` | Forces QVM bytecode interpreter — no JIT, portable to any arch |
| `-DMEMTEST_SEL4` | Stubs `fd_set` and `ntohs` in `null_net_ip.c` for seL4 cross-compile |

## Architecture

### What's included in the memtest build
- **`code/qcommon/`** — engine core: VM dispatcher (`vm.c`), bytecode interpreter (`vm_interpreted.c`), BSP collision (`cm_*.c`), hunk allocator, filesystem/pk3 loading, command system
- **`code/server/`** — server game loop (`SV_Frame`), client slot management, bot interface
- **`code/botlib/`** — bot AI: AAS pathfinding (A* over navigation graph), weapon/item scripting, movement logic (54 files)
- **`code/sys/sys_bench.c`** — benchmark harness `main()`: parses `--bench-frames N`, drives `Com_Frame()`, prints hunk stats
- **`code/null/null_net_ip.c`** — socket-free network backend: `NET_GetPacket` always returns false, `NET_Sleep` is a no-op

### What's excluded
- `code/client/`, `code/cgame/`, `code/renderer_*/` — replaced by stubs in `code/null/`
- `qcommon/net_ip.c` — replaced by `null/null_net_ip.c`
- JIT VM backends (`vm_x86.c`, `vm_aarch64.c`) — replaced by interpreter-only path via `-DNO_VM_COMPILED`

### Platform modifications
- `code/qcommon/q_platform.h` — added `__aarch64__`/`__arm64__` to the macOS platform block (Apple Silicon was missing)
- `code/sys/sys_main.c` — original `main()` wrapped in `#ifndef MEMTEST_BUILD`

### Game data
Game data lives in `gamedata/baseoa/*.pk3` (downloaded by `run_bench.sh`). The engine loads `default.cfg` (fatal if missing), then `q3config_server.cfg`, then `autoexec.cfg` from `bench_config/baseoa/`.

## Critical Runtime Flags

These must be passed at runtime — they are not set in autoexec.cfg:
- `+set vm_game 0` — force QVM interpreted mode (required; without this the engine tries to JIT-compile and exits 99)
- `+set sv_fps 60` — server tick rate (default is 20; set to 60 for the benchmark target)

## seL4 Porting — Remaining POSIX Dependencies

| File | Function | Replacement |
|---|---|---|
| `sys/sys_unix.c` | `Sys_Milliseconds()` via `clock_gettime` | seL4 timer syscall |
| `sys/sys_unix.c` | `Sys_Sleep()` | `seL4_Yield()` |
| `sys/sys_bench.c` | `signal()` handlers | seL4 fault endpoint |
| `sys/sys_bench.c` | `CON_Init/Shutdown` | Remove entirely |
| `qcommon/files.c` | `fopen`, `stat`, zip decompression | seL4 IPC-based VFS |
| `null/null_net_ip.c` | `NET_Sleep()` | `seL4_Yield()` (already no-op) |

Build with `-DMEMTEST_SEL4` to stub the remaining `fd_set`/`ntohs` usages in `null_net_ip.c`.
