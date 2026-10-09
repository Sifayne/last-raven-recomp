# Settings

Run `scripts/15-settings.sh` after building the game. The script rebuilds the
host as needed, and opens psprecomp's launcher with this pack linked in.
**Save and play** starts the game. `OPT=1` and `TRACE=1` select the same
builds as the existing boot/replay scripts.

The launcher is drawn with the in-game menu's Dear ImGui toolkit, so it needs
SDL2 and a C++ compiler but no SDL2_ttf. It looks for DejaVu Sans or Noto
Sans; `--font /path/to/font.ttf` selects another, and without any it uses
ImGui's own. The settings parser and headless tool need neither SDL nor game
data. The launcher process handoff uses the project's POSIX host
environment; Windows packaging remains part of M6.

## One set of settings, in two groups

Since stage 10 of psprecomp's player layer there are no presets: one set of
settings, in two groups.

- **All games** -- psprecomp's own options, the same for every game it plays:
  - **Display:** rendering resolution, window mode, window size, start on
    display;
  - **Audio:** volume;
  - **Controller:** active controller;
  - **Save states:** loading a state, when the game starts;
  - **Advanced:** renderer, audio buffering, the intro movie.
- **Armored Core** -- this pack's own, shared by Last Raven, AC3 Portable and
  Silent Line:
  - **Graphics:** aspect ratio, Higher FPS, FPS cap;
  - **Controls:** scheme, controller and keyboard layouts, mouse capture and
    sensitivity, deadzones, response curve, camera smoothing.

A new player starts with a window, the intro movie and **Match window**
rendering (which picks OpenGL), plus this pack's dual-stick controls.
Resolution and aspect are separate settings:
- **Match window** resolution makes the image sharper;
- **Match window** aspect expands the 3D view to fit wider windows.

Enhanced modes require OpenGL.

**Window mode → Windowed fullscreen** fills the display with a borderless
game window at the desktop resolution. Window size then reads *Desktop
size*; its saved value returns when you switch back to Windowed.

**Start on display** lists Primary display and the connected screens by
number and name. If a saved screen is unavailable, the game uses the primary
display without changing the setting. Screen numbers follow SDL's current
display order, so check the selection after rearranging or reconnecting
monitors.

**FPS cap** offers common rates, and **Other...** takes any whole number from
30 to 1000. Window size works the same way, taking any `WIDTHxHEIGHT`.

**Reset to defaults** puts the group of the page you are on back to its
defaults, after asking. The bindings, made in the in-game menu, stay as they
are.

Settings take effect when the game starts. The in-game menu (Escape, or View +
Menu on a controller) changes them while playing, and writes them back to the
same file.

## Navigation and saving

- **Mouse:** choose a game tab, a page, or an option's list.
- **Keyboard:** arrows move; Enter opens or chooses; Escape goes back, or
  closes an open list. **Other...** values are typed.
- **Controller:** the D-pad or left stick moves, A chooses, and B goes back.
  The bumpers step through the pages, and Start chooses Save and play.

**Save** writes the file; **Save and play** also starts the game. **Cancel**
exits, asking before discarding unsaved changes.

The file is psprecomp's: `~/.config/psprecomp/settings.ini`, under
`$XDG_CONFIG_HOME` when that is set. The packaged app reads the same file.
The launcher prints the path when it opens. Use an explicit file to keep a
separate setup:

```bash
scripts/15-settings.sh --config /path/to/settings.ini
```

**Earlier settings.** Settings this game's earlier launcher saved, in
`~/.local/share/Last Raven/settings.ini` or the packaged app's
`~/.config/last-raven/settings.ini`, come in the first time the launcher
shows this pack, as psprecomp's file has no section for it yet. The selected
preset becomes the settings. The other presets stay in the file, unused.

A malformed file produces an error and cannot be overwritten from the
screen. Saves write and flush a temporary file in the same directory before
replacing the destination; failed writes leave the previous file intact.

**Save location.** Save and play runs the game from
`~/.local/share/psprecomp/saves/<slug>/ms/PSP/SAVEDATA/` (under
`$XDG_DATA_HOME` when that is set). That is the same folder the packaged app
uses, so one folder holds the saves however the game was built, and the
launcher prints the path on each launch. Save states go to
`~/.local/share/psprecomp/states/<slug>/`. Direct runs such as
`scripts/06-boot.sh` keep using their working directory as the memory stick.
To play from more than one computer, sync the saves folder: see [the save
sync guide](SAVE-SYNC.md).

The file (`version=2`) has one section per group:
- `[player]` holds psprecomp's options;
- `[pack last-raven]` holds this pack's options, and the bindings as
  `bind.*` keys;
- other packs' sections, and earlier `[preset ...]` sections, are kept as
  they are.

Keys match their environment suffixes, such as `RESOLUTION=window` and
`INPUT=dual`. The authoritative keys, defaults, ranges and descriptions live
in two places:
- psprecomp's `src/host/settings.c`, for the player's;
- `host/settings.c`, for this pack's.

## Environment overrides and reproducible runs

Precedence is **built-in defaults → the file → environment → explicit
command-line launch policy**. Environment overrides are marked and locked in
the screen, and their variable names appear in the explanation. The
temporary values are never written into the file. The boot host prints every
effective setting and its source before loading the game.

An empty environment value means unset. Booleans accept `0`/`1`,
`false`/`true`, or `off`/`on`. Invalid names, non-finite numbers,
out-of-range values and malformed dimensions are reported rather than
silently clamped or ignored. Some values have special meanings:
- `GAMEPAD=auto` follows the control scheme;
- `CAMERA_LAG=game` keeps the game's own smoothing;
- audio `auto` keeps the existing buffer defaults.

`PSPRECOMP_WINDOW_MODE=borderless` selects windowed fullscreen, and also
enables a window and real-time pacing for direct boot.
`PSPRECOMP_WINDOW_MODE=windowed` selects the ordinary resizable window.
`PSPRECOMP_DISPLAY=2` starts on the second listed display, and
`PSPRECOMP_DISPLAY=primary` on the primary display (the default).

Direct boot, fixed-frame replay, oracle and regression tools never discover
the user's preferences file automatically. To inspect defaults and the
environment, or a saved file, without opening a window:

```bash
scripts/15-settings.sh --print-settings
scripts/15-settings.sh --print-settings --config /path/to/settings.ini
```

`--create-defaults /path/to/new.ini` writes a new player's settings without
opening SDL, and refuses an existing file. That headless tool does not probe
decoder availability; set `MPEG_DECODE=0` if the runtime has no OpenH264.

The boot executable accepts `--config FILE`, `--print-settings`, `--window`
and `--load-state FILE`. The launcher passes the file explicitly and
requests a window; it does not construct a shell command or change the
environment to apply the saved settings.

## Checks

`scripts/16-settings-tests.sh` checks:
- defaults, validation and precedence;
- the file's sections, and earlier presets;
- failure handling, and isolation from direct boot defaults.

`scripts/16-settings-tests.sh --ui` also builds psprecomp's launcher with
this pack and draws every page under SDL's dummy video driver, to BMPs in
`build/settings/ui-checks/`. It saves a change to each group, then checks
Save and play with a harmless child process.

Gameplay and renderer validation for the initial implementation is recorded
in [the settings plan](SETTINGS-PLAN.md#implementation-validation).
