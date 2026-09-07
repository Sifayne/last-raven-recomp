# Review of the mission-tick investigation

Checked 6 Sep 2026 against `f78db5b`, reviewing [GAME-TICK-MAP.md](GAME-TICK-MAP.md)
and [HIGH-FRAMERATE-RESEARCH.md](HIGH-FRAMERATE-RESEARCH.md). The loop map,
the three gate sites, the tick/countdown tail and the camera exception all
check out against the disassembly. The findings below are what the report did
not measure. Evidence and scripts are in `reports/game-tick-review/`
(git-ignored); the probe binaries are the report's own `boot-5` (alternating
gate) and `boot-6` (30 Hz reference), unchanged.

## 1. The skipped iteration is not render-only: it drops the HUD

The report counted GE commands on the inserted frames but never compared them
with the frames around them. Its own `alternate.err` shows a consistent gap:

| Iteration (mode 5, polls 2104–2155) | GE commands, median | Vertices, median |
| --- | ---: | ---: |
| Simulated (even polls) | 19,488 | 42,130 |
| Inserted (odd polls) | 18,291 | 41,863 |
| Reference run, either parity | ~19,410 | ~41,650 |

A GE capture of polls 2120 (simulated), 2121 (inserted) and 2122 (simulated),
replayed through the software backend, shows what the missing ~1,200 commands
are: **the whole HUD** — AP, mission timer, radar, weapon panels, energy bar
and the targeting box (`reports/game-tick-review/composite.png`, top row;
bottom-left is the pixel difference). The inserted frame differs from its
predecessor in 4,131 pixels; two consecutive simulated frames differ in 1,029.
The 9 line primitives of the reticle are absent from the inserted list.

Cause: the gate used for the experiment is the game's stop word, and the HUD
family (`002216B0`, `00221B5C`, `00221CE4`, `00221E6C`, `0022201C`, `0023883C`)
queries it through `002231F8` on the drawing side too. Under the alternating
scheme the HUD would flash at 30 Hz. The report's "next bounded prototype"
already says to replace the three mission-loop query sites instead of the
global word; this is the concrete reason it is mandatory, not a nicety.

## 2. What the inserted iteration still writes

Whole-RAM snapshots (`PSPRECOMP_RAMSNAP`, polls 2120–2125) in both runs,
diffed per iteration with parity held equal (the game double-buffers its
per-frame draw data by frame parity at `0x4E0000`/`0x560000`, so odd frames
must be compared with odd frames):

| Per iteration | Module data+BSS words | RAM words |
| --- | ---: | ---: |
| Reference, odd poll | 15,264 | 15,054 |
| Inserted, odd poll | 4,353 | 8,048 |
| Written on inserted frames but never in the reference | 50 | 222 |
| Written in the reference but not on inserted frames | 10,978 | 7,527 |

So the stop word does not trigger a different code path in memory terms; it
suppresses one. What it suppresses, besides the AC and tick state, is the
per-frame draw generation: ~9,400 words at `0x4E8D64–0x4F4A58` and the
220-word blocks at `0x01514C40+n·0x1B00` and `0x01CA5F80+n·0x9C0` (per-model
pose/skin output, 18 matrices each). Those are produced inside the gated
regions, so an inserted frame draws from whatever was left in its parity
buffer. That is fine only while every odd frame is inserted; a scheme that
inserts frames irregularly will read a stale buffer.

What the inserted frame does keep writing, confirmed by address:

- The camera at `0x0043C080` (eye/target/pitch/yaw, `host/replacements.c`)
  and its second instance at `0x0043D9D0`, plus the matrix blocks that follow
  it — the report's confirmed exception, seen directly.
- Present/pacer bookkeeping in the render objects at `0x421080`, `0x4211C0`,
  `0x424860`, `0x425420`, `0x4254C0`.
- Sound-side words at `0x317244–0x317254` (writers `0008D100`, `001FCE50`)
  and a tick timestamp at `0x3E67F4` written by `00206DC8` — small, but they
  show the unconditional calls after the gates are not pure drawing.

## 3. Input edges survive the skipped frames

The raw pad sample at `0x386EB0` and its previous copy at `0x386EC0` are
refreshed every iteration by `000FED24` (unconditional, `00102150`). The
edge logic that matters for gameplay does not use that previous copy: the
AC's input work `0005EBF0` keeps its own previous sample inside the AC
(`ac+8268`) and derives the pressed/held words (`ac+9668/9670`, read by
`0005EFA0`/`0005EFD4`) from it. A press that starts on an inserted frame is
still seen as an edge on the next simulated frame. The unconditional
handlers (`000FEF3C` pause toggle, `001005A4`, `000FF4C4`) use the global
pair at 60 Hz, which is also correct. Input latency stays at the 30 Hz
cadence either way — the scheme buys smoothness, not response.

## 4. The pacing switch is a boot-time constant, not a scene mode

`002594A8(pacer, mode)` is reached only from the pacer constructor
`0025920C` (mode 0) and from render init `0025AAD8`, which passes render+12.
Nothing writes render+12 after `000013FC` sets the object up. The game has no
scene that runs the mission loop at one-interval pacing; the garage and
result screens use a different loop (`00102AA8 → 0018A630`) that passes a
literal 1/30 s. So there is no hidden 60 Hz mode to borrow — the pacer switch
in the probe is the only way in.

## 5. What "true 60 Hz simulation" would actually cost

Inventory of the compiled rate constants (scan of `aclr_funcs.c`):

| Pattern | Count |
| --- | ---: |
| Functions loading both 30.0f and 60.0f | 176 |
| Total 30.0f loads / 60.0f loads | 269 / 261 |
| Functions with 30.0f only | 27 |
| Functions with 60.0f only | 21 |
| Functions with 1/30 (`0x3D08…`) / 1/60 | 6 / 1 |
| Reads of the tick `0x30F008` | 146 sites in 108 functions |

The near-equal 30/60 totals say the port converted a 60 Hz engine
systematically: `0004EE9C` is `(v·30/60 + a + a)·60/30` with both constants
as `lui` immediates, and the animation sampler reads authored 60 fps data at
`frame·60/30`. That makes the float side look mechanical — flip 30→60 in the
paired functions and the factors become 1. Three things stop it being a
flip:

- The 27 unpaired 30.0f sites mix tick-rate uses (`cvt.s.w` then `/30` in
  `0007A228`, `000B52BC`, `00151788`; `c.lt.s x, 30` thresholds in
  `0005CEE0`, `000DE124`, `00251184`) with plain numbers (`00259820`'s fps
  statistic, the 30-vs-17.9 choice in `00073F98`/`000757E8`).
- The tick's 146 readers are time inputs: 51 sites divide or take a
  remainder of it (periodic phase — flicker, cadence), 23 compare it against
  a deadline, 15 store it as a timestamp. Every one changes meaning at 60.
- Integer tick counts (`if (timer > 20)`, `5400` for the 3:00 countdown) were
  folded by the compiler and are invisible to any scan. That tail is
  open-ended; the interpolation path's is not. The report's recommendation
  stands, for a sharper reason than it gave.

## 6. Where interpolation should live

`0004CF74` copies each AC's position (`ac+16`) to `ac+48` and its orientation
block (`ac+32`, yaw at `+36`) to `ac+64` before integrating, so previous and
current poses already exist per object with stable identity. A guest-side
prototype through `host/replacements.c` — on an inserted frame, set
`ac+16/+36` to the midpoint before the draw call `0022DCAC` (`001024C8`) and
restore after — needs no renderer work and no display-list identity
recovery. The open question to settle first is whether the draw composes
matrices from `ac+16/+36` or from a matrix cached by the update; the
per-frame matrix buffers are rewritten on inserted frames (section 2), which
suggests the former. Skinned poses (the 220-word blocks) would stay at 30 Hz
in that prototype, which is the usual compromise.

The camera must be handled explicitly: gate `000FF280` with the same phase
and interpolate `0x0043C080`'s eye/target/yaw/pitch, rather than letting its
0.83 smoothing run twice per tick.

Sections 7–9 test this. The pose fields are the right input, but the draw
does not read them where this section assumed; the working bracket is in
section 9.

## 7. A scoped gate gives exact duplicate frames

`probe2.c` is built against a private re-emit (`build-probe2.sh`, three
extra entries in the replace list; nothing under `build/` or
`game/generated` is touched). Mode 0 replaces the stop query `002231F8` so
that only its three mission-loop return addresses (`0010227C`, `001023A4`,
`00102664`) see "stopped" on an inserted frame, holds the camera caller
`000FF280` on those frames, and switches the pacer exactly as the report's
mode 5 does.

| Mode 0, polls 2104–2155 | Simulated | Inserted |
| --- | ---: | ---: |
| GE commands, median | 19,488 | 19,454 |
| Vertices, median | 42,159 | 42,159 |
| Pixels differing from the previous frame (poll 2121) | | 0 |

The inserted frame is pixel-identical to the tick before it, HUD included
(`scoped-gate-composite.png`); the tick, countdown, position and yaw per
poll match the reference. This is the base a display-only frame has to
start from, and the global stop word cannot provide it.

## 8. Where the drawn pose actually comes from

Poking `ac+16/+36` around the layer dispatcher `0022DCAC` changes nothing on
screen (modes 1–3, 0 pixels), and neither does poking plus the game's own
transform rebuild `00052D38(ac, 1)` there (first attempt at modes 4–6). A
checksum probe (`probe3.c`, pass-through wrappers around every direct callee
of the loop) and traced memory watches settle the pipeline:

- Region 1: the AC update integrates `ac+16` and `ac+32..40`; then
  `00052D38(ac, 1)` copies them to the model, `00045DAC` builds the skeleton
  root at `0x0979EB80` (translation = AC position, rotation = yaw),
  `0004D9CC` per part composes joint × local into the part node (`node+48`)
  and `001DF338` pushes that into the mesh (`mesh+0x140…`).
- Unconditional tail: the world is emitted as a real GE display list by the
  `0008D28C` path — 9,654 + 144 + 1,690 words land in `0x4E8D64–0x4F4A58`
  before the stop queries inside `0008D384`, `0008D474` and `0008D540`. The
  block decodes as 2,079 `WORLD_D` words (173 world matrices), `BASE`,
  `VADDR`, `VTYPE`, `PRIM` and texture state; it is parity double-buffered,
  and the view is already composed into the world matrices (no world-space
  translation survives in it). `0022DCAC` only dispatches registered layers.
- `PSPRECOMP_WATCHMEM` never sees the list writes (unhooked stores); the
  checksum wrappers do.

So an inserted frame re-renders the world from unchanged mesh matrices, and
a pose poke has to be in place *when the tail runs*. With the bracket opened
at the end-of-scene thunk `00000B78` (`0010261C`) and closed at the tail stop
query (`00102664`), an 8-unit push on the player moves it on screen: 1,841
pixels differ (`offset-diff-tail.png`).

## 9. Half-step interpolation, working

`probe4.c` modes 5 and 6: on every simulated frame each AC is posed at the
midpoint of its previous (`ac+48`, `ac+68`) and current (`ac+16`, `ac+36`)
pose, rebuilt with `00052D38`, rendered by the tail, then restored and
rebuilt again; inserted frames render the current pose. The AC therefore
advances by half a tick every 60 Hz frame.

| Pixels differing, frame to frame, polls 2118→2123 | Values |
| --- | --- |
| Scoped gate only (mode 0 pattern) | 0 / 561 / 0 / 542 / 0 |
| Half-step, camera held (mode 5) | 326 / 517 / 330 / 480 / 313 |
| Half-step, camera stepping on ticks (mode 6) | 328 / 885 / 337 / 900 / 329 |

The alternating larger value in mode 5 is the walk cycle, which still steps
at 30 Hz; mode 6 shows the camera's full steps dominating until it gets the
same treatment. In all runs the per-poll tick, countdown, position and yaw
are identical to mode 0, with zero bad memory accesses: the interpolation
adds no authoritative state. `interpolated-halfsteps.png` is the six frames
cropped around the AC.

What this establishes and what it leaves:

- The camera is done in section 10.
- Skeletal animation: done at the joint level in section 11.
- Projectiles, effects and anything not in the AC array need their own
  prev/cur seam. The list-level alternative — pairing the 173 world matrices
  between two builds — is not a byte-offset job: 10,537 of the 12,093 words
  change opcode position between builds, so it needs a parse of the list.
- Cost is two extra `00052D38` per AC per tick; latency grows by half a tick.
- Production shape: the same three replacements in `host/replacements.c`
  keyed on a frame-phase flag, plus the pacer switch and real-time pacing at
  60 Hz. `00000B78` has no interior entries; `000FF280` and `0022DCAC` do,
  but only as loop labels.

## 10. Camera half-step, working

The camera array starts at `0x0043C080` (288 bytes per camera; the chase
update `00074168(index)` addresses `base + 288·index`). Per tick the update
writes eye (`+16`), pitch (`+32`), yaw (`+36`) and target (`+48`), then
`00074FF8(camera)` builds the view matrices from those fields (its other two
inputs are `camera+272` and the present object, derived internally) and
`00004410([0x307B38])` hands them to the render system at `0x424400` through
`00259ABC`. The matrices themselves are written by stores the memory watch
cannot see, which is why the earlier watch on `0x43C770` reported nothing.

`probe5.c` records the camera fields before and after each real update and,
inside the same tail bracket as the ACs, writes the midpoint eye, target,
pitch and yaw on simulated frames, calls `00074FF8` and `00004410`, and
restores and rebuilds after the render. Mode 7 pushes the eye and target by
8 units on inserted frames instead: 4,107 pixels differ, so the rebuild path
reaches the screen.

| Pixels differing, frame to frame, polls 2118→2123 | Values |
| --- | --- |
| ACs only, camera stepping on ticks (mode 6) | 328 / 885 / 337 / 900 / 329 |
| ACs and camera at half-step (mode 8) | 408 / 706 / 439 / 701 / 442 |

The remaining alternation is what the difference images show
(`camera-halfstep-diffs.png`): the walk cycle, which still steps at 30 Hz,
and the radar sweep in the HUD. World edges move by the same amount in
both transitions. Tick, countdown, position and yaw per poll are identical
to the scoped-gate run; zero bad memory accesses.

Two extra matrix uploads per simulated frame show up as about 34 more GE
commands. Not interpolated yet and visible in the numbers: the camera's up
vector at `+240` (constant in this scenario) and the unknown per-tick words
at `+80..+92`, `+200` (a frame counter), `+256` and `+264`.

## 11. Joint-level half-step: limbs, position and yaw in one hook

`00045DAC(model, flag)` is where the sampled animation and the root pose
become world matrices: it builds the root from `model+0` and `model+16`
(`002B0134`, `002AFFEC`), copies it to `[[model+120]+96]`, and walks the
skeleton (`001DF338` from `[model+120]+128`) into the joint block that
follows — 24 consecutive 4x4 matrices for an AC, each with column 3 equal to
0,0,0,1. The part nodes and meshes are built from those joints afterwards by
`0004D9CC`, so interpolating the joints is the single seam below both the
walk cycle and the AC's pose. The animation frame counters are never
touched: the sampler still advances only in region 2.

`probe6.c` mode 9 replaces `00045DAC`. On a normal tick it records the
block before the original overwrites it; when the tail bracket re-runs
`00052D38` per AC it lets the original rebuild the current joints and then
pulls every matrix halfway back toward the recorded ones — rows lerped, the
three rotation rows re-orthogonalised and kept at their interpolated length,
translation lerped — and rebuilds again on close. No AC pose field is
written; the camera is handled as in section 10.

| Pixels differing, frame to frame, polls 2118→2123 | Values |
| --- | --- |
| ACs at part level and camera (mode 8) | 408 / 706 / 439 / 701 / 442 |
| Joints and camera (mode 9) | 515 / 594 / 554 / 584 / 586 |

The alternation is gone: every 60 Hz frame carries about the same amount of
motion, and the difference images (`joint-halfstep-diffs.png`) show the
limbs moving on both transitions. What still steps on ticks is the HUD (the
radar sweep and the reticle) and anything outside the AC array. Per-poll
tick, countdown, position and yaw are identical to the reference, zero bad
memory accesses. Cost: 48 matrix interpolations and the same rebuilds as
before per simulated frame.

The 0.5 in the lerp is the only place the 60 Hz ratio is hard-coded on the
object side now; with a time accumulator it becomes the frame's alpha.

## 12. The native loop with a wall-clock accumulator

`probe7.c` replaces `00102018` with the emitter's own C for it (extracted
with `scripts/fn-source.sh`, 452 instructions as gotos over the register
globals) and adds three hooks, each anchored on an instruction comment:

- `frame_begin()` before `0010209C`, the first instruction of an iteration:
  it charges the accumulator with the guest clock (`psp_clock_peek`, which
  tracks wall time in real-time mode), takes `n = floor(acc / tick)` ticks
  with `tick = 1/30 s` and a cap of 4, and sets `inserted = (n == 0)` for the
  gate and `alpha = acc / tick + OFFSET` (clamped to 1) for the lerps.
- a `tick_entry` label before `00102268`, the pause-byte check that opens
  gate 1.
- at `L_001024C8`, where the region-2 path and its skip converge: while more
  ticks are owed, run the loop's own tick tail (stop query at its real
  return address, pause byte, ready flag, tick++, countdown-- unless
  `0x386ED8`) and jump back to `tick_entry`.

One tick per frame therefore executes the original instruction sequence
unchanged, including the camera call between the regions and the tail in
its original place after the render. Zero ticks is the scoped gate. Two or
more run the tick block back to back before rendering once. The joint and
camera lerps take `alpha` instead of 0.5.

| Mode | Result |
| --- | --- |
| 10, accumulator off, 30 Hz pacing | Identical to the 30 Hz reference in every logged column: poll, tick, countdown, position, yaw, GE commands, vertices. |
| 13, synthetic clock, exactly half a tick per frame | Identical to probe6 mode 9 in every column but the cumulative GE count, which carries a constant 34-command offset from the activation frame; the six captured frames are pixel-identical to mode 9's. |
| 12, real clock, 55 ms stalls after polls 2130 and 2160 | `ticks_due = 2` on the frame after each stall, the tick jumps by two there, and 62 ticks land in the 2.11 s window: 29.4 per second. |
| 11, real clock, one-interval pacing | 61 ticks in 120 frames. Frame times under the null renderer jitter between 13.7 and 26.1 ms, so ticks land on uneven polls and alpha varies. |

Zero bad memory accesses in all four. Mode 13 is the end-to-end proof: the
loop rewrite, the accumulator and the alpha-driven lerps together reproduce
the validated parity probe exactly when the clock is regular.

Mode 11 shows the one design choice left open. With `OFFSET = 0.5` the
render sits half a tick past the previous state, which is the half-step
scheme and its latency, but whenever a frame arrives more than half a tick
after the last tick the alpha clamps at 1 and that frame repeats the current
pose. `OFFSET = 0` never clamps and steps evenly for any frame timing, at the
cost of one tick of latency. On a display with steady vsync the difference
is small; under the jitter measured here it is visible in the per-frame
pixel counts.

Not handled yet by the native loop: catch-up ticks reuse the frame's pad
sample; the pause abort in gate 1 (`000FF4C4`) jumps to the loop end and
drops any ticks still owed; the other loops (garage, results, cutscenes) are
untouched. Pacing moves to the host in section 13.

## 13. Host pacing

This runtime's `sceDisplayWaitVblank` returns immediately, so the game paces
itself in `00259250`: a spin on the emulated scanline counter that yields
through `sceKernelDelayThread` until the pacer's vcount target passes. That
is what held the loop at 30 or 60 Hz in every run above. `probe8.c` skips
`00259250` while the accumulator is on (its state is read only by itself and
the present statistics) and, in the frame-buffer-flip wrap, sleeps to a host
deadline of `1/PROBE_HOST_HZ` on `CLOCK_MONOTONIC`; a late frame resets the
deadline instead of trying to catch up. `PROBE_OFFSET` sets the render
offset; these runs use 0, the evenly stepping choice.

| Host target | Achieved | Frame time, mean ± sd (min) | Ticks per second | Frames with 2+ ticks |
| --- | ---: | ---: | ---: | ---: |
| 60 Hz | 60.5 fps | 16.66 ± 2.14 ms (12.8) | 29.5 | 0 |
| 120 Hz | 96.7 fps | 10.43 ± 1.91 ms (9.0) | 29.3 | 0 |
| 144 Hz | 98.4 fps | 10.25 ± 1.06 ms (9.2) | 29.8 | 0 |
| uncapped | 99.6 fps | 10.13 ± 1.08 ms (9.0) | 29.3 | 0 |
| uncapped, lerps off | 109.4 fps | 9.22 ms (8.3) | 29.4 | 0 |

Zero bad memory accesses; the countdown decrements once per tick in every
run. At 144 Hz and uncapped the logged alphas step by about 0.31 of a tick
per frame — 0.10 0.45 0.74 0.01 0.36 0.64 0.93 0.21 — which is the
frame-rate-independent rendering the accumulator was built for: the display
runs at whatever rate the host allows and the simulation stays at 30 Hz.
The 120 Hz row's alphas are disturbed by the display-list captures at polls
2118–2123, which make those frames slow; the 144 Hz and uncapped rows have no
captures. Host pacing at 60 Hz also halves the frame-time jitter the game's
own pacer produced under the null renderer (13.7–26.1 ms in section 12).

The ceiling is the CPU: a display-only frame costs about 9.2 ms of the
recompiled game's own tail in this `-O0` build, and the interpolation adds
about 0.9 ms (eight `00052D38` rebuilds, two camera rebuilds, 48 matrix
lerps). That is with no GL work at all; the report's 1080p GL run reached 56
fps with a full tick in every frame. Reaching 120 Hz therefore needs the
optimised build of the generated code before anything else, which is the
roadmap's Phase 1 concern rather than a pacing problem.

Production shape: the limiter or a swap-interval choice belongs in
`host/present.c`, the pacer skip in `host/replacements.c` next to the loop,
both gated on the same flag as the accumulator. While the pacer is skipped
its statistics freeze, and the emulated vblank counter still advances for
anything else that reads it.

## 14. The optimised build, and what the frame is actually spent on

The emitted module is one 2.06M-line file: a five-line prelude, 15,850
self-contained function blocks (static bodies, public `psp_func_*` and
`psp_at_*` entries, no file-static data), and a 59k-line registration
function that names every `psp_at_*` thunk. `reports/game-tick-review/opt/split.py`
cuts it at block boundaries into 32 chunks that share the prelude, and puts
the registration function in its own file with a generated header of thunk
declarations. `opt/build-opt.sh` compiles the chunks at
`-O2 -fno-strict-aliasing -fwrapv` twelve at a time and the registration
table at `-O0` (one giant function; `-O2` spent 400 s on it before failing on
the declarations). Wall time is about 30 s against 45 s for the `-O0`
monolith; the objects total 49 MB against 70 MB. Nothing under `build/` or
`scripts/` is touched.

Exactness first: the native loop with the accumulator off, linked against
the `-O2` objects, matches the 30 Hz reference in every logged column —
poll, tick, countdown, position, yaw, GE commands, vertices — with zero bad
memory accesses. Through the real scripts (`OPT=1 scripts/04-emit-build.sh`,
then `OPT=1 scripts/06-boot.sh`, objects in `build/host-opt/`), the
optimised boot and the normal one produce a byte-identical final frame and
input log on the report's `walk.pad`, with the same GE totals; the whole
fast-clock replay takes 9.2 s against 10.8 s.

The frame, however, barely moves:

| Uncapped, null renderer | `-O0` game code | `-O2` game code |
| --- | ---: | ---: |
| With interpolation | 99.6 fps, 10.13 ms | 112.7 fps, 8.94 ms |
| Interpolation off | 109.4 fps, 9.22 ms | 121.7 fps, 8.28 ms |

So the recompiled game is not where the frame goes. `probe9.c` times the
loop's sections and the GE and display imports, per display-only frame:

| Section | µs |
| --- | ---: |
| Frame top (input, housekeeping) | 18 |
| Tick block, when a tick runs | 1,380 |
| Layer dispatcher `0022DCAC` to end of scene | 542 |
| End of scene to the tail stop query (world list emission, sound) | 2,033 |
| Tail query to the frame-buffer flip | 261 |
| The flip itself | 306 |
| After the flip to the next frame top | 5,639 |
| of which `sceGeListUpdateStallAddr` from `002B8394` | ~5,000 (808 µs per call) |

The runtime executes the display list synchronously inside every stall
update (`run_list` in `hle_ListUpdateStallAddr`), and the null backend only
discards the *output*: transform, lighting of 41k vertices, fog, clipping and
backface culling all run in `ge.c` regardless of backend, about 120 ns per
vertex. `libpsprecomp` is already built `-O3`. The `sceKernelDelayThread`
totals in the same profile are other guest threads' sleeps and not a cost.

Where that leaves 120 Hz: the 8.3 ms budget holds about 5 ms of GE
pipeline, 2 ms of game tail, 0.6 ms of present and 0.9 ms of interpolation
before any GL work, and the report measured GL draw-plus-blit at 1.7 ms
average on top. Reaching it means either making the runtime's vertex
pipeline cheaper or moving the transform onto the GPU in the GL backend,
which currently receives already-transformed screen-space vertices. Both are
renderer work. The optimised game build is still worth adopting for the
1.2 ms per frame and the faster loading, and it is cheap. It is now the
`OPT=1` path of `scripts/04-emit-build.sh` (split by `scripts/emit-split.py`,
parallel `-O2`, `ld -r` back into one `aclr_funcs.o` in `build/host-opt/`),
with `scripts/06-boot.sh` and `scripts/09-replay.sh` taking the same `OPT=1`
to link and run from that directory. The normal `-O0` build is untouched.

## 15. Moving the transform onto the GPU: an assessment

`PSPRECOMP_GE_PROFILE=1` is now an env-gated instrument in `ge.c`: six
cycle-counter marks per vertex and one per batch, reported at the end of the
run. Over the whole walk replay, uncapped, on the optimised build:

| Phase of the transform pipeline | Share | Per vertex |
| --- | ---: | ---: |
| Position decode (three hooked 8/16-bit reads) | 8.4% | ~9 ns |
| Model, view, projection products | 7.4% | ~8 ns |
| Colour read, fog, normal read, normal products, lighting | 35.5% | ~39 ns |
| Texture coordinates and texgen | 9.2% | ~10 ns |
| Screen space, 12.4 snap, bounding box | 7.7% | ~8 ns |
| Per-batch near clip, guard band, cull, assembly, backend draw | 31.8% | ~35 ns |

About 110 ns a vertex once the marks' own ~17 ns are taken out, 42k
vertices a mission frame, which is the ~5 ms section 14 attributed to the
stall updates. The scene has **no lights enabled**, yet the lighting bucket
is the largest: it pays three hooked reads for the normal, two 3x3 products,
a square root and three divisions to normalise it, and a loop over four
disabled lights, for a colour that does not depend on the normal at all.
The null backend's draw is a no-op, so the last row is clipping, culling and
the per-triangle copies of `psp_vertex` into clip vertices.

**What a GPU path would take.** The roadmap already names it: a second
backend input of untransformed vertices plus the matrices, alongside the
present screen-space `draw`, decided once the 1x backend is exact. The GL
vertex shader today deliberately receives post-divide screen space with
`w = 1` so the GE's coverage and depth decisions stand. A raw path would
move the products, fog, lighting, texgen and the near clip into shaders and
would have to reproduce, in the vertex stage, the 1/16-pixel snap, the
PSP's guard-band discard (GL clips where the PSP drops), its near-plane
behaviour, and the four-light fixed-function model the `gpu/` tests pinned;
the software backend would keep the exact path as the reference and a
compare mode would measure the deviation. Decode stays on the CPU unless
the shader reads the PSP vertex formats itself. By the shares above, that
moves roughly 85% of the pipeline off the CPU: from ~8.9 ms a display-only
frame to ~5 ms before GL's own ~1.7 ms of draw and blit, so on the order of
140 fps rather than 100. It is renderer work of M5's size, and the
fidelity questions are the same ones M5 is settling for the 1x backend.

**What is cheaper and exact.** Three CPU-side changes take a large part of
the same time with no fidelity question:

- Decode each vertex through one `psp_mem_ptr` span instead of a hooked
  read per component: the position, normal, colour and UV reads are eight
  to eleven accessor calls a vertex, each with its own translation and
  watch check.
- Skip the normal read, its products and the normalisation when no light
  is enabled, since the output is then emissive plus ambient only. In this
  scene that is the whole 35% bucket less the colour read and fog.
- Assemble triangles without copying whole `psp_vertex` records into clip
  vertices, or clip only when a batch's bounding box actually crosses a
  plane.

Those three are now in `ge.c` (section 16). The GPU transform is the step
after, and its value is then what remains of the pipeline plus what it does
for internal resolution.

## 16. The three exact GE changes, measured

- `read_pos_model_at`, `read_normal_model_at`, `read_uv_model_at` and the
  colour read take one `psp_mem_ptr` span per vertex record; a record the
  span cannot cover falls back to the hooked reads, which keep reporting the
  bad access.
- `light_vertex` normalises the normal and runs the light loop only when a
  light is enabled, and the vertex loop skips the normal read and its two
  products in the same case; the colour is emissive plus ambient either way.
- `emit_tri_indexed` draws a triangle from the vertex batch's own
  screen-space values when `emit_tri` would have passed it through
  unclipped, on the clipper's own tests; everything else still goes through
  `emit_tri`.

| Uncapped, null renderer, optimised game code | Before | After |
| --- | ---: | ---: |
| Transform pipeline, whole replay (with the profile's own marks) | 3,764 ms, 127 ns/vertex | 2,573 ms, 87 ns/vertex |
| of which clip, cull, assembly, draw | 1,197 ms | 338 ms |
| of which colour, fog, normal, lighting | 1,334 ms | 1,146 ms |
| Stall updates per display-only frame | 5.0 ms | 3.4 ms |
| Frame time, interpolation on | 8.94 ms, 112.7 fps | 7.16 ms, 140.9 fps |

Exactness, software backend, `build/host-opt/boot` before and after:
586 frames dumped every 10th (walk) and 20th (mission 1) presented frame
are byte-identical; both input logs are identical; the run summaries differ
only in raster time; `scripts/12-render-tests.sh` passes 430 checks on the
software backend and 430 on GL both times. Zero bad memory accesses.

The lighting bucket kept most of its cost because the run's menus and
garage do light their vertices, and the mission's lit-but-lightless
vertices still pay the colour conversion; the two decodes and the products
are each about 10%. The remaining ~3.4 ms of pipeline per frame is what a
GPU path would now be buying.

## 17. The transform on the GPU

The backend interface gains an optional pair, `model_ok()` and
`draw_model(prim, model vertices, count, xform state)`. When a backend offers
them, `draw_prim_transformed` decodes the batch into `psp_model_vertex`
(model-space position and normal, the colour or the material colour, texels)
and hands it over with `psp_xform_state`: the four matrices, the lights in
eye space, material, fog, viewport, cull, depth clamp and texgen, everything
its own pipeline would have applied. Triangles only, and never for a
screen-space projection, whose flag the GL backend uses to tell HUD from
scene; points, lines, sprites and the whole 2D layer keep the CPU path, as
does any backend that does not offer the pair (software, null).

The GL backend implements it as a second program whose fragment shader is
the existing one. The vertex stage is `draw_prim_transformed`'s per-vertex
work transcribed: the three products term by term, fog, `light_vertex`,
texgen. The geometry stage is `emit_tri`: the near test or the near clip
with its attribute rule (coordinates and texels move, the rest come from the
first vertex), the guard band, the cull by the sign of the 1/16-snapped
integer area, and the per-triangle texture LOD the batched path computes in
`push_triangle`; it emits exactly what `push()` would have put in the batch,
so the fragment stage's PSP edge rules are untouched. Strips and fans arrive
as triangle lists through static index tables so the geometry stage sees the
PSP's vertex order and takes the winding flip from the primitive's parity.
`precise` and the explicit expression order keep the float arithmetic in C's
order; the exact 64-bit area needs a `double`, so the program is GLSL 4.0 and
`present.c` now asks for a 4.0 core context first, falling back to 3.3, in
which case the program stays unbuilt and `model_ok()` answers 0.

The first version issued one fully-stated draw per display-list primitive,
683 a frame, and ran *slower* than the CPU transform at 1080p (42 fps
against 70). Three changes fixed that: the transform state travels in a
std140 uniform block written once per draw into a ring of aligned slots;
vertices go into a ring buffer with base-vertex draws so no upload waits on a
draw still reading the last one; and state is re-applied only when a setter
or the target changed since the previous model draw.

| Capture | Software vs GL, CPU transform | Software vs GL, GPU transform | GL CPU vs GL GPU |
| --- | ---: | ---: | ---: |
| poll 300 (title, all screen-space) | 203 px | 203 px | 0 |
| poll 1400 | 50.6 px | 50.7 px | 0.1 |
| poll 2120 (mission, 683 model draws) | 52.2 px | 52.2 px | 0 |

The GPU path reproduces the CPU-transform GL rendering pixel for pixel on
these frames, and both stand at the same small distance from the software
reference that the GL backend already had. The render tests pass 430 checks
on each backend. Then, at 1920x1080 window resolution, uncapped, the
optimised game code, interpolation on:

| GL backend at 1080p | Transform on the CPU | Transform on the GPU |
| --- | ---: | ---: |
| Frame rate | 70.0 fps | 78.1 fps |
| Frame time, mean (min) | 14.40 ms (11.63) | 12.91 ms (7.02) |
| Stall updates per frame | 8.1 ms | 6.2 ms |
| Vertices through the CPU pipeline per run | 29.7 M | 0.9 M |

The CPU pipeline is now a few per cent of what it was; what remains in the
frame is the GL driver's per-draw cost, about 9 µs for each of the ~680
draws, and the 1080p blit. The next step there is batching consecutive
model draws that share GL state into one multi-draw with a per-draw block
index, which the block-on-a-ring layout already suits. Two fidelity notes:
`log2` and `pow` on the GPU are not glibc's, so a mip level or a specular
byte can differ at exact boundaries, and none of the three frames showed it;
and `PSPRECOMP_GL_TRANSFORM=cpu` keeps the old path for comparison.

## 18. Batching the model draws, and what the GE does before every primitive

The first multi-draw attempt merged nothing: 683 draws became 646 batches.
The reason is in `ge.c`'s `push_pixel_state`, which pushes clut, texture,
blend, depth and fog to the backend before *every* primitive whether or not
they changed, and every GL setter flushed and bumped the state generation.
Neither path could ever batch across a primitive.

The setters now skip a call that sets what is already set. Blend, depth,
fog, scissor and clut are pure GL state and compare their arguments. The
texture setter is the one with a memory dependence: the cache validates its
entries against the guest-memory write serial and re-uploads in place when
the contents changed, which the pending batch cannot see, so an identical
texture call is skipped only while `psp_mem_write_serial()` still equals the
value at the bind and no clut parameter changed. About 3,000 setter calls a
frame are skipped that way, and the CPU-transform path's own output is
unchanged pixel for pixel, since a skipped flush only lets more triangles
share a draw.

With that, consecutive model draws under one GL state accumulate into a
multi-draw: each vertex carries its draw's index into an array of sixteen
transform blocks in one uniform slot, the triangles of strips, fans and
lists go into an index ring in the PSP's vertex order with a base vertex per
draw, and `glMultiDrawElementsBaseVertex` issues the batch. Primitive IDs
restart per sub-draw, so the geometry stage still takes the strip's winding
flip from parity. The mission frame's 683 draws become 376 batches, 1.8 per
batch; the display list changes texture about 330 times a frame, which is
the ceiling.

| GL backend, uncapped, optimised game code, interpolation on | Transform on the CPU | Transform on the GPU |
| --- | ---: | ---: |
| 1920x1080: frame rate, frame time mean (min) | 71.4 fps, 14.12 ms (11.20) | 80.2 fps, 12.57 ms (7.13) |
| 1920x1080: GPU draw+blit per frame, mean | 1.21 ms | 2.92 ms |
| 480x272 native: frame rate, frame time mean (min) | 88.0 fps, 11.46 ms (10.16) | 138.5 fps, 7.28 ms (6.19) |
| 480x272 native: GPU draw+blit per frame, mean | 0.56 ms | 2.19 ms |

Still pixel-identical to the CPU-transform renders on the three captures,
render tests 430/430 on each backend, zero bad memory accesses. The model
path's own CPU time is now negligible, about 0.3 s over a 50 s run.

Two things the numbers say. At native resolution the GPU transform reaches
138 fps: the CPU side is no longer the limit, and 120 Hz is reachable on the
GL backend. At 1080p both paths are bound by fill and the blit, and the
geometry stage costs about 1.6 ms of GPU time a frame at either resolution:
the exact `emit_tri` port with its doubles and six-vertex output is not free
on the GPU. The next steps there are on the GPU side, not the CPU: a cheaper
geometry stage (the exact area in split 32-bit integers instead of doubles,
fewer varyings), or a non-exact mode that leaves clipping and culling to GL
for displays where 1080p at 120 Hz matters more than PSP-exact edges.

## 19. The geometry stage, made cheaper, and measured from three sides

Three changes, all keeping the output exact (three captures identical to
the CPU-transform renders, render tests 430/430 on each backend):

- The cull sign is taken in 64-bit integers, as `ge.c` takes it, from the
  two products in split 32-bit form (`imulExtended`) compared as signed
  64-bit values. No doubles.
- The stage emits one four-vertex strip instead of up to six vertices in
  separate primitives: a clipped quad's fan triangles (0,1,2) and (0,2,3)
  are the strip 1,2,0,3, and each triangle's provoking vertex carries its
  LOD. GL never culls on this path, so the strip's alternating winding is
  irrelevant.
- The per-triangle LOD derivatives are skipped when the fragment stage
  cannot depend on the value: filters of the same kind and either no mip
  filter or a single level, which the host puts in the block.

| GL, native 480x272, uncapped | Frame rate | GPU draw+blit per frame |
| --- | ---: | ---: |
| Exact geometry stage, before | 138.5 fps | 2.19 ms |
| Exact geometry stage, after | 138.6 fps | 2.21 ms |
| Pass-through geometry stage (experiment, not exact) | 138.3 fps | 2.02 ms |
| No geometry stage, vertex shader maps to screen (experiment, not exact) | 145.9 fps | 1.72 ms |
| CPU transform, for reference | 88.0 fps | 0.56 ms |

At 1080p the exact stage is unchanged too: 79.9 fps, 2.91 ms. So on this
GPU, an RX 6900 XT, the stage's logic costs about 0.2 ms, its presence
about 0.5 ms, and the other 1.2 ms of the model path's GPU time is in the
vertex and buffer path: every vertex is shaded before culling where the CPU
path uploads only survivors, the rings take a `glBufferSubData` per draw
and a uniform-range bind per batch, and the vertex stage does the lighting
under `precise`. The two experiments are gated on nothing and not kept; the
three exact changes are, because they remove the doubles and halve the
stage's output for free.

If more is wanted from the GPU side, in order of expected return:
persistent-mapped rings written once per frame instead of a `SubData` per
draw; one uniform upload and bind per frame with the block index covering
the frame's draws; and, to recover the stage's 0.5 ms, a geometry-stage-free
exact path that draws each triangle as an instance fetching its three
vertices from a buffer texture, which is a larger rewrite.

## Reproduction

```sh
bash reports/game-tick-review/snapshot.sh        # RAM snapshots, report modes 5 and 6
bash reports/game-tick-review/capture.sh         # GE captures + software replays
python3 reports/game-tick-review/snapdiff.py A.mod B.mod --list 40
reports/game-tick-review/asm.sh 000FF4C4         # disassembly-only view of a function
bash reports/game-tick-review/build-probe2.sh    # scoped gate, modes 0-3 (private re-emit)
bash reports/game-tick-review/run-probe2.sh
bash reports/game-tick-review/build-probe4.sh    # tail bracket, modes 4-6
bash reports/game-tick-review/run-probe4.sh && PREFIX=q python3 reports/game-tick-review/analyze2.py 4 5 6
bash reports/game-tick-review/build-probe3.sh    # checksum attribution of the list writes
bash reports/game-tick-review/build-probe5.sh    # camera, modes 7-8
bash reports/game-tick-review/run-probe5.sh && PREFIX=r python3 reports/game-tick-review/analyze2.py 7 8
bash reports/game-tick-review/build-probe6.sh    # joints, mode 9
bash reports/game-tick-review/run-probe6.sh && PREFIX=s python3 reports/game-tick-review/analyze2.py 9
bash reports/game-tick-review/build-probe7.sh    # native loop, modes 10-13
bash reports/game-tick-review/run-probe7.sh      # 10 (exactness), 11 (60 Hz), 12 (stalls); run-probe7b.sh for 13
bash reports/game-tick-review/build-probe8.sh    # host pacing, mode 14; PROBE_HOST_HZ and PROBE_OFFSET
bash reports/game-tick-review/run-probe8.sh      # 60, 120, 144 Hz and uncapped
python3 reports/game-tick-review/opt/split.py reports/game-tick-review/gen8/aclr_funcs.c reports/game-tick-review/opt/gen 32
bash reports/game-tick-review/opt/build-opt.sh   # -O2 chunks, boot8opt-10 (exactness) and boot8opt-14 (pacing)
bash reports/game-tick-review/opt/run-opt.sh     # exactness + 60/120/144/uncapped at -O2
bash reports/game-tick-review/opt/link-probe9.sh # section profiler, boot9opt-14
PSPRECOMP_GE_PROFILE=1 <any boot> ...            # per-vertex pipeline breakdown at the end of the run
build/host-opt/gereplay frame.gcap gl out.ppm    # GL with the GPU transform; PSPRECOMP_GL_TRANSFORM=cpu for the old path
OPT=1 scripts/04-emit-build.sh && OPT=1 BOOT_NO_RUN=1 scripts/06-boot.sh   # the optimised build, build/host-opt/
```

The emitted trees and captures are deleted after use; the scripts regenerate
them in about a minute each.
