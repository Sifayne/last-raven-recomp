/* Stick arithmetic shared by the titles' native replacements.
 *
 * Moved here verbatim from host/replacements.c so that a second title's
 * replacements (host/replacements-<slug>.c) read the sticks the way Last
 * Raven's do -- same radial deadzones, same look curve, same gates -- and the
 * settings screen's sliders mean the same thing in every game.
 *
 * The including file defines, before this header:
 *     static int input_mode(void);   INPUT_CLASSIC / INPUT_MODERN / INPUT_DUAL
 * from the settings, and includes settings.h. */
#ifndef LAST_RAVEN_STICK_H
#define LAST_RAVEN_STICK_H

#include <math.h>
#include <stdint.h>
#include <stdio.h>

#include "settings.h"

enum { INPUT_CLASSIC = 0, INPUT_MODERN = 1, INPUT_DUAL = 2 };

static int input_mode(void);

typedef struct {
    float x, y;                    /* processed components, magnitude `m` */
    float m;                       /* 0..1 after the radial deadzones       */
    float raw_m;                   /* magnitude before them                */
} stick2;

typedef struct {
    int init;
    float move_dead, move_enter;
    float look_dead, outer_dead, look_expo;
} input_tuning;

static const input_tuning *input_tune(void) {
    static input_tuning t;
    if (!t.init) {
        t.move_dead = (float)lr_settings_current()->number[LR_MOVE_DEADZONE];
        t.move_enter = fminf(t.move_dead + 0.03f, 0.55f);
        t.look_dead = (float)lr_settings_current()->number[LR_LOOK_DEADZONE];
        t.outer_dead = (float)lr_settings_current()->number[LR_STICK_OUTER_DEADZONE];
        t.look_expo = (float)lr_settings_current()->number[LR_LOOK_EXPO];
        t.init = 1;
        if (input_mode() != INPUT_CLASSIC)
            printf("      sticks    radial -- move %.0f/%.0f%% exit/enter, "
                   "look %.0f%%, outer %.0f%%, expo %.2f\n",
                   100.0f * t.move_dead, 100.0f * t.move_enter,
                   100.0f * t.look_dead, 100.0f * t.outer_dead, t.look_expo);
    }
    return &t;
}

static float byte_axis(uint8_t v) {
    const int x = (int)v - 128;
    return x < 0 ? x / 128.0f : x / 127.0f;
}

/* One radial inner deadzone, applied after the byte is in the recorded input
 * lane.  The host deliberately does no deadzoning of its own.  Rescaling the
 * remaining radius makes the first live value continuous at zero, preserves
 * the stick's angle, and still reaches one on a pad whose rim falls short. */
static stick2 stick_radial(uint8_t xb, uint8_t yb, float dead) {
    const input_tuning *t = input_tune();
    stick2 out = { byte_axis(xb), byte_axis(yb), 0.0f, 0.0f };
    out.raw_m = hypotf(out.x, out.y);
    if (out.raw_m <= dead) { out.x = out.y = 0.0f; return out; }
    const float outer = 1.0f - t->outer_dead;
    const float rim = fmaxf(outer, dead + 0.01f);
    const float radius = fminf(out.raw_m, rim);
    out.m = (radius - dead) / (rim - dead);
    const float scale = out.m / out.raw_m;
    out.x *= scale;
    out.y *= scale;
    return out;
}

/* Shape the look stick's radial magnitude, then restore its direction.  A
 * component-wise cubic bends diagonals; doing it once to the radius does not. */
static stick2 stick_look(uint8_t xb, uint8_t yb) {
    const input_tuning *t = input_tune();
    stick2 out = stick_radial(xb, yb, t->look_dead);
    if (out.m == 0.0f) return out;
    const float curved = (1.0f - t->look_expo) * out.m +
                         t->look_expo * out.m * out.m * out.m;
    const float scale = curved / out.m;
    out.x *= scale;
    out.y *= scale;
    out.m = curved;
    return out;
}

static int hysteresis(int held, float value, float enter, float leave) {
    return held ? value > leave : value >= enter;
}

#endif
