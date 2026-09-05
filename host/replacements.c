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
 * goes through the recorded path -- which is why the look channel below is a
 * lane of the sceCtrl HLE and not a side door.
 *
 * Prefer deferring. Calling psp_func_<addr>__orig() and adjusting around it is
 * both safer and more honest than reimplementing a function whose full
 * behaviour nobody has established. A replacement that only intervenes in the
 * case it understands, and defers otherwise, is the shape to aim for.
 * ---------------------------------------------------------------------------
 *
 * PSPRECOMP_INPUT selects the scheme; unset or `classic`, every replacement
 * here defers to the original, so a run without it is the game as shipped and
 * every gate stays what it was.
 *
 *   modern  the game's own stick, with its magnitude honoured: the yaw rate is
 *           the cap scaled by how far the stick is pushed, and the turning
 *           state engages at any deliberate deflection instead of at 100/127.
 *   dual    the second stick looks and the first moves: right X is yaw, right
 *           Y look up/down, left X strafe, left Y forward/back as before; mouse
 *           motion, when the host provides it, is yaw and pitch as well
 *           (PSPRECOMP_MOUSE_SENS scales it, default 1.0 = 0.001 rad per
 *           count).
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

enum { INPUT_CLASSIC = 0, INPUT_MODERN = 1, INPUT_DUAL = 2 };

static int input_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("PSPRECOMP_INPUT");
        mode = INPUT_CLASSIC;
        if (e && !strcmp(e, "modern")) mode = INPUT_MODERN;
        if (e && !strcmp(e, "dual"))   mode = INPUT_DUAL;
        if (mode == INPUT_MODERN)
            printf("      input     modern -- yaw rate proportional to the stick; "
                   "PSPRECOMP_INPUT=classic for the game's own\n");
        if (mode == INPUT_DUAL)
            printf("      input     dual -- right stick and mouse look, left stick "
                   "moves; PSPRECOMP_INPUT=classic for the game's own\n");
    }
    return mode;
}

/* Radians of yaw per mouse count. 0.001 puts a full turn at ~6,300 counts,
 * about 20 cm of desk at 800 dpi -- a middling FPS default. */
static float mouse_sens(void) {
    static float k = -1.0f;
    if (k < 0.0f) {
        const char *e = getenv("PSPRECOMP_MOUSE_SENS");
        const float m = (e && *e) ? (float)atof(e) : 1.0f;
        k = 0.001f * (m > 0.0f ? m : 1.0f);
    }
    return k;
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

/* A stick byte as -1..1 with a small deadzone, for the yaw law. */
static float stick_axis(uint8_t v) {
    float x = ((int)v - 128) / 127.0f;
    if (x >  1.0f) x =  1.0f;
    if (x < -1.0f) x = -1.0f;
    const float dead = 0.06f;                 /* ~8 of 127: stick noise only */
    if (x > -dead && x < dead) x = 0.0f;
    return x;
}

/* ---- the AC's yaw integrator ------------------------------------------------
 *
 * psp_func_0004F248(a0 = the AC object, f12 = accel, f13 = max rate), called
 * by each of the fourteen movement-state handlers while the turning state is
 * engaged. The original, read from its listing and confirmed by measurement
 * (docs/findings/state.md, Controls):
 *
 *   - the yaw rate persists at ac+8312 (float, radians per frame), the yaw
 *     itself at ac+36; a direction byte at ac+8351 records which way;
 *   - gate: the byte at (*(ac+9728))+182 must be -1, else nothing happens;
 *   - the pad reaches it as *buttons*. Its default path asks pressed(2) /
 *     pressed(3) -- the stick has already been thresholded into those two bits
 *     upstream, at 100 of 127 -- and moves the rate by 4*accel per frame
 *     toward the cap, or back toward zero with nothing held. That is the
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
 * pushed -- the left stick in `modern`, the right in `dual`, plus mouse travel
 * as a displacement, uncapped, in `dual`. No ramp: the chase camera already
 * smooths the result, and the ramp was the other half of what felt clunky.
 * Everything it reads comes through the sceCtrl lanes, so a recording replays
 * it exactly. */

enum { AC_YAW = 36, AC_RATE = 8312, AC_DIR = 8351, AC_STATE_PTR = 9728 };

/* The player's AC is a static in the module's BSS: 0x25F0 bytes cleared at
 * mission start by the memset psp_func_002AD4D0, from psp_func_00262A70. Every
 * AC in the mission -- the enemies too -- runs the same movement code and the
 * same integrator, so a replacement that did not check would steer them all
 * with the player's stick. */
enum { PLAYER_AC = 0x0042D6C0u };

void psp_func_0004F248(void) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC || r_a0 != PLAYER_AC) {
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

    uint8_t ax, ay, rx, ry;
    int mdx, mdy;
    psp_ctrl_last_stick(&ax, &ay);
    psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);

    /* PSPRECOMP_INPUT_LOG=<file>: one line per call, so "did the game even ask
     * for a turn this frame" is a fact and not a guess. Which of the fourteen
     * movement handlers calls this is the game's decision, made upstream from
     * the thresholded bits. */
    static FILE *log; static int log_init;
    if (!log_init) {
        log_init = 1;
        const char *p = getenv("PSPRECOMP_INPUT_LOG");
        if (p && *p) log = fopen(p, "w");
    }
    if (log) fprintf(log, "%u ac=%08X ax=%u rx=%u mdx=%d gate=%d max=%.5f\n",
                     psp_ctrl_polls(), ac, ax, rx, mdx, gated, max);

    if (gated) { r_v0 = 0; return; }

    /* The pause gate, as the original: a global object, s16 at +24. */
    psp_func_000506F8();
    const uint32_t g = r_v0;
    const int paused = g && (int16_t)(psp_read32(g + 24) & 0xFFFFu) != 0;

    /* Left is negative radians here, matching the original's sign; mouse
     * travel to the right is positive yaw. */
    float rate = max * stick_axis(mode == INPUT_DUAL ? rx : ax);
    if (mode == INPUT_DUAL) rate += mouse_sens() * (float)mdx;

    f32_write(ac + AC_RATE, rate);
    if (rate != 0.0f) psp_write8(ac + AC_DIR, rate < 0.0f ? 0 : 1);
    if (!paused) f32_write(ac + AC_YAW, f32_read(ac + AC_YAW) + rate);

    r_v0 = (uint32_t)(int32_t)(rate > 0.0f ? 1 : rate < 0.0f ? -1 : 0);
}

/* ---- the stick, as buttons ----------------------------------------------------
 *
 * psp_func_00279A50(state, ax, ay): the PSP stick, already through a circular
 * deadzone of radius 30 (psp_func_00279910), becomes four virtual button bits
 * -- 0x8000 right, 0x4000 left, 0x1000 forward, 0x2000 back -- whenever an
 * axis passes the threshold the game keeps at 0x0042B784: 100 of 127. That
 * one compare is where the stick's travel is thrown away. Physical buttons
 * are remapped into bits 0-11 separately (d-pad 0-3, circle 0x10, cross
 * 0x20, triangle 0x40, square 0x80, L 0x100, R 0x200, select, start), so the
 * four are all the game ever learns about the stick's direction; the
 * key-assign rows bind actions 0-3 to exactly them, and the movement state
 * machine chooses its handler by them. Below the threshold the yaw integrator
 * above is not even called.
 *
 * `modern` keeps the game's threshold for forward/back -- the walk law has not
 * been read, and a two-state walk that engages at a nudge would be a
 * regression -- and lowers it for left/right to just above the deadzone, so
 * the turning state engages at any deliberate deflection and the integrator
 * gets to use the magnitude. 16 rather than 0: a straight forward push leaks
 * a little X once it clears the 30-unit circle, and that must not turn.
 *
 * `dual` re-sources the bits: turning from the right stick's X (or from mouse
 * travel, held for a few polls after the last motion so the state does not
 * flicker at the mouse's rate), look up/down from the right stick's Y (or
 * mouse Y) as the triangle/circle bits the game's own look code answers to,
 * strafe from the left stick's X as the L/R bits, forward/back from the left
 * stick's Y unchanged. The game's strafe and look are still its own, still
 * two-state; what changed is which hand asks for them.
 *
 * Precedence is the original's: right before left, forward before back. */

enum { STICK_THRESHOLD = 0x0042B784u, MODERN_TURN_THRESHOLD = 16,
       DUAL_LOOK_THRESHOLD = 48, MOUSE_HOLD_POLLS = 4 };

/* Mouse travel arrives at the mouse's rate and this runs at the guest's; a
 * bit that dropped the poll after a motion event would flicker the state
 * machine. Keep the direction for a few polls. Per poll, not per call: the
 * adaptor calls this once for each of two virtual pads. */
static struct { uint32_t poll; int x_left, y_left; int sx, sy; } g_mouse_hold;

static void mouse_hold_update(int mdx, int mdy) {
    const uint32_t poll = psp_ctrl_polls();
    if (poll == g_mouse_hold.poll) return;
    g_mouse_hold.poll = poll;
    if (g_mouse_hold.x_left > 0) g_mouse_hold.x_left--;
    if (g_mouse_hold.y_left > 0) g_mouse_hold.y_left--;
    if (mdx) { g_mouse_hold.x_left = MOUSE_HOLD_POLLS; g_mouse_hold.sx = mdx > 0 ? 1 : -1; }
    if (mdy) { g_mouse_hold.y_left = MOUSE_HOLD_POLLS; g_mouse_hold.sy = mdy > 0 ? 1 : -1; }
}

void psp_func_00279A50(void) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC) { psp_func_00279A50__orig(); return; }

    const int ax = (int8_t)(psp_cpu.r[5] & 0xFF);       /* a1 */
    const int ay = (int8_t)(psp_cpu.r[6] & 0xFF);       /* a2 */
    const int ty = (int32_t)psp_read32(STICK_THRESHOLD);
    const int tx = MODERN_TURN_THRESHOLD;

    uint32_t bits = 0;
    if (mode == INPUT_MODERN) {
        if      (ax >  tx) bits  = 0x8000u;
        else if (ax < -tx) bits  = 0x4000u;
        if      (ay < -ty) bits |= 0x1000u;
        else if (ay >  ty) bits |= 0x2000u;
        r_v0 = bits;
        return;
    }

    uint8_t rxb, ryb;
    int mdx, mdy;
    psp_ctrl_last_look(&rxb, &ryb, &mdx, &mdy);
    mouse_hold_update(mdx, mdy);
    const int rx = (int)rxb - 128, ry = (int)ryb - 128;

    /* Turn: right stick X, else recent mouse X. */
    if      (rx >  tx) bits = 0x8000u;
    else if (rx < -tx) bits = 0x4000u;
    else if (g_mouse_hold.x_left > 0) bits = g_mouse_hold.sx > 0 ? 0x8000u : 0x4000u;
    /* Forward/back: left stick Y, the game's own threshold. */
    if      (ay < -ty) bits |= 0x1000u;
    else if (ay >  ty) bits |= 0x2000u;
    /* Strafe: left stick X as L/R. */
    if      (ax >  tx) bits |= 0x200u;
    else if (ax < -tx) bits |= 0x100u;
    /* Look up/down: right stick Y, else recent mouse Y, as triangle/circle.
     * Stick up and mouse away from the player both look up. */
    if      (ry < -DUAL_LOOK_THRESHOLD) bits |= 0x40u;
    else if (ry >  DUAL_LOOK_THRESHOLD) bits |= 0x10u;
    else if (g_mouse_hold.y_left > 0)   bits |= g_mouse_hold.sy < 0 ? 0x40u : 0x10u;

    r_v0 = bits;
}
