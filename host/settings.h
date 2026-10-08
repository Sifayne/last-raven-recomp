#ifndef LR_SETTINGS_H
#define LR_SETTINGS_H
/* Armored Core's player options. The mechanism -- types, presets, precedence,
 * the preferences file -- is psprecomp's (psprecomp/host/settings.h); this
 * enum indexes the table host/settings.c hands it, as psp_title_settings. */
#include <psprecomp/host/settings.h>

enum lr_option {
    LR_RESOLUTION, LR_ASPECT, LR_WINDOW_SIZE, LR_WINDOW_MODE, LR_DISPLAY,
    LR_INPUT, LR_GAMEPAD, LR_KEYS,
    LR_MOUSE, LR_MOUSE_SENS, LR_MOVE_DEADZONE, LR_LOOK_DEADZONE,
    LR_STICK_OUTER_DEADZONE, LR_LOOK_EXPO, LR_CAMERA_LAG, LR_ACTIVE_PAD,
    LR_RENDER, LR_AUDIO_LEAD_MS, LR_AUDIO_PREROLL_MS, LR_MPEG_DECODE,
    LR_WINDOW, LR_REALTIME, LR_HIGH_FPS, LR_FPS_CAP, LR_OPTION_COUNT
};

extern const psp_option_def lr_options[LR_OPTION_COUNT];

/* What the shared host prints at startup about Armored Core's own controls,
 * for each title's psp_title_info (psprecomp/host/title.h). */
#define AC_KEYS_WASD_HELP \
    "keys wasd stick | space/z cross, x circle, c square, v triangle | " \
    "q change weapon, e R shoulder | enter start | backspace select | " \
    "mouse: left = right weapon, right/middle = left weapon (default key assign) | " \
    "close window to stop"
#define AC_GAMEPAD_MODERN_HELP \
    "gamepad LT boost | RT right arm | LB left arm | RB switch | L3 extension | " \
    "R3 OB/EO | A inside | B view reset | Y purge modifier | X spare"

#endif
