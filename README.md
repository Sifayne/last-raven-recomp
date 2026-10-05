# last-raven-recomp

A static recompilation of **Armored Core: Last Raven Portable** (PSP) — turning
the game's Allegrex MIPS code into C ahead of time, linked against a native
runtime, to produce a real PC executable rather than an emulated one. Same model
as N64Recomp / *Zelda 64: Recompiled*.

**Status: the game boots, plays its intro movie, renders the title menu, and
runs New Game through to its first settings screen with no bad memory
accesses.** Translation is done — the differential oracle agrees on every
function it can compare, and the one codegen bug it was blind to has been
found and fixed — and 171 of the 218 firmware imports are implemented. What comes next, in order, is
[docs/ROADMAP.md](docs/ROADMAP.md); where things stand right now, and how to
measure them, is [docs/findings/state.md](docs/findings/state.md); Phase 0's
numbers are in [docs/findings/phase0.md](docs/findings/phase0.md).

Phase 1 added the differential oracle: an Allegrex interpreter sharing the
toolkit's decoder, runtime and HLE, so a disagreement with the recompiled C
localises to the emitter. It found **four silent-truncation bugs** in the
emitter, each one making the generated C do less than the hardware does — a
dropped fall-through, a missing label, a dropped `jr $ra` delay slot (where the
stack restore lives), and an indirect call treated as the end of a function.

Across all 26,462 discovered functions: **12,240 compared, 12,237 agree.** Of
the 3 that do not, 2 are an IEEE-754 NaN sign bit and 1 is documented as
unresolved. See [docs/findings/oracle.md](docs/findings/oracle.md).

## This repository contains no game data

No ROM, no ISO, no assets, no firmware, no decryption keys — and it never will.
You supply your own dump of media you own. `game/` is gitignored in its entirety,
as is all emitted C, since that is a derivative of the binary and is meant to be
regenerated rather than committed.

## Requirements

For the standalone **Linux AppImage** and Steam Deck test
instructions, see [the packaging guide](docs/PACKAGING.md). It builds without
a game dump. Use Add Game to prepare your supported PSP ISO locally; the
package includes its compiler and media dependencies.
The requirements below apply to the development/game pipeline.

- Linux build: CMake ≥ 3.16, Ninja, a C/C++ compiler, `make`, Python ≥ 3.9,
  `pkg-config`, `curl`, `tar`/`xz`, `readelf` (binutils), and `patchelf`
- `zlib` and `openssl` (for `pspdecrypt`)
- Your own dump of the game as `.iso` in `game/`

## Quick start

```bash
git clone --recurse-submodules https://github.com/Sifayne/last-raven-recomp.git
cd last-raven-recomp
```

```bash
scripts/build-tools.sh
```

This also downloads and builds a checksum-pinned LGPL FFmpeg with the two
required ATRAC decoders. It installs privately under `build/deps/ffmpeg/`;
no system FFmpeg installation is needed. Later builds reuse the verified
dependency. See [the FFmpeg build and distribution guide](docs/FFMPEG.md).

Then drop your dump in `game/` and run the pipeline in order:

```bash
scripts/00-identify.sh && scripts/01-extract-decrypt.sh && scripts/02-analyze.sh
```

```bash
python3 scripts/03-imports.py | tee reports/03-imports.txt && scripts/04-emit-build.sh
```

Then the oracle, which needs stage 04's objects:

```bash
scripts/05-oracle.sh 400
```

Then the boot host, which loads the module, runs its constructors and threads
against a scheduler, a software GE and the HLE:

```bash
scripts/06-boot.sh
```

For normal play, open the **settings launcher**:

```bash
scripts/15-settings.sh
```

Choose **Classic**, **Controller**, or **Mouse & Keyboard**, adjust graphics
and controls, then **Save & Play**. You can create your own named presets.
For smoother missions in ACLR, AC3P and ACSL, enable **Graphics → Higher FPS**
and choose **FPS cap** (including custom limits or unlimited). Simulation stays
at its original speed; see [the FPS guide](docs/FPS.md) for scope and validation.
The launcher needs SDL2 and SDL2_ttf; existing command-line and headless
tools continue to work without the launcher. Settings apply on the next
launch. See [the settings guide](docs/SETTINGS.md) for navigation, file
locations, environment overrides and headless inspection. Saves go to
`~/.local/share/last-raven/saves/`, the same folder the AppImage uses; to play
from more than one computer, see [the save sync guide](docs/SAVE-SYNC.md).

For the current dual-stick controls directly in a window:

```bash
PSPRECOMP_WINDOW=1 PSPRECOMP_INPUT=dual scripts/06-boot.sh
```

To let the 3D view follow the window's aspect ratio, with the HUD drawn 1:1 in
a centred 480x272 area so its text stays pixel-exact, enable the adaptive GL
path and resize the window (or open it at a size with
`PSPRECOMP_WINDOW_SIZE=2560x1080`):

```bash
PSPRECOMP_ASPECT=window PSPRECOMP_INPUT=dual scripts/06-boot.sh
```

`PSPRECOMP_ASPECT=window` selects the GL renderer when no renderer is named.
The game still renders into its native PSP framebuffer; the GL target behind it
grows sideways to the window's shape. To also rasterize at the window's physical
pixel resolution, including high-DPI scaling:

```bash
PSPRECOMP_RESOLUTION=window PSPRECOMP_ASPECT=window PSPRECOMP_INPUT=dual scripts/06-boot.sh
```

Resolution and aspect are independent: leave aspect unset for a sharp,
letterboxed PSP-shaped image. `PSPRECOMP_RESOLUTION=window` selects GL when no
renderer is named; `psp` (or unset) keeps the reference resolution. Resizing
preserves the GPU framebuffer history. Guest memory, original texture/movie
assets and game speed retain their PSP layout and behavior. The small bitmap
body font uses nearest sampling when enlarged, preserving its stroke weight.
See
[resolution validation and limits](docs/RENDER-CHECKS.md#window-resolution-rendering).

The modern pad layout is LT boost, RT right arm, LB left arm/event, RB change
weapon, L3 extension, R3 OB/EO, A inside, B view reset, and Y as the purge
modifier; X is deliberately unassigned. Start, Back and the d-pad retain their
usual roles, and A/B/X/Y remain conventional confirm/cancel/face buttons in
menus. Set `PSPRECOMP_GAMEPAD=classic` to use the direct PSP button mapping
with the enhanced sticks. The response and deadzone tuning variables are
documented in [the current findings](docs/findings/state.md#the-instruments-and-what-each-can-and-cannot-tell-you).

For mouse and keyboard, use `PSPRECOMP_KEYS=wasd PSPRECOMP_MOUSE=1`
with `PSPRECOMP_INPUT=dual`. With the default in-game key assignment,
left click fires the right weapon, right click fires the left weapon,
and Q switches the right weapon. Middle click also fires the left weapon.
WASD moves, Space boosts, and Escape releases the mouse.

Then a scripted run — the intro skipped, the title menu reached, and stop:

```bash
scripts/09-replay.sh --decode scenarios/skip-intro.pad
```

Stage 04 emits ~2.1M lines of C, compiles it, and links it against the runtime.
It takes a couple of minutes. The boot host runs the game today: with the
decoder on it plays the intro and renders the title menu headless, and a
scenario drives it from there — see
[docs/findings/state.md](docs/findings/state.md) for exactly where it stops
and why.

### Another title

Every stage is keyed on `GAME`, a profile in `scripts/games/<slug>.sh` (module
name, emit prefix, expected disc id, window title). Unset, it is Last Raven at
the paths above. Any other slug -- `ac3p` and `acsl` are checked in, for
Armored Core 3 Portable and Silent Line Portable -- keeps its own
`games/<slug>/` (drop the dump there), `build/<slug>/host` and
`reports/<slug>/`, so two titles never overwrite each other's objects:

```bash
GAME=ac3p scripts/00-identify.sh && GAME=ac3p scripts/01-extract-decrypt.sh
```

Native replacements are per title too: `host/replace-<slug>.txt` and
`host/replacements-<slug>.c`, both empty until a survey says otherwise.
The launcher (`scripts/15-settings.sh`) shows a tab for every title whose
module has been emitted, relinks each one's boot host first, and remembers
the tab you pick; `GAME=<slug>` in the environment opens on that title.

## Layout

| Path | What |
|---|---|
| `scripts/` | the pipeline, one stage per file — 00–06 bring-up, 07–08 the behavioural oracle (pspautotests), `09-replay.sh` scripted play |
| `host/` | the native host: `boot` (module load, scheduler, GE rasterizer, HLE) and the link probe |
| `scenarios/` | pad-input scripts the host can replay — the title menu, the New Game fault, and the control run |
| `docs/` | [the roadmap](docs/ROADMAP.md), decisions and findings |
| `tools/psprecomp` | submodule — the recompiler, runtime and interpreter (MIT) |
| `tools/pspdecrypt` | submodule — decryption, for the mode-9 path psprecomp lacks |
| `game/`, `games/`, `reports/` | gitignored working directories (`games/<slug>/` for a title other than Last Raven) |

## Why PSP and not PS2

Short version: the PS2 needs two instruction sets recompiled (VU1 runs its own
microcode program) and offers no clean game↔OS boundary, while PSP games call
named firmware functions through an enumerable import table. Long version with
the full comparison: [docs/DECISION-target.md](docs/DECISION-target.md).

## Licensing of vendored tools

`psprecomp` is MIT and is **linked into** anything this project builds.

`pspdecrypt` is **GPL-3.0** and is deliberately only ever invoked as a separate
process by `scripts/01-extract-decrypt.sh`. That keeps it at arm's length. Do not
link its code into the host or copy routines out of it without deciding what that
means for this project's own license first.

**OpenH264** (BSD) is an optional system library for the intro movie's video.
**FFmpeg's libavcodec and libavutil** decode ATRAC3/ATRAC3+ music and movie
audio. This project's build scripts use a private, pinned **LGPL-2.1-or-later**
FFmpeg build, with GPL/nonfree components disabled, and link it dynamically.
They reject a runtime cache pointing at system FFmpeg, so a checkout built
before the bundle existed stops at every script until it is migrated: run
`scripts/build-tools.sh` once. That step downloads the pinned FFmpeg source
and builds it, which takes a few minutes; to do it offline, place the pinned
tarball named in `tools/psprecomp/third_party/ffmpeg/source.json` under `build/deps/downloads/`
first, and its checksum is still verified.

The [FFmpeg bundle command](docs/FFMPEG.md) stages the shared libraries,
license notices, exact source archive, and reproduction instructions together.
Keep those materials with/alongside releases. The launcher's About button
includes the FFmpeg attribution. The upstream runtime can still be built
without decoders independently of this project's player build.

## Upstream

`tools/psprecomp` is our fork, tracked as an ordinary submodule. What we have
added is `git log upstream/main..` there — read that for the current list
rather than a count here, which rots. Broadly:

| Area | What |
|---|---|
| build | `libm` was never linked — invisible on MSVC, a hard failure on Linux |
| emitter | codegen bugs, PRX relocations, and function discovery: shared epilogues, computed jumps, switch-case ownership |
| oracle | an Allegrex interpreter to diff against, filling an unchecked box in upstream's Phase 5 roadmap |
| runtime | thread scheduler and guest clock, disc filesystem, GE display-list lifetime, sceMpeg |
| diagnostics | firmware call logging, semaphore narration, function tracing and reachability |

The first three are upstream bugs whose fix belongs in
[sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp); they are kept
here so a fresh clone reproduces the same build in the meantime. `0004` onward
is this project's bring-up — HLE additions, discovery fixes, and the
diagnostics the findings documents lean on. They are driven from this repo's
needs rather than upstream's, and each patch's own diff carries the reasoning.
