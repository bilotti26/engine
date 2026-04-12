#pragma once
/*
 * sel4_platform.h - seL4/Microkit platform glue for the OA benchmark.
 *
 * Included by sel4/src/*.c files.  Provides:
 *   - forward declarations for the Microkit API
 *   - heap region extern (set by Microkit from bench.system)
 *   - sel4_dbg_print() for all debug output
 */

#include <microkit.h>
#include <stddef.h>
#include <stdint.h>
#include <stdarg.h>

/* Heap region exported by Microkit via setvar_vaddr */
extern uintptr_t sel4_heap_base;

/* Platform init (called once from init()) */
void sel4_platform_init(void);

/* Debug serial output (wraps microkit_dbg_puts) */
void sel4_dbg_print(const char *s);

/* seL4 benchmark entry point (in sys_bench.c) */
void bench_sel4_main(void);
