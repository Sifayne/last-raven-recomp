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
    LR_STICK_OUTER_DEADZONE, LR_LOOK_EXPO, LR_CAMERA_LAG,
    LR_RENDER, LR_AUDIO_LEAD_MS, LR_AUDIO_PREROLL_MS, LR_MPEG_DECODE,
    LR_WINDOW, LR_REALTIME, LR_HIGH_FPS, LR_FPS_CAP, LR_OPTION_COUNT
};

extern const psp_option_def lr_options[LR_OPTION_COUNT];

#endif
