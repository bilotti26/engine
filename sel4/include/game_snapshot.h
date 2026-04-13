/*
 * game_snapshot.h - Per-frame game state snapshot shared between the engine
 *                   PD and cheat-detector PDs via a read-only memory region.
 *
 * Included by:
 *   - code/sys/sys_bench.c   (writer, seL4 build only)
 *   - sel4/src/monitor.c     (reader, aimbot detector PD)
 *   - sel4/src/physics.c     (reader, speedhack/noclip detector PD)
 *
 * Uses only plain C types (no stdint.h) so it compiles in both the engine
 * build (which has q_shared.h) and the minimal detector PD build.
 */

#pragma once

/* Must be <= MAX_CLIENTS (64) and fit comfortably in one 4 KB page. */
#define SNAP_MAX_CLIENTS  16

/*
 * Per-client data extracted from playerState_t after SV_Frame().
 * Size: 3*4 + 3*4 + 3*4 + 4 + 4 + 2*4 + 2*4 + 4 = 64 bytes.
 */
typedef struct {
    float        origin[3];       /* world position (units) */
    float        velocity[3];     /* movement velocity (units/s) */
    float        viewangles[3];   /* pitch / yaw / roll (degrees) */
    int          weapon;          /* current weapon index */
    int          pm_flags;        /* PMF_NOCLIP, PMF_DUCKED, etc. */
    int          event[2];        /* last two EV_* entity events */
    int          event_parm[2];   /* accompanying event parameters */
    unsigned int alive;           /* 1 = slot CS_ACTIVE and data valid */
} snap_client_t;

/*
 * Full snapshot for one server frame.
 * Size: 16 + 16*64 + 16*4 = 1104 bytes — fits in 4 KB page.
 */
typedef struct {
    unsigned int  frame_num;                    /* monotonically increasing */
    unsigned int  frame_msec;                   /* nominal ms/frame (1000/sv_fps) */
    unsigned int  num_clients;                  /* number of active client slots */
    unsigned int  _pad;
    snap_client_t clients[SNAP_MAX_CLIENTS];
    /* vis_mask[i]: bitmask of which clients are visible from client i */
    unsigned int  vis_mask[SNAP_MAX_CLIENTS];
} game_snapshot_t;

/* Compile-time size guard */
typedef char _snap_size_check[sizeof(game_snapshot_t) <= 4096 ? 1 : -1];
