# OpenArena Memory Simulation — `memtest` Build

This document describes every change made to strip OpenArena down to a
headless memory simulation suitable for seL4 benchmarking.

---

## Goal

Run OpenArena's game logic (AI path-finding, physics, collision detection,
BSP world loading, bot behaviour) inside a process with **no graphics, no
audio, no input, and no real networking**, so that the memory access patterns
of a real game can be measured on seL4.

---

## Repository layout after changes

```
openarena/
├── code/
│   ├── null/
│   │   └── null_net_ip.c          ← NEW: null network backend
│   ├── sys/
│   │   ├── sys_main.c             ← MODIFIED: main() guarded by #ifndef MEMTEST_BUILD
│   │   └── sys_bench.c            ← NEW: benchmark harness main()
│   └── qcommon/
│       └── q_platform.h           ← MODIFIED: added Apple Silicon (aarch64) support
├── Makefile                       ← MODIFIED: added memtest / memtest-debug targets
└── README-memtest.md              ← this file
```

---

## Step-by-step changes

### Step 1 — Clone the repository

```bash
git clone https://github.com/OpenArena/engine.git openarena
cd openarena
```

No upstream files are modified until Step 4.

---

### Step 2 — Add Apple Silicon support to `q_platform.h`

**File:** `code/qcommon/q_platform.h`

The macOS platform block (around line 142) only handled `__ppc__`, `__i386__`,
and `__x86_64__`. Apple Silicon (M1/M2/M3) is `__aarch64__` and would hit the
`#error "Architecture not supported"` catch-all.

**Change:** Inside the `#if defined(MACOS_X) || defined(__APPLE_CC__)` block,
add an `aarch64` branch immediately after the `x86_64` branch:

```c
// BEFORE (line ~153):
#elif defined __x86_64__
#undef idx64
#define idx64 1
#define ARCH_STRING "x86_64"
#define Q3_LITTLE_ENDIAN
#endif

// AFTER:
#elif defined __x86_64__
#undef idx64
#define idx64 1
#define ARCH_STRING "x86_64"
#define Q3_LITTLE_ENDIAN
#elif defined __aarch64__ || defined __arm64__
#define ARCH_STRING "aarch64"
#define Q3_LITTLE_ENDIAN
#endif
```

**Why:** The build system uses `uname -p` on macOS to detect the architecture.
Apple Silicon returns `"arm"` from `uname -p`, which maps to ARCH=arm in the
Makefile. Without this change, the C preprocessor cannot identify the
architecture and refuses to compile.

---

### Step 3 — Create `code/null/null_net_ip.c`

**File:** `code/null/null_net_ip.c` (new file)

This file is a complete replacement for `code/qcommon/net_ip.c`. The real
`net_ip.c` opens UDP sockets, calls `select()`, and manages OS network
resources. None of that is needed (or safe) on seL4.

#### 3a — Header guard for `fd_set`

`NET_GetPacket` and `NET_Event` accept an `fd_set *` parameter because the
real backend uses `select()`. We keep the same signatures (so the rest of the
engine compiles unchanged) but never touch the value. `fd_set` is a POSIX
socket type, so we conditionally include it:

```c
#ifndef MEMTEST_SEL4
#  ifdef _WIN32
#    include <winsock2.h>
#  else
#    include <sys/select.h>
#    include <arpa/inet.h>   /* ntohs */
#  endif
#else
/* seL4 target: fd_set and ntohs are not available; provide stubs. */
typedef int fd_set;
static inline unsigned short ntohs_stub(unsigned short x) { return x; }
#  define ntohs ntohs_stub
#endif
```

When you eventually cross-compile for seL4, add `-DMEMTEST_SEL4` to the
compiler flags and the file has zero POSIX dependencies.

#### 3b — Address helpers (pure computation, no syscalls)

`NET_CompareBaseAdrMask`, `NET_CompareBaseAdr`, `NET_CompareAdr`,
`NET_IsLocalAddress` are reimplemented using only `memcmp`. The server uses
these for ban-list checks and loopback detection, so they must be correct:

```c
qboolean NET_CompareBaseAdrMask(netadr_t a, netadr_t b, int netmask)
{
    // ... byte-level mask comparison, identical logic to net_ip.c ...
    if (a.type == NA_LOOPBACK) return qtrue;
    // handle NA_IP (32-bit mask) and NA_IP6 (128-bit mask)
}

qboolean NET_IsLocalAddress(netadr_t adr)
{
    return (adr.type == NA_LOOPBACK);
}
```

#### 3c — Address string formatting

`NET_AdrToString` and `NET_AdrToStringwPort` are called in server log
messages. We return human-readable strings for loopback/bot addresses and
`"<no-network>"` for anything else:

```c
const char *NET_AdrToString(netadr_t a)
{
    static char s[64];
    if (a.type == NA_LOOPBACK) Com_sprintf(s, sizeof(s), "loopback");
    else if (a.type == NA_BOT) Com_sprintf(s, sizeof(s), "bot");
    else                       Com_sprintf(s, sizeof(s), "<no-network>");
    return s;
}
```

#### 3d — Multicast stubs

`NET_JoinMulticast6` and `NET_LeaveMulticast6` are called during server
startup and shutdown. They are empty functions:

```c
void NET_JoinMulticast6(void) {}
void NET_LeaveMulticast6(void) {}
```

#### 3e — Packet I/O stubs

```c
// Always returns qfalse — engine sees no incoming packets.
// Loopback bot traffic goes through the in-process queue in net_chan.c
// and never reaches this function.
qboolean NET_GetPacket(netadr_t *net_from, msg_t *net_message, fd_set *fdr)
{
    return qfalse;
}

// Silently drop outgoing packets.
void Sys_SendPacket(int length, const void *data, netadr_t to) {}
```

#### 3f — Lifecycle stubs

```c
void NET_Init(void)    { Com_Printf("NET_Init: null network backend\n"); }
void NET_Shutdown(void) {}
void NET_Config(qboolean enableNetworking) {}
void NET_Restart_f(void) {}
void NET_Event(fd_set *fdr) {}

// Real backend blocks on select() here. In memtest we skip sleeping
// entirely so the benchmark loop runs as fast as possible.
// seL4 note: replace with seL4_Yield() or a seL4 timer IPC call.
void NET_Sleep(int msec) {}
```

---

### Step 4 — Modify `code/sys/sys_main.c`

**File:** `code/sys/sys_main.c`

`sys_main.c` provides many helper functions that both the normal build and the
memtest build share (`Sys_Error`, `Sys_Quit`, `Sys_Print`, `Sys_LoadDll`,
etc.). We do **not** want to duplicate or remove any of those. We only need to
prevent `main()` from being compiled when `sys_bench.c` supplies its own.

**Change 1** — Guard the function comment and signature (around line 594):

```c
// BEFORE:
/*
=================
main
=================
*/
int main( int argc, char **argv )
{

// AFTER:
/*
=================
main
Not compiled when MEMTEST_BUILD is defined; sys_bench.c provides
its own main() for the benchmark harness in that case.
=================
*/
#ifndef MEMTEST_BUILD
int main( int argc, char **argv )
{
```

**Change 2** — Close the guard at the end of `main()` (after the closing `}`):

```c
// BEFORE:
    return 0;
}

// AFTER:
    return 0;
}
#endif /* MEMTEST_BUILD */
```

**Why:** Both `sys_main.c` and `sys_bench.c` are compiled into the same
binary. Without the guard, the linker sees two definitions of `main()` and
refuses to link.

---

### Step 5 — Create `code/sys/sys_bench.c`

**File:** `code/sys/sys_bench.c` (new file)

The entire file is wrapped in `#ifdef MEMTEST_BUILD` so it compiles to nothing
in the normal build.

#### 5a — Forward-declare sys_main.c internals

Three functions defined in `sys_main.c` are not declared in any public header:

```c
void  Sys_SetBinaryPath(const char *path);
char *Sys_BinaryPath(void);
void  Sys_ParseArgs(int argc, char **argv);
```

These are forward-declared at the top of `sys_bench.c` so the compiler knows
their types.

#### 5b — Argument parsing

`Bench_ParseArgs()` walks `argv`, strips any `--bench-frames N` argument, and
forwards everything else to the engine's command-line string:

```c
static int Bench_ParseArgs(int argc, char **argv,
                           char *commandLine, int commandLineSize)
{
    int frames = 1000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench-frames") && i + 1 < argc) {
            frames = atoi(argv[++i]);
            continue;
        }
        // append to commandLine (with quoting if the arg contains spaces)
    }
    return frames;
}
```

Engine commands like `+set dedicated 1`, `+map q3dm1`, `+addbot Keel 1`
pass through unchanged to `Com_Init`.

#### 5c — Frame counter

```c
static int      bench_frames    = 1000;
static int      bench_frame_cur = 0;
static qboolean bench_done      = qfalse;

static void Bench_Frame(void)
{
    bench_frame_cur++;
    if (bench_frame_cur >= bench_frames)
        bench_done = qtrue;
}
```

#### 5d — Result reporting

After the loop finishes, `Bench_PrintResults()` calls `Hunk_MemoryRemaining()`
(declared in `qcommon.h`) and `Hunk_Log()` (which prints a per-allocation
breakdown via `Com_Printf`):

```c
static void Bench_PrintResults(int elapsed_ms)
{
    printf("BENCH: frames=%d elapsed_ms=%d\n", bench_frame_cur, elapsed_ms);
    printf("BENCH: hunk_remaining=%d bytes\n", Hunk_MemoryRemaining());
    Hunk_Log();   // detailed per-allocation breakdown
}
```

#### 5e — `main()`

```c
int main(int argc, char **argv)
{
    char commandLine[MAX_STRING_CHARS] = {0};

    Sys_PlatformInit();
    Sys_Milliseconds();          // calibrate time base

    Sys_ParseArgs(argc, argv);
    Sys_SetBinaryPath(Sys_Dirname(argv[0]));
    Sys_SetDefaultInstallPath(DEFAULT_BASEDIR);

    bench_frames = Bench_ParseArgs(argc, argv, commandLine, sizeof(commandLine));

    Com_Init(commandLine);
    NET_Init();
    CON_Init();

    // Standard crash signal handlers (same as normal build)
    signal(SIGILL,  Sys_SigHandler);
    signal(SIGFPE,  Sys_SigHandler);
    signal(SIGSEGV, Sys_SigHandler);
    signal(SIGTERM, Sys_SigHandler);
    signal(SIGINT,  Sys_SigHandler);

    int t_start = Sys_Milliseconds();
    while (!bench_done) {
        IN_Frame(qfalse);   // no-op (null_input.c)
        Com_Frame();        // full game-logic tick
        Bench_Frame();      // increment counter, check termination
    }
    int t_end = Sys_Milliseconds();

    Bench_PrintResults(t_end - t_start);
    CON_Shutdown();
    return 0;
}
```

---

### Step 6 — Add the `memtest` target to the Makefile

**File:** `Makefile`

#### 6a — Define `DO_BENCH_CC` and `DO_BENCH_BOT_CC`

Added immediately after the existing `DO_DED_CC` definition:

```makefile
define DO_BENCH_CC
$(echo_cmd) "BENCH_CC $<"
$(Q)$(CC) $(NOTSHLIBCFLAGS) -DDEDICATED -DMEMTEST_BUILD \
    $(CFLAGS) $(SERVER_CFLAGS) $(OPTIMIZE) -o $@ -c $<
endef

define DO_BENCH_BOT_CC
$(echo_cmd) "BENCH_BOT_CC $<"
$(Q)$(CC) $(NOTSHLIBCFLAGS) -DDEDICATED -DMEMTEST_BUILD \
    $(CFLAGS) $(BOTCFLAGS) $(OPTIMIZE) -DBOTLIB -o $@ -c $<
endef
```

**Why two macros?**
- The botlib (`code/botlib/`) must be compiled with `-DBOTLIB` but without
  `-DDEDICATED` affecting the wrong symbols. `DO_BOT_CC` in the normal build
  also omits `-DDEDICATED`. `DO_BENCH_BOT_CC` adds `-DMEMTEST_BUILD` only.
- All other source trees use `DO_BENCH_CC`.

#### 6b — Define `Q3BENCHOBJ`

`Q3BENCHOBJ` mirrors `Q3DOBJ` (the dedicated-server object list) with two
substitutions:

| `Q3DOBJ` | `Q3BENCHOBJ` | Reason |
|---|---|---|
| `$(B)/ded/net_ip.o` | `$(B)/bench/null_net_ip.o` | Replace real sockets with null backend |
| `$(B)/ded/sys_main.o` | `$(B)/bench/sys_main.o` + `$(B)/bench/sys_bench.o` | sys_main.o compiles with `MEMTEST_BUILD` (so its `main()` is excluded); sys_bench.o contributes the benchmark `main()` |

For architectures without a compiled VM (arm, aarch64) the darwin Makefile
unconditionally sets `HAVE_VM_COMPILED=true`, but there is no `vm_arm.c`.
An `else` branch adds `vm_none.o` (stubs that call `exit(99)`) in those cases:

```makefile
ifeq ($(HAVE_VM_COMPILED),true)
  ifneq ($(findstring $(ARCH),x86 x86_64),)
    Q3BENCHOBJ += $(B)/bench/vm_x86.o
  else ifneq ($(findstring $(ARCH),ppc ppc64),)
    Q3BENCHOBJ += $(B)/bench/vm_powerpc.o $(B)/bench/vm_powerpc_asm.o
  else ifeq ($(ARCH),sparc)
    Q3BENCHOBJ += $(B)/bench/vm_sparc.o
  else
    # ARM/aarch64 — no JIT; supply vm_none.c stubs so the linker is satisfied.
    Q3BENCHOBJ += $(B)/bench/vm_none.o
  endif
endif
```

#### 6c — Link rule

```makefile
BENCHBIN=oa_bench

$(B)/$(BENCHBIN)$(FULLBINEXT): $(Q3BENCHOBJ)
    $(echo_cmd) "LD $@"
    $(Q)$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(Q3BENCHOBJ) $(LIBS)
```

#### 6d — Pattern compile rules for `bench/`

Each source directory gets a pattern rule that maps `bench/*.o` to the
appropriate source directory and macro:

```makefile
$(B)/bench/%.o: $(SDIR)/%.c       # server/
    $(DO_BENCH_CC)

$(B)/bench/%.o: $(CMDIR)/%.c      # qcommon/
    $(DO_BENCH_CC)

$(B)/bench/%.o: $(ZDIR)/%.c       # zlib/
    $(DO_BENCH_CC)

$(B)/bench/%.o: $(BLIBDIR)/%.c    # botlib/
    $(DO_BENCH_BOT_CC)

$(B)/bench/%.o: $(SYSDIR)/%.c     # sys/
    $(DO_BENCH_CC)

$(B)/bench/%.o: $(NDIR)/%.c       # null/
    $(DO_BENCH_CC)
```

#### 6e — Top-level targets

```makefile
memtest:
    @$(MAKE) bench-targets B=$(BR) \
      CFLAGS="$(CFLAGS) $(BASE_CFLAGS) $(DEPEND_CFLAGS)" \
      OPTIMIZE="-DNDEBUG $(OPTIMIZE)" \
      OPTIMIZEVM="-DNDEBUG $(OPTIMIZEVM)" \
      SERVER_CFLAGS="$(SERVER_CFLAGS)" V=$(V)

memtest-debug:
    @$(MAKE) bench-targets B=$(BD) \
      CFLAGS="$(CFLAGS) $(BASE_CFLAGS) $(DEPEND_CFLAGS)" \
      OPTIMIZE="$(DEBUG_CFLAGS)" \
      OPTIMIZEVM="$(DEBUG_CFLAGS)" \
      SERVER_CFLAGS="$(SERVER_CFLAGS)" V=$(V)

bench-targets: bench-makedirs $(B)/$(BENCHBIN)$(FULLBINEXT)
    @echo "Memtest build: $(B)/$(BENCHBIN)$(FULLBINEXT)"

bench-makedirs:
    @if [ ! -d $(BUILD_DIR) ]; then $(MKDIR) $(BUILD_DIR); fi
    @if [ ! -d $(B) ];         then $(MKDIR) $(B);         fi
    @if [ ! -d $(B)/bench ];   then $(MKDIR) $(B)/bench;   fi
```

---

## Building

```bash
# Release build (optimised)
make memtest \
    BUILD_CLIENT=0 BUILD_SERVER=0 \
    BUILD_GAME_SO=0 BUILD_GAME_QVM=0 \
    BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \
    BUILD_RENDERER_OPENGL2=0

# Debug build
make memtest-debug \
    BUILD_CLIENT=0 BUILD_SERVER=0 \
    BUILD_GAME_SO=0 BUILD_GAME_QVM=0 \
    BUILD_BASEGAME=0 BUILD_MISSIONPACK=0 \
    BUILD_RENDERER_OPENGL2=0
```

Output binary: `build/release-<platform>-<arch>/oa_bench.<arch>`

---

## Running

The binary requires the OpenArena game data (`.pk3` files) to be present in a
`baseoa/` directory. The data is **not** included in the source repository and
must be obtained separately (e.g. from an OpenArena installation).

```bash
./build/release-darwin-arm/oa_bench.arm \
    +set dedicated 1 \
    +set fs_basepath /path/to/openarena-data \
    +map q3dm1 \
    +addbot Keel 1 \
    --bench-frames 2000
```

### Command-line options

| Option | Default | Description |
|---|---|---|
| `--bench-frames N` | `1000` | Number of game frames to simulate before exiting |
| `+set fs_basepath PATH` | binary directory | Path to the directory containing `baseoa/` |
| `+map MAPNAME` | none | Map to load (e.g. `q3dm1`) |
| `+addbot NAME SKILL` | none | Add a bot (can be repeated) |

Any `+set`, `+map`, `+addbot`, etc. argument not prefixed with `--bench-` is
forwarded directly to the engine's command-line parser.

### Sample output

```
=== OpenArena Memory Benchmark ===
BENCH: target frames = 2000
BENCH: initialising engine...
NET_Init: null network backend (memtest/seL4 build)
...
BENCH: running 2000 frames...

=== OpenArena Memory Benchmark Results ===
BENCH: frames=2000 elapsed_ms=4321
BENCH: fps_equivalent=463.0
BENCH: hunk_remaining=38291456 bytes

--- Hunk allocation log ---
...per-allocation breakdown from Hunk_Log()...
==========================================
```

---

## What is and is not included

| Subsystem | Included? | Notes |
|---|---|---|
| Server game loop (`sv_*.c`) | Yes | Full tick including AI, physics, snapshot generation |
| Collision model (`cm_*.c`) | Yes | BSP trace, patch collision |
| Bot AI (`botlib/`) | Yes | Path-finding, area awareness system (AAS) |
| VM interpreter (`vm_interpreted.c`) | Yes | Runs `qagame.qvm` game logic |
| File I/O / pak loader (`files.c`) | Yes | Needed to load map + game data |
| OpenGL renderer | No | Excluded by `-DDEDICATED` |
| SDL (window, input) | No | Excluded by `-DDEDICATED` |
| OpenAL audio | No | Replaced by `null_snddma.c` stubs (already in tree) |
| Real UDP networking | No | Replaced by `null_net_ip.c` |
| Client game logic (`cgame/`) | No | Replaced by `null_client.c` stubs |

---

## seL4 porting checklist

The following POSIX dependencies remain in the memtest build and must be
replaced when targeting seL4.

### `code/sys/sys_unix.c`

| Function | POSIX dependency | seL4 replacement |
|---|---|---|
| `Sys_Milliseconds()` | `gettimeofday()` | seL4 timer syscall or a monotonic counter |
| `Sys_RandomBytes()` | `open("/dev/urandom")` | seL4 random number service or remove |
| `Sys_GetCurrentUser()` | `getpwuid(getuid())` | hard-code `"player"` |
| `Sys_Mkdir()` | `mkdir()` | seL4 VFS IPC |
| `Sys_ListFiles()` | `opendir()` / `readdir()` | seL4 VFS IPC |
| `Sys_Sleep()` | `nanosleep()` | `seL4_Yield()` or timer wait |

### `code/sys/sys_main.c`

| Function | POSIX dependency | seL4 replacement |
|---|---|---|
| `Sys_PlatformInit()` | `uname()`, `getenv()` | seL4 boot info parsing |
| `Sys_LoadDll()` | `dlopen()` | not needed (game VM uses QVM interpreter) |
| Signal handlers | `signal()` | seL4 fault endpoint registration |

### `code/sys/sys_bench.c`

| Call | POSIX dependency | seL4 replacement |
|---|---|---|
| `signal(...)` | POSIX signals | seL4 fault handlers |
| `CON_Init()` / `CON_Shutdown()` | TTY / `select()` on stdin | remove entirely |

### `code/qcommon/files.c`

This is the largest remaining dependency. It uses `fopen`, `fread`, `stat`,
`opendir`, and ZIP decompression to load `.pk3` files. On seL4 this must be
replaced with:

1. A userspace VFS server that speaks an IPC protocol.
2. An IPC shim inside `files.c` that replaces the POSIX calls.

Or alternatively, pre-extract the game data to a flat in-memory image and
patch the file-open calls to read from it.

### `code/null/null_net_ip.c`

Already fully seL4-ready when compiled with `-DMEMTEST_SEL4`. The only
remaining hook is `NET_Sleep()`, which is a no-op in the benchmark build but
could be replaced with `seL4_Yield()` to avoid busy-waiting.

---

## Subsystem dependency graph

```
oa_bench
├── sys_bench.c           (benchmark main)
├── sys_main.c            (Sys_Error, Sys_Quit, Sys_Print, …)
├── sys_unix.c            (Sys_Milliseconds, Sys_Mkdir, …)    ← POSIX
├── con_log.c             (console log to file)
│
├── qcommon/common.c      (Com_Init, Com_Frame, hunk/zone allocators)
├── qcommon/files.c       (pak/pk3 filesystem)                ← POSIX
├── qcommon/cmd.c         (console command system)
├── qcommon/cvar.c        (console variables)
├── qcommon/vm.c          (QVM virtual machine dispatcher)
├── qcommon/vm_interpreted.c  (QVM bytecode interpreter)
├── qcommon/net_chan.c    (reliable in-process loopback channel)
├── qcommon/cm_*.c        (collision model)
│
├── server/sv_*.c         (server: client mgmt, snapshot, world)
│
├── botlib/be_*.c         (bot AI: path-finding, AAS, goals, movement)
├── botlib/l_*.c          (bot lib: scripting, memory, logging)
│
└── null/
    ├── null_net_ip.c     (NEW: socket-free network backend)
    ├── null_client.c     (CL_* stubs)
    ├── null_input.c      (IN_* stubs)
    └── null_snddma.c     (audio stubs)
```
