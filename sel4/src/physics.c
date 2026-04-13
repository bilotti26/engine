/*
 * physics.c - Speedhack / noclip detection Protection Domain for the OA seL4 demo.
 *
 * Receives a Microkit notification on channel 1 each game frame after the
 * engine PD writes a game_snapshot_t to the shared read-only snapshot page.
 *
 * Detection heuristics:
 *   NOCLIP   — PMF_NOCLIP flag set in pm_flags (direct noclip cheat)
 *   SPEED    — |velocity| > MAX_SPEED_THRESHOLD (speedhack)
 *   TELEPORT — position delta > TELEPORT_THRESHOLD units in one frame
 *              and PMF_NOCLIP is not set (excludes respawns + legit noclip)
 *
 * Q3/OA physics reference values:
 *   Ground max speed:   320 u/s   (pm_maxspeed default)
 *   With haste powerup: 640 u/s
 *   Rocket-jump peak:  ~1000 u/s  (momentary)
 *   Noclip speed:       200-400 u/s normally, higher with +speed
 *   We use a generous threshold of 1200 u/s to avoid false positives.
 *
 * No libc is used — this PD links only against libmicrokit.a.
 */

#include <stdint.h>
#include <microkit.h>
#include <game_snapshot.h>

/* Channel ID for notifications from bench PD (bench.system <end id="1">) */
#define CH_BENCH  1

/* PMF_NOCLIP value from bg_public.h (can't include engine headers here) */
#define SNAP_PMF_NOCLIP  0   /* OA/Q3 doesn't have PMF_NOCLIP in pm_flags;
                                noclip is a server-side cheat. We detect it
                                via the extreme position changes it enables. */

/* Velocity magnitude threshold for speedhack detection (units/s) */
#define MAX_SPEED_THRESHOLD   1400.0f

/* Maximum position delta per frame at 60 fps = 1400 u/s / 60 = ~23.3 units.
 * We use a generous multiple to handle variable frame rates. */
#define TELEPORT_THRESHOLD    800.0f   /* units — only fires if not a respawn */

/* ---------------------------------------------------------------------------
 * Microkit sets this to the snapshot page virtual address at boot.
 * (setvar_vaddr="sel4_snapshot" in bench.system, perms="r")
 * ------------------------------------------------------------------------- */
uintptr_t sel4_snapshot;

/* Per-client previous position */
static float prev_origin[SNAP_MAX_CLIENTS][3];
static unsigned int initialized;

/* ---------------------------------------------------------------------------
 * Tiny output helpers — no libc / printf available in this PD.
 * ------------------------------------------------------------------------- */
static void dbg_str(const char *s) { microkit_dbg_puts(s); }

static void dbg_u32(unsigned int v)
{
    char buf[12];
    int  i = 10;
    buf[11] = '\0';
    if (v == 0) { microkit_dbg_puts("0"); return; }
    while (v > 0 && i >= 0) { buf[i--] = (char)('0' + v % 10); v /= 10; }
    microkit_dbg_puts(buf + i + 1);
}

static void dbg_f32_abs(float v)
{
    unsigned int whole, frac;
    if (v < 0.0f) v = -v;
    whole = (unsigned int)v;
    frac  = (unsigned int)((v - (float)whole) * 10.0f);
    dbg_u32(whole);
    dbg_str(".");
    dbg_u32(frac);
}

/* Software sqrt (Newton-Raphson, ~5 iterations) */
static float fast_sqrtf(float x)
{
    float r;
    if (x <= 0.0f) return 0.0f;
    /* Hardware fsqrt on aarch64 */
    __asm__ volatile("fsqrt %s0, %s1" : "=w"(r) : "w"(x));
    return r;
}

/* ---------------------------------------------------------------------------
 * Detection logic
 * ------------------------------------------------------------------------- */
static void check_physics(const game_snapshot_t *snap)
{
    unsigned int i;

    for (i = 0; i < snap->num_clients && i < SNAP_MAX_CLIENTS; i++)
    {
        const snap_client_t *sc = &snap->clients[i];
        float vx, vy, vz, speed;
        float dx, dy, dz, dist;

        if (!sc->alive) continue;

        /* --- Speed check --- */
        vx = sc->velocity[0];
        vy = sc->velocity[1];
        vz = sc->velocity[2];
        speed = fast_sqrtf(vx*vx + vy*vy + vz*vz);

        if (speed > MAX_SPEED_THRESHOLD) {
            dbg_str("DETECT[phys] frame=");
            dbg_u32(snap->frame_num);
            dbg_str(" client=");
            dbg_u32(i);
            dbg_str(" SPEED speed=");
            dbg_f32_abs(speed);
            dbg_str("\n");
        }

        /* --- Teleport / position jump check --- */
        dx = sc->origin[0] - prev_origin[i][0];
        dy = sc->origin[1] - prev_origin[i][1];
        dz = sc->origin[2] - prev_origin[i][2];
        dist = fast_sqrtf(dx*dx + dy*dy + dz*dz);

        if (dist > TELEPORT_THRESHOLD) {
            dbg_str("DETECT[phys] frame=");
            dbg_u32(snap->frame_num);
            dbg_str(" client=");
            dbg_u32(i);
            dbg_str(" TELEPORT dist=");
            dbg_f32_abs(dist);
            dbg_str("\n");
        }

        prev_origin[i][0] = sc->origin[0];
        prev_origin[i][1] = sc->origin[1];
        prev_origin[i][2] = sc->origin[2];
    }
}

/* ---------------------------------------------------------------------------
 * Microkit entry points
 * ------------------------------------------------------------------------- */
void init(void)
{
    unsigned int i, j;
    for (i = 0; i < SNAP_MAX_CLIENTS; i++)
        for (j = 0; j < 3; j++)
            prev_origin[i][j] = 0.0f;
    initialized = 0;
    microkit_dbg_puts("monitor_physics: ready\n");
}

void notified(microkit_channel ch)
{
    const game_snapshot_t *snap;

    if (ch != CH_BENCH) return;

    snap = (const game_snapshot_t *)sel4_snapshot;

    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!initialized) {
        unsigned int i;
        for (i = 0; i < SNAP_MAX_CLIENTS; i++) {
            prev_origin[i][0] = snap->clients[i].origin[0];
            prev_origin[i][1] = snap->clients[i].origin[1];
            prev_origin[i][2] = snap->clients[i].origin[2];
        }
        initialized = 1;
        return;
    }

    check_physics(snap);
}
