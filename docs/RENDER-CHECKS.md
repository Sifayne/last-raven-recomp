# Fixed-frame renderer checks

`scripts/11-render-check.py` compares software and GL on the same captured GE
commands, registers and guest memory. The scene/input manifest is
`scenarios/render-checks.json`; `mission-effects.pad` extends the existing
mission recording with firing and boost input. Captures, logs, guest saves and
images stay in ignored `reports/` directories. No game data belongs in git.

## Run it

Capture six scenes in one approximately 71-second GL run. The host needs SDL2,
a desktop display and the user's own game files, as for `09-replay.sh`.

```bash
python3 scripts/11-render-check.py capture --output reports/render-captures
python3 scripts/11-render-check.py compare reports/render-captures/captures.json \
    --output reports/render-baseline
```

Both commands rebuild using `06-boot.sh`. Each output directory must be new;
an existing specimen or baseline is never overwritten. Capture runs use an
isolated guest working directory so the player's `ms/` saves are untouched.
The capture index records SHA-256 hashes, requested/actual polls, command
counts and the input scenario. Command-count and brightness filters have an
eight-poll maximum selection drift; a later scene cannot silently satisfy an
earlier case. In the measured suite all six started at exactly the requested
poll and ended at the next poll.

After a renderer change, reuse the captures:

```bash
python3 scripts/11-render-check.py compare reports/render-captures/captures.json \
    --output reports/render-after --baseline reports/render-baseline/results.json
```

Each backend is replayed **twice**, and those repeats must be byte-identical.
The runner checks that the complete command stream executed, rejects broken
captures and dark/wrong-target output, and writes:

- `results.json`: full-frame and selected-region errors, plus unsupported-state counts.
- `*-pair.png`: software on the left, GL on the right, enlarged 2x without smoothing.
- `*-diff-x16.png`: absolute RGB differences amplified 16x.
- Raw PPM frames and separate backend logs for diagnosis.

ImageMagick is optional; metrics and PPM output need only Python's standard
library. `--no-images` skips review images explicitly.

A baseline check fails when normalized RGB RMSE, pixels with any channel
error greater than two, maximum channel error, or unsupported-state counts
increase. It refuses different capture hashes, changed software-oracle output,
or changed comparison regions. A run without `--baseline` is labelled
`measured`, not a pixel-correctness pass. Baselines are local specimens tied to
their capture and software output; assess new drivers separately.

## First expanded baseline — 4 September 2026

Local artifacts: `reports/m5-scene-captures/captures.json` and
`reports/m5-scene-baseline/results.json`. Every image is 480x272 (130,560 pixels).

| Scene | Poll | Commands | Exact RGB pixels | Pixels >2 | Normalized RMSE |
|---|---:|---:|---:|---:|---:|
| Garage menu / AC | 740 | 12,241 | 106,800 | 204 | 0.003105 |
| Garage launch | 915 | 7,757 | 125,161 | 373 | 0.002612 |
| Mission ground / opening | 1770 | 13,184 | 126,442 | 532 | 0.002643 |
| Mission smoke | 1840 | 13,335 | 126,120 | 528 | 0.002474 |
| Combat firing input | 2050 | 21,922 | 121,188 | 427 | 0.002918 |
| Combat boost input | 2090 | 20,325 | 120,270 | 341 | 0.002700 |

The lower-left 240x136 region has 264 pixels >2 in the mission-opening frame
and 37 in combat (out of 32,640). The side-by-side images show no broad
GL-only ground stretch in these specimens. Residual differences include
isolated high-contrast edges: maximum channel errors reach 120, so low overall
RMSE does not mean every pixel is close.

The capture run delivered all 87 input events through poll 2210 with zero bad
memory accesses. GPU draw-plus-blit measurements averaged 1.01 ms and peaked
at 3.45 ms on this host. Capture instrumentation makes this a coverage run,
not a replacement for the dedicated pacing benchmark.

## What the first wider baseline exposed

1. **Stencil is exercised in combat.** Both combat captures contain 801 draws
   with stencil enabled, which GL counted as unsupported. The earlier four
   captures contain none. These counts establish coverage, not the visible
   severity of each state: a stencil operation may only change alpha or may
   be a no-op. The next stencil tests must inspect alpha and subsequent
   stencil/depth outcomes as well as RGB. The RGBA8888 path is now covered
   by the increment below; reduced-bit-depth targets remain unsupported.
2. **Lines are a shared hole.** The combat captures request 8 and 9 line draws.
   Both software and GL omitted them, so a small RGB difference could not
   validate them. The increment below restores these primitives.
3. **Target reuse needs a history test.** The full run logs scratch target
   `0x04154000` changing from 256x128/5551 to 128x64/8888. GL retains its
   original allocation and format. A fresh single-frame replay does not
   reproduce the earlier allocation, so this requires a multi-frame or
   synthetic target-reconfiguration regression. Its visible impact is not
   established by the fixed-frame RGB scores above.

No unsupported blend factors/equations were reported in these six frames.
Dithering and wider scene/driver coverage remain separate work.

## Targeting outline and RGBA8888 stencil — 4 September 2026

The user's recomp-versus-PPSSPP screenshots exposed the shared line gap:
the combat-boost capture submits eight untextured, through-mode segments
forming the targeting outline, plus a radar line. Both backends now draw
points, independent lines and line strips. A shared, scissor-bounded sample
walker supplies interpolated colour/depth/fog and perspective-divided UVs;
GL emits one-pixel quads to avoid driver-dependent native line coverage.
The GE retains the connecting vertex across line-strip decode batches and
clips transformed lines/points before projection, using its existing triangle
clip/clamp policy.

Integer endpoint coverage was checked against the public
[pspautotests primitive fixtures](https://github.com/hrydgard/pspautotests/tree/master/tests/gpu/primitives)
(including right-to-left lines and line-strip joints). This is not exhaustive
hardware validation of fractional endpoints, interpolation or clipping.

GL now uses a depth/stencil attachment for **RGBA8888** targets. GPU bit-plane
passes import the framebuffer alpha into stencil after alpha clears and
export it before readback, render-target sampling, or destination-alpha
blending. Imports/exports are lazy; overlapping primitives that both change
stencil and consume destination alpha are explicitly synchronized. The frame
GPU timer includes these passes. Initial RGBA8888 target contents come from
guest memory, so depth-only clears preserve prior colour/alpha.

`scripts/12-render-tests.sh` runs the same **430 exact RGBA checks** on software
and GL without game data. Coverage includes all 256 stencil byte values,
eight masked comparisons, all six operations on stencil-fail/depth-fail/pass,
increment/decrement saturation, alpha-test discard ordering, preservation of
depth after rejection, scissored alpha-only clears, destination-alpha blending,
render-target texture alpha, point/line endpoints, and scissored perspective
line texturing. These are backend-contract tests, not new PSP hardware captures.
They also exposed and fixed software's ignored alpha-only clear. Runtime tests
add GE-level stencil-only clears, line-strip batching and transformed clipping.

Local results: `reports/m5-render-tests-final.log` and
`reports/m5-stencil-lines/results.json`. All six fixed scenes repeat identically
per backend. The four pre-combat RGB comparisons are unchanged. Combat firing
retains 427 pixels >2 and RMSE 0.002918; boost improves from 341 to 339 pixels
>2 and RMSE 0.002698. Both now report **zero skipped line draws and zero
unsupported stencil draws**, with 801 implemented stencil draws and one alpha
import/export per combat capture. The visible targeting outline is restored.

Because software now draws previously missing HUD geometry, its combat image
hashes intentionally change. Keep the earlier baseline as evidence; establish
a new baseline from these reviewed outputs rather than bypassing the old
baseline's software-identity guard.

The final gate rerun is `reports/m5-stencil-lines-final/results.json`, against
that reviewed baseline. A fresh full replay in `reports/m5-stencil-live/`
delivered 87/87 input events through poll 2210 with zero bad accesses:
70.477 seconds guest/wall, frame interval p50 33 ms / p95 34 ms, GPU mean
0.98 ms / p95 2.6 ms / maximum 3.38 ms. This is comparable to the earlier
1.01 ms mean, not evidence of a speedup. The full history exercises **30
remaining unsupported stencil draws** on reduced-bit-depth/format-reused
targets; the six fixed snapshots do not contain those cases. It executes
290,763 RGBA8888 stencil draws and no longer skips the 1,877 point/line draws
seen in the earlier full run. Capture instrumentation and load transitions
still produce long outlier frame intervals (maximum 471 ms).

## Adaptive mission camera

`PSPRECOMP_ASPECT=window` must update two camera paths. The garage's
`00154C80` copies the adjusted aspect from `camera+724`; missions rebuild a
separate shared camera through `0000100C -> 002588D0`. Its aspect at
`0x00421040+268` previously stayed at 480/272, stretching the mission scene
across the wider output even though the HUD was centered. The camera
replacement now updates that shared aspect before rebuilding. `00088F6C`
uses the same value for horizontal culling. Resizing back to native width
restores it before taking the native-width fast path. (Since 17 September
those two aspect fields are the only thing the replacement changes; see
"Targeting HUD at wide aspect" below for why the rebuild itself must run at
480x272.)

```bash
# Actual recompiled camera/projection/culling code, without SDL or a full boot.
# Requires the local decrypted ELF and generated module from stage 04.
scripts/14-aspect-tests.sh
```

The 35 checks cover mission horizontal expansion, unchanged vertical scale,
horizontal culling, agreement with the garage camera, repeated resizes and
return to native width, unchanged scratch previews, restored guest dimensions,
and guest stack/return preservation. The previous replacement fails four
checks; the fix passes all 35. This complements the renderer fixtures, which
do not execute the game's camera code.

Ultrawide replay evidence is in `reports/aspect-mission/`: at 1920x816,
poll 1780's mission projection changes from X=1.7972368 to X=1.3479276
while Y stays 3.1715944. The extra width therefore reveals additional scenery
with the same vertical field of view. Native and ultrawide diagnostic runs of
`pitch-sweep.pad` reach all 121 events through poll 2600 with zero bad accesses.
The final build also completed a real-window 1920x816 run with window
resolution and dual controls, with zero bad accesses and zero HUD depth,
stencil, or destination-alpha hazards (`final.log`). That run accepted live
input and is marked tainted; it is a gameplay check, not determinism evidence.
Renderer fixtures passed 430/430 on each backend, and resolution fixtures
passed 40/40 in each aspect mode on the real display. The offscreen GL driver
failed two bitmap-font checks, so those resolution results use the real GPU.

## Assembly model previews — 7 September 2026

The part and AC models use inset perspective viewports directly on the display
target: the captured head viewport is 169×105 and the AC viewport is 181×106.
They retain their panel aspect. Classifying all perspective geometry as SCENE
stretched the previews and displaced them relative to the centered menu.

The GE now passes the viewport rectangle as optional backend metadata before
draws, including CPU-transformed, GPU-model, through and immediate draws. The
host identifies a preview when an inset display viewport has a matching
scissor, allowing one pixel for the game's odd-dimension rounding. PREVIEW
uses the menu's centered placement for geometry and clipping, retaining its
perspective, depth and stencil behavior. Full-screen cameras and scratch
targets keep their existing mapping. GPU batches retain their cached viewport
when a model batch fills; resetting it to SCENE would break PSP-resolution
previews on wider windows.

```bash
# Synthetic GE lists to physical pixels; requires a real desktop GL driver.
scripts/17-preview-tests.sh

# Actual Assembly menus, with controller/mouse input excluded from the replay.
scripts/09-replay.sh --decode --env PSPRECOMP_REPLAY_LIVE=0 \
  --env PSPRECOMP_ASPECT=window --env PSPRECOMP_RESOLUTION=window \
  --env PSPRECOMP_WINDOW_SIZE=1920x720 scenarios/assembly-previews.pad
```

Validation is in `reports/menu-preview/`:

- 46 synthetic checks pass in each of eight CPU/GPU transform, original/window
  resolution and original/window aspect combinations. They cover both panel
  rectangles, clipping, depth clears, depth comparison, GPU batch boundaries,
  resizing, scratch targets, and full-screen or differently scissored cameras.
  The original renderer fails 12 checks in the wide PSP-resolution GPU case;
  restoring only the erroneous viewport reset fails four.
- The fresh 1920×720 run visits head, core, arms and legs, with the rotating AC
  visible: 79/79 events through poll 1250, zero bad accesses, no live-input taint.
  Physical window captures were visually inspected.
- The six garage/mission/combat captures are pixel-identical to the pre-fix
  renderer in window-aspect mode. Original-aspect Assembly output is identical
  on each transform path, and the corrected wide frame repeats identically.
- Runtime tests pass 24/24; camera checks 35/35; renderer fixtures 430/430 on
  software and GL; resolution fixtures 40/40 in each aspect mode.

These GPU results use the real display. The offscreen driver's CPU transform
case passed, but its GPU-model case failed even at original aspect; it is not
used as the GPU validation result. CPU and GPU captured menu images differ at
15 of 1,382,400 physical pixels, so this check does not claim exact numerical
identity between transform implementations.

## Window-resolution rendering

`PSPRECOMP_RESOLUTION=window` enables physical-resolution display targets in
Last Raven's GL host. It selects GL when no backend is named and rejects an
explicit software/null backend. `psp` or unset keeps the reference mode.
Aspect and resolution are independent:

```bash
# Fill the drawable with Claude's adaptive camera and centered HUD.
PSPRECOMP_RESOLUTION=window PSPRECOMP_ASPECT=window scripts/06-boot.sh

# Keep the PSP aspect and render the fitted picture at physical resolution.
PSPRECOMP_RESOLUTION=window PSPRECOMP_ASPECT=native scripts/06-boot.sh

# Standalone GPU checks: no game files required.
scripts/13-resolution-tests.sh
```

Guest addresses, format, stride and dimensions stay in PSP units. Display
attachments use the physical drawable size (including stride padding);
byte-interpreted scratch targets remain at 1x. Projected float positions reach
the enhanced rasterizer before PSP subpixel rounding. A coherent drawable-size
snapshot controls allocation, viewport, scissor and presentation. Resizes
migrate color, depth and stencil with a GPU blit; allocation failures retain
the working surface, and hardware size limits cap allocation.

Compatible unswizzled RGBA8888 framebuffer textures use separate GPU snapshots,
including row-offset views, declared heights larger than the rendered rows and
self-composites. This keeps fine detail through framebuffer effects. Other
aliases resolve through guest bytes. CPU writes are tracked by touched byte,
including same-value stores; partial writes preserve the untouched channels
of every physical sample. Guest readback resolves with nearest sampling so
alpha/stencil values and packed reinterpretation are not averaged.

The body font has a game-specific sampling exception in `host/render_gl.c`.
Its 512x512 CLUT4 atlas is drawn as 13-pixel-high, axis-aligned, 1:1 glyph
triangles. At 1x, linear filtering samples texel centers exactly; magnification
interpolates the already shaded glyph edges again and weakens the strokes.
Enhanced rendering recognizes this draw pattern, without a fixed allocation
address, and uses nearest filtering. Other texture draws retain their filters.
This preserves the original bitmap's weight, with visibly square texels;
it does not supply a higher-resolution font asset. The garage comparison is
`reports/resolution-font-comparison.png`; only the body-text rectangle changes
in that fixed frame (8,013 pixels inside x=63..649, y=1015..1063 at 1080p).

Validation on the development desktop:

| Check | Result / local artifact |
| --- | --- |
| Physical framebuffer fixture | 40 checks in each aspect mode, all passing: `reports/resolution-fixture-final.log` |
| Detail and memory ownership | Sub-PSP-pixel stripes and pre-quantization edges survive texture views; CPU byte/same-value writes, per-sample channel preservation, scratch format reuse, depth/stencil and scissor pass |
| Resize matrix | 960x544, 1365x767, 2560x1440, 3440x1440, 3840x2160, 641x961, back to 960x544; both aspect modes; no GL errors |
| Reference backend fixture | 430/430 software and GL checks; enhanced 2x also 430/430: `reports/resolution-base-tests.log`, `resolution-1x-gl.log`, `resolution-2x-gl.log` |
| Runtime and check-runner tests | CTest 24/24 and Python 13/13: `reports/resolution-ctest.log`, `resolution-python-tests.log` |
| Fixed 1x scene gate | `reports/resolution-1x-final/results.json`, against `reports/m5-stencil-lines-final/results.json`; unchanged oracle hashes and comparison metrics |
| Repeated enhanced scenes | Six captures, each rendered twice at 1920x1080 with native aspect; physical-window images repeat identically: `reports/resolution-captures-final-1080/results.json` |
| Build/configuration | SDL and no-SDL host sources compile; invalid resolution and window-resolution/software combinations reject explicitly |

The final full gameplay replay is `reports/resolution-live-validated-1080/`:
1920x1080, window aspect, 87/87 events through poll 2210, zero bad accesses and
no live-input taint. The physical screenshots include the garage, mission
opening and combat. It exercised 2,047 GPU framebuffer views, 15 CPU uploads
and 160,850 body-font draws. The padded display pair is 2048x1080; the scratch
target ends at 128x64. GPU draw-plus-blit time averaged 1.60 ms, p95 3.6 ms,
maximum 5.36 ms. There were 2,211 rendered frames / 4,426 present callbacks over
70.976 seconds, interval p50 33 ms / p95 36 ms, with a 452.7 ms load/capture
outlier. These measurements include the screenshot instrumentation and are
not a sustained 4K performance claim.

Use `PSPRECOMP_GL_SHOT=<prefix> PSPRECOMP_GL_SHOT_EVERY=1` to inspect physical
window output. Ordinary `PSPRECOMP_FRAME` and gereplay output still read native
guest pixels and cannot prove that enhanced detail survived. Fixed captures
already contain the camera projection, so the repeated enhanced capture check
uses native aspect; the full gameplay run exercises adaptive camera placement.

Remaining work before treating this as a finished enhancement suite:

- Points and lines retain the PSP sample walker and pixel-quad coverage. Their
  staircase grows with resolution; physical-resolution vector coverage remains
  a separate increment.
- Swizzled, differently strided/formatted, non-row-aligned and mipmapped target
  aliases retain the native-byte fallback. Original image and movie assets
  keep their original resolution. Reduced-bit-depth stencil remains outside
  the existing RGBA8888 implementation.
- Presentation still chooses the current GE target; general scanout-address
  selection is unchanged. Adaptive aspect retains its documented backdrop and
  unclassified screen-space tracking limits.
- The resize matrix tests one desktop/driver. Minimize/restore, monitor DPI
  transitions, allocation exhaustion and sustained 4K gameplay need additional
  real-display coverage. The default remains PSP resolution.

Remaining: stencil semantics for 5650/5551/4444 targets, history-dependent
target format/size reuse, remaining blend modes, dithering, and broader
hardware/scene validation. Small RGB errors do not establish full PSP parity.

The recorder's memory snapshot is currently taken at the first list's
completion. This suite explicitly rejects multi-list captures rather than
assuming later lists' memory survived. Captures also omit initial depth and
GPU allocation history; the selected frames perform depth clears, but they
are not proof of arbitrary history-dependent rendering. Software remains a
useful differential oracle, not a substitute for PSP/PPSSPP reference checks.

## Targeting HUD at wide aspect — 17 September 2026

Sif reported the targeting UI leaving the screen on wide windows. Reproduced
with `yaw-sweep.pad` in a 2560x720 window (32:9, virtual width 967): the
yellow lock box was twice its size in both axes, its top and bottom past the
screen edges, and in the poll-2220 cutscene lock the red reticle sat twice as
far from the centre as its target. `reports/aspect-reticle/` holds the
frames (`native-*`, `wide32-*` before, `fixed32-*`/`fixed21-*` after) and RAM
snapshots.

Cause: the camera rebuild `000889B4` computes a focal length
`(width/2)/tan(fov/2)` from the render descriptor's width and builds the
display camera's own projection at `camera+64` (and the combined matrices
after it) from it. The GE never sees those matrices; the game projects with
them on the CPU to place the lock box, the lock-on reticle and the other
HUD elements that track a world position. Lending the rebuild the virtual
width scaled that projection by `wide/480` in both axes -- the 32:9
snapshots show P00 289.7 -> 583.6 and P11 144.9 -> 291.8 -- while the GE
scene, rebuilt from the shared aspect with a constant vertical FOV, widened
horizontally only. On top of that the renderer spread the depth-tested
reticle sprites with the scene mapping, multiplying their horizontal offset
by `wide/480` a second time.

Fix: the replacement runs the rebuild against the real 480x272 descriptor
and writes only the two aspect consumers, `0x00421040+268` (mission GE
projection and cull planes) and `camera+724` (the garage's copy), as
`virtual_w/272`. The camera's own matrices and `+716/+720` stay native. The
GL backend now places every non-full-width screen-space draw with the HUD,
the depth-tested reticle included: a target `d` native pixels from the
centre gets its reticle at guest `240+d`, the HUD mapping puts that at
`wide/2+d`, and that is the target's own pixel under the scene mapping, so
the depth test meets the target's depth. The report's "HUD hazards depth"
counter became "HUD batches depth-tested"; it counts the reticle.

Measured on the window images, in PSP pixels from the screen centre
(`native` is a 1440x816 window at the original aspect):

| Frame | native | 32:9 before | 32:9 after | 21.5:9 after |
| --- | --- | --- | --- | --- |
| Red lock box + reticle, poll ~2220: x range | -14..121 | -27..122 (clipped) | -12..121 | -11..122 |
| Same: y range | -87..41 | -38..82 | -87..42 | -86..42 |
| Yellow lock box, poll ~2340: width x height | 198 x 215 | 260 x 223 (clipped) | 198 x 215 | 198 x 214 |

The bands beside the centred 480 columns. The lock marker (the red circle
and LOCK label, `001FC6A8`, drawn from a point the mission loop records)
and the two depth-tested lock-on rings (`001FBF58`) are drawn wherever their
target projects, off the 480 columns too (a run logs ring x from -31580 to
1380; the PSP's scissor removed those). Two things kept them out of the
bands of a wide target. The host's HUD viewport and scissor clipped them:
a HUD batch beyond the 480 columns now keeps the HUD mapping but as a
shader translation over the whole target, with the whole-target scissor
when the game's scissor is the whole screen. Backdrops reach into the
stride padding as well and must stay out, as on the PSP -- the menus tile
one as 64x64 pieces whose last column spans x 448..512, others are pieces
taller than the screen at 478..512 -- so the band path is for a draw that
lies entirely off the screen, or that tests depth, or that is a line, and
that is shorter than the screen; a piece that starts on the screen is
clipped at its edge as before. Lines are the lock box itself: with camera
smoothing the box slides past the edge with its target, and the GL backend
rasterises lines through its own walker, clamped to the screen, so a band
line batch under the game's whole-screen scissor now walks into the bands.
Sif's hand-recorded AC Test session (`scenarios/lock-band-hand.pad`, with
`lock-band-hand.dialog` answering the save dialog the recorder cannot see)
replayed at 2560x720 shows the box continuing into the right band. The path is per title (`PSP_TITLE_HUD_BANDS` in a
title's `psp_title_info`, the weak `lr_hud_bands_available` until 5 Oct): Last Raven's menus, garage and missions were audited
and only the rings and the marker draw off the screen, while a replay of
AC3 Portable's menus showed some 26,000 untextured pieces parked there --
black in that run, but unaudited, so that title keeps the centred clip. And the mission loop keeps the marker only while `001FBDF8`, the lock
target's on-screen test, says the point lies within the render descriptor's
480x272 rectangle: replaced (`host/replace.txt`), it now also says yes in
the band width on either side. The rings pass a depth test against their
target, so a target behind terrain shows none; `PSPRECOMP_ASPECT_LOG=2`
logs every ring draw with the window image it lands in.

```bash
# Camera code: the HUD projection stays native while the scene widens, and the
# lock target's on-screen test accepts the bands (73 checks).
scripts/14-aspect-tests.sh

# Synthetic GE lists: screen-space sprites and lines land with the HUD, in the
# bands too; backdrop pieces that start on the screen stay clipped at its edge.
scripts/17-preview-tests.sh
```

Both GL fixtures had been running their window-aspect configurations at the
original aspect since 7 September: `present.c` offers the adaptive aspect
only when a title's replacement defines `lr_adaptive_aspect_available` (now
`PSP_TITLE_ADAPTIVE_ASPECT` in `psp_title_info`), and
the fixtures do not link one. They now define it themselves; the preview
fixture passes 49 checks at the original aspect and 51 in window aspect on
each transform path, the resolution fixture 40 in each mode, renderer
fixtures 430/430 on both backends. Fresh 2560x720 and 1720x720 `yaw-sweep`
runs deliver 117/117 events over 3100 polls with zero bad accesses; each
reports 848 depth-tested HUD batches, the two 64x64 reticle textures.

## Self-contained tests

```bash
python3 -m unittest discover -s scripts -p 'test_render_check.py' -v
ctest --test-dir build/psprecomp --output-on-failure
scripts/12-render-tests.sh             # software + GL; opens a test window
scripts/12-render-tests.sh --software  # no display needed
```

The comparison tests cover image parsing, exact/per-channel/region metrics,
truncated and multi-list capture rejection, and baseline identity/regression
guards. The runtime capture tests use synthetic lists to cover poll selection,
small-list rejection, independent memory snapshots, legacy frame selection,
and malformed schedules. None require game data or a GPU.
