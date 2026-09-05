/* Native implementations of game functions, replacing the recompiled ones.
 *
 * This is where a static recompilation stops merely *running* the game and
 * starts changing it. Each function named in host/replace.txt loses its public
 * symbol in the emitted C -- the body survives as `psp_func_<addr>__orig` --
 * and gains a definition here instead. The linker does the rest: every call
 * site the emitter wrote, including the direct `jal`s that no run-time hook can
 * intercept, binds to the version in this file.
 *
 * Why not a hook. psp_set_dispatch_hook (psprecomp/dispatch.h) looks like the
 * tool for this and is not: only *indirect* transfers go through the dispatch
 * table, and a routine called straight out of the game loop lowers to a plain C
 * call that never touches it. Link-time replacement is the only mechanism that
 * catches every caller.
 *
 * ---------------------------------------------------------------------------
 * The rules a replacement has to keep
 *
 * Guest state is not C state. A replacement runs on the same emulated machine
 * the recompiled code does: arguments arrive in the guest register file (the
 * r_* aliases and psp_cpu.f[] for floats), memory is the guest address space
 * (psp_read32/psp_write32), and the return value goes back in r_v0. Touching
 * host memory where the guest expects its own is the mistake this whole layer
 * invites. Leave r_sp alone: the call site checks it did not move.
 *
 * Determinism is load-bearing. The scenario replay in scripts/09-replay.sh is
 * how every gate in docs/findings/state.md is measured, and it reproduces a run
 * from recorded pad input alone. A replacement that reads wall time, a random
 * seed, or live input that is not recorded breaks `--repeat 2` and takes the
 * regression bar with it. Derive behaviour from guest state and from input that
 * goes through the recorded path.
 *
 * Prefer deferring. Calling psp_func_<addr>__orig() and adjusting around it is
 * both safer and more honest than reimplementing a function whose full
 * behaviour nobody has established. A replacement that only intervenes in the
 * case it understands, and defers otherwise, is the shape to aim for.
 * ---------------------------------------------------------------------------
 *
 * PSPRECOMP_INPUT=classic (the default) makes every replacement here defer to
 * the original, so a run without it is the game as shipped and every gate
 * stays what it was. PSPRECOMP_INPUT=modern is the port's own control scheme.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "aclr_funcs.h"        /* psp_func_*, the __orig originals, r_* aliases */

/* ---- configuration ------------------------------------------------------ */

enum { INPUT_CLASSIC = 0, INPUT_MODERN = 1 };

static int input_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PSPRECOMP_INPUT");
        mode = (e && !strcmp(e, "modern")) ? INPUT_MODERN : INPUT_CLASSIC;
        if (mode == INPUT_MODERN)
            printf("      input     modern -- yaw rate proportional to the stick; "
                   "PSPRECOMP_INPUT=classic for the game's own\n");
    }
    return mode;
}

static float f32_read(uint32_t addr) {
    union { uint32_t u; float f; } c;
    c.u = psp_read32(addr);
    return c.f;
}

static void f32_write(uint32_t addr, float v) {
    union { uint32_t u; float f; } c;
    c.f = v;
    psp_write32(addr, c.u);
}

/* ---- the AC's yaw integrator ------------------------------------------------
 *
 * psp_func_0004F248(a0 = the AC object, f12 = accel, f13 = max rate), called
 * once a frame by each of the fourteen movement-state handlers. The original,
 * read from its listing and confirmed by measurement (docs/findings/state.md):
 *
 *   - the yaw rate persists at ac+8312 (float, radians per frame), the yaw
 *     itself at ac+36; a direction byte at ac+8351 records which way;
 *   - gate: the byte at (*(ac+9728))+182 must be -1, else nothing happens;
 *   - the pad reaches it as *buttons*. Its default path asks pressed(2) /
 *     pressed(3) -- the stick has already been thresholded into those two bits
 *     upstream, at about 80% of travel -- and moves the rate by 4*accel per
 *     frame toward the cap, or back toward zero with nothing held. That is the
 *     5-frame ramp to 2.10 deg/frame the sweep measured, and why 100/127 of
 *     stick did exactly nothing. (It has an analog path too, scaling the
 *     *acceleration* by stick travel past a 50% deadzone; the shipped
 *     configuration does not take it.)
 *   - then clamp to +/-max and, unless the global at psp_func_000506F8()+24
 *     says the game is paused, yaw += rate;
 *   - returns -1, 0 or 1: the turn direction, which the callers use.
 *
 * This version keeps the gates, the state layout and the return value, and
 * replaces only the law: the rate is the cap scaled by how far the stick is
 * pushed. No ramp -- the chase camera already smooths the result, and the ramp
 * was the other half of what felt clunky. The stick is the same lx byte the
 * game reads through sceCtrl, so a recording replays this exactly. */

enum { AC_YAW = 36, AC_RATE = 8312, AC_DIR = 8351, AC_STATE_PTR = 9728 };

/* The player's AC is a static in the module's BSS: 0x25F0 bytes cleared at
 * mission start by the memset psp_func_002AD4D0, from psp_func_00262A70. Every
 * AC in the mission -- the enemies too -- runs the same movement code and the
 * same integrator, so a replacement that did not check would steer them all
 * with the player's stick. */
enum { PLAYER_AC = 0x0042D6C0u };

void psp_func_0004F248(void) {
    if (input_mode() != INPUT_MODERN || r_a0 != PLAYER_AC) {
        psp_func_0004F248__orig();
        return;
    }

    const uint32_t ac    = r_a0;
    const float    accel = psp_cpu.f[12];
    const float    max   = psp_cpu.f[13];
    (void)accel;

    /* The state gate, as the original. */
    const uint32_t state = psp_read32(ac + AC_STATE_PTR);
    const int      gated = (int8_t)psp_read8(state + 182) != -1;

    /* PSPRECOMP_INPUT_LOG=<file>: one line per call, so "did the game even ask
     * for a turn this frame" is a fact and not a guess. Which of the fourteen
     * movement handlers calls this is the game's decision, made upstream from
     * the same thresholded bits this replacement is trying to get away from. */
    static FILE *log; static int log_init;
    if (!log_init) {
        log_init = 1;
        const char *p = getenv("PSPRECOMP_INPUT_LOG");
        if (p && *p) log = fopen(p, "w");
    }
    uint8_t ax, ay;
    psp_ctrl_last_stick(&ax, &ay);
    if (log) fprintf(log, "%u ac=%08X ax=%u gate=%d max=%.5f\n",
                     psp_ctrl_polls(), ac, ax, gated, max);

    if (gated) { r_v0 = 0; return; }

    /* The pause gate, as the original: a global object, s16 at +24. */
    psp_func_000506F8();
    const uint32_t g = r_v0;
    const int paused = g && (int16_t)(psp_read32(g + 24) & 0xFFFFu) != 0;

    /* The stick, at the resolution the game never used. Left is negative
     * radians here, matching the original's sign. */
    float x = ((int)ax - 128) / 127.0f;
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    const float dead = 0.06f;                 /* ~8 of 127: stick noise only */
    if (x > -dead && x < dead) x = 0.0f;

    const float rate = max * x;
    f32_write(ac + AC_RATE, rate);
    if (x != 0.0f) psp_write8(ac + AC_DIR, x < 0.0f ? 0 : 1);
    if (!paused) f32_write(ac + AC_YAW, f32_read(ac + AC_YAW) + rate);

    r_v0 = (uint32_t)(int32_t)(x > 0.0f ? 1 : x < 0.0f ? -1 : 0);
}

/* ---- the stick, as buttons ----------------------------------------------------
 *
 * psp_func_00279A50(state, ax, ay): the PSP stick, already through a circular
 * deadzone of radius 30 (psp_func_00279910), becomes four virtual button bits
 * -- 0x8000 right, 0x4000 left, 0x1000 forward, 0x2000 back -- whenever an
 * axis passes the threshold the game keeps at 0x0042B784: 100 of 127. That
 * one compare is where the stick's travel is thrown away. Physical buttons
 * are remapped into bits 0-11 separately, so these four are all the game ever
 * learns about the stick's direction; the key-assign rows bind actions 0-3 to
 * exactly them, and the movement state machine chooses its handler by them.
 * Below the threshold the yaw integrator above is not even called.
 *
 * This version keeps the game's threshold for forward/back -- the walk law has
 * not been read yet, and a two-state walk that engages at a nudge would be a
 * regression -- and lowers it for left/right to just above the deadzone, so
 * the turning state engages at any deliberate deflection and the integrator
 * gets to use the magnitude. 16 rather than 0: a straight forward push leaks
 * a little X once it clears the 30-unit circle, and that must not turn.
 * Precedence is the original's: right before left, forward before back. */

enum { STICK_THRESHOLD = 0x0042B784u, MODERN_TURN_THRESHOLD = 16 };

void psp_func_00279A50(void) {
    if (input_mode() != INPUT_MODERN) { psp_func_00279A50__orig(); return; }

    const int ax = (int8_t)(psp_cpu.r[5] & 0xFF);       /* a1 */
    const int ay = (int8_t)(psp_cpu.r[6] & 0xFF);       /* a2 */
    const int ty = (int32_t)psp_read32(STICK_THRESHOLD);
    const int tx = MODERN_TURN_THRESHOLD;

    uint32_t bits = 0;
    if      (ax >  tx) bits  = 0x8000u;
    else if (ax < -tx) bits  = 0x4000u;
    if      (ay < -ty) bits |= 0x1000u;
    else if (ay >  ty) bits |= 0x2000u;
    r_v0 = bits;
}
