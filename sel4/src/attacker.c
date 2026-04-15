/*
 * attacker.c - Read-only enforcement test Protection Domain for the OA seL4 demo.
 *
 * Receives a Microkit notification on channel 3 each game frame (when built
 * with -DMEMTEST_ATTACK).  On the first notification it attempts a write to
 * its read-only mapping of the snapshot page.
 *
 * Expected outcome: the write triggers an seL4 VM fault, the kernel terminates
 * this PD, and the fault handler prints a capability violation message.  The
 * sentinel "BUG: attacker write succeeded" should NEVER appear — if it does,
 * the read-only mapping is not being enforced.
 *
 * No libc is used — this PD links only against libmicrokit.a.
 */

#include <stdint.h>
#include <microkit.h>
#include <game_snapshot.h>

/* Channel ID for notifications from the bench PD (bench_attack.system <end id="3">) */
#define CH_BENCH  3

/* ---------------------------------------------------------------------------
 * Microkit sets this to the snapshot page virtual address at boot.
 * (setvar_vaddr="sel4_snapshot" in bench_attack.system, perms="r")
 * ------------------------------------------------------------------------- */
uintptr_t sel4_snapshot;

/* Fire the write attempt only once so output is unambiguous */
static unsigned int triggered;

/* ---------------------------------------------------------------------------
 * Microkit entry points
 * ------------------------------------------------------------------------- */
void init(void)
{
    triggered = 0;
    microkit_dbg_puts("attacker: ready -- will attempt write on first frame notify\n");
}

void notified(microkit_channel ch)
{
    if (ch != CH_BENCH) return;
    if (triggered) return;
    triggered = 1;

    microkit_dbg_puts("attacker: attempting write to read-only snapshot page...\n");

    /*
     * Attempt a store to the snapshot page.  bench_attack.system maps this
     * region with perms="r", so the MMU should raise a permission fault and
     * seL4 will terminate this PD before the store completes.
     *
     * volatile prevents the compiler from eliding the store.
     */
    *((volatile uint32_t *)sel4_snapshot) = 0xDEADBEEF;

    /*
     * If execution ever reaches here the hardware did not enforce the
     * read-only mapping — that is a bug in the system configuration.
     */
    microkit_dbg_puts("BUG: attacker write succeeded — read-only enforcement FAILED\n");
}
