/* The controls, for the AC3-generation titles: the stick converter, the yaw
 * integrator, the look (pitch) controller and the walk's push.
 *
 * Armored Core 3 Portable and Silent Line Portable share their pad layer with
 * Last Raven instruction for instruction (docs/findings/sibling-titles.md):
 * the same 26-instruction stick -> virtual-bits converter, called per virtual
 * pad with the stick's bytes in a1/a2, returning the four stick bits (0x1000
 * forward, 0x2000 back, 0x4000 left, 0x8000 right) after thresholding each
 * axis at 100 of 127 on its own. This is Last Raven's replacement of that
 * function (host/replacements.c, psp_func_00279A50). It decides what the
 * stick *asks* for; the integrators below decide how far the AC goes:
 *
 *   modern  the left stick's X turns from 4% of travel instead of 79%, and
 *           its Y walks from 4%; the turn rate and the walk's speed follow
 *           the deflection.
 *   dual    the left stick moves as eight sectors (a diagonal walks and
 *           strafes together, through the L/R virtual bits 0x100/0x200) and
 *           the walk goes where it points, as fast as it is pushed; the
 *           right stick's X, or the mouse's, turns; its Y, or the mouse's,
 *           looks -- rates, not buttons, in the game's own caps. Triangle
 *           and circle still look the game's own way.
 *
 * The including file provides, before this header:
 *     #include "<prefix>_funcs.h"                     the title's emitted header
 *     static int unit_list_live(void);               is a sortie loaded
 *     #define AC3_STICK_CONVERTER <hex address>       the title's twin of 00279A50
 * plus what each section below asks for, and lists every replaced address in
 * its host/replace-<slug>.txt. */
#ifndef AC3_CONTROLS_H
#define AC3_CONTROLS_H

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <math.h>

#include "psprecomp/cpu.h"
#include "psprecomp/mem.h"
#include "psprecomp/hle.h"
#include "psprecomp/vfpu.h"     /* psp_lv_q, psp_vtfm, psp_sv_q: the push's rotation */
#include "controls.h"           /* LR_PAD_*: the modern pad's carrier bits */
#include "present.h"            /* present_adaptive_aspect(), the wide width */
#include "settings.h"

static int input_mode(void);
static int in_play(void);
#include "stick.h"

/* Is the player being played? Two signs together, as Last Raven's in_play
 * (host/replacements.c): the title's own sign that a sortie is loaded -- the
 * unit-object list's head, unit_list_live() -- and a heartbeat, the poll at
 * which the player's AC last ran its per-frame update, which the look
 * controller below records because that update calls it once a frame. The
 * list alone is not enough: on Silent Line it stays set through the
 * mission's results and the menus after them (Sif's recording, 9 Sep: set
 * from the sortie to the recording's end at 5,305 polls) while the AC's
 * update stops at 5,137, so a gate on the list alone would leave the modern
 * pad's face buttons meaning gameplay actions in menus that need cross. */
static int unit_list_live(void);
static uint32_t g_ac_update_poll;
static int in_play(void) {
    return unit_list_live() && psp_ctrl_polls() - g_ac_update_poll <= 2;
}

static float f32_read(uint32_t addr) {
    const uint32_t v = psp_read32(addr); float f; memcpy(&f, &v, sizeof f); return f;
}
static void f32_write(uint32_t addr, float f) {
    uint32_t v; memcpy(&v, &f, sizeof v); psp_write32(addr, v);
}

/* Radians of yaw per mouse count. 0.001 puts a full turn at ~6,300 counts,
 * about 20 cm of desk at 800 dpi -- a middling FPS default. */
static float mouse_sens(void) {
    static float k = -1.0f;
    if (k < 0.0f) k = 0.001f * (float)lr_settings_current()->number[LR_MOUSE_SENS];
    return k;
}

#define AC3_CAT_(a, b) a##b
#define AC3_CAT(a, b)  AC3_CAT_(a, b)
#define AC3_FN(addr)   AC3_CAT(psp_func_, addr)
#define AC3_ORIG(addr) AC3_CAT(AC3_FN(addr), __orig)

/* Button layout is independently overridable so the analog work can be
 * compared with the PSP buttons, or the modern buttons can be used with the
 * original one-stick movement. By default it follows modern/dual. */
static int gamepad_modern(void) {
    static int modern = -1;
    if (modern < 0) {
        modern = lr_settings_current()->gamepad;
        if (modern)
            printf("      gamepad   modern -- triggers, bumpers and stick clicks are gameplay actions\n");
    }
    return modern;
}

static int input_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = (int)lr_settings_current()->number[LR_INPUT];
        if (mode == INPUT_MODERN)
            printf("      input     modern -- the stick asks to turn and walk from a light touch; "
                   "PSPRECOMP_INPUT=classic for the game's own\n");
        if (mode == INPUT_DUAL)
            printf("      input     dual -- right stick and mouse turn and look, left stick "
                   "moves in eight sectors; PSPRECOMP_INPUT=classic for the game's own\n");
    }
    return mode;
}

/* PSPRECOMP_INPUT_LOG=<file>; NULL when unset. Opened once, on the first call. */
static FILE *input_log(void) {
    static FILE *log; static int init;
    if (!init) {
        init = 1;
        const char *p = getenv("PSPRECOMP_INPUT_LOG");
        if (p && *p) log = fopen(p, "w");
    }
    return log;
}

/* Mouse travel arrives at the mouse's rate and this runs at the guest's; a
 * bit that dropped the poll after a motion event would flicker the state
 * machine. Keep each direction for a few polls. Per poll, not per call: the
 * adaptor calls this once for each of two virtual pads.
 *
 * The X travel itself is banked, and the yaw integrator draws on the bank
 * rather than on the poll's own delta: the integrator runs only inside the
 * turn state, which the bits above engage a few frames after they rise, so
 * a flick's first polls would otherwise be lost. Banked, they are applied on
 * the integrator's first frame instead. */
enum { MOUSE_HOLD_POLLS = 4 };
static int g_move_gate, g_turn_gate;
static struct { uint32_t poll; int x_left, sx, dx; } g_mouse_hold;

static void mouse_hold_update(int mdx) {
    const uint32_t poll = psp_ctrl_polls();
    if (poll == g_mouse_hold.poll) return;
    g_mouse_hold.poll = poll;
    if (g_mouse_hold.x_left > 0) g_mouse_hold.x_left--;
    if (mdx) { g_mouse_hold.x_left = MOUSE_HOLD_POLLS; g_mouse_hold.sx = mdx > 0 ? 1 : -1; }
    g_mouse_hold.dx += mdx;
}

/* The banked mouse yaw, emptied. */
static int mouse_take_dx(void) {
    const int dx = g_mouse_hold.dx;
    g_mouse_hold.dx = 0;
    return dx;
}

void AC3_FN(AC3_STICK_CONVERTER)(void) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC) { AC3_ORIG(AC3_STICK_CONVERTER)(); return; }

    const int ax = (int8_t)(r_a1 & 0xFF);
    const int ay = (int8_t)(r_a2 & 0xFF);
    const int play = in_play();
    /* Once per transition: "<poll> play=<0|1>", so a log shows where a menu
     * took the stick back. */
    static int last_play = -1;
    if (play != last_play) {
        FILE *log = input_log();
        if (log) fprintf(log, "%u play=%d\n", psp_ctrl_polls(), play);
        last_play = play;
    }
    /* "<poll> stick ax ay rx ry mdx mdy -> bits", once per change, so a replay
     * shows what the sticks asked for without a window. */
    static uint32_t last_bits = 0xFFFFFFFFu, last_log_poll;
    if (!play) {
        /* The original reads a1/a2 itself; hand them back untouched. A drag
         * across a menu is not owed to the AC on the next sortie. */
        g_mouse_hold.x_left = 0;
        g_mouse_hold.dx = 0;
        g_move_gate = g_turn_gate = 0;
        r_a1 = (uint32_t)(uint8_t)ax;
        r_a2 = (uint32_t)(uint8_t)ay;
        AC3_ORIG(AC3_STICK_CONVERTER)();
        return;
    }
    uint8_t axb, ayb;
    psp_ctrl_last_stick(&axb, &ayb);
    const input_tuning *tune = input_tune();
    const stick2 move = stick_radial(axb, ayb, tune->move_dead);
    g_move_gate = hysteresis(g_move_gate, move.raw_m, tune->move_enter, tune->move_dead);

    uint32_t bits = 0;
    if (mode == INPUT_MODERN) {
        const float horizontal = fabsf(move.x);
        g_turn_gate = hysteresis(g_turn_gate, horizontal, 0.04f, 0.015f);
        if (g_turn_gate) {
            if      (move.x > 0.0f) bits  = 0x8000u;
            else if (move.x < 0.0f) bits  = 0x4000u;
        }
        if (g_move_gate) {
            if      (move.y < -0.04f) bits |= 0x1000u;
            else if (move.y >  0.04f) bits |= 0x2000u;
        }
        if (bits != last_bits) {
            FILE *log = input_log();
            if (log) fprintf(log, "%u stick ax=%d ay=%d -> bits=%04X\n",
                             psp_ctrl_polls(), (int)axb - 128, (int)ayb - 128, (unsigned)bits);
            last_bits = bits;
        }
        r_v0 = bits;
        return;
    }

    uint8_t rxb, ryb;
    int mdx, mdy;
    psp_ctrl_last_look(&rxb, &ryb, &mdx, &mdy);
    mouse_hold_update(mdx);
    const stick2 look = stick_look(rxb, ryb);
    g_turn_gate = hysteresis(g_turn_gate, fabsf(look.x), 0.04f, 0.015f);
    (void)mdy;                              /* the pitch controller's, below */

    /* Turn: right stick X, else recent mouse X. The look stick's Y sets no
     * bit: the pitch controller below reads it as a rate, so a press of the
     * triangle or circle it would stand in for is left to the real buttons. */
    if      (g_turn_gate && look.x > 0.0f) bits = 0x8000u;
    else if (g_turn_gate && look.x < 0.0f) bits = 0x4000u;
    else if (g_mouse_hold.x_left > 0) bits = g_mouse_hold.sx > 0 ? 0x8000u : 0x4000u;

    /* Movement: the left stick as eight sectors, not two axes.
     *
     * The game's own map thresholds each axis at 100 of 127 on its own, which
     * on a round stick makes "forward" a cone of about +/-38 degrees, leaves
     * every diagonal dead, and puts the corner where two directions would
     * both pass outside the stick's reach. Here a direction is asked for
     * when the stick points within 67.5 degrees of it, so each of the four
     * spans 135 degrees and a diagonal asks for two at once -- a walk and a
     * strafe together. The actions themselves are still the game's two-state
     * walk and strafe; only the asking follows where the stick points. */
    if (g_move_gate && move.m > 0.0f) {
        /* cos 67.5 deg = 0.383; its square is 0.1464. */
        const float r2 = move.m * move.m;
        const float fwd = -move.y, back = move.y;
        const float right = move.x, left = -move.x;
        if (fwd   > 0.0f && fwd   * fwd   > 0.1464f * r2) bits |= 0x1000u;
        if (back  > 0.0f && back  * back  > 0.1464f * r2) bits |= 0x2000u;
        if (right > 0.0f && right * right > 0.1464f * r2) bits |= 0x200u;
        if (left  > 0.0f && left  * left  > 0.1464f * r2) bits |= 0x100u;
    }
    if (bits != last_bits || psp_ctrl_polls() != last_log_poll) {
        FILE *log = input_log();
        if (log && bits != last_bits)
            fprintf(log, "%u stick ax=%d ay=%d rx=%d ry=%d mdx=%d mdy=%d -> bits=%04X\n",
                    psp_ctrl_polls(), (int)axb - 128, (int)ayb - 128, (int)rxb - 128, (int)ryb - 128,
                    mdx, mdy, (unsigned)bits);
        last_bits = bits; last_log_poll = psp_ctrl_polls();
    }
    r_v0 = bits;
}

/* ---- the yaw integrator ----------------------------------------------------
 *
 * Found on AC3 Portable, 8 Sep, the way Last Raven's was: a full-stick turn
 * held in a sortie, RAM snapshots a poll apart, the words stepping by a
 * constant (scripts/ram-diff.py), and a memory watch on the traced build to
 * name the writer. The player's AC object is the first of the four the record
 * builder walks (0x0046EED0, stride 0x2EC0; the stick's sign flips its rate,
 * the enemy's does not). Its yaw is the float at +100 (radians, right
 * positive, wrapped elsewhere) and its turn rate the float at +168.
 *
 * AC3P's integrator, psp_func_00106B3C(ac, f12 = accel, f13 = max), is
 * called every frame by the turn state's handler psp_func_00102B9C, itself
 * reached through the movement state machine only while a turn bit is set;
 * the handler zeroes the rate on the state's first frame and passes accel and
 * twice a half-cap from the parts block at *(0x004F0394). The law, read from
 * the listing: the turn bit for one side held (the pad word at +180 against
 * the key-assign masks at +1860/+1862) -> rate += 4*accel toward that side;
 * neither -> the rate decays by 4*accel and snaps to zero within it; clamp to
 * +/-max; then, unless the hold at +7108 is set, store the rate and add it to
 * the yaw. Return the turn's sign. Measured: 0, 0.0073, 0.0146, 0.0220,
 * 0.0293, then 0.03658 rad/frame held -- 0.42 deg/frame^2 for five frames to
 * 2.10 deg/frame, Last Raven's numbers to the third digit. Silent Line's
 * integrator (psp_func_00009D80) has Last Raven's structure instead --
 * pressed(pad, 3)/pressed(pad, 2) through *(*(ac+8892)+4), a state byte at
 * (*(ac+8888))+340 that must be -1, the hold from a getter -- with the same
 * rate and yaw offsets.
 *
 * This replacement is Last Raven's (host/replacements.c, psp_func_0004F248):
 * the rate is the stick, not a two-state ramp. modern: max * the left stick's
 * X through the radial deadzone; dual: max * the look stick's X through the
 * look curve, plus the banked mouse X at mouse_sens() radians per count. The
 * game's own state machine still decides when this runs (the converter's
 * bits engage the turn state), so the classic and other-AC paths are the
 * original, byte for byte.
 *
 * The including file provides:
 *     #define AC3_YAW_INTEGRATOR <hex address>
 *     #define AC3_PLAYER_AC      <address of the player's AC object>
 *     static int yaw_gate(uint32_t ac, int *held);
 *         1 if the integrator must do nothing this frame (the original would
 *         return 0 without touching the AC); else 0, with *held set when the
 *         rate and yaw must not be stored. */
enum { AC3_AC_YAW = 100, AC3_AC_RATE = 168 };
static int yaw_gate(uint32_t ac, int *held);

void AC3_FN(AC3_YAW_INTEGRATOR)(void) {
    const int mode = input_mode();
    const uint32_t ac = r_a0;
    if (mode == INPUT_CLASSIC || ac != AC3_PLAYER_AC || !in_play()) {
        AC3_ORIG(AC3_YAW_INTEGRATOR)();
        return;
    }
    const float max = psp_cpu.f[13];
    int held = 0;
    if (yaw_gate(ac, &held)) { r_v0 = 0; return; }

    uint8_t ax, ay, rx, ry;
    int mdx, mdy;
    psp_ctrl_last_stick(&ax, &ay);
    psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    (void)mdy;
    mdx = mode == INPUT_DUAL ? mouse_take_dx() : 0;

    const stick2 turn = mode == INPUT_DUAL
        ? stick_look(rx, ry)
        : stick_radial(ax, ay, input_tune()->move_dead);
    float rate = max * turn.x;
    if (mode == INPUT_DUAL) rate += mouse_sens() * (float)mdx;

    if (!held) {
        f32_write(ac + AC3_AC_RATE, rate);
        f32_write(ac + AC3_AC_YAW, f32_read(ac + AC3_AC_YAW) + rate);
    }
    FILE *log = input_log();
    if (log) fprintf(log, "%u yaw ac=%08X rx=%d mdx=%d max=%.5f held=%d rate=%.6f yaw=%.4f\n",
                     psp_ctrl_polls(), ac, (int)rx - 128, mdx, max, held, rate,
                     f32_read(ac + AC3_AC_YAW));
    r_v0 = (uint32_t)(int32_t)(rate > 0.0f ? 1 : rate < 0.0f ? -1 : 0);
}

/* ---- the look (pitch) controller -------------------------------------------
 *
 * Found on AC3 Portable, 8 Sep, as the yaw was: triangle held in a sortie
 * (scenarios/ac3p/look-probe.pad), snapshots a poll apart, a second-difference
 * scan for the word whose steps grow by a constant, and a memory watch to name
 * the writer. The look's state is a small struct at ac+32 in every title here:
 * a lockout counter at +4, the pitch angle at +16 (radians, up positive), the
 * pitch rate at +32. Its constants are Last Raven's six floats to the digit
 * (0x0025DEE0 on AC3P, 0x00275B60 on Silent Line): [0] 27.0, the lockout after
 * a recentre; [1] 0.00136 accel; [2] 0.009 decel; [3] 0.027 max rate; [4] and
 * [5] -/+1.1781, the clamp at 67.5 degrees. The controller scales [1]..[3] by
 * 60/30 as it reads them and AC3P calls it once a frame from the player's
 * per-frame update: measured, the rate climbs 0.00272 a frame to 0.054 and the
 * angle follows, clamped.
 *
 * AC3P's psp_func_00104FC4(ac) reads its pad directly -- the held word at
 * +180 and the pressed word at +184 against the key-assign masks at +1876
 * (up) and +1878 (down). Silent Line's psp_func_000081A4(ac) is Last Raven's
 * shape instead: a gate on the movement-state byte, actions 10 and 11 through
 * pad helpers. The law is one: a look key held ramps the rate toward its cap
 * by 2*accel a call; both keys recentre the view and start the lockout;
 * neither decays the rate to zero; angle += rate, clamped, and the rate is
 * zeroed at a limit.
 *
 * This is Last Raven's replacement (host/replacements.c, psp_func_00053234):
 * under `dual`, the right stick's Y is a rate in the game's cap, eased in at
 * the game's own ramp and eased off at once, and mouse Y a displacement at
 * mouse_sens() radians per count -- into the same three words, under the same
 * clamp. The buttons and the lockout stay the game's: when either is in play
 * this defers, and both laws act on the same state, so they compose.
 *
 * The including file provides:
 *     #define AC3_PITCH_CONTROLLER <hex address>
 *     #define AC3_LOOK_PARAMS      <address of the six floats>
 *     static int pitch_gate(uint32_t ac);   1 when the original would return
 *                                           without touching the AC
 *     static int look_keys(uint32_t ac);    nonzero while a look key is held
 *                                           or was pressed this frame */
enum { AC3_LOOK_COUNTER = 36, AC3_PITCH = 48, AC3_PITCH_RATE = 64 };
static int pitch_gate(uint32_t ac);
static int look_keys(uint32_t ac);

static void pitch_log(uint32_t ac, const char *how, int ry, int mdy) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u pitch=%.6f rate=%.6f ry=%d mdy=%d %s\n",
                     psp_ctrl_polls(), f32_read(ac + AC3_PITCH),
                     f32_read(ac + AC3_PITCH_RATE), ry, mdy, how);
}

void AC3_FN(AC3_PITCH_CONTROLLER)(void) {
    const uint32_t ac = r_a0;
    if (ac == AC3_PLAYER_AC) g_ac_update_poll = psp_ctrl_polls();   /* the heartbeat in_play reads */
    if (input_mode() != INPUT_DUAL || ac != AC3_PLAYER_AC || !in_play()) {
        AC3_ORIG(AC3_PITCH_CONTROLLER)();
        if (ac == AC3_PLAYER_AC) pitch_log(ac, "orig", 0, 0);
        return;
    }
    if (pitch_gate(ac)) return;

    uint8_t rx, ry;
    int mdx, mdy;
    psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);
    (void)mdx;                              /* the yaw integrator's */

    /* The game's own digital look and its lockout. */
    if ((int32_t)psp_read32(ac + AC3_LOOK_COUNTER) > 0 || look_keys(ac)) {
        r_a0 = ac;
        AC3_ORIG(AC3_PITCH_CONTROLLER)();
        pitch_log(ac, "orig-button", (int)ry - 128, mdy);
        return;
    }

    const float accel = 2.0f * f32_read(AC3_LOOK_PARAMS + 4);   /* as the game scales them */
    const float max   = 2.0f * f32_read(AC3_LOOK_PARAMS + 12);
    const float lo    = f32_read(AC3_LOOK_PARAMS + 16);
    const float hi    = f32_read(AC3_LOOK_PARAMS + 20);

    /* Stick up is a smaller byte; mouse away from the player is negative dy;
     * both look up, which is positive here. The stick is a rate: the cap
     * times the expo curve of the deflection, eased in by `accel` a frame as
     * triangle is -- but eased in only. Easing off, reversing and releasing
     * take effect at once, as the game's dead stop did; a rate that coasts
     * after the thumb has stopped is the floaty feel that makes aiming miss.
     * The filter's state is the game's own word, so a replay reproduces it
     * and a frame the buttons handled hands over smoothly. The mouse is a
     * displacement and stays raw. */
    const stick2 look = stick_look(rx, ry);
    const float target = -max * look.y;
    float rate = f32_read(ac + AC3_PITCH_RATE);
    const int same_way = (target > 0.0f && rate > 0.0f) || (target < 0.0f && rate < 0.0f);
    if (target == 0.0f || (rate != 0.0f && !same_way) ||
        (same_way && fabsf(target) < fabsf(rate))) {
        rate = target;
    } else if (target > rate) {
        rate += accel; if (rate > target) rate = target;
    } else {
        rate -= accel; if (rate < target) rate = target;
    }

    float angle = f32_read(ac + AC3_PITCH) + rate - mouse_sens() * (float)mdy;
    if (angle > hi) { angle = hi; rate = 0.0f; }
    if (angle < lo) { angle = lo; rate = 0.0f; }
    f32_write(ac + AC3_PITCH, angle);
    f32_write(ac + AC3_PITCH_RATE, rate);
    pitch_log(ac, "dual", (int)ry - 128, mdy);
}

/* ---- the walk's push --------------------------------------------------------
 *
 * Found on AC3 Portable, 8 Sep: the left stick held forward in a sortie
 * (scenarios/ac3p/walk-probe.pad), snapshots a poll apart for the position
 * and velocity words, a memory watch on the velocity's z at ac+152 to name
 * the writers. The velocity is at ac+144/152 and the position at ac+80/88.
 *
 * Every moving state ends in one primitive, psp_func_00106EF8(ac, vec, cap)
 * (Silent Line 00009500): add four times the world-frame vector to the
 * velocity, then scale the speed back to `cap` when it passes it -- or, when
 * the AC was already faster, down toward it by four times a parts constant a
 * frame. The boosts and jumps feed it through the direction-table push
 * psp_func_00106D94, Last Raven's 0004F06C to the instruction. The walk does
 * not: its two handlers (one per leg family, identical code) call an
 * assembler, psp_func_0010D5A4 or 0010DF98(ac, direction) (Silent Line
 * 000193D4/00019E24), which takes the vector from the *animation* --
 * psp_func_000DC628(*(ac+3164), out) is the root's travel between this frame
 * of the playing walk cycle and the next -- rotates it into the world by the
 * yaw at ac+100 (psp_func_0021A790 builds the matrix from the table at
 * 0x00268800; a VFPU vtfm4 applies it), scales it by the start-up ramp at
 * ac+3060 while that is between 0 and 1 (from the legs' accel fraction at
 * parts+328, or +332 walking back, up to 1), and passes its own length as the
 * cap. So the AC walks at the animation's speed, in the direction the
 * animation was chosen for: eight directions, one speed, stick or no stick.
 * Measured: the speed's z steps -0.75 then back to -0.1875 the first frame
 * (four times the vector, capped to its length) and the ramp carries it to
 * the cycle's ~1.2 over the next second.
 *
 * Here, for the player under `dual`, the direction is the left stick's own
 * and the cap is the animation's speed times how far the stick is pushed
 * past the deadzone, so the AC creeps at a nudge and walks at the edge, in
 * the direction pointed -- a walk and a strafe are one push at an angle, not
 * two states. `modern` keeps the animation's direction (that stick's X is the
 * turn) and scales the cap by Y alone. The travel, the rotation, the ramp,
 * the clamp and the overspeed decay are the game's own code on the game's
 * own stack frame, exactly as the original calls them; only the vector's
 * direction and the cap are changed. A centred stick, which can happen
 * inside a jump or a boost, defers entirely.
 *
 * The including file provides:
 *     #define AC3_WALK_PUSH_A, AC3_WALK_PUSH_B   the two assemblers
 *     #define AC3_ANIM_TRAVEL, AC3_YAW_MATRIX, AC3_PUSH   the helpers they call
 *     #define AC3_ROT_TABLE      <address the matrix builder is handed>
 *     enum { AC3_AC_ANIM, AC3_AC_PARTS, AC3_AC_RAMP }   the AC's offsets */
enum { AC3_AC_X = 80, AC3_AC_Z = 88, AC3_AC_VX = 144, AC3_AC_VZ = 152 };

/* The left stick as a direction and a magnitude for the walk: the unit vector
 * in the AC's frame and how far past the profile's radial deadzone the stick
 * sits, 0..1. Zero inside the circle. The frame is the game's direction
 * table's (0x0025E4E0 on AC3P, Last Raven's order): z back and x to the
 * *left* -- entry 3 is (+1, 0) and the game orders its stick actions forward,
 * back, left, right. The stick's x is to the right, hence the sign. */
static float stick_walk(uint8_t axb, uint8_t ayb, float *sx, float *sz) {
    const stick2 s = stick_radial(axb, ayb, input_tune()->move_dead);
    if (s.m == 0.0f) { *sx = 0.0f; *sz = 0.0f; return 0.0f; }
    *sx = -s.x / s.m;
    *sz =  s.y / s.m;
    return s.m;
}

/* `ra` is the guest return address at entry: which handler made this push.
 * The animation's own body-frame vector is logged beside the direction used,
 * so a sign mistake between the two frames shows as a walk that goes the
 * wrong way on paper before it does on screen. */
static void walk_cadence_note(float m);   /* defined with the cadence, below */

static void push_log(uint32_t ac, uint32_t ra, const char *how, float m,
                     float sx, float sz, float ax, float az, float cap) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u push m=%.3f dir=%+.3f,%+.3f anim=%+.3f,%+.3f cap=%.4f speed=%.4f pos=%.3f,%.3f ra=%08X %s\n",
                     psp_ctrl_polls(), m, sx, sz, ax, az, cap,
                     hypotf(f32_read(ac + AC3_AC_VX), f32_read(ac + AC3_AC_VZ)),
                     f32_read(ac + AC3_AC_X), f32_read(ac + AC3_AC_Z), ra, how);
}

static void walk_push(uint32_t ac, uint32_t idx, uint32_t ra, void (*orig)(void)) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC || ac != AC3_PLAYER_AC || !in_play()) {
        orig();
        if (ac == AC3_PLAYER_AC) push_log(ac, ra, "orig", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    uint8_t axb, ayb;
    psp_ctrl_last_stick(&axb, &ayb);
    float sx, sz, m = stick_walk(axb, ayb, &sx, &sz);
    if (mode == INPUT_MODERN) {
        /* The animation's direction, the processed magnitude from Y alone. */
        const stick2 move = stick_radial(axb, ayb, input_tune()->move_dead);
        m = fabsf(move.y);
    }
    if (m <= 0.0f) {
        r_a0 = ac; r_a1 = idx;
        orig();
        push_log(ac, ra, "orig-centred", 1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }

    /* The original's frame: the vector at sp+16, the matrix at sp+32. r_sp is
     * put back before returning, which is what the call site checks. */
    const uint32_t sp = r_sp;
    r_sp -= 128;
    const uint32_t vec = r_sp + 16, mat = r_sp + 32;

    /* The animation's travel this frame, in the AC's frame. */
    r_a0 = psp_read32(ac + AC3_AC_ANIM); r_a1 = vec;
    AC3_FN(AC3_ANIM_TRAVEL)();
    const float ax = f32_read(vec + 0), az = f32_read(vec + 8);
    const float body = hypotf(ax, az);
    if (mode == INPUT_DUAL) {
        /* The stick's direction at the animation's length. */
        f32_write(vec + 0, sx * body);
        f32_write(vec + 8, sz * body);
    } else {
        sx = body > 0.0f ? ax / body : 0.0f;
        sz = body > 0.0f ? az / body : 0.0f;
    }

    /* Into the world by the yaw: the original's helper and its VFPU transform. */
    r_a0 = mat; r_a1 = AC3_ROT_TABLE; psp_cpu.f[12] = f32_read(ac + AC3_AC_YAW);
    AC3_FN(AC3_YAW_MATRIX)();
    psp_lv_q(4, mat); psp_lv_q(5, mat + 16); psp_lv_q(6, mat + 32); psp_lv_q(7, mat + 48);
    psp_lv_q(8, vec);
    psp_vtfm(0, 36, 8, 4, 0);
    psp_sv_q(0, vec);

    /* The start-up ramp, as the original scales it. */
    const uint32_t parts = psp_read32(ac + AC3_AC_PARTS);
    float accel = f32_read(parts + ((int32_t)idx < 5 ? 328 : 332));
    if (accel == 0.0f) accel = 0.5f;
    const float ramp = f32_read(ac + AC3_AC_RAMP);
    if (ramp > 0.0f && ramp < 1.0f) {
        const float k = accel + (1.0f - accel) * ramp;
        for (int i = 0; i < 3; i++) f32_write(vec + 4 * (uint32_t)i, f32_read(vec + 4 * (uint32_t)i) * k);
    }

    /* Accelerate toward it, with the cap the stick asked for. */
    const float cap = hypotf(f32_read(vec + 0), f32_read(vec + 8)) * m;
    r_a0 = ac; r_a1 = vec; psp_cpu.f[12] = cap;
    AC3_FN(AC3_PUSH)();
    r_sp = sp;
    walk_cadence_note(m);
    push_log(ac, ra, mode == INPUT_DUAL ? "dual" : "modern", m, sx, sz, ax, az, cap);
}

void AC3_FN(AC3_WALK_PUSH_A)(void) { walk_push(r_a0, r_a1, r_ra, AC3_ORIG(AC3_WALK_PUSH_A)); }
void AC3_FN(AC3_WALK_PUSH_B)(void) { walk_push(r_a0, r_a1, r_ra, AC3_ORIG(AC3_WALK_PUSH_B)); }

/* ---- physical gamepad -> game actions -----------------------------------------
 *
 * Last Raven's mechanism (host/replacements.c, psp_func_00279A10 and the
 * action helpers), on these titles' own pad path. The PSP packet has no
 * triggers or stick clicks; present.c carries the ten modern-pad controls in
 * button bits the game's twelve-entry PSP converter ignores (host/controls.h),
 * still in the sceCtrl lane, so recordings keep them. The converter --
 * psp_func_001ECE44(state, raw) on AC3P, 0020CE44 on Silent Line, Last
 * Raven's 00279A10 to the instruction -- is replaced to strip those bits and
 * remember their held and edge state per poll; in menus it turns the face
 * buttons and bumpers back into PSP buttons so navigation stays conventional.
 *
 * In play the actions are answered semantically, by the game's own action
 * numbering: the key-assign row is sixteen 16-bit masks on the AC's
 * *remapped* pad word -- on AC3P at ac+1856+2n, on Silent Line at
 * cfg+1792+2n through the pad object, Last Raven's layout. The numbering is
 * Last Raven's up to the strafes and the look (0..3 the stick's
 * forward/back/left/right, 4 change weapon, 5 boost, 6 arm R, 7 arm L/event,
 * 8/9 strafe, 10/11 look up/down), measured on AC3P by injecting each and
 * watching the frames (9 Sep: the boost lifts the AC, the rifle's count
 * drops, the back unit becomes the selected weapon, the blade swings). Above
 * that the AC3 generation differs: there is no inside button (AC3's insides
 * are cycled as weapons) and no view-reset action -- the reset is triangle
 * and circle together, the look controller's own recentre path -- so 13 is
 * the extension, 14 and 15 share one mask and are OB/EO (the default d-pad
 * left: the OB indicator lights on either), and 12 is the "dump" of the
 * purge chord (L, R, triangle and circle held), not bound here. Last Raven's
 * action 16, the disarmament modifier, has no counterpart either.
 *
 * Two ways in, because the titles differ in how the game asks:
 *
 *   - Silent Line asks through two helpers, held(pad, action) and
 *     pressed(pad, action), Last Raven's shape: replace them and answer 1 when
 *     the action's carrier is down or was pressed this poll, for the player's
 *     pad in play. AC3_ACTION_HELD / AC3_ACTION_PRESSED.
 *   - AC3 Portable reads the masks inline, in two dozen handlers, so there is
 *     nothing to replace per query. Instead the per-frame copy that lands the
 *     AC's held and pressed words (psp_func_00100894(ac, word): a ring of
 *     eight past words at +192, indexed by +190 and the delay at +188, then
 *     held at +180 and pressed = held & ~previous at +184) is replaced to OR
 *     the action's *own mask* from the AC's table into those words afterwards.
 *     Independent of the key assignment by construction: whatever button the
 *     row binds boost to, that mask is what is set. AC3_PAD_COPY.
 *
 * The including file provides:
 *     #define AC3_PAD_MAPPER <hex>              the twelve-entry PSP converter
 *     static int player_pad_is(uint32_t pad);  is this pad object the player's
 * and one of
 *     #define AC3_PAD_COPY <hex>                the AC pad copy (a0 = the AC)
 *     static uint32_t action_mask(uint32_t ac, uint32_t action);
 * or
 *     #define AC3_ACTION_HELD <hex>, AC3_ACTION_PRESSED <hex>
 * and sets lr_modern_controls_available = 1 so present.c offers the layout. */
static int player_pad_is(uint32_t pad);

static struct {
    uint32_t poll;
    uint32_t down;
    uint32_t pressed;
} g_extra_pad;

static uint32_t extra_menu_buttons(uint32_t extra) {
    uint32_t psp = 0;
    if (extra & LR_PAD_A)  psp |= 0x004000u;       /* Cross    */
    if (extra & LR_PAD_B)  psp |= 0x002000u;       /* Circle   */
    if (extra & LR_PAD_X)  psp |= 0x008000u;       /* Square   */
    if (extra & LR_PAD_Y)  psp |= 0x001000u;       /* Triangle */
    if (extra & (LR_PAD_LB | LR_PAD_LT)) psp |= 0x000100u;
    if (extra & (LR_PAD_RB | LR_PAD_RT)) psp |= 0x000200u;
    return psp;
}

/* Which carrier answers an action. B answers both look actions at once: on
 * this engine that is the view reset, the controller's own recentre and
 * lockout, in every mode. A, X and Y have no in-play meaning here (menus
 * alias A and B to cross and circle above). */
static uint32_t extra_for_action(uint32_t action) {
    switch (action) {
    case 4:  return LR_PAD_RB;       /* Change weapon       */
    case 5:  return LR_PAD_LT;       /* Boost / jump        */
    case 6:  return LR_PAD_RT;       /* Arm unit R          */
    case 7:  return LR_PAD_LB;       /* Arm unit L / event  */
    case 10: return LR_PAD_B;        /* Look up   -- both:  */
    case 11: return LR_PAD_B;        /* Look down    reset  */
    case 13: return LR_PAD_L3;       /* Extension           */
    case 14: return LR_PAD_R3;       /* OB / EO             */
    case 15: return LR_PAD_R3;       /* OB / EO (one mask)  */
    default: return 0;
    }
}

static void extra_action_log(uint32_t action, int edge) {
    static uint32_t poll, held_seen, edge_seen;
    const uint32_t now = psp_ctrl_polls();
    if (now != poll) { poll = now; held_seen = edge_seen = 0; }
    uint32_t *seen = edge ? &edge_seen : &held_seen;
    const uint32_t bit = action < 32 ? 1u << action : 0;
    if (!bit || (*seen & bit)) return;
    *seen |= bit;
    FILE *log = input_log();
    if (log) fprintf(log, "%u action=%u semantic-%s\n", now, action,
                     edge ? "pressed" : "held");
}

void AC3_FN(AC3_PAD_MAPPER)(void) {
    const uint32_t state = r_a0;
    const uint32_t raw = r_a1;
    const uint32_t extra = raw & LR_PAD_EXTRA;
    const uint32_t poll = psp_ctrl_polls();
    if (poll != g_extra_pad.poll) {
        const uint32_t old = g_extra_pad.down;
        g_extra_pad.poll = poll;
        g_extra_pad.down = extra;
        g_extra_pad.pressed = extra & ~old;
        if (gamepad_modern() && extra != old) {
            FILE *log = input_log();
            if (log) fprintf(log, "%u pad extra=%08X pressed=%08X play=%d\n",
                             poll, extra, g_extra_pad.pressed, in_play());
        }
    } else {
        g_extra_pad.pressed |= extra & ~g_extra_pad.down;
        g_extra_pad.down = extra;
    }

    uint32_t psp = raw & ~LR_PAD_EXTRA;
    if (gamepad_modern() && !in_play()) psp |= extra_menu_buttons(extra);
    r_a0 = state;
    r_a1 = psp;
    AC3_ORIG(AC3_PAD_MAPPER)();
}

#ifdef AC3_PAD_COPY
static uint32_t action_mask(uint32_t ac, uint32_t action);

void AC3_FN(AC3_PAD_COPY)(void) {
    const uint32_t ac = r_a0;
    AC3_ORIG(AC3_PAD_COPY)();
    if (ac != AC3_PLAYER_AC || !gamepad_modern() || !in_play()) return;
    uint32_t held = 0, pressed = 0;
    for (uint32_t action = 4; action <= 15; action++) {
        const uint32_t bit = extra_for_action(action);
        if (!bit) continue;
        if (g_extra_pad.down & bit)    { held    |= action_mask(ac, action); extra_action_log(action, 0); }
        if (g_extra_pad.pressed & bit) { pressed |= action_mask(ac, action); extra_action_log(action, 1); }
    }
    if (held)    psp_write16(ac + 180, psp_read16(ac + 180) | held);
    if (pressed) psp_write16(ac + 184, psp_read16(ac + 184) | pressed);
}
#endif

#ifdef AC3_ACTION_HELD
void AC3_FN(AC3_ACTION_HELD)(void) {
    const uint32_t pad = r_a0, action = r_a1;
    AC3_ORIG(AC3_ACTION_HELD)();
    const uint32_t bit = extra_for_action(action);
    if (gamepad_modern() && bit && in_play() && player_pad_is(pad) &&
        (g_extra_pad.down & bit)) {
        r_v0 = 1;
        extra_action_log(action, 0);
    }
}

void AC3_FN(AC3_ACTION_PRESSED)(void) {
    const uint32_t pad = r_a0, action = r_a1;
    AC3_ORIG(AC3_ACTION_PRESSED)();
    const uint32_t bit = extra_for_action(action);
    if (gamepad_modern() && bit && in_play() && player_pad_is(pad) &&
        (g_extra_pad.pressed & bit)) {
        r_v0 = 1;
        extra_action_log(action, 1);
    }
}
#endif

/* ---- adaptive aspect -----------------------------------------------------------
 *
 * All three titles build their projection through one 19-instruction routine,
 * identical instruction for instruction (Last Raven psp_func_0025879C, AC3P
 * 001CC4DC, Silent Line 001EA920): given a camera, it converts the field of
 * view at +192 from degrees to radians and hands that, the aspect at +204, the
 * near plane at +196 and the far plane at +200 to the matrix builder, which
 * writes the projection into the camera at +128. Found 10 Sep by scanning for
 * the pi and 180.0f constants and confirmed by a memory watch on the module
 * camera's projection; the camera is three 4x4 matrices then those four floats,
 * and the shape is the same in each title (the sortie camera measured at
 * 45 degrees, near 6, far 4000 with the aspect 480/272).
 *
 * The guest framebuffer cannot become window-sized: movies, 2D drawing, CPU
 * reads and alpha-backed stencil all depend on its 480x272 layout. So in
 * adaptive mode the camera is lent a *wider aspect* for exactly the duration of
 * the call and its own value is put back before returning. The vertical field
 * of view is untouched, so a wider drawable shows more to the left and right
 * rather than stretching what was there.
 *
 * Only cameras at the native aspect are widened. The assembly's part and AC
 * previews project at their panel's aspect and keep it, which is what the GL
 * backend expects of an inset (host/render_gl.c, menu_preview).
 *
 * Last Raven does this one level up, in its camera rebuild, because its display
 * cameras cache width/480 and height/272 beside the aspect; the AC3 generation
 * has no such fields, so the projection call itself is the place.
 *
 * The including file provides:
 *     #define AC3_PERSPECTIVE <hex address>
 * and defines lr_adaptive_aspect_available = 1 so present.c offers the option. */
enum { AC3_CAM_FOV = 192, AC3_CAM_NEAR = 196, AC3_CAM_FAR = 200, AC3_CAM_ASPECT = 204 };

void AC3_FN(AC3_PERSPECTIVE)(void) {
    const uint32_t cam = r_a0;
    if (!present_adaptive_aspect()) { AC3_ORIG(AC3_PERSPECTIVE)(); return; }

    const float native = 480.0f / 272.0f;
    const float aspect = f32_read(cam + AC3_CAM_ASPECT);
    const float wide = (float)present_aspect_wide_width() / 272.0f;
    /* An inset preview, or a drawable no wider than the PSP: the original,
     * unchanged. At the native ratio wide == native exactly, so enabling the
     * option without resizing stays on the original path bit for bit. */
    if (aspect != native || wide == aspect) { AC3_ORIG(AC3_PERSPECTIVE)(); return; }

    f32_write(cam + AC3_CAM_ASPECT, wide);
    r_a0 = cam;
    AC3_ORIG(AC3_PERSPECTIVE)();
    f32_write(cam + AC3_CAM_ASPECT, aspect);

    static float last_wide;
    if (wide != last_wide) {
        int dw = 0, dh = 0;
        present_gl_drawable_size(&dw, &dh);
        printf("      aspect    drawable %dx%d, camera aspect %.4f (native %.4f)\n",
               dw, dh, (double)wide, (double)native);
        last_wide = wide;
    }
}

/* ---- the chase camera's lag -----------------------------------------------------
 *
 * Both siblings keep Last Raven's camera struct, field for field, in a table
 * in module memory: AC3P at 0x00497800, Silent Line at 0x00474100, stride 256.
 * The eye is at +16, the pitch at +32 and the yaw at +36 (the atan2/asin of
 * target minus eye), the look-at target at +48, and the two blend ratios the
 * lag is made of at +104 for the eye and +108 for the target -- 0.900 and
 * 0.821 in both titles, against Last Raven's 0.830 and 0.831.
 *
 * The per-frame update (AC3P psp_func_0011B440, Silent Line 00025268; the same
 * function to 0.82 by instruction sequence) takes a camera index and an output
 * matrix, dispatches on the camera's mode byte at +0 through a table of
 * handlers, and hands the eye and target the handler produced to the installer
 * that derives the yaw, the pitch and the view matrix. Mode 0 is the chase
 * camera (AC3P 0011C320, Silent Line 000263C4), and it does not go where it
 * should: it blends, `current + (ideal - current) * r`, with r read from those
 * two fields. Last Raven's is the same law in two small helpers; here it is
 * inlined into the handler, so there is no leaf to replace.
 *
 * So this replaces the update instead and lends the camera the wanted ratio in
 * its own two fields for the duration of the call, putting the game's values
 * back afterwards -- the same borrow-and-restore the adaptive aspect above
 * does. Keeping 90% of the gap each frame is a camera half a second behind the
 * AC; with the game's own 2.1 deg/frame turn cap that reads as weight, and
 * behind a mouse it reads as a rubber band.
 *
 * PSPRECOMP_CAMERA_LAG=<r> sets r for the player's camera under `modern` or
 * `dual`: 0.90 is the game's, 0.5 halves the time to settle, 0 fixes the
 * camera to the AC. Unset, the game's values stand; classic never reads it.
 * Only camera 0 in its chase mode, and only while the player is being played,
 * so the garage and the replay cameras keep the game's own feel.
 *
 * The including file provides:
 *     #define AC3_CAMERA_UPDATE <hex address>
 *     #define AC3_CAMERA_TABLE  <address of camera 0> */
enum { AC3_CAM_EYE = 16, AC3_CAM_PITCH = 32, AC3_CAM_YAW = 36, AC3_CAM_TARGET = 48,
       AC3_CAM_EYE_LAG = 104, AC3_CAM_TARGET_LAG = 108, AC3_CAM_STRIDE = 256 };

/* PSPRECOMP_CAMERA_LAG, clamped to 0..0.99; negative when unset. */
static float camera_lag(void) {
    static float r = -2.0f;
    if (r < -1.5f) {
        r = (float)lr_settings_current()->number[LR_CAMERA_LAG];
        if (r >= 0.0f) {
            if (r > 0.99f) r = 0.99f;
            printf("      camera    lag %.2f per frame (the game keeps 0.90); "
                   "PSPRECOMP_INPUT=classic ignores it\n", (double)r);
        }
    }
    return r;
}

void AC3_FN(AC3_CAMERA_UPDATE)(void) {
    const uint32_t idx = r_a0, out = r_a1;
    const uint32_t cam = AC3_CAMERA_TABLE + idx * (uint32_t)AC3_CAM_STRIDE;
    const float r = camera_lag();
    /* Camera 0 in the chase mode, the player's, and only in play. */
    const int ours = idx == 0 && r >= 0.0f && input_mode() != INPUT_CLASSIC &&
                     psp_read8(cam) == 0 && in_play();
    float eye_lag = 0.0f, target_lag = 0.0f;
    if (ours) {
        eye_lag = f32_read(cam + AC3_CAM_EYE_LAG);
        target_lag = f32_read(cam + AC3_CAM_TARGET_LAG);
        f32_write(cam + AC3_CAM_EYE_LAG, r);
        f32_write(cam + AC3_CAM_TARGET_LAG, r);
    }
    r_a0 = idx; r_a1 = out;
    AC3_ORIG(AC3_CAMERA_UPDATE)();
    if (ours) {
        f32_write(cam + AC3_CAM_EYE_LAG, eye_lag);
        f32_write(cam + AC3_CAM_TARGET_LAG, target_lag);
    }
    /* One line a frame for the player's camera, in every mode: the rate the
     * eye was blended with and the camera's angles, beside the AC's, so the
     * lag is a number and not an impression. */
    if (idx == 0) {
        FILE *log = input_log();
        if (log) fprintf(log, "%u cam r=%.4f yaw=%.6f pitch=%.6f ac_yaw=%.6f %s\n",
                         psp_ctrl_polls(), (double)(ours ? r : f32_read(cam + AC3_CAM_EYE_LAG)),
                         (double)f32_read(cam + AC3_CAM_YAW),
                         (double)f32_read(cam + AC3_CAM_PITCH),
                         (double)f32_read(AC3_PLAYER_AC + AC3_AC_YAW),
                         ours ? "ours" : "game");
    }
}

/* ---- the walk cycle's cadence ----------------------------------------------------
 *
 * The walk animation plays at one speed. The push above walks at any speed, so
 * a creep slides its feet: the cycle was authored for the cap.
 *
 * Where Last Raven keeps a per-slot playback *step* it can hold at zero, the
 * AC3 generation keeps one time counter for the whole skeleton -- the u16 at
 * +204 of the pose array, `*(anim + 160)`, where anim is the AC's animation
 * object at ac+3164 (Silent Line ac+3104). It advances by exactly one a frame,
 * and the pose is sampled at twice it: `psp_func_000DC628`, the same routine
 * the push reads its root travel from, looks up 2t and 2t+1 and takes the
 * difference. Found 10 Sep by diffing three consecutive walk frames for a word
 * that steps by one, then watching it to name the updater.
 *
 * That counter is all the cadence needs. The animation updater (AC3P
 * psp_func_000DD29C, Silent Line 00099E38, the same function to 0.90 by
 * instruction sequence) advances it and poses all 36 bones from it; this
 * replacement reads it before the call and writes it back afterwards on the
 * frames an accumulator fed with the stick's magnitude says to hold. The cycle
 * then advances m frames per frame on average, one stride per stride's worth
 * of ground. At full deflection the accumulator carries every frame and
 * nothing changes; at half, every other frame; a creep at 0.3 advances three
 * frames in ten. Only a value the game itself wrote is ever restored, so there
 * is no wrap to handle and the game's own state is never invented.
 *
 * Which pushes are walks needs no guessing here: the two walk assemblers are
 * the only callers of walk_push above, so the note is taken there. Last Raven
 * had to recognise its walk by the return address into the push helper.
 *
 * The including file provides:
 *     #define AC3_ANIM_UPDATE <hex address>
 * and AC3_AC_ANIM, already given for the push. */
enum { AC3_ANIM_POSE = 160, AC3_ANIM_TIME = 204 };

static struct { uint32_t poll; float m, phase; int hold; } g_walk;

static void walk_cadence_note(float m) {
    const uint32_t poll = psp_ctrl_polls();
    if (poll == g_walk.poll) return;              /* a diagonal walk pushes twice a frame */
    if (poll != g_walk.poll + 1) g_walk.phase = 0.0f;   /* a fresh walk starts in phase */
    g_walk.poll = poll;
    g_walk.m = m;
    g_walk.phase += m;
    if (g_walk.phase >= 1.0f) { g_walk.phase -= 1.0f; g_walk.hold = 0; }
    else g_walk.hold = 1;
}

/* One line a frame for the player's animation: the counter the pose was taken
 * at, so the cadence is a number and not an impression. */
static void anim_log(uint32_t pose, int hold) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u anim hold=%d m=%.3f t=%u\n", psp_ctrl_polls(), hold,
                     (double)(hold >= 0 ? g_walk.m : 0.0f),
                     pose ? psp_read16(pose + AC3_ANIM_TIME) : 0u);
}

void AC3_FN(AC3_ANIM_UPDATE)(void) {
    const uint32_t obj = r_a0;
    const int player = obj && obj == psp_read32(AC3_PLAYER_AC + AC3_AC_ANIM);
    /* The walk noted this frame or the one before -- the updater's place in
     * the frame relative to the push is not assumed. */
    const int walking = player && input_mode() != INPUT_CLASSIC &&
                        psp_ctrl_polls() - g_walk.poll <= 1;
    const uint32_t pose = player ? psp_read32(obj + AC3_ANIM_POSE) : 0;
    if (!walking || !g_walk.hold || !pose) {
        AC3_ORIG(AC3_ANIM_UPDATE)();
        if (player) anim_log(pose, walking ? 0 : -1);
        return;
    }
    const uint16_t t = psp_read16(pose + AC3_ANIM_TIME);
    r_a0 = obj;
    AC3_ORIG(AC3_ANIM_UPDATE)();
    psp_write16(pose + AC3_ANIM_TIME, t);
    anim_log(pose, 1);
}

#endif
