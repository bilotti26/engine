/*
===========================================================================
sys_bench.c - Benchmark harness main() for the OpenArena memtest build.

Replaces sys_main.c's main() when compiled with -DMEMTEST_BUILD.
All other functions in sys_main.c (Sys_Quit, Sys_Error, Sys_Print, …)
are still compiled from sys_main.c — this file only contributes main().

Usage:
  ./oa_bench [+set dedicated 1] [+map q3dm1] [+addbot Keel 1] \
             [--bench-frames 2000] [--bench-basedir /path/to/data]

  --bench-frames N   run exactly N game frames then exit (default 1000)

The binary prints a memory report to stdout on exit:
  BENCH: frames=<N> elapsed_ms=<T>
  BENCH: hunk_remaining=<bytes>
  (full hunk log follows)

seL4 porting checklist:
  - Replace Sys_PlatformInit() with seL4 environment init
  - Replace Sys_Milliseconds() (in sys_unix.c) with a seL4 timer call
  - Replace signal() calls with seL4 fault handler registration
  - Replace Com_Init's filesystem layer (files.c) with a seL4 IPC-based VFS
  - NET_Sleep() in null_net_ip.c: replace with seL4_Yield() or timer wait
  - Remove CON_Init() / CON_Input() — no interactive console on seL4
===========================================================================
*/

#ifdef MEMTEST_BUILD

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

#include "../qcommon/q_shared.h"
#include "../qcommon/qcommon.h"
#include "sys_local.h"

/* Functions defined in sys_main.c but not in any public header. */
void        Sys_SetBinaryPath( const char *path );
char       *Sys_BinaryPath( void );
void        Sys_ParseArgs( int argc, char **argv );

#ifndef DEFAULT_BASEDIR
#  ifdef MACOS_X
#    define DEFAULT_BASEDIR Sys_StripAppBundle( Sys_BinaryPath() )
#  else
#    define DEFAULT_BASEDIR Sys_BinaryPath()
#  endif
#endif

/* -----------------------------------------------------------------------
   Benchmark state
   ----------------------------------------------------------------------- */

static int      bench_frames    = 1000;
static int      bench_frame_cur = 0;
static qboolean bench_done      = qfalse;

static void Bench_Frame( void )
{
	bench_frame_cur++;
	if ( bench_frame_cur >= bench_frames )
		bench_done = qtrue;
}

/* -----------------------------------------------------------------------
   Result reporting
   ----------------------------------------------------------------------- */

static void Bench_PrintResults( int elapsed_ms )
{
	printf( "\n" );
	printf( "=== OpenArena Memory Benchmark Results ===\n" );
	printf( "BENCH: frames=%d elapsed_ms=%d\n", bench_frame_cur, elapsed_ms );
	if ( elapsed_ms > 0 )
		printf( "BENCH: fps_equivalent=%.1f\n",
		        (float)bench_frame_cur * 1000.0f / (float)elapsed_ms );
	printf( "BENCH: hunk_remaining=%d bytes\n", Hunk_MemoryRemaining() );
	printf( "\n--- Hunk allocation log ---\n" );
	/* Hunk_Log dumps a per-allocation breakdown via Com_Printf */
	Hunk_Log();
	printf( "==========================================\n" );
}

/* -----------------------------------------------------------------------
   Argument parsing
   ----------------------------------------------------------------------- */

/*
Strip bench-specific args before passing the command line to Com_Init.
Returns the frame count, or the default.
*/
static int Bench_ParseArgs( int argc, char **argv,
                            char *commandLine, int commandLineSize )
{
	int frames = bench_frames;
	int i;

	commandLine[0] = '\0';

	for ( i = 1; i < argc; i++ )
	{
		if ( !strcmp( argv[i], "--bench-frames" ) && i + 1 < argc )
		{
			frames = atoi( argv[++i] );
			if ( frames <= 0 )
				frames = 1000;
			continue;
		}

		/* Pass everything else through to the engine command line */
		{
			const qboolean hasSpaces = strchr( argv[i], ' ' ) != NULL;
			if ( hasSpaces )
				Q_strcat( commandLine, commandLineSize, "\"" );
			Q_strcat( commandLine, commandLineSize, argv[i] );
			if ( hasSpaces )
				Q_strcat( commandLine, commandLineSize, "\"" );
			Q_strcat( commandLine, commandLineSize, " " );
		}
	}

	return frames;
}

/* -----------------------------------------------------------------------
   main  (Linux/macOS build only -- not used in the seL4 Microkit build)
   ----------------------------------------------------------------------- */

#ifndef MEMTEST_SEL4
int main( int argc, char **argv )
{
	char commandLine[MAX_STRING_CHARS] = { 0 };
	int  t_start, t_end;

	Sys_PlatformInit();
	Sys_Milliseconds();   /* calibrate time base */

	Sys_ParseArgs( argc, argv );
	Sys_SetBinaryPath( Sys_Dirname( argv[0] ) );
	Sys_SetDefaultInstallPath( DEFAULT_BASEDIR );

	bench_frames = Bench_ParseArgs( argc, argv, commandLine, sizeof( commandLine ) );

	printf( "=== OpenArena Memory Benchmark ===\n" );
	printf( "BENCH: target frames = %d\n", bench_frames );
	printf( "BENCH: initialising engine...\n" );

	Com_Init( commandLine );
	NET_Init();
	CON_Init();

	signal( SIGILL,  Sys_SigHandler );
	signal( SIGFPE,  Sys_SigHandler );
	signal( SIGSEGV, Sys_SigHandler );
	signal( SIGTERM, Sys_SigHandler );
	signal( SIGINT,  Sys_SigHandler );

	printf( "BENCH: running %d frames...\n", bench_frames );
	t_start = Sys_Milliseconds();

	while ( !bench_done )
	{
		IN_Frame( qfalse );
		Com_Frame();
		Bench_Frame();
	}

	t_end = Sys_Milliseconds();

	Bench_PrintResults( t_end - t_start );

	CON_Shutdown();
	return 0;
}
#endif /* !MEMTEST_SEL4 */

/* -----------------------------------------------------------------------
   seL4 / Microkit entry point

   Called from sel4/src/entry.c after the heap and timer are initialised.
   Runs the same benchmark loop as main() but:
     - command line is hardcoded (no argv)
     - no signal() setup (seL4 uses fault endpoints)
     - no CON_Init/CON_Shutdown (no interactive console)
     - +set fs_basepath /gamedata points into the embedded CPIO filesystem

   Compile with -DMEMTEST_SEL4 (implies -DMEMTEST_BUILD).
   ----------------------------------------------------------------------- */
#ifdef MEMTEST_SEL4

void bench_sel4_main( void )
{
	int t_start, t_end;

	/* Hardcoded command line: same flags run_bench.sh passes on Linux.
	 * Must be a mutable array, not a const pointer: Com_ParseCommandLine
	 * writes null terminators in-place to split tokens. */
	static char commandLine[] =
		"+set dedicated 1 "
		"+set fs_basepath /gamedata "
		"+set fs_homepath /gamedata "
		"+set vm_game 0 "
		"+set sv_fps 60 "
		"+set g_gametype 0 "
		"+set bot_enable 1 "
		"+set bot_nochat 1 "
		"+set g_forcerespawn 1 "
		"+set fraglimit 0 "
		"+set timelimit 0 "
		"+map oa_dm6 "
		"+addbot Gargoyle 3 "
		"+addbot Grism 3 "
		"+addbot Kyonshi 3 "
		"+addbot Major 3";

	bench_frames = 1000;   /* override via BENCH_FRAMES define if desired */
#ifdef BENCH_FRAMES
	bench_frames = BENCH_FRAMES;
#endif

	printf( "=== OpenArena seL4 Memory Benchmark ===\n" );
	printf( "BENCH: target frames = %d\n", bench_frames );
	printf( "BENCH: initialising engine...\n" );

	Com_Init( (char *)commandLine );
	NET_Init();

	/* No signal() or CON_Init() on seL4 */

	printf( "BENCH: running %d frames...\n", bench_frames );
	t_start = Sys_Milliseconds();

	while ( !bench_done )
	{
		IN_Frame( qfalse );
		Com_Frame();
		Bench_Frame();
	}

	t_end = Sys_Milliseconds();

	Bench_PrintResults( t_end - t_start );

	/* Spin forever -- Microkit PD must not return from init() */
	printf( "BENCH: done. Halting.\n" );
	while (1) {}
}

#endif /* MEMTEST_SEL4 */

#endif /* MEMTEST_BUILD */
