/*
 * entry.c - seL4 Microkit protection domain entry point.
 *
 * The Microkit runtime calls init() once at startup, then dispatches
 * notified() for incoming notifications.  We run the full benchmark
 * inside init() and never return.
 */

#include "sel4_platform.h"

/* Heap region base - set by Microkit via setvar_vaddr in bench.system */
uintptr_t sel4_heap_base;

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
    /* Single-PD benchmark -- no incoming notifications expected */
}
