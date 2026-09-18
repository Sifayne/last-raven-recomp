# Settings and presets

Run `scripts/15-settings.sh` after building the game. The script rebuilds the
host as needed, opens the settings screen, and **Save & Play** starts the game
with the selected preset. `OPT=1` and `TRACE=1` select the same builds as the
existing boot/replay scripts.

The launcher uses SDL2 and SDL2_ttf, plus a system TrueType font. It tries Noto
Sans and DejaVu Sans on Linux; `--font /path/to/font.ttf` selects another font.
The settings parser and headless inspection tool require neither SDL nor game
data. The current launcher process handoff uses the project's POSIX host
environment; Windows packaging remains part of M6.

## First launch

Three starter presets are created in memory. They become persistent when
you choose Save or Save & Play:

| Preset | Starting setup |
| --- | --- |
| Classic | Original controls, original resolution/aspect, OpenGL window |
| Controller | Dual controls, modern controller actions, rendering resolution matching the window, original aspect |
| Mouse & Keyboard | Controller setup plus WASD and mouse capture |

All start at a 960x544 window size. Intro decoding is enabled when the
runtime supports it. Resolution and aspect are separate settings: **Match
window** resolution makes the image sharper, while **Match window** aspect
expands the 3D view to fit wider windows. Enhanced modes require OpenGL.

On Graphics, set **Window mode → Windowed fullscreen** to fill the display
with a borderless game window at the current desktop resolution. This choice
is saved with each preset. **Window size** is hidden in windowed fullscreen;
its saved value returns when you switch back to Windowed. Rendering
resolution and aspect ratio still follow their own settings.
Existing presets default to Windowed until you change them.

**Start on display** selects the game screen for both window modes. Cycle
through **Primary display** and the connected screens, listed by number and
name. Each preset saves its own choice; existing presets use Primary display.
If a saved screen number is unavailable, the game uses the primary display
without changing the preset. Screen numbers follow SDL's current display
order, so check the selection after rearranging or reconnecting monitors.

The Controls page includes sensitivity, movement/look deadzones, the outer
stick deadzone, look response curve and camera smoothing. Scroll to see its
lower rows. Advanced contains renderer selection, audio buffering and intro
decoding. The explanatory panel describes the focused option and the current
controller/keyboard layouts. The game's own key assignment still determines
the meaning of classic PSP buttons.

Host volume/mute, arbitrary rebinding and high-framerate gameplay
are not implemented by this screen. Settings take effect on the next launch;
the launcher does not open over a running game.

## Navigation and saving

- **Mouse:** select a preset or page; click an option to cycle it or enter a
  value. The minus/plus controls adjust values. Scroll the Controls page or
  the preset list when there are more entries than fit.
- **Keyboard:** Tab/Shift+Tab and Up/Down move focus; Left/Right adjust an
  option; Enter activates or edits; Escape cancels. Text edits support
  Backspace and Ctrl+A.
- **Controller:** D-pad or left stick navigates/adjusts, A activates, B
  cancels, bumpers switch pages, Start chooses Save & Play. A new or copied
  preset has an automatic name that can be accepted without typing; custom
  names and exact numeric entry use the keyboard.

**New**, **Duplicate**, **Rename** and **Delete** edit the preset collection.
**Reset preset** restores original settings for the selected preset while
keeping windowed launch enabled. **Save** persists the whole collection and
last selection; **Save & Play** also launches the game. **Cancel** exits,
asking before discarding unsaved edits. Deletion and reset ask before
changing the editable collection and become persistent only on Save.

The launcher prints the preferences path when it opens. It uses SDL's
per-user preferences directory, usually
`~/.local/share/Last Raven/settings.ini` on Linux. Use an explicit file to
keep a separate setup:

```bash
scripts/15-settings.sh --config /path/to/settings.ini
```

An absent file starts with starter presets; its parent directory must exist
when an explicit path is used. A malformed existing file produces an error
and cannot be overwritten from the screen. Saves write and flush a temporary
file in the same directory before replacing the destination; failed writes
leave the previous file intact.

**Save location.** Save & Play runs the game from
`~/.local/share/last-raven/saves/<slug>/ms/PSP/SAVEDATA/` (under
`$XDG_DATA_HOME` when that is set), the same folder the packaged AppImage
uses, so one folder holds the saves however the game was built; the launcher
prints the path on each launch. Direct runs such as `scripts/06-boot.sh` keep
using their working directory as the memory stick. To play from more than one
computer, sync that folder: see [the save sync guide](SAVE-SYNC.md).

The versioned INI format stores named sections such as `[preset Controller]`,
with `version=1` and `selected=Controller` at the top. Option keys match their
environment suffixes, such as `RESOLUTION=window` and `INPUT=dual`. The
authoritative keys, defaults, ranges and descriptions live together in
`host/settings.c`. Up to 32 presets are supported, with names up to 63 UTF-8
bytes; names cannot include brackets, `=`, `;`, `#`, control characters or
leading/trailing spaces.

## Environment overrides and reproducible runs

Precedence is **built-in defaults → selected preset → environment → explicit
command-line launch policy**. Environment overrides are marked and locked in
the screen. Their variable names appear in the explanation, and the
temporary values are never written into the saved preset. The boot host
prints every effective setting and its source before loading the game.

For migrated player settings, an empty environment value means unset.
Booleans accept `0`/`1`, `false`/`true`, or `off`/`on`; `WINDOW=0` and
`REALTIME=0` now mean off. Invalid names, non-finite numbers, out-of-range
values and malformed dimensions are reported rather than silently clamped
or ignored. `GAMEPAD=auto` follows the control scheme, `CAMERA_LAG=game`
preserves the game's smoothing, and audio `auto` preserves its existing
buffer defaults. The other diagnostic variables retain their current readers.

`PSPRECOMP_WINDOW_MODE=borderless` selects windowed fullscreen;
`PSPRECOMP_WINDOW_MODE=windowed` selects the ordinary resizable window.
Borderless mode also enables a window and real-time pacing for direct boot.
`PSPRECOMP_DISPLAY=2` starts on the second listed display;
`PSPRECOMP_DISPLAY=primary` uses the primary display (the default).

Direct boot, fixed-frame replay, oracle and regression tools never discover
the user's preferences file automatically. Inspect defaults and environment
without opening a window:

```bash
scripts/15-settings.sh --print-settings
```

Inspect or list a saved setup:

```bash
scripts/15-settings.sh --list-presets --config /path/to/settings.ini
scripts/15-settings.sh --print-settings --config /path/to/settings.ini --preset Controller
```

`--create-defaults /path/to/new.ini` writes starter presets without opening
SDL and refuses an existing file. That headless tool does not probe decoder
availability; set `MPEG_DECODE=0` if the runtime has no OpenH264.

The boot executable also accepts `--config FILE`, `--preset NAME`,
`--print-settings` and `--window`. The launcher passes the selected file and
preset explicitly and requests a window; it does not construct a shell
command or change the environment to apply the saved settings.

## Checks

`scripts/16-settings-tests.sh` checks defaults, validation, precedence, preset
round trips, failure handling and isolation from direct boot defaults.
`scripts/16-settings-tests.sh --ui` also drives the real launcher event
handlers with mouse, keyboard and controller events, renders screenshots
under SDL's dummy video driver, and checks Save & Play with a harmless child
process. Screenshot BMPs are written to `build/settings/ui-checks/`.

Gameplay and renderer validation for the initial implementation is recorded
in [the settings plan](SETTINGS-PLAN.md#implementation-validation).
