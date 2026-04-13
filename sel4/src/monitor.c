/*
 * monitor.c - Aimbot detection Protection Domain for the OA seL4 demo.
 *
 * Receives a Microkit notification on channel 1 each game frame after the
 * engine PD writes a game_snapshot_t to the shared read-only snapshot page.
 *
 * Detection heuristics:
 *   SNAP  — yaw change > 120 deg or pitch change > 90 deg in a single frame
 *           (instant 180-degree flick impossible for human input at 60 Hz)
 *   JERK  — angular velocity > 900 deg/s
 *           (sustained spin far beyond human capability)
 *
 * Output goes to the seL4 debug UART via microkit_dbg_puts().
 * No libc is used — this PD links only against libmicrokit.a.
 */

#include <stdint.h>
#include <microkit.h>
#include <game_snapshot.h>

/* Channel ID for notifications from the bench PD (bench.system <end id="1">) */
#define CH_BENCH  1

/* Detection thresholds */
#define SNAP_YAW_DEG    120.0f   /* deg/frame — single-frame yaw snap        */
#define SNAP_PITCH_DEG   90.0f   /* deg/frame — single-frame pitch snap       */
#define JERK_DEG_S      900.0f   /* deg/s    — sustained angular velocity cap */

/* ---------------------------------------------------------------------------
 * Microkit sets this to the snapshot page virtual address at boot.
 * (setvar_vaddr="sel4_snapshot" in bench.system, perms="r")
 * ------------------------------------------------------------------------- */
uintptr_t sel4_snapshot;

/* Per-client previous-frame angle state */
static float prev_yaw[SNAP_MAX_CLIENTS];
static float prev_pitch[SNAP_MAX_CLIENTS];
static unsigned int initialized;   /* 0 = first frame, skip delta calc */

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

/* Print a float as "NNN.N" (one decimal place, no negatives needed here) */
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

/* ---------------------------------------------------------------------------
 * Detection logic — called once per frame notification.
 * ------------------------------------------------------------------------- */
static void check_aimbot(const game_snapshot_t *snap)
{
    unsigned int i;
    unsigned int msec = snap->frame_msec ? snap->frame_msec : 16u;
    float dt_s = (float)msec / 1000.0f;

    for (i = 0; i < snap->num_clients && i < SNAP_MAX_CLIENTS; i++)
    {
        const snap_client_t *sc = &snap->clients[i];
        float dyaw, dpitch, yaw_vel;

        if (!sc->alive) continue;

        dyaw   = sc->viewangles[1] - prev_yaw[i];
        dpitch = sc->viewangles[0] - prev_pitch[i];

        /* Normalise deltas to (-180, 180] */
        while (dyaw   >  180.0f) dyaw   -= 360.0f;
        while (dyaw   < -180.0f) dyaw   += 360.0f;
        while (dpitch >  180.0f) dpitch -= 360.0f;
        while (dpitch < -180.0f) dpitch += 360.0f;

        if (dyaw   < 0.0f) dyaw   = -dyaw;
        if (dpitch < 0.0f) dpitch = -dpitch;

        /* Single-frame snap detection */
        if (dyaw > SNAP_YAW_DEG) {
            dbg_str("DETECT[aim] frame=");
            dbg_u32(snap->frame_num);
            dbg_str(" client=");
            dbg_u32(i);
            dbg_str(" YAW_SNAP deg=");
            dbg_f32_abs(dyaw);
            dbg_str("\n");
        }
        if (dpitch > SNAP_PITCH_DEG) {
            dbg_str("DETECT[aim] frame=");
            dbg_u32(snap->frame_num);
            dbg_str(" client=");
            dbg_u32(i);
            dbg_str(" PITCH_SNAP deg=");
            dbg_f32_abs(dpitch);
            dbg_str("\n");
        }

        /* Angular velocity check */
        if (dt_s > 0.0f) {
            yaw_vel = dyaw / dt_s;
            if (yaw_vel > JERK_DEG_S) {
                dbg_str("DETECT[aim] frame=");
                dbg_u32(snap->frame_num);
                dbg_str(" client=");
                dbg_u32(i);
                dbg_str(" HIGH_YAW_VEL deg_s=");
                dbg_f32_abs(yaw_vel);
                dbg_str("\n");
            }
        }

        prev_yaw[i]   = sc->viewangles[1];
        prev_pitch[i] = sc->viewangles[0];
    }
}

/* ---------------------------------------------------------------------------
 * Microkit entry points
 * ------------------------------------------------------------------------- */
void init(void)
{
    unsigned int i;
    for (i = 0; i < SNAP_MAX_CLIENTS; i++) {
        prev_yaw[i]   = 0.0f;
        prev_pitch[i] = 0.0f;
    }
    initialized = 0;
    microkit_dbg_puts("monitor_aim: ready\n");
}

void notified(microkit_channel ch)
{
    const game_snapshot_t *snap;

    if (ch != CH_BENCH) return;

    snap = (const game_snapshot_t *)sel4_snapshot;

    /* Acquire barrier: observe bench's release-store of the snapshot */
    __atomic_thread_fence(__ATOMIC_ACQUIRE);

    if (!initialized) {
        /* Seed previous angles from first snapshot — no delta on frame 0 */
        unsigned int i;
        for (i = 0; i < SNAP_MAX_CLIENTS; i++) {
            prev_yaw[i]   = snap->clients[i].viewangles[1];
            prev_pitch[i] = snap->clients[i].viewangles[0];
        }
        initialized = 1;
        return;
    }

    check_aimbot(snap);
}
