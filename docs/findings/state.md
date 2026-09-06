# Where this is, and how to find things out

Written at a session boundary. The commits carry the reasoning for individual
changes; this is the part that lives between them — what the game currently
does, which instrument answers which question, and what has already been ruled
out so it is not investigated twice.

## Every measurement here names its configuration

There are two, and they are not variants of one run — they end differently, read
different amounts of disc, and leave different threads alive. An unlabelled
number in this file was a bug, and it is what made an earlier version of this
document contradict itself:

- **default** — `PSPRECOMP_MPEG_DECODE` unset. sceMpeg refuses playback.
- **decoder** — `PSPRECOMP_MPEG_DECODE=1`. Demuxer and openh264 video path on.
- **pad-driven** — the decoder configuration plus a scenario from `scenarios/`,
  replayed by `scripts/09-replay.sh --decode`. The only configuration that
  gets past the intro: without the decoder the game parks at the movie gate
  and never polls the pad again, so a scenario run without `--decode` measures
  the default run with a script attached — 211 polls, 1 of 3 events delivered,
  639 lists, checked 1 Sep.

The switch landed in `4462226`. Every doc commit after it recorded numbers
without saying which side of it they came from, so by the time anyone read them
there was no way to tell. **Label the configuration or do not write the number.**

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, and the frame loop runs. Both configurations stall at the same gate
by different routes.

### default — the failure path

> **The ending changed, and the picture did not.** This section described a run
> that `wait_deadlock` force-stopped. Since the threads work it reaches the
> 60-second drain with threads alive instead, and everything else is
> bit-identical — 633 lists, 106,108 commands, 93,354,668 pixels, 0 bad
> accesses, 1,912,832 bytes read. The reason is that the audio thread now
> *sleeps* rather than spinning (see the item below), so no wait is provably
> unsatisfiable any more. The force-stop was a true report and its absence is
> also a true report; the numbers below stand, the `threads:` line does not.
>
> The pixel count in the block below is older still and predates the renderer
> work; the run reports 93,875,406 today.
>
> **The GE figures below moved once more, and upward.** Registering the savedata
> dialog (src/hle/utility.c) took the run to **639 lists, 106,762 commands, 214
> finishes, 93,875,406 pixels**, still 0 bad accesses. The game polls a savedata
> dialog during startup; unimplemented, its InitStart returned zero, which reads
> as "started", so the poll never ended. It now gets a documented lifecycle and
> stops waiting. The frame comparisons are unchanged to the pixel -- 0, 751 and
> 9,020 of 130,560 -- so this is the same picture reached with less spinning,
> not a different one.

`sceMpegGetAvcAu` refuses and the game prints its own diagnosis:

```
Fatal Error!!! : sceMpegGetAvcAu() is failed...ret=806101FE
```

```
entry:     returned
threads:   stopped by the host (sceKernelWaitSema)
bad mem:   0 accesses
disc read: 1,912,832 bytes
pixels:    73,854,102 drawn by the rasterizer (2,086,960 textured, rest flat)
GE: 633 lists, 106,108 commands, 212 finishes
    texture 0x09170080 512x64 stride 352, clut8, modulate, swizzled (153 clut loads)
    drawn   868 prims, 2,958 vertices, 611 triangle-strips, 257 sprites
    depth: test on func 7, write on          clear-mode draws: 211 (211 clearing depth)
    blend on (src 2 dst 3 eq 0), alpha test on func 7 ref 1:
        44,737,298 blended, 826,728 alpha-killed
    transformed 2,444 vertices; viewport scale 240,-136 centre 2048,2048 offset 1808,1912
    draws: 2d 0 textured / 257 flat, 3d 152 textured / 459 flat
```

The 2D sprites drawing untextured is correct: their vertex type carries no
texture coordinates at all.

`sceDisplaySetFrameBuf` is called **422** times and the run ends in seconds. The
sequence into the stop, from the HLE log:

- `0x40024` (MovieDecodeThread) signals **Movie Start** (`0x0004001E`) exactly
  once on its way out;
- the movie controller `0x40001` consumes it, runs a frame, and waits on Movie
  Start a **second** time — `sceKernelWaitSemaCB`, no timeout;
- nothing can ever signal it again: both movie threads have exited, and the
  frame consumer `0x40021` is parked on `Movie Sync`, which receives **zero**
  signals in the whole run;
- `wait_deadlock` stops the run and names the call.

### decoder — real frames, same gate

The movie machinery runs for real: the ring buffer fills through the game's own
callback (the first put carries `00 00 01 BA`, a program-stream start code), the
demuxer and openh264 produce real pictures, and `sceMpegAvcDecode` writes them
into the buffer the game passes — checked against ffmpeg at the byte level; the
header comment in `mpeg.c` has the details.

```
mpeg: GetAvcAu -> frame 1, pts 3600      AvcDecode frame 1 -> ready=1
mpeg: GetAvcAu -> frame 2, pts 7200      AvcDecode frame 2 -> ready=1
mpeg: GetAvcAu -> frame 3, pts 10800     AvcDecode frame 3 -> ready=1
```

Then it stalls. The run reaches the 60-second drain with `threads: still alive`,
having read 3,289,088 bytes, drawn 33,292,801 pixels across 630 GE lists, and
issued **562,236,014** `sceMpegRingbufferAvailableSize` and **281,122,074**
`sceKernelSignalSema` calls — the whole stream demuxed, almost none of it
consumed, and Movie Start signalled millions of times while `AvailableSize`
reports nothing free.

**`Movie Sync` receives zero signals in this configuration too.** An earlier
summary claimed both never-signalled semaphores now fire; only Movie Start does.

### pad-driven — the title menu renders, and New Game faults

Measured 1 Sep on `3f402c5`, decoder on, three scenarios. Every run is
deterministic (`clock virtual`) and ends at its own `stop` or at the first bad
access, so the numbers compare between runs of the same build.

| | `title-idle.pad` | `skip-intro.pad` | `new-game.pad --stop 1` |
|---|---|---|---|
| ends | `stop` at poll 1800, t=48.3s | `stop` at poll 2500, t=40.2s | **bad access #1** at poll 2567 |
| bad mem | **0** | **0** | 1, stopped there |
| GE lists / commands | 5,406 / 874,060 | 7,506 / 2,280,256 | 7,707 / 2,356,535 |
| pixels | 716,878,913 | 1,499,561,979 | 1,543,165,451 |
| 3d draws textured / flat | 12,864 / 2,064 | 41,753 / 4,983 | 43,073 / 5,131 |
| formats | 5650 8888 clut8, modulate, cull off | same, 40,954 CLUT loads, cull ccw | same |
| disc read | 27,060,224 (the movie) | 5,310,464 | 5,588,992 |
| events delivered | 3/3 | 50/50 | 51/70 |
| unimplemented calls | 11 | 17 | 19 |

**The title menu renders, headless.** `skip-intro`'s displayed frame is the
ARMORED CORE LR / LAST RAVEN Portable logo over the mech artwork, the
CONVERT / NEW GAME / LOAD GAME menu and the copyright line — 127,050 of
130,560 pixels differing from the corner, legible. That is real 3D: culling
on, 41,753 textured draws, 8888 textures alongside the clut8 ones.

**New Game faults exactly as recorded on 30 Aug.** The register dump at the
first bad access matches that session to the register:

```
read32 at 0x461CC570
a0=0x461CC578 a1=0x00000046 a2=0x4612ED37 a3=0x461CC570
s0=0x4612ED37 s1=0x4680C36A sp=0x09FB9980 ra=0x0002E2F8
vfpu cc at the fault: 0x0C
```

One distinct call site, `ra=0x0002E2F8` — `psp_func_0002E1D8`'s call into the
vertex decoder `psp_func_0002E790` — 67 polls after the cross on NEW GAME.
`title-idle` goes further in polls and touches nothing: it is the press, not
the elapsed time.

**What the game was told on the way, in order.** stderr keeps the ordering;
the 16-deep zero ring captured at the fault holds only semaphore and
interrupt noise, which is the ring's depth showing, not an absence.

- boot: `sceImposeSetLanguageMode`, `sceUtilityLoadModule` ×6, the unnamed
  scePower NID `0xEBD177D6` (45 candidate names hashed, none match),
  `__sceSasGetOutputmode` ×3 — every one answered 0 with nothing written;
- after the intro is skipped: `sceUtilityUnloadModule`, then **the title BGM
  is started** — `sceAtracGetAtracID`, `SetData`, `GetRemainFrame`,
  `SetLoopNum`, `GetStreamDataInfo`, each once, each answered 0 with **none of
  its out-parameters written**;
- cross on NEW GAME: `0xEBD177D6` again, `sceAtracReleaseAtracID`, and the
  fault.

The ATRAC calls bracket the title screen and the release lands right before
the fault. That is why "the 13 sceAtrac NIDs fail honestly" is first in
[../ROADMAP.md](../ROADMAP.md) M1 — not because it is proven to be the cause,
but because it is the cheapest lie on the path and it sits exactly there.

**Read the best-frame heuristic with care here.** `frame(best)` on every
decoder run picks the same washed intro shot (peak 218, mean 195), which
outscores the menu on variation. For a run that ends at `stop`, the
*displayed* frame is the one to look at.

Logs: `reports/09-<scenario>-<stamp>.{txt,err}`, frames `reports/m0-*.ppm`,
both gitignored. Reproduce with the commands in the regression table.

**By the end of the day NEW GAME runs through.** The fault was a codegen bug
— the emitter and interpreter never translated the VFPU condition branches,
so the game's polygon clipper doubled its output at every plane and ran over
its caller's frame — reachable only after three audio lies (an import that
wrote nothing, a refusal the game could not survive, a SAS voice that never
ended) were removed from in front of it. `new-game.pad` now reaches its
`stop` at poll 6000 with 0 bad accesses and the game's initial sound-settings
panel on screen. The whole chain, with the numbers, is
[autotests.md](autotests.md) item 27. Title screen and headless bar unchanged
throughout. Two things for M2 seen in that run: smeared glyphs (affine
texturing) and non-finite texture coordinates in the GE summary.

### The picture

The FromSoftware logo renders, cleanly. See open item 0 for how, and for the
three instruments that had to be corrected before it could be seen at all.

### The renderer *was* the blocker, and this document said it was not

Until `31473af` the GE accepted **eight display lists for the life of the
process** and refused every one after that. `enqueue` took a slot from a pool of
eight and set `used = 1`; that was the only write to `used` in the file, and
neither `ListSync` nor `DrawSync` released anything.

Measured on the intro, default: 212 enqueues attempted, 8 accepted, **204
refused** with `SCE_KERNEL_ERROR_NO_MEMORY`. Fixing it moved the numbers a long
way:

```
                 before      after
GE lists             21        633
commands          1,900    106,108
prims                19        257
vertices             62      2,958
pixels        2,350,081 33,423,361
```

An earlier version of this file concluded from the same evidence that "the
renderer is not the blocker" and that "whatever would produce geometry never
runs". The geometry-producing code ran the whole time and was turned away at the
door. **Nothing pointed at it could see the refusal**: the GE summary counts
lists that *ran*, the firmware histogram is a top-12, and a non-zero error
escapes the zero-return ring. It was found by grepping an `HLE_LOG` capture for
every return of the form `= 0x8…`, which is now the thing to do when something
inexplicable is happening.

### A yield cannot give way to a lower priority

`handoff_locked` picks the most urgent READY thread, and `psp_sched_yield`
leaves the caller READY — so a yield from the top priority hands the token
straight back to itself. A game whose main loop is `render(); delay();` at
priority 16 therefore starves its own priority-17 and -18 workers forever, and
they show up in the thread list as **ready, not blocked**: not waiting for
anything, simply never chosen.

That is what `sceKernelDelayThread` had been doing. Standing aside for a single
round was not enough either: this game runs *two* threads at priority 16, so
when one gave way the handoff picked the other and they passed the CPU back and
forth while 17 and 18 still starved. `psp_sched_delay` therefore sleeps until a
deadline. The duration is honoured; only its relation to wall time is not.

**Ready-but-never-running is the signature.** A thread parked on something names
what it waits for; a starved one names nothing, which reads like an idle thread
rather than a stuck one.

### How a run ends, and how to tell the endings apart

Three endings, three signatures. Reading one as another has cost sessions.

- **Deadline.** `psp_sched_drain` gives up after 60 seconds — headless.
  `PSPRECOMP_DRAIN=<seconds>` widens it, `PSPRECOMP_DRAIN=0` removes it, and
  **a window removes it by default**: a person is driving, closing the window
  already ends the run cleanly, and a wall-clock cut lands exactly where the
  interesting part was starting. Headless keeps its limit deliberately — an
  automated run with no deadline is a hung machine nobody is watching. Prints
  `still running after 60s` with the live list. Not a deadlock — a run still
  going round. **The decoder run ends here.** Slots saying `running` are what the
  timeout path does: a guest thread cannot be unwound from outside, so the main
  context stops waiting for it and takes the token back, leaving the runaway
  genuinely still running. `sched.h` documents it.
- **Force-stop.** `wait_deadlock` finds a no-timeout wait that nothing can ever
  satisfy, prints the live list itself, and marks every thread dead. **The
  default run ends here.** The summary names the call since patch 0021:
  `stopped by the host (sceKernelWaitSema)`.
- **Deadlock.** `deadlock -- N thread(s) alive, none runnable`. This game has
  never printed it, in either configuration. Check for that string before
  concluding anything about the scheduler.

`threads: all finished` is reserved for a run where every thread ended on its
own. Before patch 0021 it was printed for force-stops too, because
`psp_sched_stop_all` marks every thread dead and the drain then counts zero —
indistinguishable from success. See *Six ways to measure nothing*.

### Controls — measured, read, and seven functions replaced (4–5 Sep)

The scheme as shipped: left stick Y is forward/back, left stick X is **turn**,
L/R strafe, triangle/circle look up/down. The complaint was that turning and
moving feel like separate motions. Every number below is pad-driven, decoder
on, headless, on `scenarios/mission-1.pad`'s prefix into the first mission;
gameplay begins at about poll 2050 (the HUD and the 03:00 timer appear), and
in play polls are 1:1 with frames at ~36 a second.

**What the stick does, as shipped.** The GE view matrix is useless as a camera
observable — this game uploads it ~155,000 times a mission and it is the same
`diag(1,-1,-1)` every time; the camera is composed on the CPU into ~240 world
matrices a frame. So the instrument is the world-matrix log
(`PSPRECOMP_VIEW_LOG`, `_WORLD=all`, `_POLLS`) read by
`scripts/view-analyze.py --deltas`, which clusters every object's yaw change
between consecutive frames: static scenery is the biggest cluster and its
delta is the camera's. Full stick: ~4 frames of latency, then the AC's yaw
rate ramps **linearly at 0.4204°/frame² for 5 frames to a cap of
2.1019°/frame** (0.036685 rad — a full turn in ~4.9 s) and decays the same way
on release; the chase camera smooths that with a ×0.83-per-frame filter, which
is the geometric tail one sees. **Deflections 104 and above turn at the full
rate; 100 and below do nothing at all** — the stick is two-state. (A first
reading of "camera frozen, object count 321" as the AC having been destroyed
was wrong: the frames — `reports/thr-contact.png` — show it alive with the
timer running. Check frames before inferring game state from counts.)

**Where that happens.** The only caller of the `sceCtrlPeekBufferPositive`
thunk is `psp_func_00255DE4`, which hands each `SceCtrlData` to the adaptor
`psp_func_00279648`: `0x002799B8` HOLD/HOME; `0x00279910` a circular deadzone
of radius 30 (`u32 at 0x0042B788`), stick to signed bytes at `state+80/81`;
`0x002797D4` builds the game's button word as `0x00279A10(PSP buttons → bits
0–11)` OR **`0x00279A50(state, ax, ay)`, 26 instructions, the stick-to-bits
converter: `T = u32 at 0x0042B784 = 100`; `ax > T → 0x8000` right, `ax < -T →
0x4000` left, `ay < -T → 0x1000` forward, `ay > T → 0x2000` back** — then
edges and per-bit hold/repeat counters. So forward/back are two-state through
the same compare. `psp_func_0005EBF0` packs the record (buttons u16, then the
sticks as *nibbles* — the game's own stick resolution is 16 steps of 16 from
−120 to +120, with slots for a second stick), `psp_func_0005EACC` keeps an
8×8-byte ring and copies the current record to `ctx` at `0x0033BC60` (found
by `WATCHMEM` on the byte with `_FROM` and the new call-context line), the
key-assign row for control type 0 binds actions 0–3 to exactly those four
bits, and the movement state machine picks one of fourteen handlers by them.
Each handler calls **`psp_func_0004F248(ac, accel, max)`, the yaw
integrator**: rate at `ac+8312`, yaw at `ac+36`, direction byte at `ac+8351`;
gate `(*(ac+9728))[182] == -1`; `pressed(3)` → rate += 4·accel, `pressed(2)`
→ −= , neither → decay and snap; clamp ±max; `yaw += rate` unless
`psp_func_000506F8()->s16[24]` says paused. It has an analog path too
(`0x0005EF8C(ctx)` then `0x0005F008(ctx, 2)`, deadzone 60, acceleration scaled
by travel past it, same cap) that the shipped configuration does not take.
`scripts/fn-source.sh <addr>` prints any of these from the emitted C.

Two traps on the way, both worth knowing. `0x09630F10` steps by exactly
`0x180` a frame while turning and is a **pointer** into a 0x180-byte-row table
— the turning animation's cursor, not the heading (`ram-diff.py` finds
constant steps; it cannot say what they mean). And `WATCHMEM` names the last
function *entered*, so a store made by a caller after a leaf returns is
blamed on the leaf: `0x0005EFA0` is a 13-instruction `pressed(ctx, action)`,
and the write was its caller's.

**The replacements** (`host/replace.txt`, `host/replacements.c`; the emitter's
`--replace` leaves the public symbol to the host and keeps the body as
`psp_func_<addr>__orig`). With `PSPRECOMP_INPUT` unset both defer to the
original and every number in this document stands. With
`PSPRECOMP_INPUT=modern`: `0004F248` sets the rate to `max × (lx−128)/127`
directly, for the player's AC only — every AC in the mission runs the same
integrator, the three enemies once a frame, and a first version steered them
with the stick; and `00279A50` keeps `T` for forward/back (the walk law has
not been read) but uses 16 for left/right, just above the deadzone circle, so
the turning state engages at any deliberate deflection. The player's
integrator is then called exactly once a poll while engaged, and the camera's
mid-hold rate is the stick's: **127 → 2.105°/frame, 96 → 1.588, 64 → 1.056,
32 → 0.528, 16 and 8 → 0** against 2.1019×d/127 of 2.102, 1.589, 1.059, 0.530.
The AC responds in the frame; what remains of the old ramp is the camera's
own filter. Replay reproduces all of it, because the stick it reads is the
same `lx` byte the recorder writes. These figures describe the first linear
cut; the later single radial stage and tuning values below supersede its raw
near-centre response.

**The second axis** (`PSPRECOMP_INPUT=dual`, same day, later). The right
stick and the mouse reach the game the same way the pad does: as fields of
the sceCtrl lanes in `misc.c` — a second stick centred like the first, and
mouse travel summed by the host per motion event and *taken* by each poll,
so nothing is lost between a 1 kHz mouse and a 36 Hz game — merged in
`ctrl_fill`, recorded by the same recorder as optional trailing fields
(`state … rx ry [dx dy]`, `analog … rx ry`, a `mouse dx dy` directive; see
`scenarios/README.md`) and replayed by the same player. Nothing reaches
`SceCtrlData`; only `host/replacements.c` reads them. In `dual` the
integrator's rate is the cap scaled by the right stick's X plus mouse travel
as a displacement (`0.001 rad` per count, `PSPRECOMP_MOUSE_SENS` scales it),
and the converter re-sources the bits: turn from the right stick or recent
mouse X, strafe from the left stick's X as its L/R bits, forward/back from
the left stick's Y unchanged (look up/down was the right stick's Y or mouse
Y as the game's own triangle/circle bits for a day; it is the pitch
integrator's own input now, below). `scenarios/look-sweep.pad`
measures it headless — **right stick 127 → 2.099°/frame, 64 → 1.051, a
30-count-per-poll mouse drag → 1.683°/poll each way, and the left stick at 32
→ 0.002: a strafe, not a turn** — and classic and modern reproduce their
numbers to the command with the lanes changed under them. `host/present.c`
feeds the lanes from the right stick, and with `PSPRECOMP_MOUSE=1` captures
the pointer (Escape releases it, focus loss releases it, a click retakes).
Those rates likewise predate the radial deadzone/expo pass documented below;
they remain the proof of lane separation, not the current response curve.

**Walking and turning at once, and why they never were.** The first
windowed try of `dual` still felt like separate motions, so the question
"does the game turn while walking when both are asked for" was measured
rather than assumed: `scenarios/walk-turn.pad` holds the left stick up alone,
then up with the right stick each way, then back, then the right stick
alone, and `view-analyze.py --deltas` now also reports the **camera's own
motion** per frame — `W₂·W₁⁻¹` over the static objects, which is `V₂·V₁⁻¹`
with the object cancelled out (a true inverse, not a transpose: the world
matrices carry the objects' scale). Forward alone moves the camera 1.23
units a frame with no yaw; **forward with the right stick yaws 2.06°/frame
and still walks at 1.17** — the game combines them, with a mild coupling of
its own (2 % off the solo turn, 5 % off the solo walk); back is 0.56, about
half speed, as AC games have it; a turn in place orbits the camera 0.64 a
frame sideways. So the game was never the obstacle. The obstacle is the
converter thresholding **each axis on its own at 100 of 127**: on a round
stick that makes forward a cone of about ±38°, leaves every diagonal dead,
and puts the corner where two directions would both pass outside the stick's
reach — walking and turning were physically impossible to request together,
and that is the "narrow". `dual`'s left stick is now mapped by direction
instead: eight sectors, each of forward, back, left, right spanning 135°, so
a diagonal asks for a walk and a strafe together, gated at a magnitude just
above the game's 30-unit deadzone circle (`scenarios/sectors.pad` measures
the seven directions). The walk and the strafe are still the game's own
two-state actions; only the asking follows the stick.

**Menus, and the observable this document said did not exist.** The
second windowed try was "substantially better, a little weirdness in
menus" — as it would be: the converter runs everywhere the pad is read, and
in a menu mouse jitter became circle, which cancels, and an off-axis push
became L/R, which changes tabs. The plan had written "there is no reliable
'in a mission' observable"; the hunt found the game's own. The player's
movement-state object at `ac+9728` is **NULL in the garage and the menus
and `0x003364B0` in a mission**, with its byte `+182` at −1 while the AC is
under control — the same gate the yaw integrator uses — and the mode byte
at `0x0042D6B0→[12]`, a tempting candidate, is 0 in both and useless.
`in_play()` in `host/replacements.c` gates every re-sourcing on that
pointer, that byte and the pause flag, and outside play `modern` and `dual`
call the original converter with its arguments restored. Measured: in
`dual`, garage, main-menu and hanger reproduce their classic rows to the
command (928 / 11,948,318; 751 / 9,556,252; 433 / 1,613,365), and the
sectors run in play is unchanged to the decimal.

**Pitch, and the gate that read garbage (5 Sep).** The windowed report on
mouse look was that up/down ran far faster than left/right and had no
proportion to the hand: "tiny movements move the camera all the way up or
down, just slowly". Both follow from pitch still being a button. The
function was found statically this time, no watch needed: `pressed(pad,
action)` is `psp_func_0005EFA0` — the pad's button word against the
key-assign mask for one action, `*(cfg + 34·row + 80 + 2·action)` — and a
scan of the emitted C for its call sites by action index puts actions 10
and 11 (triangle, circle) in four functions, one of which asks them twice
and integrates: **`psp_func_00053234(ac)`, the look integrator**, once a
frame from `psp_func_0004CF74` for the AC under control, right after the
auto-face pass `psp_func_00053048`. Its state is a struct at `ac+8160`:
lockout counter at +4, **pitch angle at +16 (`ac+8176`, radians, up
positive)**, pitch rate at +32. Its constants are six floats at
`0x0030ADC0`: 27.0 (the lockout after a recentre), **accel 0.00136**, 0.009
(a decel the code computes and then overwrites with zero), **max rate
0.027**, and the clamp **±1.1781 rad = ±67.5°**. The digital path uses three
tiny helpers — `000F4084`/`000F40B0` approach a limit and return 1 on
reaching it, `000F40DC` integrates with a clamp and returns ±1 at it — and
reads: triangle held, the rate approaches 2·max by 2·accel *twice a frame*;
circle the mirror; neither, the rate is zero; both together recentre and
start the lockout; then angle += rate, clamped. Measured from the new
`PSPRECOMP_INPUT_LOG` lines (`scripts/pitch-analyze.py`), classic: **a
10-frame ramp at 0.00544 rad/frame² to 0.054 rad/frame = 3.09°/frame**,
against yaw's 2.10; a dead stop on release; the clamp at 67.500°; and
triangle+circle snapping the angle to 0.000 with a 13-frame lockout. That
3.09 against 2.10 is the "faster", and a button that any mouse travel
pressed for four polls is the "no control".

The third replacement, `dual` only: the right stick's Y is a rate in the
game's own cap, mouse Y a displacement at the same 0.001 rad per count as
yaw, into the same three words with the same clamp; the physical triangle
and circle, the recentre and the lockout defer to the original, and since
both laws act on the same words they compose. `scenarios/pitch-sweep.pad`
measures it: **stick −127 → +0.054000 rad/frame from the first frame,
+127 → −0.054000, −64 → +0.027213 (64/127 of the cap), a 50-count-per-poll
drag → ±0.050000**, all clamped at ±67.500°, and the triangle/circle holds
in the same run reproduce the classic ramp to six decimals through the
deferral. Frames at the stick-up and stick-down holds show the sky and the
ground with the AC seen from below and above; classic at the same polls is
level. The converter no longer synthesises 0x40/0x10 from the look channel,
and the mouse-Y hold-off is gone with it.

The gate bug this exposed: since the in-play gate landed the evening
before (`32a7bc4`), every `dual` and `modern` run had been carrying **two
bad 32-bit reads per in-play poll**, invisible because that evening's
re-check compared lists and commands and not the bad-access count, which
the menu rows (where the gate returns before the getter) kept at zero
anyway. `in_play()` called
the guest's pause getter `psp_func_000506F8` — which is `*(*(a0+9728)+276) +
5808`, *taking the AC in a0* — from the stick converter, whose a0 is not an
AC; the integrators had passed the test only because a0 happened to hold
the AC there. The getter is now read in C as `game_paused(ac)` for all
three, and every dual and modern row in the table below reads 0 bad mem
again, with the same lists and commands as before: the garbage read had
returned "not paused", which is what the gate wanted anyway. Two lessons
for `host/replacements.c`: a replacement that calls into the guest owns
that call's arguments, and **a dual run's bad-access count is part of its
row**.

**The walk, and the push it comes from (5 Sep, afternoon).** The fourteen
movement handlers that call the yaw integrator share three primitives, and
reading them is the walk law. `psp_func_0004F1C4(ac, decel)` is the brake:
it scales the horizontal velocity at **`ac+80` (x) and `ac+88` (z)** down by
4·decel a frame and zeroes it when that crosses zero. **`psp_func_0004F06C(ac,
dir, accel, cap)` is the push** — the one thing every moving state does,
walk (`psp_func_000440B4`), jump, boosts: `dir` indexes eight unit vectors
at `0x0030AEB0` in the AC's frame — **x to the left, z back**: 0 forward, 1
forward-left, 2 forward-right, 3 left, 4 right, 5 back, 6 back-left, 7
back-right, the order the game keeps its stick actions in (forward, back,
left, right; the first cut read entry 3 as right and the first mission
played with the strafes swapped) — the vector times `accel` goes into a
scratch at `0x0030AF00`,
`psp_func_002B007C`/`00255BBC` rotate it by the yaw at `ac+36` on the VFPU
(`vrot`, `vtfm`), and `psp_func_0004EE9C(ac, vec, cap)` adds four times it to
the velocity and, when the speed passes `cap`, scales the vector back to
`cap` — or, when the AC was already faster than the cap, down toward it by
4·`ac+1412` a frame. The walk handler passes `accel = ac+1392` and **`cap =
2·ac+1400`, the legs' own numbers**: 1.2868 units/frame forward and
sideways, 0.5814 backward (a separate parameter), the first seven frames of
a walk at half cap (the game's own start phase), nine frames from rest to
the cap. Every handler passes them whole and the stick chose one of eight
directions, so the walk was two-state by construction, and the camera-motion
figures below (−1.23 forward, −0.88/−0.87 on a diagonal) were this cap in
this table's directions.

The fourth replacement: for the player under `dual`, the push takes the
left stick's own unit vector as the direction and scales the cap by the
profile's radially processed magnitude; `modern` keeps the game's direction
(that stick's X is the turn) and scales by the processed Y component. The
single current movement deadzone is 10%, rescaled continuously to full output
at a 98% outer radius, with a 13% enter / 10% leave gate so a resting stick
does not chatter. The accel, the clamp, the overspeed decay, the start phase,
the back cap and the animation are the game's; a centred stick inside a jump
or a boost defers. The rotation is
done by calling the game's two helpers on a guest stack frame, as the
original does, rather than re-deriving VFPU arithmetic. Measured
(`scenarios/walk-sweep.pad`, from the push's own log): **stick 127 →
1.2868 units/frame at frame 9, 96 → 0.8756, 64 → 0.4511, 45 → 0.1990;
straight right 1.2868; back 0.5814**; and the direction of motion, from
the position now in the log and read with right positive, is the AC's 70°
heading plus the stick's angle in every hold to 0.2°: forward +70.0°,
right +160.0°, 30° left +39.8°, back −110.0°. `scenarios/sectors.pad`
under the new law moves at 0°, 30°, 45°, −45°, 135° and 90° from the
heading, exactly the stick's angles, at 0.928 of the cap for its 120-unit
pushes and 0.309 of it for its half-deflection hold — though three of its
seven holds now end against terrain, since the corrected paths cross
ground the old eight-direction paths did not (its row says which).

One trap, worth its paragraph: the first cut of the sweep held the stick
30° right for sixty frames, and the speed reached the cap, held three
frames, and collapsed to a steady 0.293. The modern hold at the same poll
did the same and then crept back up as the heading turned away. That is a
wall, not the law — but the speed alone cannot say so, which is why the
push log now carries the position (`ac+16`, `ac+24`) and
`scripts/walk-analyze.py` prints the direction actually moved and flags a
hold whose speed fell back under its cap as `BLOCKED`. The mirrored hold
ran clear at full speed; the sweep was then shortened to thirty-poll holds
and ordered to stay inside the cone the classic run had proved clear. (With
the strafe sign still wrong at the time, that first "30° right" push had in
fact walked 30° *left*, into the wall the corrected forward-left holds of
sectors.pad and walk-turn.pad now find as well.)

**Better pitch, the stick's half (5 Sep, afternoon).** Sif's call, with the
caps left alone because they come from the parts: ease-in and finer
control near centre. The right stick first takes one 8% radial deadzone and
2% outer deadzone; its resulting radius takes the expo curve
`(1−e)r + e·r³` with `e = 0.6`, preserving the stick angle, and the stored rate at `ac+8192`
climbs toward it at the game's own 4·accel a frame — the ten-frame ramp
triangle had — but climbs only: easing off, reversing and releasing take
effect at once, because a rate that coasts after the thumb has stopped is
what makes aiming miss. The same curve is on the stick's yaw, so a diagonal
push is not bent, with no ease-in there since the chase camera's own filter
smooths yaw. The mouse is untouched on both axes. Measured: **pitch at −127
ramps +0.005440, +0.010880 … to +0.054000 over ten frames, exactly the
classic ramp; at −64, 0.015031 rad/frame (was 0.027213); yaw at 64,
0.585°/frame (was 1.051); full deflection unchanged on both; the mouse
unchanged at ±0.050000 and 1.719°/frame.** These numbers were measured before
the curve moved from each component to the radius; the current formula above
keeps diagonal aim directions exact.

**The walk's cadence (5 Sep, evening).** The creep slid its feet because
the walk cycle plays at one speed. Read from the listing: the player's walk
handler is `psp_func_00065824` (the fourteen-handler list of the morning
was the *direct* callers of the push; the player's walk reaches it through
`psp_func_00054894`, which halves the cap for the start and scales it for a
back walk — the push line now carries `ra=` so the caller is a fact), and
every frame it names the cycle for the direction — a byte from the table at
`0x0030B5DA`: forward 2, then 4, 6 … 16 round the compass, 0 idle; the
eight-frame start is anim 1 — through `psp_func_000460C0 → 001DF060 →
001DFA58`, with loop set and a 20-tick cross-fade. That play call fills the
legs model's 24 animation slots (`model = *(ac+128)`, count at +116, base
at +120, 128 bytes each: anim +73, frame +76, frame count +78, loop +75,
fade +80/+84/+88, and the per-frame **step at +92, written as 1 every
frame**), and the updater `psp_func_001DFC80` adds the step to the frame
once a frame, wrapping at the count, and samples the pose at 2·frame in
60 Hz ticks. Integer step, integer frame: the engine has no fractional
playback. It does have a step of 0. So the fifth replacement is the
updater: the push notes each walk frame's stick magnitude, an accumulator
carries it, and on the frames it says to the looping layer-1 slots' steps
are zeroed for that one call and put back — the frame holds, and the cycle
advances m frames per frame on average. At full deflection nothing changes.
Measured with the new `anim` log lines and `walk-analyze.py`'s cadence
column (frames advanced per game frame over a row, from the first looping
slot): **classic 1.000 everywhere; dual forward 127 → 1.000 on anim 2,
right on 10, back on 12, the 30° push on 4; 96 → 0.706 for m = 0.680, 64 →
0.353 for 0.351, 45 → 0.118 for 0.155 — the accumulator over seventeen
frame pairs, so 12/17, 6/17, 2/17.** Fifteen frames a second at half speed
is a visible cadence; the feet stop sliding. Boost and the turn are not
touched: the hold is keyed on the return address being inside the walk's
push helper, and the turn cycle on humanoid legs does not loop anyway
(psp_func_00043F34 passes loop only for leg type 2).

**The pause gate was not one (5 Sep, evening).** `scenarios/pause-look.pad`
— Start mid-mission, then the right stick, a mouse drag and the left stick
in the menu, then Start again — showed the game paused for 190 polls (the
integrators were not called once) while the converter's `in_play()` stayed
true throughout: the s16 the yaw integrator honours at
`(*(*(ac+9728)+276)+5808)+24` is not a pause flag (what it is stays
unknown; the replacement keeps the original's behaviour for it). The pause
menu sets nothing on the AC — it simply stops running it. So the gate is
now a heartbeat: the look integrator records the poll every time it runs
for the player, and `in_play()` needs that to be within two polls (two, so
the order of the pad read and the AC update within a frame does not
matter). The converter logs `play=0/1` on each transition: **play=1 at
1291, the mission's first AC frame; play=0 at 2162, two polls after Start;
play=1 at 2351, one poll after the resume** — with nothing turning between.

**A flick's first frames were lost (5 Sep, evening).** The first cut of
`cam-step.pad` — one poll of 200 counts — turned the AC by nothing. The yaw
integrator runs only inside a movement state, the state the turn bits ask
for engages three polls after they rise, and the poll's mouse delta is
delivered once. So the converter, which runs every poll, now banks mouse
yaw and the integrator draws on the bank: **the integrator's first frame
after a flick applies the banked counts (4 for the first four polls of a
count each), and a 200-count poll inside the state lands at that poll,
rate 0.200000.** A drag across the pause menu is dropped, not banked.

**The camera filter, found (5 Sep, evening).** RAM snapshots a poll apart
after the yaw step, read by `scripts/ram-diff.py --geometric` (new: float
words whose successive changes shrink by a constant ratio), put the
camera's state at `0x0043C080`: the eye at +16, the look-at target at +48,
yaw at +36 converging on the AC's yaw − π with ratio 0.830, and copies of
its matrix in six places. `WATCHMEM` on the yaw named the camera update
`psp_func_00074168` (288 bytes per camera, a mode byte at +0 dispatching a
handler table at `0x0030C1D0`; mode 0 is the chase, `psp_func_000757E8`).
The yaw is only atan2(target − eye). The lag is in the handler's last
step: it computes where the camera should be — target = AC position,
lifted by the legs' height parameter, 7.0 units ahead; eye 17.9 behind it
(30.0 in one mode) along heading and look pitch — and then blends,
`current + (ideal − current)·(1 − r)`, through `psp_func_0007467C` for the
target and `psp_func_000746D4` for the eye, **r = 0.83 for the eye (cam+104)
and 0.8309 for the target (cam+108)**, 0.9208 for the eye's first frames,
re-set to 0.83 by a distance rule in `psp_func_000752A0`. Seventeen percent
of the gap a frame: sixteen frames to 95%, half a second behind the AC.
Whether it should stay is Sif's to feel: the sixth and seventh replacements
are those two blends, substituting `PSPRECOMP_CAMERA_LAG=<r>` for the
player's camera under `modern`/`dual` (0.83 the game's; 0 fixes the camera
to the AC; the eye's vertical rate keeps the game's look-up modulation as a
ratio), with a `cam` log line a frame. Measured on the 0.2 rad step
at poll 2100, from the `cam` lines (the camera's yaw against the AC's − π):
**the game's 0.83 leaves a gap of 0.1698 after one frame, 0.1410, 0.1170 …
0.0059 at frame 19, every ratio 0.830; at 0.5 the gap is 0.1005, 0.0501,
0.0250 … under 0.001 by frame 8; at 0 the camera is on the AC's heading the
next frame**. During the count-a-poll drag before the step the game's
camera trailed by a steady 0.005 rad — the rate times r/(1−r).

**Modern gamepad polish (5 Sep, late).** The enhanced movement and button
profiles are independently selectable: `PSPRECOMP_INPUT=modern|dual` chooses
the stick law, while `PSPRECOMP_GAMEPAD=modern|classic` chooses the physical
button layout; without the second variable it follows the first. The modern
gamepad is **LT boost, RT right arm, LB left arm/event, RB change weapon, L3
extension, R3 OB/EO, A inside, B view reset, Y purge modifier, X spare**.
Start, Back and the d-pad stay Start, Select and d-pad.

Triggers and stick clicks do not exist in a PSP packet, so the SDL layer puts
all ten controls in unused carrier bits. They remain in the normal sceCtrl
lane, which means recordings and replays preserve them. `psp_func_00279A10`
captures and removes them before the game's PSP converter; outside play it
aliases A/B/X/Y to cross/circle/square/triangle and either shoulder pair to
PSP L/R, so menus retain conventional navigation. In play, replacements for
the game's held and pressed action queries inject actions 4, 5, 6, 7, 12,
13, 14 and 15 without depending on the saved PSP key assignment. Actions 8/9
are the strafes despite being bound to the PSP shoulders in the default row;
the gameplay call sites identify 4 as change unit and 7 as arm unit L/event.
B reproduces the original action-12 pitch reset and its lockout explicitly,
because the shipped digital layout cannot reach the original function's
analog-only branch. Y is injected at `psp_func_0005F348`, the composite disarmament check:
it remains a modifier while RT, LB or L3 selects the part, rather than faking
the four-button chord used by the default PSP layout.

The host now owns one active controller rather than allowing idle pads to
overwrite one another, samples its complete state on open and focus regain,
and clears it on removal or focus loss. Keyboard, mouse and controller buttons
are separate sources; keyboard W/S and A/D own only their axes while held, and
opposites deliberately centre the axis. Trigger press/release hysteresis is
50%/25%. SDL stick values remain raw until the one radial processing stage
after recording, avoiding the former 25% axial cut followed by the game's
30-unit circle. `modern-menu.pad` and `modern-buttons.pad` are the menu and
in-play carrier gates.

**A keyboard for the mouse (5 Sep, evening).** `PSPRECOMP_KEYS=wasd` in
`host/present.c`: W/A/S/D *are* the left stick (digital, a diagonal a full
push at 45°; under `dual` the walk and the strafe), Space is cross (boost;
confirm in menus), the mouse buttons are the fire buttons — left square,
the right arm's weapon on the game's default assign; right d-pad up and
middle d-pad down — and the letters the stick took move: square to `c`,
triangle to `v`; the rest is the classic layout, which stays the default.
The click that captures the pointer does not fire. First windowed try: A and D walked, W and S did nothing — an Xbox 360 pad left plugged in streams its resting stick as centre values (500 events in 2.5 s on left Y alone, 300 on X, measured from `/dev/input/js1`), and each one overwrote the keyboard's byte between key events; the keyboard's stick is now its own pair of bytes, merged with the pad's per axis at publish time, the keyboard winning wherever a key is held, and the host names any controller it opens. Read from the snapshot,
the game's default key-assign (`cfg + 34·row + 80 + 2·action`, on the
game's remapped bits): **actions 0–3 the stick's four bits; 5 cross; 6
square; 8/9 L/R; 10/11 triangle/circle; 4, 7, 13, 14 the d-pad** — which
button *does* what is the game's table, not the keyboard's. Untested in a
window by anyone yet; the layout is a proposal.

Still open: hands-on tuning across controller models; the turn animation's
cadence (the humanoid turn anim plays once
and holds, so there is nothing to modulate; the quad/tank loop is untried);
other leg types' walk handlers, which may not go through
`psp_func_00054894` (the `ra=` on the push line will say, and the cadence
then stays the game's); the wasd layout in a window; and whether 0.83
stays. The evening's build and measurements were made in the same detached
worktree with the fork at `e2c40b4`, because the other agent's VFPU edits
are still uncommitted there; the main-tree caveat below stands.
`build/host-trace` in that worktree was current for that seven-replacement
snapshot; the main tree's is not.

The afternoon's build and measurements were made in a
detached worktree with the fork at its committed pointer, because the other
agent was mid-edit in the fork's VFPU runtime; the numbers are against
`e2c40b4`. The main tree, rebuilt with those uncommitted edits in it so the
controls could be played, gives **every** scenario about 7% fewer GE
commands with lists and bad accesses unchanged — look-probe in classic
29,413,090 against its 31,629,673 — so until that work lands and is
re-baselined, a main-tree count is not comparable with this table.

## The instruments, and what each can and cannot tell you

All are off by default and cost nothing when off.

| | |
|---|---|
| `PSPRECOMP_HLE_LOG=1` | Every firmware call, tagged with the calling thread, with arguments and result. The first thing to reach for. Unfilterable — see the volume note. |
| `PSPRECOMP_HLE_TRACE=<name>` | Dumps the guest function trace at every call to that firmware function. Exact name match, not substring. Up to 512 lines per hit, so aim it at a call the histogram has already shown to be cold. Needs `TRACE=1`. |
| `PSPRECOMP_WATCH=<hex addr>` | Argument registers on entry to one guest function, dereferencing the pointer-looking ones. Hooks `PSP_ENTER`, so it only ever fires on a **function entry**. Needs `TRACE=1`. |
| `PSPRECOMP_REACHED=<hex>[,...]` | Whether control ever arrived at each address. Covers every **label**, not just entries — see below. Needs `TRACE=1`. |
| `PSPRECOMP_SEMA=<substring>` | Narrates every wait, take and signal on matching semaphores, and unlocks the thread and signalled-uid censuses in the summary. See the volume note. |
| `PSPRECOMP_TEXDUMP=<path>` | Every distinct texture the game binds, decoded through the renderer's own sampler, as `<path>-NN.ppm`. Separates "the sampler reads wrong texels" from "the texture is not what we think", which look identical on screen. |
| `PSPRECOMP_FINDPTR=<hex>` | Every address in the module, RAM and VRAM holding that value as a word, tagged by region. For pointers that only exist once the PRX is relocated — and for tracking a value's *identity* rather than one of its addresses. |
| `PSPRECOMP_PEEK=<hex>[,...]` | The word and the byte at each address when the run stops. Answers "what is this field", where FINDPTR answers "where is this value". |
| `PSPRECOMP_WATCHMEM_FROM=<poll>` | Holds `PSPRECOMP_WATCHMEM`'s 32-report budget until that poll, so a word cleared at mission start and rewritten every frame does not spend it all on the clears. Each report now also lists the four functions entered before the writer — the ring, not a stack, but enough to see past a memcpy and its wrapper to the caller that decided the value. |
| `PSPRECOMP_REACHED_DUMP=<file>` | Every label control reached, one address per line — the whole mark bitmap rather than 32 named addresses. For diffing a run that turned against one that did not. Same label-not-function caveat as `REACHED`; needs `TRACE=1`. |
| `PSPRECOMP_VIEW_LOG=<file>` | The GE's matrices as they are uploaded, poll-stamped: tag `V` for view, `W` for the k-th world upload of each frame (`_WORLD=<k>`, default 1). `_WORLD=all` with `_POLLS=lo-hi[,lo-hi]` dumps every world upload in those polls with an `F <poll>` line per frame; `scripts/view-analyze.py --deltas` clusters per-object yaw change across frames and the largest cluster is the camera; it also prints the camera's own motion per frame, `W₂·W₁⁻¹` over that cluster with a true inverse (the matrices carry object scale), so a walk reads as z and a strafe as x beside the turn. The only camera observable this game offers — its view matrix is a constant. |
| `PSPRECOMP_RAMSNAP=<prefix>` + `_POLLS=n[,...]` | The 32 MB of RAM and the module image (guest 0; where this game keeps its player object) at each named poll, before that poll's pad state is written. `scripts/ram-diff.py` finds the words stepping by a constant across three or more — an integrator's output — at 16-bit, 32-bit and float, without knowing the representation; `--geometric LO-HI` the float words whose successive changes shrink by a constant ratio — a filter converging after its input stopped, which is how the camera's state was found. Half a gigabyte for sixteen; capped there. |
| `PSPRECOMP_INPUT=modern\|dual` / `PSPRECOMP_INPUT_LOG=<file>` | The port's control schemes, from `host/replacements.c`; unset, every replacement defers to the game's own code. `modern` honours the left stick's magnitude for turning and walking; `dual` looks with the right stick and the mouse — yaw and pitch both proportional — and walks with the left in the direction it points, at a speed set by its deflection. The log is one line per call of each replaced function, in every mode (the deferring paths read the result back after the original ran): the yaw lines carry poll, object, sticks, gate and the rate, which is how "the game did not even ask" was told apart from "the law is wrong"; the pitch lines carry angle, rate, stick and mouse (`scripts/pitch-analyze.py` folds them into the law they show); the push lines carry the stick's magnitude and direction, the cap asked for, the speed reached, the position and the caller's return address (`scripts/walk-analyze.py` folds them, prints the direction actually moved, and flags a hold a wall stopped); the `anim` lines, one a frame from the model updater, every slot's anim, frame, step and loop (the analyzer's cadence column); `play=0/1` on each transition of the converter's in-play gate; and the `cam` lines, one a frame, the rate the chase camera's eye was blended with and its yaw and pitch beside the AC's. |
| `PSPRECOMP_GAMEPAD=modern\|classic` | Selects the physical gamepad independently of the stick scheme; unset, it follows `PSPRECOMP_INPUT`. Modern gives the triggers, bumpers, stick clicks and face buttons semantic gameplay actions while retaining conventional menu aliases. Classic is the direct PSP mapping. |
| `PSPRECOMP_MOVE_DEADZONE=<r>` / `PSPRECOMP_LOOK_DEADZONE=<r>` / `PSPRECOMP_STICK_OUTER_DEADZONE=<r>` / `PSPRECOMP_LOOK_EXPO=<e>` | Radial stick tuning, as fractions. Defaults: move 0.10 with a derived 0.13 enter threshold, look 0.08, outer 0.02, look expo blend 0.60. Processing happens once, after the recorded lane, so replay sees the same values as live input. |
| `PSPRECOMP_CAMERA_LAG=<r>` | The chase camera's blend rate for the player's camera under `modern`/`dual`, 0..0.99: the fraction of the gap to its ideal position kept each frame. The game's is 0.83 (0.8309 for the look-at target); 0 fixes the camera to the AC. Unset, the game's values; classic ignores it. |
| `PSPRECOMP_KEYS=wasd` | `host/present.c`'s second keyboard layout, for a mouse player: W/A/S/D are the left stick, Space cross, the mouse buttons square / d-pad up / d-pad down, square and triangle on `c` and `v`; the rest as the classic layout, which is the default. |
| `PSPRECOMP_MOUSE=1` / `PSPRECOMP_MOUSE_SENS=<k>` | Windowed only: capture the pointer for mouse-look (Escape releases, a click retakes, losing focus releases). Travel reaches the game through the sceCtrl lanes, so a recording holds it and a replay reproduces it; `k` scales the 0.001 rad per count default. Means nothing outside `PSPRECOMP_INPUT=dual`. |
| `PSPRECOMP_PAD=start,cross` | Holds pad buttons for the run. There is no window and no gamepad. |
| `PSPRECOMP_PAD_PRESS=start,15,0.5` | Presses a button at a wall-clock moment — down at `delay` seconds, up `duration` (default 0.5) later. A held button never reads as *pressed*, because a press is a transition. Headless only; in a windowed run the SDL layer owns the pad. |
| `PSPRECOMP_FRAME=<path>` | Where to write the frame. Defaults to `frame.ppm`, and dumps the GE's render target rather than the scanned-out buffer. |
| `PSPRECOMP_MPEG_DECODE=1` | Demuxer and openh264 video path. Refused, loudly, in a build without openh264. |
| `PSPRECOMP_DRAIN=<seconds>` | Widens the scheduler drain past its 60-second headless default, for runs that are supposed to still be going — a movie, for one. `0` means no limit, which is also what a window gives you by default. An empty value is unset, not unlimited. |
| `PSPRECOMP_WINDOW=1` | An SDL2 window, the gamepad and audio out. Implies real-time pacing. The frame is published at `sceDisplaySetFrameBuf` — the flip — because this game never asks for a vblank; hanging the hook off one publishes nothing, which looks exactly like a broken renderer. Closing the window stops the run through the scheduler, so the end-of-run summary still prints. Needs SDL2 at build time; without it the host still builds and says so when asked for a window. |
| `PSPRECOMP_REALTIME=1` | Real-time pacing without a window, so a wall-clock measurement of a real scene is honest. Ignored when `PSPRECOMP_WINDOW` is set, which already implies it. |
| `PSPRECOMP_RENDER=<name>` | The presentation backend: `software` (the default and the oracle) or `null` (counts primitives, draws nothing -- the run that answers "did the game ask to draw" without paying for pixels). An unknown name is **fatal**, because `psp_render_select` leaves the current backend in place on failure and a warning would mean running software while believing otherwise. A set-but-empty value is unset. Every run prints the backend it used. Wired 3 Sep, findings item 50; before that nothing outside the unit tests could select one. |
| `PSPRECOMP_ASPECT=native\|window` | `native` (and unset) preserves the PSP's 480x272 picture with letterboxing. `window` selects GL when no backend is named, fills the current drawable, rebuilds the game's camera and frustum at that aspect while retaining its vertical FOV, and keeps screen-space HUD draws in a centred native-aspect safe area. Full-frame clears, fades and composites still cover the drawable. This changes aspect only: the guest framebuffer and internal rendering remain 480x272. |
| `PSPRECOMP_MPEG=1` | Narrates the movie path. Everything it prints is throttled except `RingbufferPut`, which is bounded by the disc read — safe in either configuration. |
| `PSPRECOMP_MPEG_DUMP=<path>` | The demuxed elementary stream, once it exceeds 1 MB. For checking against a decoder that is not ours. |
| `PSPRECOMP_MPEG_FRAME=<path>`, `PSPRECOMP_MPEG_FRAME_NO=<n>` | One decoded frame as a PPM. |
| `TRACE=1 ./scripts/04-emit-build.sh` | Rebuilds the generated C with tracing, into `build/host-trace` so it no longer destroys the plain build. `TRACE=1 ./scripts/06-boot.sh` runs it. |
| `scripts/07-autotests.sh <dir>` | One pspautotests directory at the real instruction budget, against recorded hardware output. What you work a suite with. |
| `scripts/08-autotest-sweep.sh` | All 432 tests, reduced budget and a per-test wall timeout, into `reports/08-sweep.tsv`. A map, not a verdict: the reduced budget truncates tests that legitimately run long. Rank by the differing-line column — one line away is one bug, five hundred is an unimplemented library. |

**The GE list count doubles as a cost signal, and it earned that on 2 Sep.**
New Game sits at 6,003 lists and 6,003 finishes. It fell to 17,200-17,500
across several runs while the guard band was wrongly relaxed -- the replay is
keyed on pad polls and the game paces against elapsed time, so letting
enormous off-screen triangles reach the rasterizer cost enough frames to show
here. With the rule restored it is back at 6,003 exactly, lighting and all.
So a drop in this number without a change in what the run reaches means
something got dramatically more expensive, and is worth chasing rather than
shrugging at.


**`scripts/05-oracle.sh` now refuses to run against stale generated C.** It
rebuilds the interpreter every time but links C emitted whenever
`04-emit-build.sh` last ran, so editing the emitter and re-running only the
oracle compares two different programs and reports it as `differ` — which reads
exactly like a codegen regression. It cost one this session. Run
`scripts/04-emit-build.sh` after touching `emit.c`, `decode.c`, `decode.h` or
`recomp_rt.h`.

**The trace build goes stale the same way, and louder.** `build/host-trace`
holds its own `aclr_funcs.o`, and `06-boot.sh` reuses it without rebuilding,
so a trace run against last night's objects links a different program than
the plain build runs. It cost one this session too: a trace build from Sep 1
22:44 against a plain build from 22:54 diverged at poll 333 with 1.3 billion
bad accesses, which reads exactly like a tracing-instrumentation bug and is
nothing of the sort. Refresh with `TRACE=1 scripts/04-emit-build.sh` whenever
the plain objects move; the run is faithful again when polls, bad accesses
and the pixel count all agree with the plain build.

The boot summary also reports, without any flag: the firmware-call histogram,
the GE state, a VRAM survey, and the live thread list with what each is parked
on.

### `PSPRECOMP_REACHED` is label-level, and the difference matters

`PSP_MARK` is emitted only at **labels** — branch and jump targets, function
entries, split entries, fall-through targets. A `jal` site or a store in the
middle of a block is not a label and can never be marked, and
`psp_trace_was_marked` only distinguishes the outside-the-module case (`-1`). An
unlabelled in-module address returns 0, which reads as *not reached*.

So map an address to its covering label before asking:

```bash
grep -o 'L_[0-9A-F]\{8\}: PSP_MARK' game/generated/aclr_funcs.c | sort -u
```

and take the greatest label ≤ it within the same function. Control reaching that
label is control reaching the block the instruction sits in, which is the
question worth asking anyway.

### Output volume — two instruments can fill a disk

`PSPRECOMP_SEMA=Movie` **with the decoder on** produced **2.5 GB of stderr in
sixty seconds**: Movie Start is signalled hundreds of millions of times in that
configuration and each one prints a line plus a trace dump. In the default
configuration the same flag produces 115 lines. `PSPRECOMP_HLE_LOG` is
unfilterable at two lines per call — 3.2 MB in the default configuration, and
not usable at all with the decoder on.

Both write to stderr, so they land wherever it is redirected. Send them to
`reports/` and not to a scratch directory on tmpfs.

### Six ways to measure nothing

Each of these produced a confident number that meant nothing.

- **A capped oracle run used to be positional.** `--limit` bounds *attempts*, and
  the old work-list walked `.text` from the bottom testing whatever the dispatch
  table resolved. 5,000 attempts reached the first ~256KB of 3.03MB, so two runs
  across an emitter change that reclaimed 171 functions produced
  **byte-identical output files**. Fixed since.
- **Counting `UNBALANCED` log lines counts the print cap.** `psp_trace_sp` stops
  at 24 sites, `psp_trace_sp_call` at 16, and both saturate here. The boot summary
  reports the real totals now.
- **The unbalanced-return total is not a before/after metric.** It counts
  returns, so it tracks how many frame-loop iterations fit in the window. Two runs
  of the *same* build differed by 5,688. Use distinct sites and leaks.
- **Absence from a top-N list is not absence.** The firmware histogram prints
  twelve entries. `47807ad` concluded "the decode loop never reaches the fetch"
  because `sceMpegGetAvcAu` was not in it. It is called — twice in the default
  configuration, and repeatedly with the decoder on.
  `sceKernelWaitEventFlag` was declared never to appear at all, on the same
  evidence; it is called once. The GE queue exhaustion hid in the same blind spot.
  Grep an `HLE_LOG` capture; do not read the histogram as a census.
- **`threads: all finished` used to mean "or the host killed it".** A force-stop
  marks every thread dead, so the drain counts zero — the same answer a clean
  finish gives. Fixed by patch 0021, which threads the reason through.
- **"300+ frames published" counts publishes, not pixels.** The window's first
  working build measured that and read it as the display path being right. It
  was not: `present_frame` used `sceDisplaySetFrameBuf`'s `bufferwidth` as a
  *byte* pitch when it is in **pixels**, so each row advanced 512 bytes instead
  of 2048. The unwritten 480..511 stride padding — which the rasterizer never
  touches, because it clips at `x < 480` — then walked across the picture as
  three 32-pixel black bands at x 96, 224 and 352, one visible every fourth
  row, and only the top 68 scanlines were ever read. Every row stayed inside
  the buffer, so nothing faulted and `bad mem` stayed at zero. A copy-path
  fault that presents as a rasterizer fault, and the frame counter cannot tell
  them apart. `dump_one` and `psp_display_capture` had the arithmetic right all
  along; the window was the odd one out.

### The trace ring is not a call stack

It records function *entries in order*. A loop calling a four-function chain 116
times looks identical in that output to 116-deep recursion. Reading it as a stack
cost three turns of wrong diagnoses on one fault.

**When the question is structural — how deep, who called whom — use gdb.** For
"did this ever run", use `PSPRECOMP_REACHED`, which replaced the breakpoint
workflow.

```bash
gdb -batch -ex "handle SIGSEGV stop nopass" -ex run -ex "bt 45" \
    --args ./build/host/boot game/extracted/ACLR_App.elf "game/<disc>.iso"
```

`libpsprecomp` is built Release without debug info; configure it
`-DCMAKE_BUILD_TYPE=RelWithDebInfo` to read locals and globals.

## The pattern behind most of the bugs found here

An unimplemented firmware call returns 0, and 0 is `SCE_KERNEL_ERROR_OK`. The
game proceeds on a lie, and fails somewhere unrelated much later. Four separate
blockers were this exact shape: `sceIoGetstat`, `sceKernelVolatileMemLock`,
`sceKernelWaitSemaCB`, and every one of sceMpeg's twenty-three.

The corollary is that a call reporting success while writing nothing to its
out-parameters is worse than one that fails honestly. When a game does something
inexplicable, check what it was last told.

**The GE queue was the mirror image** and is worth holding alongside it: a call
that fails *honestly*, every time, and is never looked at because no instrument
reports failures it does not already expect.

## Ruled out — do not re-investigate

- **`sceKernelDelayThread` causing the deadlock.** It yields, which leaves the
  thread READY and always runnable.
- **Stack exhaustion in the MovieReadThread fault.** 64MB did not help; the gdb
  backtrace showed a stack depth of three.
- **The `$k0`/reent gap as the cause of an allocator failure.** Only twelve sites
  in the whole module read `r_k0`, and none are on that path.
- **`psp_mem_ptr`'s bounds logic.** It was never wrong. The memory was being freed
  underneath it by the boot host's teardown.
- **The GE block transfer as how textures arrive.** Implemented, and never used.
  The game now loads 153 CLUTs, so this is worth re-asking.
- **`sceDisplayGetFramePerSec` as the reason the game draws nothing.** Implemented:
  the picture did not change by a single pixel.
- **Returning a clean end-of-stream from sceMpeg to end the movie.** The game
  already ends it. `SCE_MPEG_ERROR_INVALID_VALUE` makes the AU-fetch wrapper at
  `0x002750C0` print its own `Fatal Error!!!` and proceed into the teardown chain.
  `NO_DATA` would restore the spin — see the header comment in `mpeg.c`.
- **A `SignalSema` on the semaphore SoundThread waits for.** *(Half retracted —
  see open item 2.)* It is true that `Movie Sync sema` (`0x0004001C`) is never
  signalled **at run time** in either configuration; the signalled-uid census
  says so. It is **false** that nothing in the module signals it: three call
  sites do, via offset 396 rather than offset 100. The static half of that
  entry searched one struct's offsets and drew a conclusion about a value.
- **A colour-conversion or decode-mode stage as the missing display step.** The
  game imports **none** of `sceMpegAvcDecodeMode`, `sceMpegAvcCsc`,
  `sceMpegAvcDecodeYCbCr`, `sceMpegAvcCopyYCbCr` or `sceMpegAvcQueryYCbCrSize`.
  All 23 sceMpeg imports were identified by SHA-1 and they are the plain path
  only. So `sceMpegAvcDecode` writing into the buffer the game passes **is** the
  whole display mechanism.
- **Skipping the intro movie with `PSPRECOMP_PAD`.** The skip path routes back
  into the movie subsystem rather than around it. Useful negative result, because
  it shows the movie machinery has **two** gates, Movie Start and Movie Sync.
  Satisfying only one moves it to the other.
- **The scheduler as the reason the game does not progress.** Measured three ways
  — current build, token fix reverted, fully original semantics — all functionally
  identical, `deadlock --` printed zero times in all three.
- **An HLE answer in the movie init chain being wrong.** Every firmware call from
  `sceMpegInit` through `sceMpegQueryStreamSize` is answered correctly and with
  real data. The one unimplemented call in the chain is
  `sceUtilityLoadModule(PSP_MODULE_AV_MPEGBASE)` — NID `0x2A2B3DE0`, argument
  `0x0303`, immediately before `sceMpegInit` — and it returns 0, which is what
  hardware returns on success.
- **A guard inside the audio pump holding back the producer.** See open item 2:
  it is `sceMpegGetAtracAu` refusing, in the default configuration only.

### Retracted

- **"The `sceKernelWaitSema cannot be satisfied` report is teardown noise."** It
  was, once, when the run reached the 60-second drain first. Since `cb06a25` the
  default configuration never reaches the drain, and this report is **the thing
  that ends the run**.
- **"The renderer is not the blocker."** It was, and it was refusing 204 of 212
  submissions.
- **"Nothing in this chain is a psprecomp bug: every call behaves correctly."**
  `sceGeListEnQueue` did not.
- **"`0x0013B02C` is the target of no transfer at all and is probably a
  pointer-scan artifact rather than live code."** It is live and it runs.
- **"Both never-signalled semaphores now fire."** Only Movie Start does.

## The open-work list, kept as history

**The plan of attack lives in [../ROADMAP.md](../ROADMAP.md).** It says what
to build next, in what order, and what each step has to produce. This list is
the history of the items that fed it, kept because the measurements and the
retractions are the reusable part. The numbering is chronological, not a
priority order; 1, 3 and 8 are closed and 0 is a status entry. Nothing new is
added here — a new piece of work is a milestone step in ROADMAP.md, and its
measurements land in this file once they exist.

0. **There is a picture.** The FromSoftware logo renders — 13,156 of 130,560
   pixels varying, 28 distinct colours, in a 360×48 band across the middle of
   the screen. It took patches 0025–0028: the transform pipeline, the texture
   formats the game actually uses, depth, and alpha blending.

   **Three instruments had to be fixed before it could be seen**, and each was
   reporting a confident falsehood:

   - The dump took the GE's *render target*, which is the back buffer. A run
     stopping just after a frame-start clear finds it empty and reports that
     nothing was drawn. Both buffers are dumped now.
   - Neither buffer says anything about frames that came and went, so the
     display HLE now scores every presented frame and keeps the best.
   - **Ranking frames by non-black pixels put a white flash at the top.** A
     solid fill is maximally lit and carries no information. Ranking by how
     much a frame *varies from its own corner* asks the question that was
     actually meant, and is what surfaced the logo.

   Read the frame with `PSPRECOMP_FRAME=<path>`; it writes `<path>`,
   `<path>.display.ppm` and `<path>.best.ppm`. The best frame is dim — peak
   channel value 68 of 255 — because it is caught mid fade-in, so brighten it
   before judging.

   **The speckling is fixed** (patch 0029) and the logo is clean — 17 colours,
   no noise. It was the palette address. Both the texture and CLUT bases arrive
   in two registers, and the second carries address bits 24..27 in its own bits
   **16..19**, not in its low byte:

   ```
   getClutAddress()    = (clutaddr & 0x00FFFFF0) | ((clutaddrupper << 8) & 0x0F000000)
   getTextureAddress() = (texaddr  & 0x00FFFFF0) | ((texbufwidth  << 8) & 0x0F000000)
   ```

   Taking the low byte put the palette at `0x0016FC00`, inside the loaded
   module, instead of `0x0916FC00` — in RAM immediately below the texture it
   belongs to. **Indices were right the whole time, which is exactly why the
   shape was legible and only the colours were wrong.** That signature is worth
   remembering: legible-but-speckled means the palette, not the sampler.

   `PSPRECOMP_TEXDUMP=<path>` is what found it — it writes each distinct
   texture through the same sampler, so "the sampler reads wrong texels" and
   "the texture is not what we think" stop looking identical. The logo texture
   is an **alpha mask**: white throughout, letterforms in the alpha channel, cut
   out by the blend. Its RGB dump being uniformly white is correct.

   **There was a second speckle, and it *was* the sampler** (patches 0058–0061).
   The paragraph above is still true and its diagnostic rule is still worth
   keeping — but "legible-but-speckled means the palette" is a rule about
   *colours* being wrong. This one was about *coverage*: the letterforms came
   out with texel-sized holes punched along every edge, against a reference
   render whose edges are smooth.

   `sw_tri` evaluated its edge functions at the pixel **corner**, interpolated
   u/v there, and truncated. At a 1:1 blit — which is what this logo is — the
   exact u at a corner is an *integer*, so the sample point sits precisely on a
   texel boundary, and the few ULP of error in the barycentric reconstruction
   (`1.0f/(float)area`, then three multiplies and two adds) chose texel N or
   N-1 pseudo-randomly, per pixel. Inside a glyph both texels are the same and
   nothing shows; on its outline the neighbour is background. Sampling half a
   pixel across puts the point half a texel from that discontinuity and the
   error stops deciding anything.

   The measurement, on the best frame: **127 pixels of 130,560 changed**, all of
   them on glyph edges. That was the prediction before the change — both this
   and the fill-rule defect below live only on edges — and it is the kind of
   claim worth stating in advance, because "the picture looks better" cannot be
   distinguished from "the picture looks different" after the fact.

   Three things were found in the same pass and are worth separating:

   - **No fill rule.** `w == 0` was accepted in *both* winding branches, so a
     quad's shared diagonal rasterized twice. Opaque geometry hides it; with
     blending on it double-composites, on exactly the antialiased rim of each
     letter. A 13x11 quad wrote **170 pixels for 143 positions**. The fix
     normalises the winding first and then applies a top-left rule — applied to
     a mixed pair it would drop the shared edge instead of assigning it.
   - **Wrap modes did not exist.** `GE_TEXWRAP` was never decoded and the
     sampler clamped unconditionally. This game sets **repeat on both axes**, so
     the default was not a neutral one.
   - **`GE_TEXFILTER` was parsed and read by nothing.** `set_texture` had no
     filter parameter, so the state could not reach a backend even in
     principle. This game asks for **mag linear**.

   The sampling state is now in the GE report, which is what settled all three:

   ```
   sampling   filter min lin/mip-near mag linear, wrap s repeat t repeat
   ```

   **Bilinear changed nothing on this frame, and that is correct rather than
   disappointing.** Texel k covers [k, k+1), so the taps belong around u - 0.5;
   at an exact 1:1 blit the fractional part is zero, all the weight lands on one
   tap, and the result is bit-identical to nearest. That is the property that
   keeps a UI layer sharp, and `test_bilinear_equals_nearest_at_1to1` pins it.
   Bilinear buys smoothness where a blit is *scaled* or sub-pixel offset, and
   the mechanism there is specific: linear filtering ramps the mask's alpha
   across the glyph boundary, alpha test GEQUAL ref 1 kills only exact zero so
   the ramp survives, and blend src 2 / dst 3 composites it.

   One consequence for reading reference renders: PPSSPP screenshots are
   normally taken at a raised internal resolution, so some of the smoothness
   difference against ours is resolution and not correctness. The speckle was
   ours; the residual softness is not the same claim.

   `tests/test_raster.c` had ten tests and **no texture coverage at all** — the
   whole sampling path was unmeasured, which is how this survived. It now has
   seven more. Against the pre-fix build they produce 13 failures, and the shape
   of them is the diagnosis: at 1:1 the texels at k = 1, 2, 4 and 7 come back
   wrong while 0, 3, 5 and 6 come back right, which is the pseudo-random flip
   and not an off-by-one.

   **The intro movie plays.** With `PSPRECOMP_MPEG_DECODE=1` and patch `0030`'s
   clock fix, the decoder run reaches 13,064 GE lists and 1.7 billion pixels,
   and the frames are the real intro — ruined cityscapes, mechs, a lit sensor
   eye, weapon fire.

   A single frame looked washed out, which reads as a colour-range or blend
   fault. **It is not one.** `PSPRECOMP_FRAMES=<prefix>` dumps every Nth
   presented frame; across 215 of them the mean brightness sits between 49 and
   101 of 255 and touches 218 exactly once, at the shot that had been picked.
   The best-frame heuristic ranks by variation, which is right for "is there a
   picture" and biased towards precisely the bright high-contrast frame a
   wash-out would also produce — **one sample cannot separate those**, and
   reaching for the sequence is what settled it in one run.

   **The logo was 68px left of where it belongs. Fixed — it was the VFPU after
   all.** A reference render (PPSSPP) puts the glyphs at x 69..420, y 119..147.
   They now land at **x 68..416, y 118..147**, and the frame reads FROM SOFTWARE
   across the middle of the screen. Before the VFPU matrix work they were at
   x 1..349, y 118..148 — the y position and the width always matched, and only
   x was short, by exactly 68.

   The world matrix reaching the GE is now

   ```
   [0..2]  1  0  0        [3..5]  0  1  0
   [6..8]  0  0  1        [9..11] 68 117 0     <- translation
   ```

   which is exactly what the game's own object data says (`x 68, y 117`). It
   used to arrive with the 68 in `[7]` — column 2's y — and `[9]` zero, which
   is why there was no x translation at all.

   **Nothing in the renderer changed.** The fix is patches 0043/0044/0046:
   matrix orientation and sub-matrix addressing, `mtv`/`mfv`, and the operand
   prefixes. The investigation below is left in full because its conclusion —
   "the matrix is already wrong in the game's own memory, and the upload and
   the transform are both faithful to it" — was *correct*, and correctly
   located the fault upstream of everything it could reach. What it could not
   do was reach further: the code building that matrix runs on the VFPU, and

   > `psp_vtfm` and `psp_vmmul` are shared by the interpreter and the emitted
   > code, so the differential oracle can never see a bug in them

   is the sentence that entry ended on. It was right, and the instrument that
   could see them — pspautotests, run against real-hardware output — did not
   exist yet. Building it found seven VFPU bugs; this was one of the things
   they had been breaking.

   **The lesson is about the retraction, not the bug.** This entry retracted a
   VFPU lead (below) on the grounds that `0x002AFEF8` is never called. That
   retraction was correct and the *conclusion drawn from it* — that the VFPU
   was not involved — did not follow. One dead address does not clear a
   subsystem. Worth re-reading before the next time a negative result gets
   generalised.

   Everything the GE is given has been verified byte-for-byte:

   - the vertex layout — raw bytes `00000000 00000000 808080FF 00000000...`,
     stride 24, UV at 0, colour at 8, position at 12, model v0 = (0,0,0);
   - the viewport and offset — scale 240,-136, centre 2048,2048, offset
     1808,1912, **identical** to the fullscreen quads that land correctly;
   - the view matrix (identity) and the projection (plain ortho, 0..480 → screen
     1:1, so screen x equals model x).

   The world matrix arrives as `1,0,0 | 0,1,0 | 0,68,1 | 0,117,0`. Under the
   layout `sceGuSetMatrix` uses — a column-major 4x4 with the last row dropped —
   translation is the last group, `(0,117,0)`. The y is right and there is no x.
   **No reading of that matrix can put 68 into x**: with the vertices' z at 0 it
   can only ever affect y.

   And the game's own data says x should be 68. At `0x09ACE27C`:

   ```
   0.0   352.0   38.0   68.0   117.0   1.0   1.0
         width   height  x      y      scale
   ```

   The uploader is `psp_func_002B752C`, which packs 24-bit floats with `lwr` at
   offsets 4k+1 — `lui $t, 0x3B00` then `lwr $t, 1($a2)` — reading elements
   0,1,2, 4,5,6, 8,9,10, 12,13,14. So DATA word 7 is element **m[9]** and word 9
   is **m[12]**. Word 7 carried `0x428800` (68.0) and word 9 carried zero, which
   says directly that the source matrix has the x translation in `m[9]` —
   col2's y — instead of `m[12]`.

   `lwr` itself is fine: it yields exact floats (1.0 -> `0x3F8000`, 68.0 ->
   `0x428800`). And the source matrix has now been read directly at upload time:

   ```
   | 1  0  0  0 | 0  1  0  0 | 0  68  1  0 | 0  117  0  1 |
   ```

   `m[9] = 68`, `m[13] = 117`. So the **matrix is already wrong in the game's own
   memory** — the upload is faithful to it, and the transform is faithful to the
   upload. PPSSPP's `Vec3ByMatrix43` reads the world translation from the
   streamed array's `[9],[10],[11]`, which is exactly what this does, so the
   reading is not in question either.

   ### It is not proven to be codegen

   The obvious next move was the differential oracle, and it does **not** support
   the hypothesis. Every function around the upload matches:

   ```
   002B9548  002B938C  002B85A0  002B989C
   002B99C8  002B63C0  002B899C  002B752C     all: match 1, differ 0
   ```

   That is weak evidence — `--from` seeds its own inputs, so a divergence that
   only appears on real data would not show — but it is evidence, and it means
   the GU library is not where this goes wrong. Whatever builds the matrix from
   the object's `(68, 117)` is upstream of all of it and has not been found.

   ### The chain, walked -- and where it runs out

   `PSPRECOMP_WATCHMEM=<addr>[,<value>]` now answers "who wrote this word" --
   another instrument that was written, declared and never called. Following it
   upward:

   ```
   GE upload            psp_func_002B752C   packs 24-bit floats with lwr
     <- sceGumLoadMatrix  psp_func_002B99C8   16-word copy from a stack buffer
       <- psp_func_002AFEB4                   16-word copy
         <- ambiguous from here
   ```

   **It runs out at overlapping stack buffers.** LoadMatrix is fed from
   `0x09FBBCE0` and `0x09FBBD00`, which are **8 words apart**, so the address
   carrying the 68 is word 9 of one matrix and word 1 of the other. Attribution
   by address alone cannot separate them, and the static source both copies
   from holds zero at both of those words. Distinguishing them needs the watch
   to record the *base* a copy was made from, not just the address written.

   ### Retracted: the VFPU lead

   The previous version of this entry said the chain reached a VFPU matrix
   routine at `0x002AFEF8` that the differential oracle is structurally blind
   to, and named the behavioural oracle as the way to settle it. **`0x002AFEF8`
   is never called** -- `PSPRECOMP_WATCH` on it fires zero times. It was
   inferred from `psp_func_002AFEB4` sitting immediately before it in the
   disassembly, which is the same mistake as reading `psp_body_002741C4` as
   part of MovieReadThread: **adjacency in an address range is not
   involvement**, and this is the second time in one session it produced a
   confident wrong answer.

   The structural point in that entry still holds and is worth keeping on its
   own merits: `psp_vtfm` and `psp_vmmul` are shared by the interpreter and the
   emitted code, so the differential oracle can never see a bug in them, and
   `vfpu.c`'s own note says the matrix operand orientation is unverified. That
   remains a real gap and a real candidate for *some* future geometry fault. It
   is not evidence about this one.

   ### The autotests can answer it, and now do *(corrected)*

   This section previously said the behavioural oracle "cannot be run at all
   yet" because pspautotests needs a PSP toolchain and none is installed. Both
   halves were wrong. The tests ship as committed `.prx` binaries with
   `.expected` files holding real-hardware output, so no toolchain is
   involved; and they now run, emit, and are compared. Seven of the eight
   `cpu/vfpu` tests produce output, three run to their own
   `sceKernelExitGame`. See [autotests.md](autotests.md) for what it took --
   the load-bearing one being that **`$gp` was never loaded from the module
   info**, which no Armored Core run could have exposed (`-G0`) and the
   differential oracle excludes from comparison by construction.

   **It settled the orientation question, and the answer closed the logo
   offset.** `matrix.prx` now matches real hardware on all 50 lines. Three bugs
   stood between: `lv`/`sv` decoded the wrong `vt` field (the arithmetic ops'
   contiguous 22..16, where load/store puts the low five at 20..16 and the top
   two at 1..0, because 22..21 are the base register); a matrix register names
   a *sub-matrix* and both base offsets were being forced to zero; and `vmmul`
   and `vtfm` indexed their matrix operand transposed. `vfpu.c`'s note that the
   orientation was unverified is resolved -- it was wrong.

   The first of those **is** the 68-pixel logo offset. Fixing it moves the
   logo to x 69..417, midpoint 243 on a 480-wide screen -- centred. The `m[9]`
   versus `m[12]` observation was four lanes rotated by two, seen from the far
   end of the chain; that is why chasing the write that produced the 68 never
   reached a cause. The retraction above about `psp_func_002AFEF8` still
   stands -- that function is still never called, and was never the route.

   **Also left:** perspective-correct interpolation is absent (affine only,
   exact on a fullscreen quad), and there is no clipper.

   **And sub-pixel vertex precision.** `ge.c`'s `o->x = (int)sx` truncates a
   projected vertex to whole pixels, where the hardware rasterizes at
   sixteenths. This was deliberately *not* bundled with 0058–0061, because it
   does not cause the speckle those fix: truncating a vertex shifts a whole quad
   uniformly and the UVs stay coherent with the shifted geometry, rather than
   scattering texels. It matters for 3D silhouettes and for 2D elements that
   should slide smoothly instead of jittering a pixel at a time.

   `sw_tri` was written with the substitution in mind — `SUBPX`/`SUBPX_HALF`
   become 16 and 8, positions snap with `lrintf(x * 16.0f)`, and the integer
   edge functions and the top-left rule carry over untouched. The real cost is
   elsewhere: `ge.c`'s culling area becomes a float and `area != 0` stops being
   a reliable degeneracy test, so that wants snapping to 28.4 too. The tests are
   unaffected — they write guest memory, not `psp_vertex` structs. Note also
   that `(int)sx` is undefined for out-of-range floats, and `to_screen` only
   guards `clip[3] > 1e-6f`, so `sx` can reach ~1e9; a clamp belongs with this.

   **Still stored and never read:** `TEXFUNC` (`modulate()` is hard-coded, so
   REPLACE/DECAL/ADD would all draw as modulate — this game only sets modulate),
   and `MASKRGB`/`MASKALPHA`, logged by 0033 and never applied. `TEXSCALEU/V`
   and `TEXOFFSETU/V` (0x48–0x4B) are not decoded anywhere.

   One texture oddity, visible in the texdump and not yet chased: the logo is
   declared **512 wide with a stride of 352**, so `sample_texel` will clamp u to
   511 while a row is only 352 bytes, and texels 352..511 read into the next
   row. The alpha dump shows exactly that — the letterforms, then the start of
   the next row wrapping in. Harmless while nothing samples past 352.

1. **How the renderer got there.** *(Closed — kept for the measurements, which
   are the reusable part.)*

   `0025` split the declined-vertex counter by cause and found **all** 2,444
   were the missing transform, none of the other two; the arithmetic closed
   exactly, 2,958 − 2,444 = 514 = the 257 sprites × 2.

   `0026` added the pipeline. The viewport it read back — scale 240,−136,
   centre 2048,2048, offset 1808,1912 — is the textbook PSP arrangement, which
   is what made the decode believable rather than merely non-crashing.

   `0027` found `texture_usable()` demanding 5650 and non-swizzled where the
   game's textures are clut8 and swizzled, so **no texture had ever sampled in
   this game**; and `sw_tri` filling every pixel with `a->rgba`, ignoring UVs
   entirely. Added the formats, the swizzle, barycentric interpolation, and
   depth.

   `0028` found the white screen was a fade overlay — vertex alphas of 1, 3, 5
   composited opaque — and honoured blending.

   `0029` fixed the palette address, above.

   Two instruments were found dead along the way: `u_lo`/`u_hi` had been
   declared and printed since the file's first version and **never once
   assigned**, so every run ever made reported `u 0.0..0.0` as though it were a
   measurement; and the GE's "unsupported format" counter summed three
   unrelated causes into one number.

2. **Movie Sync has three signallers. None of them runs.** The frame consumer
   `0x40021` parks on it and never wakes; the movie never advances and never
   ends; `user_main` waits on the movie thread forever.

   ### Retracted: "nothing signals it, anywhere"

   That claim rested on a static search showing no instruction loads **offset
   100** — where SoundThread reads the uid — and reaches `SignalSema`. The
   search was correct and the conclusion did not follow. **Which offset a value
   sits at is an argument about one struct.** The same uid can live at two
   offsets in two objects, and here it lives at five addresses in RAM:

   ```
   findptr: 0x0004001C stored at: ram:0x08D3EAA0 ram:0x08D3EB0C
                                  ram:0x08EF4974 ram:0x08EF9D74 ram:0x08EFAFD4
   ```

   `0x08D3EB0C` is the MPEG context (`0x08D3E980`) **+ 396** — and 396 *is* in
   the set of offsets feeding `sceKernelSignalSema`. Confirmed by enumerating
   all 35 `SignalSema` call sites in the module and resolving every one's `$a0`:
   the offsets are {0, 4, 8, 12, 20, 36, 40, 52, 96, 128, 176, 180, 184, 396,
   668}. The initialiser stores the semaphore there itself, at `0x002733C4`:
   `sw $s1, 396($s0)`, two instructions before it sets `flag741`.

   **The lesson is the method, not the address.** A static claim of the form
   "nothing does X" needs the value's *identity* tracked, not one of its
   addresses. `PSPRECOMP_FINDPTR` now scans RAM and VRAM for exactly this.

   ### The three signallers, and why each is silent

   | signaller | where | status |
   |---|---|---|
   | `MovieDisplayThread` | `0x0027429C`, `0x002742C8` | **thread never created** |
   | the drain `psp_func_00273E30` | `0x00273EA4` | never reached |

   **`MovieDisplayThread` (entry `0x002741C4`) is a thread nobody knew about.**
   It has zero `jal` sites and is stored as a word nowhere, because its address
   is built for `sceKernelCreateThread` across a delay slot — `lui $a1, 0x27` at
   `0x0027477C`, `addiu $a1, $a1, 16836` at `0x00274798`, with the `jal` between
   them. Its creation is gated:

   ```
   00274770  lbu  $a0, 744($s0)
   00274774  beq  $a0, $zero, 0x002747E4     ; zero -> skip creating it
   00274794  jal  sceKernelCreateThread      ; MovieDisplayThread
   ```

   `flag744` is set to 1 by the initialiser at `0x002733C8` — and its only
   caller overwrites it with 0 four instructions later, at `0x0027262C`,
   alongside setting `flag741` from a runtime argument. **So this build asks for
   no display thread.** That is a decision the game makes, not a fault to fix,
   and it means the drain is the signaller that matters.

   It also carries a **second decrement of `struct[168]`**, at `0x00274278` —
   the frame-queue counter that item 3 says is decremented in exactly one place.
   There are two.

   ### What is actually left

   Measured with `PSPRECOMP_REACHED`, identically in both configurations:

   | block | reached |
   |---|---|
   | `psp_body_002729C0`, the display-side body | **yes** |
   | `0x00272D8C`, the block holding `jal 0x00273A50` | no |
   | `0x00273A50`, the drain's wrapper | no |
   | `0x00273E30`, the drain | no |

   `0x00272DA0` is the **only** `jal` to the wrapper in the whole module.

   ### The guard, and why it is a race rather than a condition

   `psp_body_002729C0` is the movie's display update. Its second instruction
   loads the object and its body is gated on one call:

   ```
   00272A2C  lw   $a0, 0($s5)
   00272A30  jal  0x0027399C
   00272A44  or   $s0, $v0, $zero
   00272A58  beq  $s0, $zero, 0x00272DB0   ; 0 -> skip the body, drain included
   ```

   `psp_func_0027399C` returns 0 on either of two fields being zero:

   ```
   0027399C  lbu $a1, 740($a0)      ; flag740
   002739A0  beq $a1, $zero, ...    -> return 0
   002739A8  lw  $a1, 156($a0)
   002739AC  beq $a1, $zero, ...    -> return 0
   ```

   Both branches share an exit, so reachability cannot say which fired — it
   only says the function **always** leaves by `0x002739C8`, the `return 0`
   path, and never reaches the checks beyond. `PSPRECOMP_PEEK` answers it
   directly, and the answer is that **neither field is zero**:

   ```
   peek: 0x08D3EC64  byte 0x01     context + 740 (flag740)
   peek: 0x08D3EA1C  word 0x01     context + 156
   ```

   `PSPRECOMP_WATCH` on `0x0027399C` fires **once**, with
   `a0 = 0x08D3E980` — the MPEG context itself.

   **So the condition is not false — at the moment it was asked, the movie had
   not started yet.** And the reason it is asked only once is not what it looks
   like.

   ### Correction: the update loop does not stop. The movie is simply last.

   An earlier version of this item concluded "the game polls the movie once from
   an update loop that only runs again after the movie finishes". That was
   wrong, and measuring it rather than reasoning about it is what showed why.

   `psp_func_0013680C` is the per-frame update dispatcher. It calls the movie's
   display update as **virtual slot `+0x4C`** of the child at `this->[16]` —
   `psp_body_0013B02C` has no `jal` sites at all and is reached only through the
   vtable at `0x003248A0`. Watching the dispatcher and dumping `this` on every
   hit gives the sequence of children it updated, run-length encoded, identical
   in **both** configurations:

   ```
   09ACDFA0 x34
   09ACD030 x153
   09ACDEC0 x1      <- the movie, and the last update of the run
   ```

   **188 updates, not one.** The loop was running the whole time. The movie
   becomes the current child at the very end and gets its first update — which
   correctly does nothing, because the movie has not started — and there is no
   second update.

   ### Why there is no second update

   That first movie update is where the movie is *started*, and it starts it by
   blocking the thread that runs the update loop. From the HLE log, all on
   `0x40001`:

   ```
   sceKernelStartThread(0x40021)   SoundThread
   sceKernelStartThread(0x40022)   MovieReadThread
   sceKernelStartThread(0x40024)   MovieDecodeThread
   sceKernelWaitSemaCB(0x0004001E) Movie Start
   ```

   So the update thread hands off to the movie and waits for it to report
   progress. On hardware that wait is satisfied repeatedly and the loop resumes,
   updating the movie each frame and draining its queue.

   Here `Movie Start` is signalled **once** — by MovieDecodeThread on its way
   out, after `sceMpegGetAvcAu` is refused. `0x40001` wakes, runs one frame,
   waits again, and by then both movie threads have exited. In the decoder
   configuration it sleeps instead and the run reaches the 60-second drain.

   **That is the deadlock, and it is one wait, not a chain of them:** the
   movie's own start step parks the thread that would otherwise drive the movie.
   Everything downstream — the drain never running, `Movie Sync` never being
   signalled, `struct[168]` never being decremented — follows from this and is
   not independently broken.

   ### Correction: Movie Start signalling once is a *default-configuration*
   fact, not a general one

   The uid lives at **context + 176** (`findptr` → `ram:0x08D3EA30`), and three
   `SignalSema` sites load that offset. The one that fires is `0x0027510C`,
   inside the AU-fetch wrapper `psp_func_002750C0`:

   ```
   002750FC  lw   $a0, 140($s0)          ; movie state
   00275100  bnel $a0, $s2, 0x00275124   ; state != 1 -> skip the signal
   00275108  lw   $a0, 176($s0)          ; Movie Start
   0027510C  jal  SignalSema
   00275118  sw   $a0, 156($s0)          ; struct[156] = 1
   0027511C  sb   $a0, 742($s0)          ; flag742  = 1
   ```

   Note what the signal is bundled with: **`struct[156] = 1` is the other field
   the display guard `psp_func_0027399C` tests.** Signalling Movie Start and
   declaring the movie ready are the same step — a handshake, not two
   independent things.

   How often that wrapper runs, measured:

   | configuration | calls to `psp_func_002750C0` |
   |---|---|
   | default | **1** |
   | decoder | **177,546** |

   So Movie Start is signalled once **because sceMpeg refuses**: MovieDecodeThread
   runs the wrapper once, is refused, prints its `Fatal Error!!!`, signals Movie
   Start on the way out, and tears down. With the decoder on it is signalled
   continuously. An earlier version of this item called the single signal *the*
   deadlock; it is the failure path's shape, and only in one configuration.

   ### What that leaves, per configuration

   - **default** — sceMpeg refuses, the decode thread aborts after one
     iteration, `0x40001` consumes the single Movie Start, waits again, and
     nothing alive can signal it. Force-stop. Nothing here is a psprecomp bug;
     the game is on an error path its authors did not expect.
   - **decoder** — *(fixed by patch `0030`; see below.)* Movie Start signalled
     continuously and `0x40001` woke each time, but the scene dispatcher still
     ran only its original 188 times.

   ### `MovieDisplayThread` can never exist in this build

   This is structural, and worth stating plainly because it removes a whole
   line of enquiry. The frame-queue consumer — the thread that signals Movie
   Sync twice and carries the second decrement of `struct[168]` — is created
   only when `flag744` is set. The **only** write of 1 is `0x002733C8`, in the
   initialiser; the initialiser has exactly **one** `jal` caller, `0x00272608`;
   and that caller writes a hard-coded 0 to `flag744` four instructions later at
   `0x0027262C`. So the flag is 1 for four instructions and 0 thereafter, on
   every path, always.

   The consumer therefore cannot be what drains the queue here, and the drain
   inside the scene update is the only remaining candidate — which is the thread
   that item 2 above is already about.

   ### Measured, post-0030: Movie Sync is a startup handshake, and the drain runs

   The whole reachability table from earlier in this item re-measured, in the
   decoder configuration with patch `0030` (a `TRACE=1` build,
   `PSPRECOMP_REACHED`, `PSPRECOMP_SEMA=Sync`, sixty seconds):

   | block | reached |
   |---|---|
   | `0x002729C0`, the display-side body | **yes** |
   | `0x0027399C`, the guard | **yes** |
   | `0x002739D0`, the guard's return-nonzero exit | **yes** |
   | `0x00272D8C`, the block holding `jal 0x00273A50` | **yes** |
   | `0x00273A50`, the drain's wrapper | **yes** |
   | `0x00273E30`, the drain | **yes** |
   | `0x00273E98`, the block signalling Movie Sync | **yes** |

   Everything is reached. The drain decodes, from the emitted C, into a
   **startup state machine on `struct[152]`**: while it holds 0 or 1, each call
   advances it by one and signals Movie Sync — which is why Movie Sync is
   signalled **exactly twice** per movie, and why the pre-0030 runs, which
   never got past the first update, saw zero. At `struct[152] >= 2` the drain
   switches to the retire path: `sceKernelWaitSemaCB(struct[180], 1, 0)` —
   **Movie Ring buffer sema**, counted against the *ring's* fill rather than
   the frame queue — then the `struct[168]` decrement at `0x00273E7C`, the
   matching signal, and finally a signal to `struct[184]`, which is
   `0x00040020` **Movie Display wait**. That last one is signalled 4,077 times
   and taken zero — its consumer is `MovieDisplayThread`, which item 2 above
   proves can never exist here. Growth without a taker is what that semaphore
   does in this build, by the game's own choice.

   Per-uid counts over a sixty-second decoder run (`PSPRECOMP_SEMA=Movie`):
   Ring buffer 5,208 signal / 5,208 take / 5,208 park, perfectly balanced —
   the video queue cycles at display rate. Sound lock 165,816/165,816, Movie
   Lock 8,154/8,154, Movie Start 53,306 signals against one take (each decode
   fetch signals it; only the startup handshake waits for it). Nothing here is
   stuck. **Item 3 below is thereby closed as a frame of its own: the queue
   full *is* the drain pacing the decode loop, not a deadlock.**

   ### The real blocker was end-of-stream, and patch `0051` is the fix

   With `0030` in, the movie plays — 1.68 billion pixels in sixty seconds,
   67 MB of stream read, `PSPRECOMP_FRAMES` dumps showing the actual intro —
   and then plays forever, because `sceMpegGetAvcAu` answered `NO_DATA` at the
   end of the stream, and `NO_DATA` is precisely what this game's decode loop
   reads as *go round again* (`0x0027528C`). Audio was worse: `GetAtracAu`
   succeeded unconditionally, so SoundThread pumped silence down its channel
   at a steady 55 thousand calls per minute for as long as the run lasted.

   Patch `0051` makes the end observable where it already was visible: the
   ring callback's contract says a short delivery is the end of the file, so
   `RingbufferPut` records it in the `es_eof` field that had been declared in
   the context since the decoder landed and never once read; `avc_pump` decodes
   the final NAL at end-of-stream instead of waiting forever for a successor
   that will not come; and both AU fetches return `SCE_MPEG_ERROR_INVALID_VALUE`
   once everything fed has been consumed — the code path this game's decode
   loop reads as *report and stop*.

   Measured with the fix: the first movie **ends cleanly, with no Fatal Error
   print**, its six semaphores torn down, and a second movie instance created
   with fresh uids — 18 creates across a 240-second run, i.e. three movies
   back to back. The intro sequence progresses. What it ends *into* is the
   next open question: at the 240-second drain `user_main` is still in
   `sceKernelWaitThreadEnd` and three fresh movie threads are alive, so the
   game is in its next-attract/loop state, unverified past that.

   ### ~~The pad cannot skip the intro, because nothing reads the pad~~ — WRONG

   > **Retracted.** The game reads the pad constantly, the intro *is*
   > skippable, and pressing circle skips it. This entry was written from the
   > top-12 call histogram, which is the trap two sections above this one —
   > "absence from a top-N list is not absence".
   >
   > The game polls with **`sceCtrlPeekBufferPositive`**, not
   > `ReadBufferPositive`. Both NIDs have always routed to the same handler
   > (`src/hle/misc.c`), so the input path was working the whole time; Peek
   > simply landed 12th-and-a-bit and never made the printed list.
   >
   > The summary now prints a `polls:` line unconditionally, so this cannot
   > recur: **209 polls** in the default configuration, **2494** in a
   > 40-second decoder run. Never zero.
   >
   > What actually failed was the *timing*: `PSPRECOMP_PAD_PRESS` slept out a
   > wall-clock delay on a detached pthread, so where the press landed in the
   > guest's instruction stream depended on host speed. It is now evaluated at
   > the read point against guest time (patch `0076`), and a full input
   > sequence can be scripted and replayed (patch `0077`,
   > `scenarios/README.md`).
   >
   > The original text is kept below because the reasoning is instructive.

   `PSPRECOMP_PAD_PRESS` (patch `0052`) presses a button at a wall-clock
   moment — the held variant cannot, because a game reads *pressed* as a
   transition and a button down before the first poll never transitions. The
   press fires; the game ignores it; and the histogram says why:
   **`sceCtrlReadBufferPositive` is called zero times in the whole run.** The
   movie loop does not poll the pad in either configuration. Either the intro
   is unskippable on hardware too, or input arrives through a path not
   implemented here (a sampling callback — `sceCtrlSetSamplingMode` is
   currently a no-op). Not a movie-gate problem; recorded so it is not
   re-chased as one.

3. **The decode loop's frame queue fills and its drain is never reached.**
   *(decoder)* The loop fetches — frames 1, 2, 3 with `ready=1` — then stops,
   because `struct[168]` reaches capacity `struct[172]` and the guard two
   instructions before the fetch turns it back:

   ```
   0027516C  lw   $a0, 172($s0)      queue capacity
   00275170  lw   $a1, 168($s0)      frames outstanding
   00275174  subu $a0, $a0, $a1
   00275178  blez $a0, 0x00275268    full -> return 1, go round again
   ```

   `struct[168]` is decremented in exactly one place, `0x00273E7C` inside the
   drain — which is not reached. Downstream of item 2.

   **Closed, post-0030.** The drain is reached (see the measured table in
   item 2), the retire path runs, and the Ring buffer sema balances at
   5,208 signal / 5,208 take per sixty seconds: the queue fills and drains at
   display rate. The pre-0030 numbers described a movie that never started;
   with `0030`'s clock fix and `0036`'s end-of-stream, the queue is simply the
   pipeline working.

4. **The two composed-chain oracle divergences.** Each callee agrees in isolation;
   the disagreement only appears in the chain. Needs instruction-level trace
   diffing, not another hypothesis — which means giving the recompiled side a
   per-instruction register dump to match `allegrexrecomp interp --regs`.

5. **Shared epilogues split into pseudo-functions — the remainder.** There are
   **zero** stack leaks; every imbalance is a *positive* delta, a continuation
   holding an epilogue without its matching prologue. The reported address is the
   **return instruction**, not the function.

   ```
   session start                    52 sites   7,067,959 returns
   fall-through merge               50         4,509,431
   computed jump jumps              37           889,348
   switch cases join their function 36             1,159
   ```

   Confirmed on a fresh `TRACE` build: **36 sites, 1,159 unbalanced returns, 0
   leaks**, the hottest at 85 hits.

6. **The guest's panic message.** Partly fixed by patch 0020: the synchronous
   write path already reached stderr (the movie's `Fatal Error!!!` line is the
   proof), but the *async* variant returned BADF for fds 1 and 2 and dropped
   exactly the write a panic path makes right before `abort()`. Closed and pinned
   by `test_stdio_async`. No run has yet reached the abort itself, so whether any
   text still fails to arrive is untested.

7. **`$k0` thread control block and reent.** Bring-up completeness, not a live
   blocker — only twelve sites in the whole module read `r_k0`.

8. **Make kernel waits block, then implement the five missing object types.**
   *(Closed 31 Aug — [autotests.md](autotests.md) item 26 and the entries
   after it. Waits block on the real scheduler, every object type the suite
   exercises exists, and `threads` went 0 → 89 of 127. Kept for the diagnosis,
   which was right.)*

   This was the next substantial piece, and the diagnosis was already done.

   `threads` is **0 of 127** matching hardware. Four real gaps were closed
   getting there (see [autotests.md](autotests.md) item 25: the entry thread's
   priority, NULL-name and attribute validation, the three `Refer*Status`
   calls, `sceKernelTerminateThread`) and the aggregate moved from 8,870
   differing lines to 8,057 — but **not one test crossed to a match**, because
   none of those were the blocker.

   The blocker is one line of behaviour. `hle_WaitEventFlag`, in
   `tools/psprecomp/src/hle/threadman.c`, ends an unsatisfiable wait with

   ```c
   psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
   ```

   rather than blocking, and only `WaitSema` and `WaitThreadEnd` call
   `psp_sched_block` at all. The threads suite is largely a test of blocking
   and wakeup **ordering**, and there is nothing here to order. On top of that
   `fpl`, `vpl`, `mbx`, `msgpipe` and `lwmutex` have no implementation
   whatsoever — `grep -c MsgPipe` over threadman.c returns 0.

   ### Settle the architecture before writing code

   The autotest path does not use the real scheduler, and there are two ways
   forward. The facts needed to choose are established:

   - `sched.c` runs each guest thread on a **host thread** with a handoff
     token, saving and restoring `psp_cpu` across switches, precisely because
     a blocked thread's control flow *is* its C stack. That reasoning applies
     to the interpreter unchanged.
   - `thread_main` calls `psp_dispatch(t->entry)`, and `interp.c` already
     installs a dispatch hook. So an interpreted thread could run inside a
     scheduler host thread with no new mechanism — but `g_active` and `g_nest`
     in interp.c are plain globals and would have to become thread-local, and
     `dispatch_hook` would have to *start* a run when there is no active one
     instead of declining.
   - The alternative is what is there now: `interp.c`'s `spawn_hook` parks
     threads that do not outrank their starter and drains them when the
     top-level run ends. It models "runnable is not running", which is real and
     measurable, but it cannot interleave — so it cannot satisfy a test that
     waits.

   ### Two things to carry forward

   `threads/events/events` regressed 16 → 22 differing lines under the parked
   model, and it *is* implemented, so that one is a genuine debt rather than
   churn in an unimplemented subsystem.

   A drain-on-block hook was written for exactly that case and then **removed**:
   measured across all 432 tests it changed nothing, byte for byte, because no
   wait path reaches `psp_sched_block` to trigger it. It becomes the right
   thing the moment waits block — but it was dead and untestable until then,
   and shipping it with a comment claiming otherwise would have been worse than
   not writing it. Expect to want it back as part of this item.

### Two measurements worth taking before building further

- **What the rasterizer costs on a real scene. — Measured. It is a placeholder,
  not the architecture.** `PSPRECOMP_GE`'s summary now carries a `raster time`
  line, timed around `sw_draw` with `CLOCK_MONOTONIC` (patch 0067).

  | | *default* | *decoder* |
  |---|---|---|
  | raster time, 60s run | 2.387 s | **48.318 s** |
  | pixels | 93,354,668 | 1,475,092,138 |
  | cost | 25.6 ns/px | 32.8 ns/px |
  | GE finishes | 212 | 3,741 |

  **The movie path spends 80% of wall-clock inside the rasterizer**, and that
  is with the guest paced in real time, so the two are competing for the same
  seconds. Per GE finish it is 394k pixels and **12.9 ms** — of a 16.7 ms
  frame. A single full-screen fill (130,560 px) costs **4.3 ms**, so the budget
  is about **3.9 screens of overdraw per frame**, and the movie already uses
  three of them drawing what is essentially a blit.

  That is the answer to the question as posed. Gameplay is strictly heavier
  than movie playback — real geometry, more passes, effects — so the software
  path will not hold 60fps on a real scene, and **GPU-backed display-list
  translation is required before anything is judged on how it plays**.

  What it does *not* mean is that the software rasterizer was a mistake or
  should be replaced now. It is what made the picture visible at all, it is
  exact, and it is the reference any GPU backend gets checked against — the
  backend interface (`psp_render_backend`) already exists for exactly this. It
  is correct and too slow, which is the right order to arrive in.

  Caveat on the number: `ns/pixel` is higher in the decoder run (32.8 vs 25.6)
  because that path is textured — 462M of its 1.47bn pixels sample a texture,
  against 2M of 93M in the default run. Do not read the two as the same
  workload measured twice.
- **The behavioural oracle.** Built, and at 129 of 432 tests matching hardware
  — see [autotests.md](autotests.md), `scripts/07-autotests.sh` and
  `scripts/08-autotest-sweep.sh`. When it is worked is now a rule in
  [../ROADMAP.md](../ROADMAP.md): a suite a milestone names, or one a game bug
  points at; otherwise it is a regression map, diffed per test. The differential
  oracle validates *translation*, and everything left is *environment*, which is
  missing from both sides and so agrees perfectly. The GE queue bug is the proof:
  the oracle held at 3110/3108/2 throughout, because both sides called the same
  broken runtime.

  **It has now found three game bugs the differential oracle cannot see**, which
  is the point of it existing. Two are recorded in autotests.md item 26 —
  `sceKernelCreateEventFlag` refusing a legal attribute (633 GE lists to 0), and
  `sceAudioOutputPannedBlocking` returning instantly so its thread spun. The
  third is `$gp`: a scheduler-spawned thread started with it zeroed, in a second
  place, and Armored Core is built `-G0` so nothing it does could ever have
  exposed it.

- **A spinning thread is not harmless because nothing has outranked it yet.**
  The audio thread had been running flat out since audio was implemented and
  cost nothing measurable, because it shared a priority with everything else and
  the timeslice rotated between equals. It starved the whole game the moment
  `sceKernelChangeThreadPriority` became real. Worth applying to the other
  never-blocking calls before priorities are trusted anywhere else.

## The fork

`tools/psprecomp` is a checkout of our fork of psprecomp, tracked like any
other submodule. It used to be a pristine upstream checkout with a 152-patch
series in `patches/` replayed over it on every build; everything that series
did is now history in the fork, one commit per patch, in order, each carrying a
`Last-Raven-Patch:` trailer naming the patch it came from.

That trailer is what keeps this document honest, since it names patch numbers
in about forty places:

```bash
git -C tools/psprecomp log \
    --format='%h %(trailers:key=Last-Raven-Patch,valueonly)' | grep 0021
```

`scripts/verify-patches.sh` is gone with the series. What it proved -- that we
have not silently diverged from upstream -- is now `git log upstream/main..`,
which answers the same question better.

**Not yet pushed**, so `.gitmodules` points at `/home/sif/Projects/psprecomp` --
the fork's actual location. That works on this machine and nowhere else, which
is the honest state of a fork that has not been published; pointing it at
upstream instead would be worse, since upstream does not have the SHA recorded
here. Changing it is one command, in the fork's `FORK-NOTES.md`, and until then
the submodule's remotes are the conventional pair:

    origin    /home/sif/Projects/psprecomp
    upstream  https://github.com/sp00nznet/psprecomp.git

## The regression checks, with the numbers they should produce

```bash
ctest --test-dir build/psprecomp -C Release --output-on-failure   # 13/13
scripts/05-oracle.sh 4000
```

```
attempted: 3957 functions
compared:  3073      match: 3071      differ: 2      dispatch miss: 0
```

**And the game, four ways.** The headless run is the bar the project had; the
three replays are the bar it needs, because the headless run ends at the logo
and cannot see anything past it. Re-baselined 3 Sep on `4192624` /
fork `72664ec`, every row re-run back to back on a quiet machine.

**The list counts here were wrong for a day, and the reason is worth keeping.**
`264a9b2` ("deferred GE -- queue on EnQueue, drain on Sync") made a list count
**once at enqueue** instead of once per stall-resumed run, so every count in
this table read three times high. Findings item 39 saw the drop, attributed it
to another session's uncommitted `ge.c` change, and left the rows alone -- but
the change was committed, and the rows stayed wrong through the whole of M3.
Lists now equal finishes on every row, which is the tell that the counter is
counting what its name says. Command counts were untouched by it: four of the
seven rows reproduce to the byte.

**The other three are down exactly 261 commands each, and that is not noise.**
`main-menu`, `garage` and `mission-1` each read 261 fewer than the numbers
written during the M2 gate; `title-idle`, `skip-intro`, `hanger` and
`new-game` are identical. `--repeat 2` on main-menu reports the guest-visible
trajectory identical, so the row is deterministic on this build, and the same
constant across three scenarios of very different length rules out run-to-run
variation. These three are also exactly the rows whose values were transcribed
*last* (item 38, 3 Sep), so whatever did it landed after that and before the
evening runs -- `998dc8e` (blend and stencil) and `264a9b2` are the
candidates. **Unexplained, and left that way deliberately**: 261 is small
enough to shrug at and constant enough not to be random, which is the profile
of the bugs this file keeps being written by. The new numbers are the
baseline; the question is whose 261 commands stopped being counted.

| command | expect |
|---|---|
| `scripts/06-boot.sh` (default) | 639 lists, 106,762 commands, 93,875,406 pixels, 0 bad mem; frames 0 / 751 / 9,020 |
| save and load (windowed, by hand) | Save from the garage writes `ms0:/PSP/SAVEDATA/NPUH10024ACLRSAVELIST00/` -- `SAVEDATA.BIN` 28,316 B, a valid `PARAM.SFO`, `ICON0.PNG`, `PIC1.PNG` -- and a later launch loads it into the hangar (3 Sep, M4's gate). No scenario covers it: `garage.pad` starts a new game every time, so a replay only makes the boot free-space call. `ms/` is gitignored; the save is the player's |
| `scripts/09-replay.sh --decode scenarios/title-idle.pad` | `stop` at poll 1800, 0 bad mem, 1,803 lists, 874,060 commands |
| `scripts/09-replay.sh --decode scenarios/skip-intro.pad` | `stop` at poll 2500, 0 bad mem, 2,503 lists, 2,280,256 commands, the menu in the displayed frame |
| `scripts/09-replay.sh --decode scenarios/hanger.pad` | **`stop` at poll 430, 0 bad mem**, 433 lists, 1,613,365 commands, the option menu over the hangar — the same scene as new-game.pad in 14 seconds rather than minutes (2 Sep); **the room renders since the `vidt` fix** (3 Sep, findings item 33) — walls, grating, doorway, light shafts — where it was nearly black; **floor, pillars and hazard stripes since the clip-space near clipper** (3 Sep, item 34), where they were shards — 7,944 near-cut vertices and 12,300 split-added in the summary, was 84 / 4,780 |
| `scripts/09-replay.sh --decode scenarios/new-game.pad` | **`stop` at poll 6000, 0 bad mem**, 70/70 events, 6,003 lists, 51,603,655 commands, the sound-settings panel in the displayed frame (1 Sep, night — M1's gate); **every glyph on it exact** since indexed draws were fixed (2 Sep); gate reproduces exactly behind a wider drain (3 Sep — it renders more now, 288 s of raster, so the default drain expires first) |
| `scripts/09-replay.sh --decode scenarios/mission-1.pad` | **`stop` at poll 1790, 0 bad mem**, 1,793 lists, 19,007,184 commands, the mission's opening with its chatter box; ~87 s wall, 56 ns/pixel (2 Sep, night — the first run into a sortie); **renders since the `vidt` fix** (3 Sep) — mean 84 against ~100, 4,200 colours, mech, buildings, smoke, lit horizon; **continuous ground since the clip-space near clipper** (3 Sep, item 34), where it was shards — same 1,793 / 19,007,184; **light-blue sky and far field since fog** (3 Sep, item 36), end-frame mean 92 against the reference's 89 |
| `scripts/09-replay.sh --decode scenarios/main-menu.pad` | **`stop` at poll 748, 0 bad mem**, 751 lists, 9,556,252 commands, the main menu with the AC standing behind it, GARAGE highlighted (3 Sep, recut from 810; findings item 38) -- mean 36 against the PPSSPP frame's 36 |
| `scripts/09-replay.sh --decode scenarios/garage.pad` | **`stop` at poll 925, 0 bad mem**, 928 lists, 11,948,318 commands, the sortie launch's AC in the hangar (3 Sep, item 38) -- mean 39 against 38 |
| `scripts/09-replay.sh --decode scenarios/look-probe.pad` | `stop` at poll 2620, 0 bad mem, 2,623 lists, 31,629,673 commands — the mission played into: a hard-left hold from poll 2100, centre, hard-right from 2360. With `PSPRECOMP_INPUT` unset this must not move, and did not when the two replacements landed (4 Sep); the same day mission-1, garage and main-menu reproduced their rows above to the command. The classic-mode check for `host/replacements.c`. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=modern --env PSPRECOMP_VIEW_LOG=reports/sweep.view --env PSPRECOMP_VIEW_LOG_WORLD=all --env PSPRECOMP_VIEW_LOG_POLLS=2210-2215,2290-2295,2370-2375,2450-2455,2530-2535,2610-2615,2690-2695 scenarios/yaw-sweep.pad` then `scripts/view-analyze.py reports/sweep.view --deltas` | `stop` at poll 3100, 0 bad mem, 3,103 lists, 41,601,520 commands (4 Sep). The largest cluster in each window is the camera's yaw per frame at that hold: **R127 −2.105, L96 +1.588, R96 −1.590, L64 +1.056, R64 −1.058, L32 +0.528, R32 −0.528** — the stick's deflection times the 2.1019°/frame cap. The functional gate for proportional turning. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_VIEW_LOG=reports/look.view --env PSPRECOMP_VIEW_LOG_WORLD=all --env PSPRECOMP_VIEW_LOG_POLLS=2130-2135,2210-2215,2290-2295,2370-2375,2440-2445,2500-2505,2570-2575 scenarios/look-sweep.pad` then `scripts/view-analyze.py reports/look.view --deltas` | `stop` at poll 2640, 0 bad mem, 2,643 lists, 33,857,103 commands, and the view log identical across two runs (4 Sep). Per window: **right stick 127 → −2.099°/frame, left 127 → +2.100, right 64 → −1.051, left 64 → +1.053, a mouse drag of −30 then +30 counts a poll → +1.683 / −1.684, the left stick at 32 → −0.002** — the second stick and the mouse turn, the first stick strafes. The functional gate for the look channel, and proof it replays. Unchanged at 2,643 / 33,857,103 with the pitch integrator replaced (5 Sep): the command count does not see the camera's pitch, so this row is not the pitch gate — the next one is. **33,988,063** with the expo curve on the look stick and the analog walk (5 Sep afternoon; its left-stick hold now strafes the game's left, which is why the count moved again); from the yaw log's new `rate` field: **right stick 127 → +2.102°/frame, −127 → −2.102, 64 → +0.585 (was 1.051), the mouse at 30 counts a poll → 1.719 unchanged, the left stick at 96 → 0.000** — a strafe, not a turn. **33,954,463** with the mouse bank and the walk cadence (5 Sep evening): the drag's first three polls, once lost while the turn state engaged, now land on the integrator's first frame, and the 96 hold walks at cadence 0.68. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/pitch.log scenarios/pitch-sweep.pad` then `scripts/pitch-analyze.py reports/pitch.log --from 2095` | `stop` at poll 2600, **0 bad mem**, 2,603 lists, **30,759,154** commands (5 Sep afternoon, eased and curved; 30,540,409 that morning with the linear, instant law, whose figures were −127 → +0.054000 from the first frame and −64 → +0.027213). The pitch law under `dual`: **right stick −127 ramps +0.005440, +0.010880 … +0.048960 over ten frames, then +0.054000 a frame — the classic ramp, from the game's own accel — to the clamp at +67.500°; +127 the mirror; −64 → +0.015031 rad/frame, the expo curve's 0.278 of the cap; a 50-count-per-poll mouse drag away then toward → +0.050000 / −0.050000, raw**; then triangle held from poll 2402 reproduces the classic ramp below to six decimals through the deferral, circle the mirror, and both together snap the angle to 0.000 with the lockout. The functional gate for proportional pitch. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT_LOG=reports/pitch-classic.log scenarios/pitch-sweep.pad` then `scripts/pitch-analyze.py reports/pitch-classic.log --from 2395` | `stop` at poll 2600, 0 bad mem, 2,603 lists, 32,344,849 commands (5 Sep). The game's own look law, with the stick and mouse rows inert: **triangle from poll 2402 ramps +0.005440, +0.010880 … +0.048960 over ten frames, then +0.054000 a frame to the clamp at +67.500° (poll 2428); release is a dead stop; circle mirrors it to −67.500°; triangle+circle at 2562 recentres to 0.000** and holds through the lockout. The classic-mode check for the third replacement. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/walk.log scenarios/walk-sweep.pad` then `scripts/walk-analyze.py reports/walk.log --from 2095` | `stop` at poll 2450, **0 bad mem**, 2,453 lists, **31,004,157** commands (5 Sep evening, with the walk cadence; 31,010,288 that afternoon with the cycle at one speed; 31,074,861 for an hour with the strafe sign inverted, which the first mission caught). The walk law under `dual`, from the push's log: **forward at 127 → 1.2868 units/frame, the legs' own cap, reached at frame 9 after the game's seven half-cap frames; 96 → 0.8756; 64 → 0.4511; 45 → 0.1990; straight right → 1.2868; back → 0.5814, the game's back cap**; and the position's direction of motion, right positive, is the AC's 70° heading plus the stick's angle in every hold — **forward +70.0°, right +160.0°, 30° left +39.8°, back −110.0°** — with no hold flagged `BLOCKED`; and the cadence column: **anim 2 at 1.000 for the full push, 10 straight right, 12 back, 4 for the 30° push; 0.706 at m = 0.680, 0.353 at 0.351, 0.118 at 0.155** — the cycle's frames advanced per game frame. The functional gate for the analog walk and its cadence. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=modern --env PSPRECOMP_INPUT_LOG=reports/walk-m.log scenarios/walk-sweep.pad` then `scripts/walk-analyze.py reports/walk-m.log --from 2095` | `stop` at poll 2450, 0 bad mem, 2,453 lists, 30,869,175 commands (5 Sep evening; 30,874,768 before the cadence). Same forward speeds as `dual` (1.2868, 0.8756, 0.4511, 0.1990) and the same cadences, the straight-right hold a 60° turn to the right and not a walk (no push lines), and the 30° push — X left of centre — a turn back to the left *and* a walk at 0.825 of the cap by Y alone, the direction of motion swinging from −127° to −108° over the hold. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/polish-walk.log scenarios/walk-sweep.pad` then `scripts/walk-analyze.py reports/polish-walk.log --from 2095` | Current single-radial-stage gate (5 Sep, late): `stop` at poll 2450, **0 bad mem**, 102/102 events, 2,453 lists, 29,060,192 commands. Full deflection stays `m=1.000`, speed and cadence 1.000. The 96/64/45 holds are now **m=0.739/0.455/0.286** with measured animation cadence **0.765/0.471/0.294**; direction remains exact. This supersedes the earlier double-deadzone magnitudes in the preceding rows. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT_LOG=reports/walk-c.log scenarios/walk-sweep.pad` then `scripts/walk-analyze.py reports/walk-c.log --from 2095` | `stop` at poll 2450, 0 bad mem, 2,453 lists, 30,237,942 commands (5 Sep). The game's own walk with the same file: **127 → 1.2868 at frame 9 after seven frames at 0.6434; 96, 64 and 45 → nothing (under the 100 threshold); the 30° push a straight walk at the full 1.2868; straight right a 59° turn; back 0.5814; cadence 1.000 on every cycle**. The classic-mode check for the fourth and fifth replacements. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_VIEW_LOG=reports/wt.view --env PSPRECOMP_VIEW_LOG_WORLD=all --env PSPRECOMP_VIEW_LOG_POLLS=2130-2135,2210-2215,2290-2295,2370-2375,2450-2455,2530-2535 scenarios/walk-turn.pad` then `scripts/view-analyze.py reports/wt.view --deltas` | `stop` at poll 2600, 0 bad mem, 2,603 lists, **32,910,374** commands (5 Sep evening, with the cadence — the 0.974 diagonal holds one frame in thirty-eight; 32,910,390 that afternoon, the analog walk: from the push log, forward at 1.2868 while the right stick turns the heading from +70° through +103° to +137° over the two turning holds, the forward-left diagonal at 0.974 of the cap at exactly −45° until it meets terrain late in the hold (`BLOCKED`), back at 0.5814; 32,921,535 with the eight-sector map on the two-state walk, whose figures follow; 32,902,603 under the first `dual` map, whose diagonal hold only strafed). Per window, yaw and the camera's own motion (x sideways, z forward): **forward alone 0°, z −1.23; forward + right stick −2.06°, z −1.17; forward + left +2.06°, z −1.17; back 0°, z +0.56**; the right-stick-alone window then reads −1.79° with the AC still drifting back at z +1.09 — the game slows its turn while moving, and under the first map, with the AC at rest, the same window read −2.10° and x +0.64 (the chase camera orbiting). The proof that the game walks and turns at once when both are asked for. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_VIEW_LOG=reports/sect.view --env PSPRECOMP_VIEW_LOG_WORLD=all --env PSPRECOMP_VIEW_LOG_POLLS=2130-2135,2210-2215,2290-2295,2370-2375,2450-2455,2530-2535,2610-2615 scenarios/sectors.pad` then `scripts/view-analyze.py reports/sect.view --deltas` | `stop` at poll 2680, 0 bad mem, 2,683 lists, **34,525,042** commands (5 Sep evening, with the walk cadence; 34,538,208 that afternoon, the analog walk at one cadence; 34,580,662 on 4 Sep under the two-state walk, when the same seven pushes moved the camera at the game's full cap in the table's eight directions: forward 0, −1.23; the 30° push −0.88, −0.87 — a diagonal, since the sector map could only ask for one of eight). With `PSPRECOMP_INPUT_LOG` and `scripts/walk-analyze.py`, the push's own log: **the AC moves at 0°, +30°, +45°, −45°, +135° and +90° from its heading, exactly the stick's angles, at 1.194 units/frame = 0.928 of the 1.2868 cap for these 120-unit pushes (0.5407 = 0.93 of the back cap on the back diagonal), and the half-deflection hold at 0.398 = 0.309 of it** — each reached, then three holds (the −45° diagonal, the straight strafe, the half-deflection forward) run into terrain and are flagged `BLOCKED`, because the corrected paths cross ground the eight-direction paths never did. Still a gate for direction and speed; `walk-sweep.pad` is the one laid out to stay clear. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/pause.log scenarios/pause-look.pad` then `grep play= reports/pause.log` | `stop` at poll 2450, 0 bad mem, 2,453 lists, **28,991,086** commands (5 Sep evening). Start at 2160 pauses the game for 190 polls — not one integrator call between 2160 and 2349 — and Start at 2340 resumes it; the converter's gate follows: **play=1 at 1291, play=0 at 2162, play=1 at 2351**, so the right stick, the mouse drag and the left stick held in the menu reach the game as the shipped converter's bits, and nothing turns. The gate for the look channel outside play. (The same count before the gate was fixed: the menu did not react to the bits either way, which is why a windowed check alone would have passed it.) |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/cam.log scenarios/cam-step.pad` then `grep ' cam ' reports/cam.log` | `stop` at poll 2160, 0 bad mem, 2,163 lists, **24,607,974** commands (5 Sep evening). A count a poll from 2090 engages the turn state at 2093 with the four banked counts applied at once; 200 counts at 2100 turn the AC 0.200000 rad in that frame; the chase camera then closes the gap by **ratio 0.830 a frame — 0.1698, 0.1410, 0.1170 … 0.0059 at frame 19**. With `--env PSPRECOMP_CAMERA_LAG=0.5`: 24,610,104 commands and ratio 0.500, under 0.001 rad by frame 8; with `=0`: 24,610,177 and the camera on the AC's heading the next frame. The gate for the camera filter and for the mouse bank. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual scenarios/modern-menu.pad` beside `scenarios/hanger.pad` | Both stop at poll 430 with **0 bad mem**, 12/12 events, 433 lists and **1,386,724 commands**, with the same geometry (5 Sep, late). The modern A/B carrier route is therefore menu-equivalent to direct PSP cross/circle input. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_RENDER=null --env PSPRECOMP_INPUT=dual --env PSPRECOMP_INPUT_LOG=reports/modern-buttons-shoulder-fix.log scenarios/modern-buttons.pad` | `stop` at poll 2280, **0 bad mem**, 105/105 events, 2,283 lists and 25,492,859 commands (5 Sep, late). The first run exposed a semantic-ID mistake during hands-on play: actions 8/9 are strafe left/right, not arm L/change unit. Corrected: LB answers action 7 and starts the starter blade's animation 49, RB answers action 4, RT/LT answer 6/5, and **Y+RT answers actions 16+6 together**. There are no semantic action 8/9 lines and no push at all during the LB-through-RB window. Right-stick pitch reaches 0.730800 rad before B; action 12 resets angle/rate to **0.000000/0.000000** and starts the original lockout. X has only its carrier edge. A/L3/R3 also have carrier edges but this starter AC has no inside, extension or OB/EO to query; their semantic hooks are equipment-conditioned. |
| `scripts/09-replay.sh --decode --env PSPRECOMP_AUDIO_DUMP=<prefix> scenarios/garage.pad` | **three channels of non-silent PCM** (3 Sep, item 49 -- M3's gate, headless half): ch1 5.5 s / peak 9,466 / ZCR 0.252, ch2 4.0 s / peak 32,768 / ZCR 0.294, ch3 15.7 s / peak 10,158 / **ZCR 0.075** -- the low crossing rate and the length make ch3 the music and the other two effects. In the same run `__sceSasCore` is called 2,712 times and `PSPRECOMP_ATRAC_LOG=1` shows 3 tracks opened, **125 `DecodeData`** and 2 `SetLoopNum`. This measures that sound is *generated*; *audible* is the windowed half, confirmed by Sif on 3 Sep (M3 struck) and re-checkable only by this dump |
| `scripts/08-autotest-sweep.sh` | **138 MATCH** of 432, 3 NOOUTPUT (3 Sep, after blend and stencil: no row moved; after fog: `gpu/commands/fog.prx` DIFFER 528 → MATCH and nothing else moved); `utility/msgdialog/dialog.prx` is back at DIFFER 139 since the interp's teardown fix (item 35); diff per test, the total is not a goal |
| `scripts/07-autotests.sh <dir with gpu/commands/blend.prx, blend565.prx + .expected>` | **every value matches** on both (3 Sep, item 37); the runner reports 128 / 140 differing lines, all of them the `[r]`/`[x]` checkpoint prefix, a scheduling artifact -- strip it before diffing |
| `scripts/07-autotests.sh <dir with gpu/commands/fog.prx + .expected>` | **MATCHES hardware**, all 272 rows including the 256 immediate-mode rounding rows (3 Sep, item 36) |
| `scripts/07-autotests.sh $PWD/game/pspautotests/tests/audio/atrac` | **decode, setdata, addstreamdata, atractest match on every value** (3 Sep, item 39: libavcodec behind sceAtrac3plus); getremainframe 6 and getsoundsample 4 lines on Reinit states and a streaming seek; the seek tests and stream.prx want what the item names. The runner links the test's directory into `disc/` now, without which every test here failed on `fopen("sample.at3")` |
| `scripts/07-autotests.sh $PWD/game/pspautotests/tests/cpu/vfpu` | **five MATCH of eight**, 13 s; `vector.prx` 16 lines (all `vasin`, sixth decimal), `prefixes.prx` 1 (a NaN's sign), `vregs.prx` 34 (`inf` for finite in the Upgrade/Combine rows); every `vidt`/`vrot` line matches (3 Sep, item 35). The directory must be absolute |

A replay without `--decode` is the default run with a script attached; see
the configuration note at the top of this file.

**This moved again, from 3006/3003/3, and both halves of the move are
explained.** More functions *compare* because more of the firmware they call is
implemented, so fewer runs are abandoned part-way — the same reason patch `0023`
moved it the last time.

The two that differ are **not codegen and not the same two as before**. Both
report `interp=00000000 recomp=FFFFFFFF` on registers and on the oracle's own
stack window at `0x09FE0000`, which is the 0xFF fill `sceKernelStartThread`
paints onto a fresh thread stack. `psp_sysmem_alloc(size, from_high)` hands out
from the top of RAM and the oracle's stack sits there unreserved, so a function
that starts a thread paints over the memory the comparison is about to read.

That is the **third** place this same overlap has appeared. The interpreter's
main context had it (fixed by taking its stack from the allocator) and so did
the thread stacks themselves. `host/oracle_diff.c` wants the same fix and has
not had it; until then the two divergences are an artifact of the harness, and
the number to watch is that they stay at two.

The three from the previous baseline:

```
0001093C  stack[09FFEF70] interp=00000001 recomp=09FFEF84
0001093C  module[00000000] interp=27BD01D0 recomp=27BDFFD0
00070760  stack[09FFFFB4] interp=0DEAD100 recomp=00000000
00195654  stack[09FFEF84] interp=005E2C2F recomp=003323A0
```

**This baseline moved from 3110/3108/2, and not for the reason it looked like.**
It changed when the interpreter began *serving* HLE re-entry rather than
skipping those runs (patch `0023`, from the merge), so the set of functions that
complete a comparison is different — `HLE re-entry: 0` in the skipped breakdown
is the tell. It was **not** patch `0030`'s clock change, which was the obvious
suspect and arrived in the same session: an A/B with the tick disabled
reproduces 3006/3003/3 and the same three functions exactly.

**Attribute a moved baseline by disabling the suspect, not by reasoning about
it.** Reasoning gave the wrong answer here, confidently.

**The limit is attempts, and it has to be this big to be comparable.**
`scripts/05-oracle.sh 400` attempts 396 and compares 316, all matching, because
the two known divergences are outside that sample. A clean run at the wrong size
is not evidence of anything — the positional-sampling trap from *Five ways to
measure nothing* wearing a different hat.

Read `LastTest.log` for the current ctest result. `LastTestsFailed.log` persists
from whenever a test last failed and will happily name a test that passes today.
