# Higher-framerate rendering investigation

Investigated 6 Sep 2026 against `f78db5b`, with psprecomp `cfdc4d9`.
This is a feasibility result, not a supported framerate option. Production
code and the normal boot executable are unchanged.

Higher-framerate output looks practical. The game contains a one-interval
pacing mode, and an isolated probe reaches 60 updates/second with the null
renderer. Selecting that mode also speeds up gameplay: the same walking
distance and animation progression occur in half the wall time. A correct
enhancement therefore needs more than the pacing switch.

A follow-up [game-tick investigation](GAME-TICK-MAP.md) locates the mission's
simulation gates and experimentally separates 30 Hz player updates from a
60 Hz loop. The player and animation states match a reference replay, while
the chase camera is a confirmed update outside the gate. That investigation
provides the next concrete game-code boundary to develop.

## Where the limit lives

SDL's GL swap interval is already zero (`host/present.c`). The guest clock
tracks wall time in presented runs. Neither a faster swap nor a faster guest
clock is the missing enhancement.

The game's render system lives at `0x00424400` in the measured run. Its
pacing object starts at render-system +4140. These are module-relative
addresses for NPUH10024; do not transplant them to a different executable.

| Function / field | Observed behavior |
| --- | --- |
| `002594A8(pacer, mode)` | Stores mode at +36; selects a scanline budget at +24: mode 0 gives `0x122` (290), nonzero gives `0x244` (580). |
| `0025941C(pacer)` | Resets pacing state and sets the initial vcount target from mode. |
| `00259250(pacer)` | Waits using vcount/vblank, with an alternate scanline-budget/`sceKernelDelayThread` path. |
| `0025A2A0(render)` | Calls that wait or resets the pacer, depending on render +4187. |
| `00259820(render, flags)` | Presents, waits, swaps buffers and maintains render statistics. Reached through `00259738` and `00000FB4`. |
| render +12 | Supplies the initial pacing mode through `0025AAD8` to `002594A8`. |

The probe selects mode 0 at poll 2080, after mission startup, by changing
render +12, pacer +36 and pacer +24 to the values selected by the game's own
setter. It retains the existing pacing history. It does not change display
counter implementations, clock speed, input, simulation routines or the GPU
backend. A linker wrapper around the imported SetFrameBuf call makes this
possible without regenerating or patching the production module objects.
The garage and opening cutscene remain at their original rate in these
experiments. Their looking normal does not establish correct behavior at
60 Hz; those modes need separate tests with the switch enabled there.

## Controlled measurement

Both null-renderer runs use the same poll-stamped replay and real-time clock.
The scenario is the existing walk sweep, stopped at poll 2160. Both deliver
84/84 events and finish with zero bad memory accesses.

| Measurement | Original mode | One-interval probe |
| --- | ---: | ---: |
| Update cadence, polls 2085–2159 | 30.01 Hz | 60.04 Hz |
| Median update interval | 33.334 ms | 16.647 ms |
| Walking distance, polls 2110–2129 | 20.0343 game units | 20.0343 game units |
| Time for that distance | 0.632863 s | 0.316774 s |
| Walking speed | 31.6566 units/s | 63.2448 units/s |

All 274 lines of movement, animation and camera diagnostics in polls
2085–2159 match exactly between the two runs. This establishes unchanged
per-tick behavior for those observables, not equality of the entire machine
state. It also demonstrates why matching a poll-based replay by itself is
insufficient validation for a framerate enhancement: real seconds matter.

A physical 1920x1080 GL replay, with window aspect and window resolution,
also runs through the switch. Its first measured segment at polls 2085–2349
achieves **56.16 updates/s**, median 17.950 ms and p95 19.927 ms. It delivers
96/96 events and reports zero bad memory accesses. Across the entire run,
including the earlier 30 Hz portion, GPU draw-plus-blit averages 1.71 ms,
p95 3.7 ms and maximum 5.33 ms. This is evidence of GPU headroom, not a
finished 60 fps pacing result or a sustained high-framerate performance gate.

A repeat with pacing-state and delay tracing measures **56.23 updates/s**
over the same segment (median 17.769 ms). All 265 recorded pacing states use
the late-frame fallback. In the detailed 15-frame delay sample, the game
requests only a 1 microsecond yield per frame at `00259358`, rather than a
remaining-frame sleep. The renderer-containing host path is already using
the frame budget. CPU execution, GE processing, GL submission and driver
synchronization need profiling before promising sustained 60 Hz. The low
GPU query time alone does not prove that the entire frame is cheap.

This repeat also observes the actual mission countdown: polls 2085–2159
decrement it from 5400 to 5326 in both the original and faster runs. Those
74 ticks take approximately 2.467 seconds in the original mode and 1.304
seconds in the GL probe. The countdown speeds up along with movement.

Count new rendered frames separately from SetFrameBuf callbacks. The GL run
reports 2,351 rendered frames and 4,706 present calls. The two callbacks per
game frame do not supply two different animation states.

## Why changing one delta-time value is insufficient

There are multiple kinds of time in the game:

- The mission loop `00102018` increments `0x0030F008` and decrements the
  countdown at `0x00448780` once per eligible iteration (`00102684–001026A4`).
  The countdown formatting helpers `0022C3B4` and `0022C43C` divide by 30,
  then by or modulo 60. This timer is expressed in game ticks.
- The animation updater `001DFC80` adds a byte step at slot +92 to the
  integer frame at +76. It cannot express a half-frame step by assigning 0.5
  to that field. Fractional sampling or an independent animation clock would
  be necessary for genuinely smoother animation.
- The movement helper `0004EE9C` explicitly uses 30.0 and 60.0 in its
  velocity/acceleration math. Existing yaw/pitch, control ramps and camera
  smoothing also operate per update.
- The separate loop `0018A630` passes the literal float `0x3D088888`
  (approximately 1/30 second) into a virtual callback and scene work. This
  is another timing site, not proof of a universal timestep variable.

Halving motion alone would leave timers, state transitions, animation,
effects, weapon cadence and other per-tick behavior needing investigation.
Halving only the global tick counter would leave the integrations running
twice as often. Neither is a complete solution.

## Recommended next experiment

Build on the mission simulation gate documented in [GAME-TICK-MAP.md](GAME-TICK-MAP.md):
keep authoritative updates at their original 30 Hz, account for the camera
and other updates outside the gate, and investigate interpolated rendering
at 60 Hz. This aims to preserve combat rules while providing a path to higher
display rates later. Only the short player/timer replay has been matched;
combat equivalence and an interpolation implementation have not been proven.

The first bounded prototype should capture consecutive camera and rigid
object transforms with stable identities, then draw one intermediate pose
without running an extra simulation tick. Validate it on a camera rotation
and a moving AC. Establish a render-only path and detect any guest-memory
side effects before adding skeletal poses, projectiles, particles and HUD
elements tied to world positions. Profile host frame work alongside this
prototype: rerunning all guest simulation and GE processing for each extra
image would retain the measured throughput problem as well as the timing
problem.

The current backend receives transformed screen-space vertices. The new
floating-point positions preserve spatial precision, but carry neither
previous poses nor object identities. In this game's existing mission
traces, camera motion is already composed into world matrices before GE
submission (`docs/findings/state.md`, controls). Blending arbitrary GL draws
or changing a single view matrix is therefore not an adequate implementation.
Capture needs to happen before that information is lost, or retain enough
draw identity and transform data to reconstruct it reliably.

Interpolated frames need separate host render targets and must not replace
the authoritative guest framebuffer/readback history. Snapshot geometry,
textures and render state that the guest can overwrite; do not blindly rerun
a display list against live memory. Reset interpolation across scene cuts,
loads, teleports and object creation/removal. Measure the latency introduced
by buffering two poses; smoother output still leaves simulation/input
response at 30 Hz unless that is addressed separately.

The validation gate should compare equal guest-time intervals: travel and
turn angles, mission countdown, firing intervals, resource consumption,
animation duration, pause behavior and audio synchronization. Retain the
existing deterministic 30 Hz replay as the simulation reference, and verify
that intermediate images add no authoritative state changes. Count and
inspect genuinely different intermediate images at the requested cadence.

Local evidence and isolated probe sources are in
`reports/framerate-research/`: `probe.c`, `build-probe.sh`, `walk.pad`,
`walk-gl.pad`, `baseline.*`, `unlock.*`, `*-input.log`, `unlock-gl.*`,
`baseline-detail.*`, `unlock-gl-detail.*`, `results.json`,
`selected-disassembly.txt` and `loop-excerpts.txt`.
