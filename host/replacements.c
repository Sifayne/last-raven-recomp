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
#include "psprecomp/host/pad.h"
#include "settings.h"
#include "psprecomp/host/present.h"
#include "psprecomp/host/title.h"
#include "stick.h"      /* INPUT_*, stick2, input_tune(), stick_radial(), stick_look(), hysteresis() */
#include "fps_aclr.h"

/* ---- configuration ------------------------------------------------------ */

static int input_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        mode = (int)psp_settings_current()->number[LR_INPUT];
        if (mode == INPUT_MODERN)
            printf("      input     modern -- yaw rate proportional to the stick; "
                   "PSPRECOMP_INPUT=classic for the game's own\n");
        if (mode == INPUT_DUAL)
            printf("      input     dual -- right stick and mouse look, left stick "
                   "moves; PSPRECOMP_INPUT=classic for the game's own\n");
    }
    return mode;
}

/* What the shared host may offer this title (psprecomp/host/title.h):
 * - the modern controller layout: this host reads the modern pad's carrier
 *   bits (psp_func_00279A10 and the action helpers below);
 * - the adaptive aspect: this title's own camera rebuild, below;
 * - HUD bands: off-screen HUD draws may go to the wide bands. This title's
 *   menus, garage and missions were audited (reports/aspect-reticle, 17 Sep)
 *   and only the lock-on rings and the lock marker ever draw there. */
/* The carriers this host reads, as actions the bindings can name
 * (bind.pad.boost=...): extra_for_action below in play, and menus alias A, B,
 * X and Y to the face buttons (extra_menu_buttons). None means anything
 * while the modern layout is off. */
static const psp_title_action ac_actions[] = {
    { "inside",      "Inside",           PSP_PAD_A,  PSP_ACTION_KEEP },
    { "view_reset",  "View reset",       PSP_PAD_B,  PSP_ACTION_KEEP },
    { "spare",       "Spare",            PSP_PAD_X,  PSP_ACTION_KEEP },
    { "purge",       "Purge modifier",   PSP_PAD_Y,  PSP_ACTION_KEEP },
    { "left_arm",    "Left arm / event", PSP_PAD_LB, PSP_ACTION_KEEP },
    { "change_unit", "Change unit",      PSP_PAD_RB, PSP_ACTION_KEEP },
    { "boost",       "Boost / jump",     PSP_PAD_LT, PSP_ACTION_KEEP },
    { "right_arm",   "Right arm",        PSP_PAD_RT, PSP_ACTION_KEEP },
    { "extension",   "Extension",        PSP_PAD_L3, PSP_ACTION_KEEP },
    { "ob",          "OB / EO",          PSP_PAD_R3, PSP_ACTION_KEEP },
};
static void ac_input(const psp_settings *s, psp_title_input *out) {
    (void)s;
    out->actions = ac_actions;
    out->action_count = sizeof ac_actions / sizeof *ac_actions;
}

const psp_title psp_title_info = {
    .name = "Armored Core: Last Raven",
    .capabilities = PSP_TITLE_MODERN_CONTROLS | PSP_TITLE_ADAPTIVE_ASPECT |
                    PSP_TITLE_HUD_BANDS,
    .keys_wasd_help = AC_KEYS_WASD_HELP,
    .gamepad_modern_help = AC_GAMEPAD_MODERN_HELP,
    .input = ac_input,
};

/* Button layout is independently overridable so the analog work can be
 * compared with the PSP buttons, or the modern buttons can be used with the
 * original one-stick movement.  By default it follows modern/dual. */
static int gamepad_modern(void) {
    static int modern = -1;
    if (modern < 0) {
        modern = psp_settings_current()->gamepad;
        if (modern)
            printf("      gamepad   modern -- triggers, bumpers and stick clicks are gameplay actions\n");
    }
    return modern;
}

/* Radians of yaw per mouse count. 0.001 puts a full turn at ~6,300 counts,
 * about 20 cm of desk at 800 dpi -- a middling FPS default. */
static float mouse_sens(void) {
    static float k = -1.0f;
    if (k < 0.0f) {
        k = 0.001f * (float)psp_settings_current()->number[LR_MOUSE_SENS];
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

/* ---- adaptive aspect ----------------------------------------------------
 *
 * psp_func_000889B4(camera) is the game's central camera rebuild. It reads
 * the active render descriptor's integer width and height, stores width/480,
 * height/272 and width/height at camera+716/+720/+724, then rebuilds the
 * projection and its culling planes. Six direct callers cover the gameplay,
 * menu and model-preview cameras, and there are no entries into its interior.
 *
 * The guest framebuffer cannot become window-sized: movies, 2D drawing, CPU
 * reads and alpha-backed stencil all depend on its 480x272 layout. What the
 * GE draws wider is decided by one number, the shared scene aspect at
 * 0x00421040+268: missions rebuild the GE projection from it through
 * 0000100C -> 002588D0, the garage copies the display camera's +724 into it
 * in 00154C80, and 00088F6C builds the horizontal cull planes from it. So
 * adaptive mode writes that aspect, and +724 as its garage source, as
 * virtual_w/272, and otherwise lets the rebuild run against the real 480x272
 * descriptor. The height stays 272, preserving vertical FOV; a wider drawable
 * expands the horizontal view and a narrower one contracts it.
 *
 * The rebuild must not see the virtual width itself. Its focal length,
 * (width/2)/tan(fov/2), goes into the camera's own projection at +64 and the
 * combined matrices after it, which the GE never sees: the game projects
 * with them on the CPU to place the lock box, the lock-on reticle and every
 * other HUD element that tracks a world position. An earlier version lent
 * the rebuild the virtual width for the duration of the call, and that
 * scaled this projection by wide/480 in BOTH axes (reports/aspect-reticle,
 * 32:9: P00 289.7 -> 583.6, P11 144.9 -> 291.8) while the GE scene widened
 * horizontally only -- the targeting box grew past the screen edges and the
 * reticle ran off its target. The GL backend places HUD draws 1:1 in the
 * centred 480 columns, which is the scene's own pixel scale, so the HUD
 * projection has to be the native one. */
enum {
    RENDER_SYSTEM = 0x0043D8D0u,
    RENDER_ACTIVE = 260,
    RENDER_WIDTH = 12,
    RENDER_HEIGHT = 16,
    CAMERA_ASPECT = 724,
    SCENE_CAMERA = 0x00421040u,
    SCENE_ASPECT = 268,
};

void psp_func_000889B4(void) {
    if (!present_adaptive_aspect()) {
        psp_func_000889B4__orig();
        return;
    }

    const uint32_t camera = r_a0;
    const uint32_t render = psp_read32(RENDER_SYSTEM + RENDER_ACTIVE);
    const uint32_t old_w = render ? psp_read32(render + RENDER_WIDTH) : 0;
    const uint32_t old_h = render ? psp_read32(render + RENDER_HEIGHT) : 0;
    /* Only the full display camera. Scratch previews and the inset Assembly
     * cameras keep their own aspect. GL places the latter in the centered
     * menu area using their viewport/scissor, on both transform paths. */
    if (old_w != 480u || old_h != 272u) {
        psp_func_000889B4__orig();
        return;
    }

    /* present_aspect_wide_width rounds to the nearest virtual PSP pixel and
     * is the number the GL backend sizes its target by, so camera and target
     * agree. At the native 480:272 ratio it is exactly 480, so enabling the
     * option without resizing remains bit-for-bit on the original path. */
    const uint32_t virtual_w = (uint32_t)present_aspect_wide_width();
    const float aspect = (float)virtual_w / (float)old_h;
    /* Before the rebuild, because 00088F6C reads it for the cull planes;
     * also when resizing back to native width, so the planes contract. */
    f32_write(SCENE_CAMERA + SCENE_ASPECT, aspect);    psp_func_000889B4__orig();
    if (virtual_w == old_w) return;
    /* After it: the rebuild stored 480/272 there, and the garage's 00154C80
     * copies this field into the shared camera before projecting. */
    f32_write(camera + CAMERA_ASPECT, aspect);

    static uint32_t last_virtual_w;
    if (virtual_w != last_virtual_w) {
        int draw_w = 0, draw_h = 0;
        present_gl_drawable_size(&draw_w, &draw_h);
        printf("      aspect    drawable %dx%d, scene %ux%u wide, HUD projection 480x272\n",
               draw_w, draw_h, (unsigned)virtual_w, (unsigned)old_h);
        last_virtual_w = virtual_w;
    }
}

/* ---- the lock target's on-screen test ------------------------------------
 *
 * psp_func_001FBDF8(out) projects the lock target's position at 0x4D4B40
 * through the render system's screen matrix (00255B14/00255B2C), stores the
 * pixel position in out[0..1] and returns 1 when it lies within the render
 * descriptor's rectangle at +40..+52 -- (0,0,480,272) for the display; with
 * no target (the word at 0x4D4B5C clear) it returns 0 at once. The
 * mission loop (00102018) records the point and sets the marker flag only
 * then; 0000356C -> 00003630 -> 001FC6A8 draws the lock marker at the recorded
 * point wherever it is, and the GL backend places a HUD draw beyond the 480
 * columns in the bands of a wide target. So in adaptive mode the horizontal
 * bounds grow by the band width on each side: the target stays "on screen"
 * for as long as the wide window shows it. Vertical bounds, the behind-the-
 * camera test and the no-target case are the original's. */
enum { LOCK_TARGET_STATE = 0x004D4B50u };   /* +12: a target is held */

void psp_func_001FBDF8(void) {
    const uint32_t out = r_a0;
    psp_func_001FBDF8__orig();
    if (r_v0 != 0 || !present_adaptive_aspect()) return;
    const int32_t band = (present_aspect_wide_width() - 480) / 2;
    if (band <= 0) return;
    /* The original said no. Not for want of a target or a point in front of
     * the camera: those leave out[] unconverted, and stay no. */
    if (psp_read32(LOCK_TARGET_STATE + 12) == 0) return;
    if ((int32_t)psp_read32(out + 8) < -4096) return;
    const uint32_t render = psp_read32(RENDER_SYSTEM + RENDER_ACTIVE);
    if (!render) return;
    const int32_t x0 = (int32_t)psp_read32(render + 40), y0 = (int32_t)psp_read32(render + 44);
    const int32_t w = (int32_t)psp_read32(render + 48), h = (int32_t)psp_read32(render + 52);
    const int32_t px = (int32_t)psp_read32(out), py = (int32_t)psp_read32(out + 4);
    if (px < x0 - band || px > x0 + w + band || py < y0 || py > y0 + h) return;
    r_v0 = 1;
}

/* stick2, input_tuning, input_tune(), stick_radial(), stick_look(): host/stick.h,
 * shared with the other titles' replacements. */

/* The left stick as a direction and a magnitude for the walk: the unit vector
 * in the AC's frame and how far past the profile's radial deadzone the stick
 * sits, 0..1. Zero inside the circle.
 *
 * The frame is the game's direction table's: z back, and **x to the left** --
 * the table's entry 3 is (+1, 0) and the game orders its stick actions
 * forward, back, left, right, so 3 is left; the first cut read it as right
 * and the first mission played with the strafes swapped. The stick's x is to
 * the right, hence the sign. */
static float stick_walk(uint8_t axb, uint8_t ayb, float *sx, float *sz) {
    const stick2 s = stick_radial(axb, ayb, input_tune()->move_dead);
    if (s.m == 0.0f) { *sx = 0.0f; *sz = 0.0f; return 0.0f; }
    *sx = -s.x / s.m;
    *sz =  s.y / s.m;
    return s.m;
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

/* A hold the yaw integrator honours. psp_func_000506F8(ac) is
 * *(*(ac+9728)+276) + 5808 -- an object hanging off the AC's movement state --
 * and while the s16 at +24 of that is non-zero the original integrates the
 * rate but leaves the yaw alone. It was first read as the pause flag, and it
 * is not: the pause menu does not set it, it stops calling the AC's update
 * altogether (see in_play below). What does set it is not established; the
 * replacement keeps the original's behaviour either way. Read in C rather
 * than by calling the guest: the guest getter takes the AC in a0, and a
 * replacement that called it from a context where a0 was something else
 * dereferenced that something. (The first in-play gate did exactly that from
 * the stick converter, whose a0 is not an AC, and it read garbage twice a
 * poll until the bad-access counter said so.) NULL where the original's
 * `bnel v0, zero` says NULL. */
static int yaw_held(uint32_t ac) {
    const uint32_t state = psp_read32(ac + AC_STATE_PTR);
    const uint32_t g = psp_read32(state + 276) + 5808;
    return g && (int16_t)(psp_read32(g + 24) & 0xFFFFu) != 0;
}

/* The poll at which the player's AC last ran its per-frame update, recorded
 * by the look integrator below, which the game calls every frame the AC is
 * under control and not at all while the pause menu is up. */
static uint32_t g_ac_update_poll;

static int mouse_take_dx(void);        /* the banked mouse yaw; with the converter, below */

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
    (void)mdx;
    /* The mouse yaw is the bank the converter fills, not this poll's delta:
     * see mouse_hold_update. Taken before the gate so a gated frame does not
     * hold a flick over for a later one. */
    mdx = mode == INPUT_DUAL ? mouse_take_dx() : 0;

    /* One line per call, so "did the game even ask for a turn this frame" is a
     * fact and not a guess. Which of the fourteen movement handlers calls this
     * is the game's decision, made upstream from the thresholded bits. */
    FILE *log = input_log();
    if (log) fprintf(log, "%u ac=%08X ax=%u rx=%u mdx=%d gate=%d max=%.5f",
                     psp_ctrl_polls(), ac, ax, rx, mdx, gated, max);

    if (gated) { if (log) fprintf(log, "\n"); r_v0 = 0; return; }

    /* The hold, as the original. */
    const int held = yaw_held(ac);

    /* Left is negative radians here, matching the original's sign; mouse
     * travel to the right is positive yaw. The look stick gets the expo
     * curve; the game's own stick in `modern` stays linear, since it is also
     * the walk stick. No ease-in here: the chase camera's own filter smooths
     * yaw, and the ramp was the other half of what felt clunky. */
    const stick2 turn = mode == INPUT_DUAL
        ? stick_look(rx, ry)
        : stick_radial(ax, ay, input_tune()->move_dead);
    float rate = max * turn.x;
    if (mode == INPUT_DUAL) rate += mouse_sens() * (float)mdx;

    f32_write(ac + AC_RATE, rate);
    if (rate != 0.0f) psp_write8(ac + AC_DIR, rate < 0.0f ? 0 : 1);
    if (!held) f32_write(ac + AC_YAW, f32_read(ac + AC_YAW) + rate);
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
 *   - action 12 recentres the view; it has no PSP key-assign mask in the
 *     active row, but the modern gamepad can expose it directly;
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
static uint32_t player_pad(uint32_t ac) {
    return psp_read32(psp_read32(ac + AC_PAD_PTR) + 4);
}

static int action_held(uint32_t ac, uint32_t action) {
    r_a0 = player_pad(ac);
    r_a1 = action;
    psp_func_0005EFA0();
    return r_v0 != 0;
}

static int action_pressed(uint32_t ac, uint32_t action) {
    r_a0 = player_pad(ac);
    r_a1 = action;
    psp_func_0005EFD4();
    return r_v0 != 0;
}

static void pitch_log(uint32_t ac, const char *how, int ry, int mdy) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u pitch=%.6f rate=%.6f ry=%d mdy=%d %s\n",
                     psp_ctrl_polls(), f32_read(ac + AC_PITCH),
                     f32_read(ac + AC_PITCH_RATE), ry, mdy, how);
}

/* Action 12's branch in the original look integrator zeroes the angle and
 * rate, then starts a short input lockout.  The branch is only reachable from
 * the PSP analog-look path, which the common digital layout never enters.
 * Reproduce those exact writes here so a dedicated modern-pad button works in
 * both enhanced movement modes. */
static void view_reset(uint32_t ac) {
    const float delay = f32_read(LOOK_PARAMS);
    const int rounded = (int)(delay + (delay < 0.0f ? -1.0f : 1.0f));
    f32_write(ac + AC_PITCH, 0.0f);
    f32_write(ac + AC_PITCH_RATE, 0.0f);
    psp_write32(ac + AC_LOOK_COUNTER, (uint32_t)((rounded * 30) / 60));
}

void psp_func_00053234(void) {
    const uint32_t ac = r_a0;
    if (ac == PLAYER_AC) g_ac_update_poll = psp_ctrl_polls();   /* the heartbeat in_play reads */
    if (ac == PLAYER_AC && gamepad_modern() && in_play() &&
        action_pressed(ac, 12)) {
        view_reset(ac);
        pitch_log(ac, "semantic-reset", 0, 0);
        return;
    }
    if (input_mode() != INPUT_DUAL || ac != PLAYER_AC) {
        r_a0 = ac;
        psp_func_00053234__orig();
        if (ac == PLAYER_AC) pitch_log(ac, "orig", 0, 0);
        return;
    }

    /* The state gate, as the yaw integrator. */
    if (!in_play()) return;

    uint8_t rx, ry;
    int mdx, mdy;
    psp_ctrl_last_look(&rx, &ry, &mdx, &mdy);

    /* The game's own digital look and its lockout. */
    if ((int32_t)psp_read32(ac + AC_LOOK_COUNTER) > 0 ||
        action_held(ac, 10) || action_held(ac, 11)) {
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
    const stick2 look = stick_look(rx, ry);
    const float target = -max * look.y;
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

static void walk_cadence_note(uint32_t ra, float m);   /* defined with the updater, below */

/* `ra` is the guest return address at entry -- which handler made this
 * push. The walk's is psp_func_00054894, see the cadence below. */
static void push_log(uint32_t ac, uint32_t ra, const char *how, float m, float sx, float sz, float cap) {
    FILE *log = input_log();
    if (log) fprintf(log, "%u push m=%.3f dir=%+.3f,%+.3f cap=%.4f speed=%.4f pos=%.3f,%.3f ra=%08X %s\n",
                     psp_ctrl_polls(), m, sx, sz, cap,
                     hypotf(f32_read(ac + AC_VX), f32_read(ac + AC_VZ)),
                     f32_read(ac + AC_X), f32_read(ac + AC_Z), ra, how);
}

void psp_func_0004F06C(void) {
    const uint32_t ac = r_a0, idx = r_a1, ra = r_ra;
    const float accel = psp_cpu.f[12], cap = psp_cpu.f[13];
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC || ac != PLAYER_AC || !in_play()) {
        psp_func_0004F06C__orig();
        if (ac == PLAYER_AC) push_log(ac, ra, "orig", 1.0f, 0.0f, 0.0f, cap);
        return;
    }

    uint8_t axb, ayb;
    psp_ctrl_last_stick(&axb, &ayb);
    float sx, sz, m = stick_walk(axb, ayb, &sx, &sz);
    if (mode == INPUT_MODERN) {
        /* The game's direction, the processed magnitude from Y alone. */
        const stick2 move = stick_radial(axb, ayb, input_tune()->move_dead);
        m = fabsf(move.y);
        if (idx > 7) m = 0.0f;
        else { sx = f32_read(PUSH_DIR_TABLE + 8 * idx); sz = f32_read(PUSH_DIR_TABLE + 8 * idx + 4); }
    }
    if (m <= 0.0f) {
        r_a0 = ac; r_a1 = idx; psp_cpu.f[12] = accel; psp_cpu.f[13] = cap;
        psp_func_0004F06C__orig();
        push_log(ac, ra, "orig-centred", 1.0f, 0.0f, 0.0f, cap);
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
    walk_cadence_note(ra, m);
    push_log(ac, ra, mode == INPUT_DUAL ? "dual" : "modern", m, sx, sz, cap * m);
}

/* ---- the walk cycle's cadence ----------------------------------------------
 *
 * The walk animation plays at one speed. The player's walk handler
 * (psp_func_00065824) re-issues its play call every frame --
 * psp_func_000460C0 -> 001DF060 -> 001DFA58 -- naming the cycle for the
 * direction (a byte from the table at 0x0030B5DA: forward 2, then 4..16,
 * idle 0), with loop set and a 20-tick fade; the play call writes the
 * slot's per-frame *step* (+92) as 1, and the model updater
 * psp_func_001DFC80 adds that step to the frame (+76) once a frame and
 * wraps it at the frame count (+78) when the slot loops. Nothing in it
 * knows the AC's speed: the cycle was authored for the cap, and with the
 * push above walking at any speed a creep slides its feet.
 *
 * The step is an integer and the frame is sampled at whole 60 Hz ticks
 * (the updater looks the pose up at 2*frame), so the engine offers no
 * fractional playback. What it does offer is a step of 0 -- the frame
 * holds -- and that is enough: hold the frame on the frames an accumulator
 * fed with the stick's magnitude says to, and the cycle advances m frames
 * per frame on average, one stride per stride's worth of ground. At full
 * deflection the accumulator carries every frame and nothing changes; at
 * half, every other frame; a creep at 0.3 advances three frames in ten.
 * Fifteen frames a second at half speed is a visible cadence, and the
 * feet stop sliding.
 *
 * Done in the updater rather than by poking the step from the push: the
 * push runs inside the handler, the updater later and for every model, and
 * a 0 left in a slot by a handler that then stops calling would freeze
 * that slot for good. Here the looping slots' steps are zeroed for the one
 * call and put back, so the game's own state is never changed. The push
 * only records that a walk happened this frame and how hard.
 *
 * Which pushes are walks: the return addresses inside psp_func_00054894,
 * the walk's push helper, which the handlers 000655D4/00065824/00065EB0/
 * 00066100 call. Measured on the mission-1 AC's legs; another leg type
 * may take another path, in which case the push line's ra= says so and the
 * cadence stays the game's (the analyzer prints it either way). */

/* The model (ac+128) owns an array of animation slots: count at model+116,
 * base at model+120, 128 bytes each. */
enum { AC_MODEL = 128, MODEL_SLOT_COUNT = 116, MODEL_SLOTS = 120, SLOT_SIZE = 128,
       SLOT_ENABLED = 72, SLOT_ANIM = 73, SLOT_LAYERS = 74, SLOT_LOOP = 75,
       SLOT_FRAME = 76, SLOT_FRAMES = 78, SLOT_STEP = 92, MAX_SLOTS = 32 };

enum { WALK_PUSH_FIRST = 0x00054A28u, WALK_PUSH_LAST = 0x00054B48u };

static struct { uint32_t poll; float m, phase; int hold; } g_walk;

static void walk_cadence_note(uint32_t ra, float m) {
    if (ra < WALK_PUSH_FIRST || ra > WALK_PUSH_LAST) return;
    const uint32_t poll = psp_ctrl_polls();
    if (poll == g_walk.poll) return;              /* a diagonal walk pushes twice a frame */
    if (poll != g_walk.poll + 1) g_walk.phase = 0.0f;   /* a fresh walk starts in phase */
    g_walk.poll = poll;
    g_walk.m = m;
    g_walk.phase += m;
    if (g_walk.phase >= 1.0f) { g_walk.phase -= 1.0f; g_walk.hold = 0; }
    else g_walk.hold = 1;
}

/* One line a frame for the player's model, every slot that plays:
 * <poll> anim hold=<0|1> m=<0..1> <slot>:<anim>:<frame>/<frames>:<step><L if looping> ... */
static void anim_log(uint32_t model, int hold) {
    FILE *log = input_log();
    if (!log) return;
    const uint32_t n = psp_read16(model + MODEL_SLOT_COUNT), base = psp_read32(model + MODEL_SLOTS);
    fprintf(log, "%u anim hold=%d m=%.3f", psp_ctrl_polls(), hold, hold >= 0 ? g_walk.m : 0.0f);
    for (uint32_t i = 0; i < n && i < MAX_SLOTS; i++) {
        const uint32_t s = base + SLOT_SIZE * i;
        if (psp_read8(s + SLOT_ENABLED) || psp_read8(s + SLOT_ANIM) == 0xFF) continue;
        fprintf(log, " %u:%u:%u/%u:%u%s", i, psp_read8(s + SLOT_ANIM), psp_read16(s + SLOT_FRAME),
                psp_read16(s + SLOT_FRAMES), psp_read8(s + SLOT_STEP), psp_read8(s + SLOT_LOOP) ? "L" : "");
    }
    fputc('\n', log);
}

void psp_func_001DFC80(void) {
    const uint32_t model = r_a0;
    const int player = model && model == psp_read32(PLAYER_AC + AC_MODEL);
    /* The walk noted this frame or the one before -- the updater's place in
     * the frame relative to the handler is not assumed. */
    const int walking = player && input_mode() != INPUT_CLASSIC &&
                        psp_ctrl_polls() - g_walk.poll <= 1;
    if (!walking || !g_walk.hold) {
        psp_func_001DFC80__orig();
        if (player) anim_log(model, walking ? 0 : -1);
        return;
    }

    const uint32_t n = psp_read16(model + MODEL_SLOT_COUNT), base = psp_read32(model + MODEL_SLOTS);
    uint8_t saved[MAX_SLOTS];
    for (uint32_t i = 0; i < n && i < MAX_SLOTS; i++) {
        const uint32_t s = base + SLOT_SIZE * i;
        saved[i] = psp_read8(s + SLOT_STEP);
        if (!psp_read8(s + SLOT_ENABLED) && psp_read8(s + SLOT_ANIM) != 0xFF &&
            psp_read8(s + SLOT_LOOP) && (psp_read8(s + SLOT_LAYERS) & 1))
            psp_write8(s + SLOT_STEP, 0);
    }
    psp_func_001DFC80__orig();
    for (uint32_t i = 0; i < n && i < MAX_SLOTS; i++)
        psp_write8(base + SLOT_SIZE * i + SLOT_STEP, saved[i]);
    anim_log(model, 1);
}

/* ---- the chase camera's lag ------------------------------------------------
 *
 * The chase camera is a mode handler, psp_func_000757E8, run once a frame by
 * the camera update psp_func_00074168 for the camera struct at 0x0043C080
 * (288 bytes; the eye at +16, the look-at target at +48, the yaw at +36 and
 * pitch at +32, which are just the atan2/asin of target minus eye). The
 * handler computes where the camera *should* be -- the target is the AC's
 * position lifted by the legs' height parameter and pushed 7.0 units ahead
 * along the AC's heading; the eye 17.9 units behind it (30.0 in one mode)
 * along the heading and the look pitch -- and then does not go there. It
 * blends: psp_func_0007467C(ideal, current, r) for the target and
 * psp_func_000746D4(ideal, current, rx, ry, rz) for the eye write
 * current + (ideal - current) * (1 - r) per axis, with r read from the
 * struct: +104 for the eye, 0.83, and +108 for the target, 0.8309 (0.9208
 * for the eye's first frames, then psp_func_000752A0 re-sets 0.83 by a
 * distance rule). Keeping 83% of the gap each frame is the geometric tail
 * every yaw measurement showed -- a 0.2 rad mouse step at poll 2100 in
 * scenarios/cam-step.pad leaves the camera's yaw converging at ratio 0.83,
 * sixteen frames to 95%, half a second behind the AC. With the game's own
 * 2.1 deg/frame turn cap that lag reads as weight; behind a mouse it reads
 * as a camera on a rubber band.
 *
 * PSPRECOMP_CAMERA_LAG=<r> sets r for the player's camera under `modern` or
 * `dual`: 0.83 is the game's, 0.5 halves the time to settle, 0 fixes the
 * camera to the AC. Unset, the game's values stand; classic never reads it.
 * The eye's vertical rate keeps the game's own modulation -- it follows
 * less when the AC looks up -- as the ratio of what the handler passed. Two
 * leaf replacements, acting only when the `current` argument is camera 0's
 * eye or target; psp_func_0007467C is a general vector blend with eight
 * callers and the rest of them see the original. */

enum { CAMERA0 = 0x0043C080u, CAM_EYE = 16, CAM_TARGET = 48, CAM_PITCH = 32, CAM_YAW = 36 };

/* PSPRECOMP_CAMERA_LAG, clamped to 0..0.99; negative when unset. */
static float camera_lag(void) {
    static float r = -2.0f;
    if (r < -1.5f) {
        r = (float)psp_settings_current()->number[LR_CAMERA_LAG];
        if (r >= 0.0f) {
            if (r > 0.99f) r = 0.99f;
            printf("      camera    lag %.2f per frame (the game keeps 0.83); "
                   "PSPRECOMP_INPUT=classic ignores it\n", r);
        }
    }
    return r;
}

static int camera_lag_wanted(uint32_t current, uint32_t which) {
    return input_mode() != INPUT_CLASSIC && current == CAMERA0 + which && camera_lag() >= 0.0f;
}

void psp_func_0007467C(void) {
    if (camera_lag_wanted(r_a1, CAM_TARGET)) psp_cpu.f[12] = camera_lag();
    psp_func_0007467C__orig();
}

void psp_func_000746D4(void) {
    const int ours = camera_lag_wanted(r_a1, CAM_EYE);
    const float game = psp_cpu.f[12];
    if (ours) {
        const float ky = game > 0.0f ? psp_cpu.f[13] / game : 1.0f;
        psp_cpu.f[12] = camera_lag();
        psp_cpu.f[13] = camera_lag() * ky;
        psp_cpu.f[14] = camera_lag();
    }
    /* One line a frame for the player's camera, in every mode: the rate the
     * eye was blended with and the camera's angles from the previous frame,
     * beside the AC's, so the lag is a number and not an impression. */
    FILE *log = input_log();
    if (log && r_a1 == CAMERA0 + CAM_EYE)
        fprintf(log, "%u cam r=%.4f yaw=%.6f pitch=%.6f ac_yaw=%.6f %s\n", psp_ctrl_polls(),
                psp_cpu.f[12], f32_read(CAMERA0 + CAM_YAW), f32_read(CAMERA0 + CAM_PITCH),
                f32_read(PLAYER_AC + AC_YAW), ours ? "ours" : "game");
    psp_func_000746D4__orig();
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
 * `modern` and `dual` read the recorded raw stick rather than the already
 * deadzoned a1/a2 here.  That is important: accepting the game's 30-unit
 * circle and adding a profile deadzone would recreate the double-deadzone we
 * removed from the SDL side.  A small enter/exit gap keeps the selected state
 * from chattering when the stick rests on the radial boundary.
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

enum { MOUSE_HOLD_POLLS = 4 };
static int g_move_gate, g_turn_gate;

/* Mouse travel arrives at the mouse's rate and this runs at the guest's; a
 * bit that dropped the poll after a motion event would flicker the state
 * machine. Keep the direction for a few polls. Per poll, not per call: the
 * adaptor calls this once for each of two virtual pads.
 *
 * The travel itself is banked here too, and the yaw integrator draws on the
 * bank rather than on the poll's own delta. The integrator runs only inside
 * a movement state, and the state the turn bits ask for engages a few
 * frames after they rise -- so the first polls of every flick from a
 * standstill went nowhere (scenarios/cam-step.pad's single poll of 200
 * counts turned the AC by nothing at all). Banked, they are applied on the
 * integrator's first frame instead: a catch-up rather than a loss. Pitch
 * needs none of this; its integrator runs every frame in play. */
static struct { uint32_t poll; int x_left; int sx; int dx; } g_mouse_hold;

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

/* Is the player being played? The converter runs everywhere the pad is read
 * -- menus, the garage, the intro, the pause menu -- and the first windowed
 * try of `dual` showed why that matters: mouse jitter read as circle, which
 * cancels menus, and an off-axis push read as L/R, which changes tabs. Two
 * things say yes. The game's own state: the player's movement-state object
 * at ac+9728, NULL in the garage and set in a mission, with its byte at +182
 * equal to -1 while the AC is under control. And a heartbeat: the AC's
 * per-frame update ran within the last two polls -- the look integrator
 * above records the poll -- because the pause menu leaves every AC flag as
 * it was and simply stops running the AC (scenarios/pause-look.pad: 190
 * polls without a single integrator call, the state byte -1 throughout).
 * Two polls rather than one so the order of the pad read and the update
 * within a frame does not matter. Outside that, every mode is the shipped
 * converter, bit for bit. */
static int in_play(void) {
    const uint32_t state = psp_read32(PLAYER_AC + AC_STATE_PTR);
    if (!state || (int8_t)psp_read8(state + 182) != -1) return 0;
    return psp_ctrl_polls() - g_ac_update_poll <= 2;
}

/* ---- physical gamepad -> game actions -----------------------------------
 *
 * The PSP packet has no triggers or stick clicks.  present.c carries the ten
 * modern-pad controls in button bits the game's twelve-entry PSP converter
 * ignores; because they are still in the sceCtrl packet, recordings retain
 * them without a live-input side channel.  This replacement removes those
 * carrier bits and remembers their held/edge state.  In menus it turns the
 * familiar face buttons and bumpers back into PSP buttons.  In play the two
 * action-query helpers below answer semantic actions directly, independent of
 * the user's PSP key-assign row.
 *
 * The action indices are the game's own list (confirmed against their gameplay
 * call sites and the active row at cfg+80): 4 change weapon, 5 boost, 6 arm R,
 * 7 arm L/event, 12 view reset, 13 extension, 14 inside, 15 OB/EO and 16
 * disarmament.  Actions 8 and 9 are strafe left/right; confusing the PSP
 * shoulder masks in the active row for their semantic meaning made the first
 * modern-pad cut faithfully strafe on LB/RB.  The last
 * three have zero masks in the PSP row, which is why merely changing a PSP
 * button table cannot expose them. */
static struct {
    uint32_t poll;
    uint32_t down;
    uint32_t pressed;
} g_extra_pad;

static uint32_t extra_menu_buttons(uint32_t extra) {
    uint32_t psp = 0;
    if (extra & PSP_PAD_A)  psp |= 0x004000u;       /* Cross    */
    if (extra & PSP_PAD_B)  psp |= 0x002000u;       /* Circle   */
    if (extra & PSP_PAD_X)  psp |= 0x008000u;       /* Square   */
    if (extra & PSP_PAD_Y)  psp |= 0x001000u;       /* Triangle */
    if (extra & (PSP_PAD_LB | PSP_PAD_LT)) psp |= 0x000100u;
    if (extra & (PSP_PAD_RB | PSP_PAD_RT)) psp |= 0x000200u;
    return psp;
}

static uint32_t extra_for_action(uint32_t action) {
    switch (action) {
    case 4:  return PSP_PAD_RB;       /* Change unit         */
    case 5:  return PSP_PAD_LT;       /* Boost / jump        */
    case 6:  return PSP_PAD_RT;       /* Arm unit R          */
    case 7:  return PSP_PAD_LB;       /* Arm unit L / event  */
    case 12: return PSP_PAD_B;        /* Look reset          */
    case 13: return PSP_PAD_L3;       /* Extension           */
    case 14: return PSP_PAD_A;        /* Inside              */
    case 15: return PSP_PAD_R3;       /* OB / EO              */
    case 16: return PSP_PAD_Y;        /* Disarmament modifier */
    default: return 0;
    }
}

static int player_pad_is(uint32_t pad) {
    const uint32_t holder = psp_read32(PLAYER_AC + AC_PAD_PTR);
    return holder && pad && psp_read32(holder + 4) == pad;
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

void psp_func_00279A10(void) {
    const uint32_t state = r_a0;
    const uint32_t raw = r_a1;
    const uint32_t extra = raw & PSP_PAD_EXTRA;
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

    uint32_t psp = raw & ~PSP_PAD_EXTRA;
    if (gamepad_modern() && !in_play()) psp |= extra_menu_buttons(extra);
    r_a0 = state;
    r_a1 = psp;
    psp_func_00279A10__orig();
}

void psp_func_0005EFA0(void) {
    const uint32_t pad = r_a0, action = r_a1;
    psp_func_0005EFA0__orig();
    const uint32_t bit = extra_for_action(action);
    if (gamepad_modern() && bit && in_play() && player_pad_is(pad) &&
        (g_extra_pad.down & bit)) {
        r_v0 = 1;
        extra_action_log(action, 0);
    }
}

void psp_func_0005EFD4(void) {
    const uint32_t pad = r_a0, action = r_a1;
    psp_func_0005EFD4__orig();
    const uint32_t bit = extra_for_action(action);
    if (gamepad_modern() && bit && in_play() && player_pad_is(pad) &&
        (g_extra_pad.pressed & bit)) {
        r_v0 = 1;
        extra_action_log(action, 1);
    }
}

/* The game's disarmament-modifier check is already a semantic helper, but
 * only one PSP control layout asks action 16 directly; the default rebuilds
 * it as the four-button chord.  Y should be the modifier regardless of the
 * saved layout, so inject here rather than pretending four unrelated actions
 * are held everywhere else.  The game's next check still requires the weapon,
 * extension or left-arm button that selects which part to purge. */
void psp_func_0005F348(void) {
    const uint32_t pad = r_a0;
    psp_func_0005F348__orig();
    if (gamepad_modern() && in_play() && player_pad_is(pad) &&
        (g_extra_pad.down & PSP_PAD_Y)) {
        r_v0 = 1;
        extra_action_log(16, 0);
    }
}

void psp_func_00279A50(void) {
    const int mode = input_mode();
    if (mode == INPUT_CLASSIC) { psp_func_00279A50__orig(); return; }

    const int ax = (int8_t)(psp_cpu.r[5] & 0xFF);       /* a1 */
    const int ay = (int8_t)(psp_cpu.r[6] & 0xFF);       /* a2 */
    const int play = in_play();
    /* Once per transition: "<poll> play=<0|1>", so a log shows where the pause
     * menu (or the results screen, or the garage) took the stick back. */
    static int last_play = -1;
    if (play != last_play) {
        FILE *log = input_log();
        if (log) fprintf(log, "%u play=%d\n", psp_ctrl_polls(), play);
        last_play = play;
    }
    if (!play) {
        /* The originals read a1/a2 themselves; hand them back untouched. A
         * drag across the pause menu is not owed to the AC on resume. */
        g_mouse_hold.dx = 0;
        g_mouse_hold.x_left = 0;
        g_move_gate = g_turn_gate = 0;
        psp_cpu.r[5] = (uint32_t)(uint8_t)ax;
        psp_cpu.r[6] = (uint32_t)(uint8_t)ay;
        psp_func_00279A50__orig();
        return;
    }
    uint8_t axb, ayb;
    psp_ctrl_last_stick(&axb, &ayb);
    const input_tuning *tune = input_tune();
    const stick2 move = stick_radial(axb, ayb, tune->move_dead);
    g_move_gate = hysteresis(g_move_gate, move.raw_m,
                             tune->move_enter, tune->move_dead);

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
        r_v0 = bits;
        return;
    }

    uint8_t rxb, ryb;
    int mdx, mdy;
    psp_ctrl_last_look(&rxb, &ryb, &mdx, &mdy);
    mouse_hold_update(mdx);
    const stick2 look = stick_look(rxb, ryb);
    const float horizontal = fabsf(look.x);
    g_turn_gate = hysteresis(g_turn_gate, horizontal, 0.04f, 0.015f);
    (void)mdy;                              /* the pitch integrator's, above */

    /* Turn: right stick X, else recent mouse X. */
    if      (g_turn_gate && look.x > 0.0f) bits = 0x8000u;
    else if (g_turn_gate && look.x < 0.0f) bits = 0x4000u;
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
    }
    r_v0 = bits;
}

/* What these replacements carry from one poll to the next, named to a save
 * state (psprecomp/state.h) by boot.c, with the mission loop's own. */
void lr_replacements_keep(void) {
    PSP_STATE_KEEP(g_ac_update_poll);
    PSP_STATE_KEEP(g_walk);
    PSP_STATE_KEEP(g_move_gate);
    PSP_STATE_KEEP(g_turn_gate);
    PSP_STATE_KEEP(g_mouse_hold);
    PSP_STATE_KEEP(g_extra_pad);
    fps_aclr_keep();
}
