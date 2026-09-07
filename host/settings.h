#ifndef LR_SETTINGS_H
#define LR_SETTINGS_H

#include <stdio.h>

enum lr_option {
    LR_RESOLUTION, LR_ASPECT, LR_WINDOW_SIZE, LR_WINDOW_MODE, LR_DISPLAY,
    LR_INPUT, LR_GAMEPAD, LR_KEYS,
    LR_MOUSE, LR_MOUSE_SENS, LR_MOVE_DEADZONE, LR_LOOK_DEADZONE,
    LR_STICK_OUTER_DEADZONE, LR_LOOK_EXPO, LR_CAMERA_LAG,
    LR_RENDER, LR_AUDIO_LEAD_MS, LR_AUDIO_PREROLL_MS, LR_MPEG_DECODE,
    LR_WINDOW, LR_REALTIME, LR_OPTION_COUNT
};
enum lr_type { LR_CHOICE, LR_NUMBER, LR_SIZE, LR_INTEGER };
enum lr_source { LR_DEFAULT, LR_PRESET, LR_ENV, LR_COMMAND_LINE };
enum { LR_MAX_PRESETS = 32, LR_NAME_SIZE = 64, LR_VALUE_SIZE = 96, LR_ERROR_SIZE = 512 };

typedef struct {
    const char *key, *env, *label, *page, *help;
    enum lr_type type;
    const char *dflt;
    /* Choice values and corresponding display labels, separated by '|'. */
    const char *choices, *labels;
    double min, max, step;
    const char *special;            /* Optional numeric sentinel, value -1. */
} lr_option_def;

typedef struct {
    char value[LR_OPTION_COUNT][LR_VALUE_SIZE];
    double number[LR_OPTION_COUNT]; /* Numeric value or choice index. */
    enum lr_source source[LR_OPTION_COUNT];
    int width, height;
    int render, gamepad, window, realtime; /* Derived choices, resolved once. */
} lr_settings;

typedef struct { char name[LR_NAME_SIZE]; lr_settings settings; } lr_preset;
typedef struct { int count, selected; lr_preset presets[LR_MAX_PRESETS]; } lr_presets;

extern const lr_option_def lr_options[LR_OPTION_COUNT];
/* Error buffers passed to this API must hold LR_ERROR_SIZE bytes. All
 * player settings apply at startup; no mutation is supported during play. */
void lr_settings_defaults(lr_settings *s);
int lr_settings_set(lr_settings *s, int id, const char *value,
                    enum lr_source source, char *error);
int lr_settings_env(lr_settings *s, char *error);
int lr_settings_resolve(lr_settings *s, char *error);
void lr_settings_print(const lr_settings *s, FILE *out);
void lr_option_label(const lr_settings *s, int id, char *out, size_t size);
/* Installation is startup-only, before any guest/SDL threads are started.
 * Standalone fixtures that do not install a snapshot get defaults + env. */
void lr_settings_use(const lr_settings *s);
const lr_settings *lr_settings_current(void);

void lr_presets_defaults(lr_presets *p);
int lr_presets_find(const lr_presets *p, const char *name);
int lr_presets_name_valid(const char *name);
int lr_presets_add(lr_presets *p, const char *name, const lr_settings *s,
                   char *error);
int lr_presets_load(lr_presets *p, const char *path, char *error);
int lr_presets_save(const lr_presets *p, const char *path, char *error);
/* No implicit preferences file: NULL path means defaults + env only. */
int lr_settings_load(lr_settings *s, const char *path, const char *preset,
                     char *error);

#endif
