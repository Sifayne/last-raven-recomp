# PC settings screen

Investigated and implemented 6 Sep 2026 for
[M7](ROADMAP.md#m7--the-pc-ports-own-settings). **The shared configuration
layer, saved presets and pre-launch screen are implemented.** Open them with
`scripts/15-settings.sh`; see [the settings guide](SETTINGS.md) for usage.
In-game access and live changes remain follow-up work. The design and
acceptance criteria below record the scope agreed before implementation.

Chosen first delivery: **settings before launch, with saved presets**.

The immediate problem is remembering launch variables. A saved configuration
and a screen for the existing graphics and controls options can solve that
without waiting for higher-framerate research or more renderer features.

## Inventory before implementation

A source inventory before migration of `host/*.c` and
`tools/psprecomp/src/**/*.c`, excluding
host test programs, finds **74 distinct `PSPRECOMP_*` option names in 14
files**. Of these, 68 appear in literal `getenv` calls; four stick-tuning
options and two audio-buffer options use helpers that call `getenv(name)`.
This count excludes build-script variables such as `OPT` and `TRACE`.

At that point there was no shared configuration object, saved preferences
file or host settings UI. The existing options were enough to populate a useful screen.
Most of the remaining variables control logs, captures, replay or fault
investigation and do not need ordinary menu rows.

The following table records the launch options before migration. All names
take the `PSPRECOMP_` prefix. Defaults describe direct boot without a preset
or environment overrides; the settings guide documents the new parser rules.

| Screen / setting | Existing variable | Values and current default |
| --- | --- | --- |
| Graphics: rendering resolution | `RESOLUTION` | `psp` (default), `window` for the physical drawable resolution; requires GL |
| Graphics: aspect ratio | `ASPECT` | `native` (default), `window` for an adaptive wider view; requires GL |
| Graphics: window size | `WINDOW_SIZE` | `WIDTHxHEIGHT`; default 960x544 logical pixels |
| Advanced graphics: renderer | `RENDER` | Default software; window resolution or window aspect selects `gl` unless explicitly overridden; `null` is a diagnostic backend |
| Controls: control scheme | `INPUT` | `classic` (default), `modern` (proportional one-stick turning), `dual` (movement plus right-stick/mouse look) |
| Controls: controller layout | `GAMEPAD` | `classic`, `modern`; unset follows the control scheme |
| Controls: keyboard layout | `KEYS` | `classic` (default), `wasd` |
| Controls: capture mouse | `MOUSE` | Off by default; `1` enables capture, Escape releases it |
| Controls: mouse sensitivity | `MOUSE_SENS` | Positive multiplier; default 1.0, equivalent to 0.001 radians per count |
| Controls: movement deadzone | `MOVE_DEADZONE` | 0–0.50; default 0.10; movement entry threshold is derived as deadzone +0.03 |
| Controls: look deadzone | `LOOK_DEADZONE` | 0–0.50; default 0.08 |
| Controls: outer stick deadzone | `STICK_OUTER_DEADZONE` | 0–0.20; default 0.02 |
| Controls: look response curve | `LOOK_EXPO` | 0–1; default 0.60 |
| Controls: camera smoothing | `CAMERA_LAG` | Unset preserves the game's behavior; explicit 0–0.99, where 0 follows immediately; ignored in classic mode |
| Advanced audio: buffer lead | `AUDIO_LEAD_MS` | Default derives from two channel buffers; explicit value is milliseconds |
| Advanced audio: preroll | `AUDIO_PREROLL_MS` | Default 4096 frames at 44.1 kHz, about 93 ms |
| Launch: intro video decoding | `MPEG_DECODE` | Off by default; `1` enables decoding when the build supports it; existing replay scripts offer `--decode` |

`WINDOW` and `REALTIME` are two further launch controls. GL implies a window,
and a window implies real-time pacing. These should be resolved by the launch
mode rather than asking a player to configure three coupled switches.

Fullscreen, host volume/mute, arbitrary button rebinding, fixed resolution
scales and a working high-framerate mode are **new features**, not existing
environment options waiting to be exposed. Keep them as separate follow-ups.
The game's own sound settings remain available in the meantime.

## Configuration comes first

Implemented in `host/settings.h` and `host/settings.c`, with no SDL dependency.
The registry covers the 17 options above and the two launch controls.
Runtime diagnostic options can migrate incrementally without blocking the screen.

Each registered option needs a stable key, type, default, allowed values or
range, environment alias, help text and an apply policy. Resolve derived
choices once: automatic renderer selection, controller layout following the
control scheme, and dependencies between mouse look and dual controls. All
consumers must read the same resolved values.

Use one documented precedence order:

1. Built-in defaults.
2. The selected saved preset, when explicitly enabled by the interactive launcher.
3. Existing environment overrides for this run.
4. Explicit command-line overrides, if introduced with this work.

The screen should show an environment-overridden setting as locked, with the
variable name available in its explanation. It must not silently save that
temporary override into the user's preferences. Report effective values and
their sources at launch, and provide an inspection command that works
without loading game data or starting SDL.

Use a small versioned INI file with named preset sections under the platform's
per-user preferences directory. The launcher resolves it with SDL_GetPrefPath;
pass a path into the parser so
the parser and tests remain independent of SDL. Save by writing a temporary
file in the same directory and replacing the destination only after success.
Show write failures and retain the previous file. Validate booleans, enums,
finite numbers and dimensions consistently, with actionable errors.

Treat a controller layout that follows the scheme and camera smoothing that
uses the game's values as explicit `auto` / `game` choices. Replacing either
with today's resolved numeric value would lose its intended behavior.

The direct boot/replay tools should continue to use defaults plus explicit
overrides unless given a config path. This prevents personal preferences
from changing regression results. `scripts/11-render-check.py` currently
clears `PSPRECOMP_*` variables, which would be insufficient if every binary
started automatically loading a preferences file.

When a player option lives in the runtime, pass its resolved value through
a small runtime API; intro movie decoding is the initial example. The
runtime should not read host paths or know about UI widgets. Do not implement
menu Apply by changing the process environment.

## First screen: choose a preset and launch

Build a small host launcher that opens before game execution and launches
the boot executable with an explicit configuration path and preset name.
Its window can close before the game starts, avoiding a live GL-context
handoff or guest pause feature. Keep the existing build and replay scripts
available; add one documented launcher command so normal play no longer
requires a remembered environment-variable recipe.

The screen should have a preset selector, Graphics and Controls pages,
collapsed Advanced options, and **Save & Play**, **Save**, and **Cancel**.
Allow creating, duplicating, renaming and deleting user presets; remember
the last selection. Cancel discards edits. Defaults reset the editable
preset, with persistence only on Save.

Provide starter presets for **Classic**, **Controller**, and **Mouse &
Keyboard**. Controller selects dual controls and the modern pad layout;
Mouse & Keyboard adds WASD and mouse capture. Make their full graphics and
controls choices visible before saving. These are convenient starting
points, not a change to the defaults used by direct boot and tests.

Keep rendering resolution and aspect ratio independent: a sharp PSP-shaped
view and a sharp wider view are both useful. Label resolution choices
**Original (480x272)** and **Match window**; label aspect choices
**Original** and **Match window**. Use percentages for deadzones and a
plain-language explanation of the response curve and camera smoothing.
Show the controller layout and mouse/keyboard bindings alongside their
selectors, since arbitrary rebinding is not part of this first version.

Saved presets contain player preferences, never capture paths, replay
scripts or fault/debug triggers. Report missing presets and launch failures
in the launcher. Capability-dependent options need an explanation when the
build lacks SDL/GL or movie decoding. A headless build keeps the config
reader and inspection command even when it cannot build the launcher.

## Why live changes need additional work

`host/replacements.c` caches control mode, controller layout, mouse
sensitivity, stick tuning and camera lag. `host/present.c` independently
chooses layouts and mouse capture during SDL startup.
`host/render_gl.c:gl_init` snapshots resolution and aspect settings.
Changing a saved value alone cannot update these consumers.

Window resizing already has a useful live mechanism:
`present_request_window_size` queues work for the SDL thread, and the GL
backend handles the drawable-size change. Switching resolution/aspect modes
on an existing renderer is a separate operation that still needs validation.

For an in-game screen, route UI input before gameplay input. Opening the
screen must release mouse capture and clear held keys, buttons, sticks and
accumulated mouse movement. Closing it must not turn its confirming click
into a shot or leave a held action behind.

The SDL thread owns the event queue, but the GE thread owns the GL context
and performs swaps. A UI toolkit must respect that split. Establish where
UI state is updated and where immutable draw data is consumed; GL UI draws
belong after the final game blit, with renderer state restored afterward.
Keep the UI out of guest framebuffer memory and fixed GE captures.

A paused game also needs a host redraw path: the current GL window swaps
when the guest presents. Pausing guest execution without that path would
freeze a menu rendered only at guest present. Pause behavior must account
for the guest clock and queued audio before an in-game menu is called done.

## Delivery and acceptance

1. **Saved settings and inspection — implemented.** Implement the registry, parser,
   validation, source reporting and migrated player-option consumers. Keep
   current defaults and the existing environment launch recipes working.
   Add focused checks for precedence, invalid/non-finite values, dependent
   settings, save/load round trips and separation from replay configuration.
2. **Pre-launch settings and presets — implemented.** Implement the screen described above
   using the same registry and saved preferences. Include help, reset
   defaults and keyboard/controller navigation. Prototype the UI toolkit in
   the launcher's own window before wiring it to game startup; it need not
   solve the game's event/render thread split.
3. **Live settings — later.** Publish validated changes through the owning threads.
   Start with input tuning and the existing window-size request. Keep
   renderer selection and unvalidated target-mode changes marked as requiring
   restart. Record or disable gameplay-affecting changes during replay
   recording so a pad file remains reproducible.

Verify preset creation, duplication, rename, deletion and the remembered
selection. Verify a saved setup by reopening the screen and starting real gameplay with
both a controller and mouse/keyboard. Check cancel, reset, environment
overrides, invalid configuration and unavailable optional capabilities.
For in-game access, also check opening/closing while holding movement/fire,
focus loss, pause/resume timing, audio and input capture.

Configuration migration must preserve classic-mode replay results with no
config loaded, and environment versus saved-config launches should resolve
identically when they specify the same settings. Run the relevant control
replays and rendering checks after consumer changes; use the roadmap's
runtime regression bar if runtime code changes.

## Implementation validation

The launcher uses SDL2 and SDL2_ttf in its own process, with a shared C
settings registry. It passes the selected file and preset directly to the
boot executable; failed game startup returns to the launcher with an error.
The runtime receives intro-decoding selection through a startup API.
Diagnostics remain environment-only. The boot/inspection tools do not
discover personal preferences implicitly.

Validation on 6 Sep:

| Check | Result |
| --- | --- |
| Settings parser and persistence | Defaults, strict validation, source precedence, derived choices, preset round trips, failure preservation and replay isolation pass; address/undefined-behavior sanitizers pass with leak detection disabled under the sandbox tracer |
| Real launcher UI fixture | Mouse, keyboard and synthetic controller events; preset create/copy/rename/delete/reset; canceled edits; invalid numeric input; override locking; unavailable decoder recovery; file reopen; normal startup/exit and child argument handoff pass |
| UI render inspection | Graphics, Controls, Advanced, environment override and invalid-input dialog rendered and visually checked |
| Runtime CTest | 24/24 pass |
| Full runtime conformance sweep | All 432 rows identical to a separately built pre-change runtime from the same revision; the old 3 Sep baseline was not used to attribute intervening changes to this work |
| Differential oracle | 3,074 comparisons, 3,072 matching, two known differences; no discovery-gap differences |
| Renderer fixtures | Software and GL each pass 430/430 assertions |
| Physical resolution fixtures | Native and window aspect each pass 40/40 checks |
| Camera aspect fixture | 35/35 pass |
| Fixed gameplay scenes | Six cases, each repeated per backend, show no regression against the existing exact-capture baseline |
| No-config replay preservation | Title idle: 1,803 lists / 874,060 commands; look sweep: 2,643 / 31,587,677; pitch sweep: 2,603 / 27,502,089. All identical before/after, with zero bad accesses |
| Saved preset versus environment | Complete look-sweep input logs are byte-identical with non-default sensitivity, deadzones, curve and camera smoothing |
| Windowed saved preset | 1280x720, window resolution/aspect and dual controls: 178/178 replay events, 2,643 lists / 31,646,172 commands, zero bad accesses, clean exit; physical gameplay screenshot inspected |
| Headless build | Compiles and links without SDL; settings inspection works and unavailable window mode is rejected |
| Legacy software boot scripts | Default boot, skip-intro and decoded New Game exit with zero bad accesses. The latter two reach their 200/300-second limits at 183/5,377 polls; they do not establish the scenario-end gates. Full scenario completion is established by the null-renderer control replays and the windowed GL replay above |

Artifacts are local and ignored: `reports/settings-validation/`,
`reports/settings-scene-checks/results.json` and `reports/settings-ui/`.
The windowed gameplay check used recorded stick/mouse input; menu controller
navigation was tested with injected SDL controller events, not a physical
controller operated by a person.
