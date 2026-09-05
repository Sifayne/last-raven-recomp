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

## What the wider coverage exposes

1. **Stencil is exercised in combat.** Both combat captures contain 801 draws
   with stencil enabled, which GL counts as unsupported. The earlier four
   captures contain none. These counts establish coverage, not the visible
   severity of each state: a stencil operation may only change alpha or may
   be a no-op. The next stencil tests must inspect alpha and subsequent
   stencil/depth outcomes as well as RGB.
2. **Lines are a shared hole.** The combat captures request 8 and 9 line draws.
   Both software and GL omit them, so a small RGB difference cannot validate
   them. Add synthetic/hardware-grounded line tests to both backends.
3. **Target reuse needs a history test.** The full run logs scratch target
   `0x04154000` changing from 256x128/5551 to 128x64/8888. GL retains its
   original allocation and format. A fresh single-frame replay does not
   reproduce the earlier allocation, so this requires a multi-frame or
   synthetic target-reconfiguration regression. Its visible impact is not
   established by the fixed-frame RGB scores above.

No unsupported blend factors/equations were reported in these six frames.
Dithering and wider scene/driver coverage remain separate work.

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
```

The comparison tests cover image parsing, exact/per-channel/region metrics,
truncated and multi-list capture rejection, and baseline identity/regression
guards. The runtime capture tests use synthetic lists to cover poll selection,
small-list rejection, independent memory snapshots, legacy frame selection,
and malformed schedules. None require game data or a GPU.
