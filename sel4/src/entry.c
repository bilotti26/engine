/*
 * entry.c - seL4 Microkit protection domain entry point for the bench PD.
 *
 * The Microkit runtime calls init() once at startup, then dispatches
 * notified() for incoming notifications.  We run the full benchmark
 * inside init() and never return.
 *
 * Microkit patches these symbols at image-build time (bench.system setvar_vaddr):
 *   sel4_heap_base  — virtual address of the 192 MB engine heap MR
 *   sel4_snapshot   — virtual address of the 4 KB snapshot MR (rw for bench)
 */

#include "sel4_platform.h"

/* Set by Microkit from the 'heap' memory region mapping in bench.system */
uintptr_t sel4_heap_base;

/* Set by Microkit from the 'snapshot' memory region mapping in bench.system.
 * Written by the bench PD after each SV_Frame(); read-only by detector PDs. */
uintptr_t sel4_snapshot;

void init(void)
{
    sel4_platform_init();   /* timer calibration, heap bootstrap */
    bench_sel4_main();      /* runs the benchmark loop, never returns */

    /* Should never reach here */
    microkit_dbg_puts("BENCH: init() returned unexpectedly\n");
    while (1) {}
}

void notified(microkit_channel ch)
{
    (void)ch;
    /* bench PD does not receive notifications in this design */
}
