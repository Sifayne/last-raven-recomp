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
