# Mission simulation tick investigation

Investigated 6 Sep 2026 against `f78db5b`, with psprecomp `cfdc4d9`.
Addresses are module-relative for this project's NPUH10024 executable.
Experiments use separate linked binaries; production sources and the normal
boot executable are unchanged. See [pacing research](HIGH-FRAMERATE-RESEARCH.md)
for the render loop's existing 30/60 Hz modes and measured host throughput.

## Result

The game has a useful simulation boundary in `psp_func_00102018`, the mission
loop. Its global tick counter records progress, but is not a master delta-time
input that controls movement. Holding that counter constant leaves walking
and animation running. An existing stop-state gate suppresses both major
simulation regions and the mission timer while drawing commands continue.

Alternating this gate in the existing faster pacing mode produces a roughly
60 Hz loop with 30 Hz player updates and animation. In a short walking replay,
all 60 corresponding simulation frames match the original run's player
position, yaw, tick, countdown and animation diagnostics. The chase camera
is a confirmed exception: it updates outside the gate and its smoothing
changes. This establishes a promising boundary, not a complete render-only
path or a finished smooth-framerate enhancement.

## Main loop and clocks

The mission iteration spans `0010209C` through `001026E8` inside `00102018`.
It polls input, performs game work, submits drawing, presents, and loops.

| Address / function | Role established by code and probes |
| --- | --- |
| `0x0030F008` | Tick counter, incremented at `00102684–00102694` once per eligible iteration. Holding it still does not stop integrations. |
| `0x00448780` | Mission countdown, decremented at `00102698–001026A4`. Formatters `0022C3B4`, `0022C43C`, `0022F598` convert `(countdown + 29) / 30` to seconds, then divide/modulo 60. |
| `0x00386EDC` | Outer loop counter, incremented at `001026D8–001026E8` even when the two simulation regions are skipped. |
| `0x003179A0` | Stop-state word. `002231F8 -> 00223220` returns whether it is nonzero; `00223230(a0)` sets it. Nonzero skips both simulation regions and the tick/countdown tail. |
| `0x0042D30B` | A second byte gate with the same broad skip destinations in the mission loop. |
| `0x0030F2FC` | A narrower freeze word. Stops the measured AC movement and animation updates, but leaves the mission tick and countdown advancing. |

Three calls to `002231F8` coordinate the broader gate:

| Call instruction | Return address | Block skipped when nonzero |
| --- | --- | --- |
| `00102274` | `0010227C` | First simulation region, resumes at `00102388`. |
| `0010239C` | `001023A4` | Second simulation region, resumes at `001024C8`. |
| `0010265C` | `00102664` | Tick/countdown tail, resumes at `001026A8`. |

The byte gate at `0x0042D30B` is checked alongside these queries. These are
existing game state controls, not newly discovered user-facing FPS settings.
`00222F44` writes stop state 2; `002230D8` writes 4 or 0 via the setter.
Their UI/state semantics need preservation in any production enhancement.

## Paths from the loop to movement and animation

The movement path includes:

```text
00102018 (mission loop)
  -> 000FF0AC
    -> 000FF5BC
      -> 0004CF74 (per-AC update)
        -> 0005EBF0 (input work)
        -> 00053048 / 00053234 (yaw/pitch paths)
        -> 000519D8 (movement-state dispatch)
```

`0004CF74` also copies previous transforms and updates AC status.
`000519D8` dispatches the callback at AC +240 with the argument at +244.
No shared runtime delta-time argument was found on this traced path.

An animation call path through the second gated region is:

```text
00102018 -> 000DCFBC -> 00223A38 -> 00223F00
  -> 00224248 -> 001DF1D0 -> 001DFC80
```

The probe directly counts `001DFC80` calls for the player's model pointer
at player +128. Each enabled simulation iteration produces one such call
in this scenario; skipped iterations produce none.

Several routines embed conversions between 30 and 60:

- `0004EE9C` contains velocity integration of the form
  `(velocity * 30 / 60 + acceleration + acceleration) * 60 / 30`.
  Other movement helpers, including `0004D030` and `00045248`, contain
  explicit `60 / 30` factors.
- `001DFA58` converts animation duration using `30 / 60`.
- `001DF5A8` samples animation displacement at `frame * 60 / 30` and scales
  the displacement difference using `60 / 30`.
- `001DFC80` samples a pose at `frame * 60 / 30`. Its slot has an integer
  frame at +76, frame count at +78, and byte step at +92. It adds that step
  once per call. The routine already interpolates between authored keyframes
  through `001DF78C`, so fractional pose sampling is a plausible future seam;
  writing 0.5 to the integer step field cannot provide it.
- `0018A630`, a separate loop, passes approximately 1/30 second as a literal
  float to a virtual callback. Its scene role has not been established here.

This pattern suggests rate adaptation was compiled into multiple systems.
It does not establish a single editable FPS variable or a complete list of
changes needed for true 60 Hz simulation. A literal scan found 176 functions
containing both float 30 and float 60 loads; those are candidates, not 176
confirmed timestep fixes. Native generated code embeds these instructions,
so patching their original instruction bytes in guest RAM would not change
the already compiled host function bodies.

## Causality experiments

Five deterministic null-renderer runs use the same classic-control replay.
The player walks forward during polls 2100–2129. Candidate gate values are
saved and restored around individual loop iterations; manipulation ends at
poll 2130. These runs establish causality, not wall-time performance.

| Intervention, polls 2100–2129 | Player update / animation calls | Tick delta | Countdown decrement | XZ travel |
| --- | ---: | ---: | ---: | ---: |
| Observe | 30 / 30 | 29 | 29 | 21.5205623 |
| Hold `0x0030F008` at its first value | 30 / 30 | 0 | 29 | 21.5205623 |
| Set `0x003179A0 = 1` | 0 / 0 | 0 | 0 | 0 |
| Set byte `0x0042D30B = 1` | 0 / 0 | 0 | 0 | 0 |
| Set `0x0030F2FC = 1` | 0 / 0 | 29 | 29 | 0 |

Call counts include both endpoints; state deltas span 29 intervals. GE
command counts continue advancing in every trial. The player update and
animation calls resume after the intervention is removed. This is evidence
that the gates control updates while the drawing path remains active; the
null renderer does not establish visual correctness.

## Alternating simulation inside the faster loop

Two additional runs use fast deterministic startup and switch to real time
at poll 2090. The reference keeps original pacing. The experiment selects
the one-interval pacing mode at poll 2100, and forces stop state only on odd
polls thereafter. It doubles the replay's walk and release durations in
polls, so the same input lasts the same number of simulation updates and
approximately the same wall time.

| Walking interval | Original | Alternating gate |
| --- | ---: | ---: |
| Poll endpoints | 2104–2128 | 2108–2156 |
| Elapsed seconds | 0.799886 | 0.800385 |
| Outer-loop cadence | 30.0043 Hz | 59.9711 Hz |
| Simulation tick delta | 24 | 24 |
| Countdown decrement | 24 | 24 |
| XZ travel | 20.1806590 | 20.1806590 |

The subsequent release interval also matches: 3.3514076 units over 28
simulation ticks in 0.932801 versus 0.932680 seconds.

Across all 60 corresponding simulation frames, player X/Z/yaw, global tick,
countdown and the animation, pitch and push diagnostic lines match exactly
at their logged precision. Position floats are logged with nine significant
digits. Each of the 60 inserted frames holds those player/timer values,
has zero counted player update/animation calls, and still issues at least
16,575 GE commands. They do not create intermediate player poses.

All seven runs deliver 84/84 replay events and report zero bad memory
accesses. These short null-renderer experiments do not establish combat,
effects, audio, pause, scene-transition or visual correctness at a higher
output rate.

## Confirmed exception: camera update

The first simulation gate resumes at `00102388`, which unconditionally calls
`000FF280`. That function calls `00074168` at `000FF2F4`, updating the chase
camera. This happens outside both major simulation regions.

The alternating run produces 120 camera diagnostics against 60 in the
reference interval. Only 8 of the 60 corresponding camera lines match
exactly. Maximum logged deviations are approximately 0.000001 radians yaw
and 0.000083 radians pitch in this straight-walk test. The small magnitude
does not make the behavior equivalent: the camera's existing 0.83 smoothing
factor is now applied twice as often. A turning replay should expose the
effect more strongly.

## Next bounded prototype

Continue at the game-side boundary. Implement an explicit simulation phase
that preserves genuine stop/UI states, with camera simulation handled at
the same authoritative cadence. The three mission query call sites are
useful candidates for a scoped replacement; overriding the stop word
globally was a causality probe and should not become the production design.
Audit other unconditional loop calls before calling the skipped iteration
render-only.

Then capture previous/current camera and object poses before GE submission
and produce an intermediate drawing pose without committing extra gameplay
updates. Audit the animation sampler for fractional authored-time sampling
or interpolate saved skeletal transforms. Validate visibly distinct images
and equal-time movement, turning, timer, weapon and resource behavior.
True 60 Hz simulation is a separate, broader path through the compiled rate
conversions and frame-based counters.

Host efficiency still determines achievable output rate, but it does not
answer the simulation-timing question. The existing 1080p GL probe reached
about 56 Hz with full simulation on every loop; the alternating-gate probe
has only been measured with the null renderer.

## Local reproduction and evidence

`reports/game-tick-research/` contains the isolated linker wrapper in
`probe.c`, `build-probes.sh`, `analyze.py`, `results.json`, selected function
extracts, `rate-literal-candidates.txt` and the seven run logs. These local
artifacts are ignored by Git. Probe mode names are documented in `probe.c`.
The ordinary build objects are linked into separate `boot-0` through
`boot-6` executables; rebuilding probes does not replace the normal boot.

For example, from the repository root after building the normal host:

```sh
bash reports/game-tick-research/build-probes.sh
PSPRECOMP_RENDER=null PSPRECOMP_MPEG_DECODE=1 PSPRECOMP_INPUT=classic \
  PSPRECOMP_REPLAY=reports/game-tick-research/walk-60.pad \
  PSPRECOMP_INPUT_LOG=reports/game-tick-research/alternate-input.log \
  PSPRECOMP_FRAME=reports/game-tick-research/alternate.ppm \
  reports/game-tick-research/boot-5 game/extracted/ACLR_App.elf \
  'game/Armored Core - Last Raven Portable.iso' \
  > reports/game-tick-research/alternate.log \
  2> reports/game-tick-research/alternate.err
python3 reports/game-tick-research/analyze.py
```

Mode 6 is the paced reference. Modes 0–4 are the deterministic causality
trials. Those six runs use `reports/framerate-research/walk.pad`, ending at
poll 2160, with their corresponding output names. `walk-60.pad` retains the
same prefix and starts walking at 2100, releases at 2160 and stops at 2220.
