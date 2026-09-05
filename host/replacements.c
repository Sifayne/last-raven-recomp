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
 *   modern  the game's own stick, with its magnitude honoured: the yaw rate
 *           and the walk speed are the game's caps scaled by how far the
 *           stick is pushed, and both states engage at a deliberate
 *           deflection instead of at 100/127.
 *   dual    the second stick looks and the first moves: right X is yaw and
 *           right Y pitch, both proportional through an expo curve, pitch
 *           eased in at the game's own ramp; the left stick walks the AC in
 *           the direction it points at a speed set by its deflection, so a
 *           strafe and a walk are one push at an angle. Mouse motion, when
 *           the host provides it, is yaw and pitch as a displacement at the
 *           same radians per count on both axes (PSPRECOMP_MOUSE_SENS scales
 *           it, default 1.0 = 0.001 rad per count).
 *
 * PSPRECOMP_INPUT_LOG=<file> writes one line per call of each replaced
 * integrator, in every mode -- the deferring ones read the result back after
 * the original ran -- so a law can be measured to the frame without a window.
 */

#include <math.h>
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

/* PSPRECOMP_INPUT_LOG=<file>, shared by every replacement here; NULL when
 * unset. Opened once, on the first call. */
static FILE *input_log(void) {
    static FILE *log; static int init;
    if (!init) {
        init = 1;
        const char *p = getenv("PSPRECOMP_INPUT_LOG");
        if (p && *p) log = fopen(p, "w");
    }
    return log;
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

/* The look stick's response: an expo blend, (1-e)x + e x^3. Full deflection
 * is still the full rate; half deflection is 0.29 of it rather than 0.5, so
 * the centre is for aiming and the edge for turning round. The same curve
 * on both axes of the stick, or a diagonal push would feel bent. */
static float look_curve(float x) {
    const float e = 0.6f;
    return (1.0f - e) * x + e * x * x * x;
}

/* The left stick as a direction and a magnitude for the walk: the unit vector
 * in the AC's frame and how far past the game's 30-unit deadzone circle the
 * stick sits, 0..1. Zero inside the circle.
 *
 * The frame is the game's direction table's: z back, and **x to the left** --
 * the table's entry 3 is (+1, 0) and the game orders its stick actions
 * forward, back, left, right, so 3 is left; the first cut read it as right
 * and the first mission played with the strafes swapped. The stick's x is to
 * the right, hence the sign. */
static float stick_walk(uint8_t axb, uint8_t ayb, float *sx, float *sz) {
    const float ax = (float)((int)axb - 128), ay = (float)((int)ayb - 128);
    const float r = sqrtf(ax * ax + ay * ay);
    const float dead = 30.0f;                 /* psp_func_00279910's circle */
    if (r <= dead) { *sx = 0.0f; *sz = 0.0f; return 0.0f; }
    *sx = -ax / r;
    *sz = ay / r;
    float m = (r - dead) / (127.0f - dead);
    return m > 1.0f ? 1.0f : m;
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

/* The pause gate the integrators honour. psp_func_000506F8(ac) is
 * *(*(ac+9728)+276) + 5808 -- an object hanging off the AC's movement state --
 * and the s16 at +24 of that is non-zero while the game is paused. Read in C
 * rather than by calling the guest: the guest getter takes the AC in a0, and
 * a replacement that called it from a context where a0 was something else
 * dereferenced that something. (The first in-play gate did exactly that from
 * the stick converter, whose a0 is not an AC, and it read garbage twice a
 * poll until the bad-access counter said so.) NULL where the original's
 * `bnel v0, zero` says NULL. */
static int game_paused(uint32_t ac) {
    const uint32_t state = psp_read32(ac + AC_STATE_PTR);
    const uint32_t g = psp_read32(state + 276) + 5808;
    return g && (int16_t)(psp_read32(g + 24) & 0xFFFFu) != 0;
}

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

    /* One line per call, so "did the game even ask for a turn this frame" is a
     * fact and not a guess. Which of the fourteen movement handlers calls this
     * is the game's decision, made upstream from the thresholded bits. */
    FILE *log = input_log();
    if (log) fprintf(log, "%u ac=%08X ax=%u rx=%u mdx=%d gate=%d max=%.5f",
                     psp_ctrl_polls(), ac, ax, rx, mdx, gated, max);

    if (gated) { if (log) fprintf(log, "\n"); r_v0 = 0; return; }

    /* The pause gate, as the original. */
    const int paused = game_paused(ac);

    /* Left is negative radians here, matching the original's sign; mouse
     * travel to the right is positive yaw. The look stick gets the expo
     * curve; the game's own stick in `modern` stays linear, since it is also
     * the walk stick. No ease-in here: the chase camera's own filter smooths
     * yaw, and the ramp was the other half of what felt clunky. */
    float rate = max * (mode == INPUT_DUAL ? look_curve(stick_axis(rx))
                                            : stick_axis(ax));
    if (mode == INPUT_DUAL) rate += mouse_sens() * (float)mdx;

    f32_write(ac + AC_RATE, rate);
    if (rate != 0.0f) psp_write8(ac + AC_DIR, rate < 0.0f ? 0 : 1);
    if (!paused) f32_write(ac + AC_YAW, f32_read(ac + AC_YAW) + rate);
    if (log) fprintf(log, " rate=%.6f\n", rate);

    r_v0 = (uint32_t)(int32_t)(rate > 0.0f ? 1 : rate < 0.0f ? -1 : 0);
}

/* ---- the look (pitch) integrator ---------------------------------------------
 *
 * psp_func_00053234(a0 = the AC), once per frame from psp_func_0004CF74 for the
 * AC under control, straight after the auto-face pass psp_func_00053048. Its
 * state is a small struct at ac+8160: a lockout counter at +4, the pitch angle
 * at +16 (radians, up positive), the pitch rate at +32. Its constants are six
 * floats at 0x0030ADC0 -- [0] 27.0, the lockout after a recentre; [1] 0.00136
 * accel; [2] 0.009, a decel the code computes and then overwrites with zero;
 * [3] 0.027 max rate; [4] -1.1781 and [5] +1.1781, the clamp: 67.5 degrees
 * either way. The shipped path, read from the listing:
 *
 *   - gate: the byte at (*(ac+9728))+182 must be -1, as the yaw integrator;
 *   - triangle and circle together recentre -- angle and rate to zero -- and
 *     start the lockout; while the counter is positive it counts down and
 *     nothing else happens;
 *   - triangle held (action 10): the rate approaches +2*max by +2*accel,
 *     twice a frame: 0.00544 rad/frame^2 to 0.054 rad/frame, 3.09 deg/frame
 *     against yaw's 2.10. Circle (action 11) is the mirror;
 *   - neither held: the rate is zero. The release stops dead;
 *   - angle += rate, clamped; at a limit the rate is zeroed.
 *
 * That is the whole of why mouse look felt wrong: pitch was a button, so any
 * travel at all was a full-rate press, faster than the yaw beside it and
 * with no relation to how far the hand moved. Under `dual` the right stick's
 * Y is a rate in the game's own cap and mouse Y a displacement at the same
 * radians per count as yaw, into the same three words, with the same clamp.
 * The buttons and the lockout stay the game's: when either is in play this
 * defers, and since both laws act on the same state they compose. */

enum { AC_LOOK_COUNTER = 8164, AC_PITCH = 8176, AC_PITCH_RATE = 8192,
       AC_PAD_PTR = 9732, LOOK_PARAMS = 0x0030ADC0u };

static int in_play(void);              /* defined with the converter, below */

/* psp_func_0005EFA0(pad, action): the pad's button word against the
 * key-assign mask for one action, where pad = *(*(ac+9732)+4). The registers
 * it clobbers are caller-saved, and a replacement is the callee. */
static int pressed(uint32_t ac, uint32_t action) {
    r_a0 = psp_read32(psp_read32(ac + AC_PAD_PTR) + 4);
    r_a1 = action;
    psp_func_0005EFA0();
    return r_v0 != 0;
}

static void pitch_log(uint32_t ac, const char *how, int ry, int mdy) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u pitch=%.6f rate=%.6f ry=%d mdy=%d %s\n",
                     psp_ctrl_polls(), f32_read(ac + AC_PITCH),
                     f32_read(ac + AC_PITCH_RATE), ry, mdy, how);
}

void psp_func_00053234(void) {
    const uint32_t ac = r_a0;
    if (input_mode() != INPUT_DUAL || ac != PLAYER_AC) {
        psp_func_00053234__orig();
        if (ac == PLAYER_AC) pitch_log(ac, "orig", 0, 0);
        return;
    }

    /* The state and pause gates, as the yaw integrator. */
    if (!in_play()) return;

    uint8_t rx, ry;
    int mdx, mdy;
    psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);

    /* The game's own digital look, its recentre and its lockout. */
    if ((int32_t)psp_read32(ac + AC_LOOK_COUNTER) > 0 ||
        pressed(ac, 10) || pressed(ac, 11)) {
        r_a0 = ac;
        psp_func_00053234__orig();
        pitch_log(ac, "orig-button", (int)ry - 128, mdy);
        return;
    }

    const float accel = 4.0f * f32_read(LOOK_PARAMS + 4);  /* 2*accel, twice a frame */
    const float max   = 2.0f * f32_read(LOOK_PARAMS + 12);
    const float lo    = f32_read(LOOK_PARAMS + 16);
    const float hi    = f32_read(LOOK_PARAMS + 20);

    /* Stick up is a smaller byte; mouse away from the player is negative dy;
     * both look up, which is positive here.
     *
     * The stick is a rate: the cap times the expo curve of the deflection,
     * eased in at the game's own ramp -- the stored rate at ac+8192 climbs
     * toward the target by 4*accel a frame, ten frames from rest to the
     * cap, as triangle did -- but eased in only. Easing off, reversing and
     * releasing take effect at once, as the game's dead stop did, because a
     * rate that coasts after the thumb has stopped is the floaty feel that
     * makes aiming miss. The filter's state is the game's own word, so a
     * replay reproduces it and a frame the buttons handled hands over
     * smoothly. The mouse is a displacement and stays raw. */
    const float target = -max * look_curve(stick_axis(ry));
    float rate = f32_read(ac + AC_PITCH_RATE);
    const int same_way = (target > 0.0f && rate > 0.0f) || (target < 0.0f && rate < 0.0f);
    if (target == 0.0f || (rate != 0.0f && !same_way) ||
        (same_way && fabsf(target) < fabsf(rate))) {
        rate = target;
    } else if (target > rate) {
        rate += accel; if (rate > target) rate = target;
    } else {
        rate -= accel; if (rate < target) rate = target;
    }

    float angle = f32_read(ac + AC_PITCH) + rate - mouse_sens() * (float)mdy;
    if (angle > hi) { angle = hi; rate = 0.0f; }
    if (angle < lo) { angle = lo; rate = 0.0f; }
    f32_write(ac + AC_PITCH, angle);
    f32_write(ac + AC_PITCH_RATE, rate);
    pitch_log(ac, "dual", (int)ry - 128, mdy);
}

/* ---- the push: where the AC goes, and how fast ----------------------------------
 *
 * psp_func_0004F06C(a0 = the AC, a1 = direction, f12 = accel, f13 = cap) is the
 * one primitive every moving state uses -- the walk handler psp_func_000440B4,
 * the jump, the boosts. `a1` indexes eight unit vectors at 0x0030AEB0 in the
 * AC's frame (x left, z back: 0 forward, 1 forward-left, 2 forward-right, 3
 * left, 4 right, 5 back, 6 back-left, 7 back-right); the vector times `accel`
 * goes into the scratch at 0x0030AF00, psp_func_002B007C/00255BBC rotate it by
 * the yaw at ac+36 on the VFPU, and psp_func_0004EE9C(ac, vec, cap) adds four
 * times it to the velocity at ac+80/88 and scales the speed back to `cap` when
 * it passes it -- or, when the AC was already faster than the cap, down toward
 * it by 4 * ac+1412 a frame. The walk handler passes accel = ac+1392 and
 * cap = 2 * ac+1400, the legs' own numbers, and every handler passes them
 * whole; the stick chose one of eight directions and the speed was always
 * the cap. That is the two-state walk.
 *
 * Here, for the player under `dual`, the direction is the left stick's own
 * and the cap is scaled by how far it is pushed past the game's deadzone, so
 * the AC creeps at a nudge and walks at the edge, in the direction pointed --
 * a walk and a strafe are one push at an angle, not two states. `modern`
 * keeps the game's direction (that stick's X is the turn) and scales the cap
 * by Y alone. The accel, the clamp, the overspeed decay and the animation
 * are the game's; a centred stick, which can happen inside a jump or a
 * boost, defers entirely. The rotation is done by calling the game's own
 * helpers on a guest stack frame, exactly as the original does, rather than
 * re-deriving VFPU arithmetic in C. */

enum { AC_VX = 80, AC_VZ = 88, PUSH_DIR_TABLE = 0x0030AEB0u,
       PUSH_SCRATCH = 0x0030AF00u, PUSH_BASE_MATRIX = 0x002F0550u };

/* The position at ac+16/24 is logged too: a speed that collapses while the
 * stick holds still is a wall, and the position says so where the speed
 * alone cannot. */
enum { AC_X = 16, AC_Z = 24 };

static void push_log(uint32_t ac, const char *how, float m, float sx, float sz, float cap) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u push m=%.3f dir=%+.3f,%+.3f cap=%.4f speed=%.4f pos=%.3f,%.3f %s\n",
                     psp_ctrl_polls(), m, sx, sz, cap,
                     hypotf(f32_read(ac + AC_VX), f32_read(ac + AC_VZ)),
                     f32_read(ac + AC_X), f32_read(ac + AC_Z), how);
}

void psp_func_0004F06C(void) {
    const uint32_t ac = r_a0, idx = r_a1;
    const float accel = psp_cpu.f[12], cap = psp_cpu.f[13];
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC || ac != PLAYER_AC || !in_play()) {
        psp_func_0004F06C__orig();
        if (ac == PLAYER_AC) push_log(ac, "orig", 1.0f, 0.0f, 0.0f, cap);
        return;
    }

    uint8_t axb, ayb;
    psp_ctrl_last_stick(&axb, &ayb);
    float sx, sz, m = stick_walk(axb, ayb, &sx, &sz);
    if (mode == INPUT_MODERN) {
        /* The game's direction, the magnitude from Y alone. */
        const float ay = fabsf((float)((int)ayb - 128));
        m = ay <= 30.0f ? 0.0f : (ay - 30.0f) / 97.0f;
        if (m > 1.0f) m = 1.0f;
        if (idx > 7) m = 0.0f;
        else { sx = f32_read(PUSH_DIR_TABLE + 8 * idx); sz = f32_read(PUSH_DIR_TABLE + 8 * idx + 4); }
    }
    if (m <= 0.0f) {
        r_a0 = ac; r_a1 = idx; psp_cpu.f[12] = accel; psp_cpu.f[13] = cap;
        psp_func_0004F06C__orig();
        push_log(ac, "orig-centred", 1.0f, 0.0f, 0.0f, cap);
        return;
    }

    /* The body-frame vector, scaled by the accel, as the original writes it. */
    f32_write(PUSH_SCRATCH + 0, accel * sx);
    f32_write(PUSH_SCRATCH + 8, accel * sz);

    /* Rotate it by the yaw: the original's own calls, on the original's own
     * frame. r_sp is put back before returning, which is what the call site
     * checks. */
    const uint32_t sp = r_sp;
    r_sp -= 80;
    r_a0 = r_sp; r_a1 = PUSH_BASE_MATRIX; psp_cpu.f[12] = f32_read(ac + AC_YAW);
    psp_func_002B007C();
    r_a0 = PUSH_SCRATCH; r_a1 = r_sp; r_a2 = PUSH_SCRATCH;
    psp_func_00255BBC();
    r_sp = sp;

    /* Accelerate toward it, with the cap the stick asked for. */
    r_a0 = ac; r_a1 = PUSH_SCRATCH; psp_cpu.f[12] = cap * m;
    psp_func_0004EE9C();
    push_log(ac, mode == INPUT_DUAL ? "dual" : "modern", m, sx, sz, cap * m);
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
 * `modern` lowers the thresholds: left/right to just above the deadzone, so
 * the turning state engages at any deliberate deflection and the integrator
 * gets to use the magnitude -- 16 rather than 0, because a straight forward
 * push leaks a little X once it clears the 30-unit circle, and that must not
 * turn -- and forward/back to 40, the same radius `dual` walks at, now that
 * the push above scales the speed by the deflection and a walk that engages
 * at a nudge is a creep rather than a march.
 *
 * `dual` re-sources the bits: turning from the right stick's X (or from mouse
 * travel, held for a few polls after the last motion so the state does not
 * flicker at the mouse's rate), strafe from the left stick's X as the L/R
 * bits, forward/back from the left stick's Y. Look up/down is not asked for
 * here at all: the pitch integrator above reads the right stick and the mouse
 * itself, and the triangle/circle bits are left to the physical buttons. The
 * bits only choose the movement *state* -- and with it the animation -- the
 * push above sets the direction and the speed from the stick itself.
 *
 * Precedence is the original's: right before left, forward before back. */

enum { MODERN_TURN_THRESHOLD = 16, MODERN_WALK_THRESHOLD = 40,
       MOUSE_HOLD_POLLS = 4 };

/* Mouse travel arrives at the mouse's rate and this runs at the guest's; a
 * bit that dropped the poll after a motion event would flicker the state
 * machine. Keep the direction for a few polls. Per poll, not per call: the
 * adaptor calls this once for each of two virtual pads. */
static struct { uint32_t poll; int x_left; int sx; } g_mouse_hold;

static void mouse_hold_update(int mdx) {
    const uint32_t poll = psp_ctrl_polls();
    if (poll == g_mouse_hold.poll) return;
    g_mouse_hold.poll = poll;
    if (g_mouse_hold.x_left > 0) g_mouse_hold.x_left--;
    if (mdx) { g_mouse_hold.x_left = MOUSE_HOLD_POLLS; g_mouse_hold.sx = mdx > 0 ? 1 : -1; }
}

/* Is the player being played? The converter runs everywhere the pad is read
 * -- menus, the garage, the intro -- and the first windowed try of `dual`
 * showed why that matters: mouse jitter read as circle, which cancels menus,
 * and an off-axis push read as L/R, which changes tabs. The game's own
 * answer is the one its yaw integrator uses: the player's movement-state
 * object at ac+9728, which is NULL in the garage and set in a mission, with
 * its byte at +182 equal to -1 while the AC is under control; and the pause
 * flag the integrator also honours. Outside that, every mode is the shipped
 * converter, bit for bit. */
static int in_play(void) {
    const uint32_t state = psp_read32(PLAYER_AC + AC_STATE_PTR);
    if (!state || (int8_t)psp_read8(state + 182) != -1) return 0;
    return !game_paused(PLAYER_AC);
}

void psp_func_00279A50(void) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC) { psp_func_00279A50__orig(); return; }

    const int ax = (int8_t)(psp_cpu.r[5] & 0xFF);       /* a1 */
    const int ay = (int8_t)(psp_cpu.r[6] & 0xFF);       /* a2 */
    if (!in_play()) {
        /* The originals read a1/a2 themselves; hand them back untouched. */
        psp_cpu.r[5] = (uint32_t)(uint8_t)ax;
        psp_cpu.r[6] = (uint32_t)(uint8_t)ay;
        psp_func_00279A50__orig();
        return;
    }
    const int tx = MODERN_TURN_THRESHOLD, ty = MODERN_WALK_THRESHOLD;

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
    mouse_hold_update(mdx);
    const int rx = (int)rxb - 128;
    (void)ryb; (void)mdy;                  /* the pitch integrator's, above */

    /* Turn: right stick X, else recent mouse X. */
    if      (rx >  tx) bits = 0x8000u;
    else if (rx < -tx) bits = 0x4000u;
    else if (g_mouse_hold.x_left > 0) bits = g_mouse_hold.sx > 0 ? 0x8000u : 0x4000u;

    /* Movement: the left stick as eight sectors, not two axes.
     *
     * The game's own map thresholds each axis at 100 of 127 on its own, which
     * on a round stick makes "forward" a cone of about +/-38 degrees, leaves
     * every diagonal dead, and puts the corner where two directions would
     * both pass outside the stick's reach. This is the "narrow" in the feel.
     * Here a direction is asked for when the stick points within 67.5
     * degrees of it, so each of the four spans 135 degrees and a diagonal
     * asks for two at once -- a walk and a strafe together, which the game
     * does support, it just could never be told to. The magnitude gate sits
     * just above the game's 30-unit deadzone circle. The actions themselves
     * are still the game's two-state walk and strafe; only the asking is
     * proportional to where the stick points. */
    {
        const int r2 = ax * ax + ay * ay;
        if (r2 >= 40 * 40) {
            /* cos 67.5 deg = 0.383; compare squares to stay in integers:
             * |component| > 0.383 r  <=>  component^2 > 0.1464 r^2 */
            const int fwd = -ay, back = ay, right = ax, left = -ax;
            if (fwd   > 0 && fwd   * fwd   * 1000 > 146 * r2) bits |= 0x1000u;
            if (back  > 0 && back  * back  * 1000 > 146 * r2) bits |= 0x2000u;
            if (right > 0 && right * right * 1000 > 146 * r2) bits |= 0x200u;
            if (left  > 0 && left  * left  * 1000 > 146 * r2) bits |= 0x100u;
        }
    }
    r_v0 = bits;
}
