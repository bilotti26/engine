/*
 * sys_sel4.c  -  seL4/Microkit platform backend for the OA benchmark.
 *
 * Replaces sys_unix.c in the seL4 build.
 * Provides: Sys_Milliseconds, Sys_Sleep, Sys_Print, Sys_Mkdir, CON_*, etc.
 *
 * Compiled only when -DMEMTEST_SEL4 is set.
 */

#ifdef MEMTEST_SEL4

#include <microkit.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

/* -----------------------------------------------------------------------
 * ARM generic timer
 *
 * seL4 Microkit (hypervisor mode, EL2) may trap CNTVCT_EL0 depending on
 * CNTHCTL_EL2 configuration. Use CNTPCT_EL0 (physical counter) which is
 * always accessible from EL0 when CNTHCTL_EL2.EL1PCTEN=1 (seL4 default).
 * Fall back to a software counter if the physical counter is also trapped.
 * ----------------------------------------------------------------------- */

static uint64_t timer_freq;     /* ticks per second */
static uint64_t timer_origin;   /* tick value at first call */

static inline uint64_t read_cntpct(void)
{
    uint64_t v;
    __asm__ volatile("mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

static inline uint64_t read_cntfrq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f;
}

/* Called once at startup to calibrate the timer */
void sel4_platform_init(void)
{
    timer_freq   = read_cntfrq();
    timer_origin = read_cntpct();
    if (timer_freq == 0)
        timer_freq = 62500000;  /* QEMU default: 62.5 MHz */
}

int Sys_Milliseconds(void)
{
    uint64_t now   = read_cntpct();
    uint64_t ticks = now - timer_origin;
    /* ticks * 1000 / freq, with overflow protection */
    return (int)((ticks / (timer_freq / 1000)));
}

void Sys_Sleep(int msec)
{
    /* Spin-wait -- no seL4 timer IRQ needed for a single-PD benchmark */
    int target = Sys_Milliseconds() + msec;
    while (Sys_Milliseconds() < target)
        __asm__ volatile("yield");
}

/* -----------------------------------------------------------------------
 * Console / output
 * ----------------------------------------------------------------------- */

void Sys_Print(const char *msg)
{
    microkit_dbg_puts(msg);
}

/* CON_* stubs -- no interactive console on seL4 */
void CON_Init(void)    {}
void CON_Shutdown(void) {}
char *CON_Input(void)  { return NULL; }

/* -----------------------------------------------------------------------
 * Filesystem helpers used by the engine's sys layer
 * ----------------------------------------------------------------------- */

qboolean Sys_Mkdir(const char *path)
{
    /* No real filesystem on seL4 -- silently succeed */
    (void)path;
    return qtrue;
}

char *Sys_Cwd(void)
{
    return "/gamedata";
}

char *Sys_DefaultBasePath(void)
{
    return "/gamedata";
}

const char *Sys_DefaultHomePath(void)
{
    return "/gamedata";
}

/* -----------------------------------------------------------------------
 * Process / signal handling stubs
 * ----------------------------------------------------------------------- */

void Sys_Quit(void)
{
    microkit_dbg_puts("BENCH: Sys_Quit called\n");
    /* Attempt PSCI SYSTEM_OFF via HVC to cleanly exit QEMU.
     * PSCI_SYSTEM_OFF = 0x84000008 (SMC32 convention). */
    {
        register uint64_t x0 __asm__("x0") = 0x84000008UL;
        __asm__ volatile("hvc #0" : "+r"(x0) :: "memory");
    }
    /* If PSCI is not available, spin */
    while (1) { __asm__ volatile("wfi"); }
}

void QDECL Sys_Error(const char *error, ...) __attribute__((noreturn));
void QDECL Sys_Error(const char *error, ...)
{
    va_list ap;
    char buf[2048];
    va_start(ap, error);
    vsnprintf(buf, sizeof(buf), error, ap);
    va_end(ap);
    microkit_dbg_puts("BENCH ERROR: ");
    microkit_dbg_puts(buf);
    microkit_dbg_puts("\n");
    while (1) {}
}

/* Signal handler -- seL4 doesn't have POSIX signals */
void Sys_SigHandler(int sig)
{
    (void)sig;
    Sys_Quit();
}

/* -----------------------------------------------------------------------
 * Platform init entry (called from sel4/src/entry.c before bench_sel4_main)
 * Declared in sel4/include/sel4_platform.h
 * ----------------------------------------------------------------------- */
/* sel4_platform_init() is defined above */

/* -----------------------------------------------------------------------
 * Stubs for functions referenced by the engine but unused in dedicated mode
 * ----------------------------------------------------------------------- */

void Sys_SetEnv(const char *name, const char *value)
{
    (void)name; (void)value;
}

char *Sys_GetCurrentUser(void)
{
    return "sel4";
}

char *Sys_GetClipboardData(void)
{
    return NULL;
}

qboolean Sys_LowPhysicalMemory(void)
{
    return qfalse;
}

/* Dynamic library loading -- not used in seL4 build */
void *Sys_LoadLibrary(const char *f)  { (void)f; return NULL; }
void *Sys_LoadFunction(void *h, const char *n) { (void)h; (void)n; return NULL; }
void  Sys_UnloadLibrary(void *h) { (void)h; }

/* Suspend / resume -- no-op on seL4 */
void Sys_Suspend(void) {}
void Sys_Resume(void)  {}

/* PlatformInit called early by sys glue */
void Sys_PlatformInit(void) {}
void Sys_PlatformExit(void) {}

/* -----------------------------------------------------------------------
 * Stubs that replace sys_main.c (not compiled in the seL4 build)
 * ----------------------------------------------------------------------- */

/* Binary / install path -- irrelevant on seL4; return a safe default */
static char binaryPath[8] = "/";
void  Sys_SetBinaryPath(const char *path) { (void)path; }
char *Sys_BinaryPath(void) { return binaryPath; }

void  Sys_SetDefaultInstallPath(const char *path) { (void)path; }
char *Sys_DefaultInstallPath(void) { return "/gamedata"; }
char *Sys_DefaultAppPath(void) { return "/gamedata"; }

/* Input restart -- no interactive console */
void Sys_In_Restart_f(void) {}

/* Console input -- no stdin on seL4 */
char *Sys_ConsoleInput(void) { return NULL; }

/* PID file -- no filesystem on seL4 */
qboolean Sys_WritePIDFile(void) { return qfalse; }

/* Sys_Init -- called by common.c; nothing to do on seL4 */
void Sys_Init(void) {}

/* ANSI colour print -- strip codes and forward to serial */
void Sys_AnsiColorPrint(const char *msg) { Sys_Print(msg); }

/* File modification time -- all files are read-only CPIO data */
int Sys_FileTime(char *path) { (void)path; return -1; }

/* Game DLL loading -- seL4 build uses QVM interpreter only */
void Sys_UnloadDll(void *dllHandle) { (void)dllHandle; }

void *Sys_LoadDll(const char *name, qboolean useSystemLib)
{ (void)name; (void)useSystemLib; return NULL; }

/* Sys_LoadGameDll -- vm.c references this even with NO_VM_COMPILED */
void * QDECL Sys_LoadGameDll(const char *name,
                              intptr_t (QDECL **entryPoint)(int, ...),
                              intptr_t (QDECL *systemcalls)(intptr_t, ...))
{
    (void)name; (void)entryPoint; (void)systemcalls;
    return NULL;
}

/* -----------------------------------------------------------------------
 * CPU / random stubs
 * ----------------------------------------------------------------------- */

/* Sys_GetProcessorFeatures -- no CPUID on aarch64; return safe defaults */
cpuFeatures_t Sys_GetProcessorFeatures(void) { return (cpuFeatures_t)0; }

/* Sys_RandomBytes -- fill with simple LCG bytes */
qboolean Sys_RandomBytes(byte *string, int len)
{
    unsigned int s = (unsigned int)Sys_Milliseconds();
    int i;
    for (i = 0; i < len; i++) {
        s = s * 1664525u + 1013904223u;
        string[i] = (byte)(s >> 24);
    }
    return qtrue;
}

/* Sys_FOpen / Sys_Mkfifo / Sys_ListFiles / Sys_FreeFileList are in
   sel4/src/fs_cpio.c where the DIR and dirent types are in scope. */

/* Sys_ParseArgs -- only used by the Linux main(); stub for completeness */
void Sys_ParseArgs(int argc, char **argv) { (void)argc; (void)argv; }

/* Sys_Dirname -- extracts directory portion of a path */
const char *Sys_Dirname(char *path)
{
    static char dir[256];
    char *p;
    if (!path || !*path) { dir[0] = '.'; dir[1] = '\0'; return dir; }
    /* copy, then truncate at last '/' */
    int len = 0;
    while (path[len] && len < 255) { dir[len] = path[len]; len++; }
    dir[len] = '\0';
    p = dir + len - 1;
    while (p > dir && *p != '/') p--;
    if (*p == '/') *p = '\0'; else { dir[0] = '.'; dir[1] = '\0'; }
    return dir;
}

/* Sys_StripAppBundle -- macOS-only; no-op on seL4 */
char *Sys_StripAppBundle(char *path) { return path; }

#endif /* MEMTEST_SEL4 */
