#ifndef LR_SETTINGS_H
#define LR_SETTINGS_H
/* Armored Core's player options. The mechanism -- types, precedence, the
 * preferences file -- and the options every game shares are psprecomp's
 * (psprecomp/host/settings.h); this enum indexes a psp_settings, the
 * player's options first under the names this pack has always used for
 * them, then the pack's own table, host/settings.c's psp_title_settings. */
#include <psprecomp/host/settings.h>

enum lr_option {
    LR_RESOLUTION = PSP_OPT_RESOLUTION, LR_WINDOW_MODE = PSP_OPT_WINDOW_MODE,
    LR_WINDOW_SIZE = PSP_OPT_WINDOW_SIZE, LR_DISPLAY = PSP_OPT_DISPLAY, LR_VOLUME = PSP_OPT_VOLUME,
    LR_ACTIVE_PAD = PSP_OPT_ACTIVE_PAD, LR_STATE_LOAD = PSP_OPT_STATE_LOAD,
    LR_STATE_START = PSP_OPT_STATE_START, LR_RENDER = PSP_OPT_RENDER,
    LR_AUDIO_LEAD_MS = PSP_OPT_AUDIO_LEAD_MS, LR_AUDIO_PREROLL_MS = PSP_OPT_AUDIO_PREROLL_MS,
    LR_MPEG_DECODE = PSP_OPT_MPEG_DECODE, LR_WINDOW = PSP_OPT_WINDOW, LR_REALTIME = PSP_OPT_REALTIME,
    /* The pack's own. */
    LR_ASPECT = PSP_PLAYER_OPTIONS, LR_HIGH_FPS, LR_FPS_CAP,
    LR_INPUT, LR_GAMEPAD, LR_KEYS,
    LR_MOUSE, LR_MOUSE_SENS, LR_MOVE_DEADZONE, LR_LOOK_DEADZONE,
    LR_STICK_OUTER_DEADZONE, LR_LOOK_EXPO, LR_CAMERA_LAG, LR_OPTION_COUNT
};

extern const psp_option_def lr_options[LR_OPTION_COUNT - PSP_PLAYER_OPTIONS];

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
