# Roadmap — what to build, in what order, and how each step is judged

This is the plan of attack. [findings/state.md](findings/state.md) records what
the game does and how to measure it; this file says what happens next and why.
When the two disagree, this one is the intent and that one is the evidence.

Each milestone has a **gate**: a command and the number it must produce. A
milestone is done when its gate is in state.md's regression table and stays
there.

## Why this file exists

Written 2026-09-01, after two days in which the pspautotests conformance number
went from 35 to 129 of 432 while the game's own regression bar — the headless
intro run — stayed byte-identical. That bar ends at the FromSoftware logo. It
could not see the work, and it could not see that the actual frontier (the
title menu, reached on 30 Aug, and the fault behind New Game) had gone
unmeasured for two days. The harness to measure it was already built
(`scenarios/`, `scripts/09-replay.sh`) and had not been run.

So: milestones judged by what the game does, a regression bar that can see
past the logo, and an explicit rule for when the conformance suite is worked.

## Where things stand

**Refreshed 3 Sep.** This section had drifted to describing 1 Sep — five of
its claims were false by the time anyone read them again, which is the exact
failure the rest of this file is written to prevent. Re-check it whenever a
milestone closes.

Translation is finished — the differential oracle agrees on every function it
can compare, and nothing on the critical path is codegen. The environment is
where the work is, and most of it now exists: **207 of the module's 218
firmware imports are implemented** (`reports/03-imports.txt`). The 11 missing
are 7 in `sceUtility` and one each in `ModuleMgrForUser`, `sceImpose`,
`sceOpenPSID` and `scePower`. Networking is registered and refuses honestly
(M6's item, done 3 Sep).

Measured 3 Sep, decoder on, from `scenarios/`: the intro plays with sound,
circle skips it, the title menu renders headless (2,503 GE lists, 0 bad
accesses), NEW GAME reaches the sound-settings panel (6,003 lists, 0 bad
accesses), and `mission-1.pad` plays a sortie through to its chatter box.
Every row is in [findings/state.md](findings/state.md)'s regression table,
re-baselined the same day.

The renderer is a software rasterizer, exact where it is implemented and a
placeholder where it is not. Implemented since this paragraph last said
otherwise: all five texture functions, a clip-space near clipper and the
guard band, the scissor, mip chains, lighting, fog, the blend factors and the
stencil. Perspective-correct texture interpolation is now carried across the
backend seam. The GL backend uploads the complete declared mip chain and uses
the software oracle's per-primitive AUTO/CONST/SLOPE LOD and 1/16 filter rules;
its cache now tracks 256-byte guest-memory write generations, so unchanged
VRAM textures remain resident while in-place updates and render-target
readbacks invalidate exactly the ranges they touch. It is still undithered.
The software path costs 43–66 ns/pixel depending
on how textured the scene is: `mission-1.pad` spends 84.6 s of raster across
1,793 GE finishes, which is 47 ms of rasterizing per frame against a 16.7 ms
budget — **about 2.8× over, on the scene M5 has to hold at 60 fps**. It is the
reference any faster backend gets checked against, and it stays.

Sound: `sceSasCore` is a real synthesiser and all twelve `audio/sascore`
files are byte-exact; `sceAtrac3plus` decodes through libavcodec, and so does
the movie's audio substream. Reverb and the noise generator are the only
pieces not built, deliberately.

Saves: the game writes a real save to `ms0:` and loads it back — M4's original
gate, passed 3 Sep. Interactive slot selection is implemented (16 Sep) and drawn
to the PSP utility's own layout from real-hardware captures (17 Sep); player
acceptance on the trilogy is still open. See
[the shared save-dialog implementation plan](SAVEDATA-UI.md).

**Sibling titles (7 Sep).** Armored Core 3 Portable (NPUH10023) and Silent
Line Portable (NPUH10025) go through the same pipeline unmodified:
`GAME=ac3p` / `GAME=acsl` select a profile in `scripts/games/`, decode
coverage is 100% with 0 unknown words, 196 of their 210 imports are
implemented (Last Raven's gaps plus three InterruptManager sub-interrupt
functions nothing has called yet), and a blind probe reaches each game's
OPTION screen headless in 3,000 polls with 0 bad accesses.
[findings/sibling-titles.md](findings/sibling-titles.md) has the numbers.
AC3P's first sortie is reached blind, and the four control laws (converter,
yaw, look, walk) are ported to both titles through `host/ac3_controls.h`
and measured on AC3P, then confirmed on Silent Line against Sif's recording
of its first mission (`scenarios/acsl/mission.pad`).

## M1 — Past New Game ✅ 1 Sep

**Gate — passed.** `scripts/09-replay.sh --decode scenarios/new-game.pad`
reaches its `stop` at poll 6000 with `bad mem: 0`, 70 of 70 events delivered,
18,006 GE lists as the counter read them that day (**6,003** since `264a9b2`
changed what a list is -- see findings item 49); the displayed frame is the
game's initial sound-settings panel. `title-idle.pad` and `skip-intro.pad`
unchanged.

**What the fault was.** Not audio. The emitter never translated the four VFPU
condition branches (`bvt`/`bvf` and likely forms) — it emitted them as never
taken, silently — and the game's polygon clipper skips a store with `bvf`, so
the vertex count doubled at every clip plane and ran over the caller's frame.
The interpreter had the same gap, so the oracle agreed with the emitter; no
test used the branches. Three real audio lies had to be removed before the
fault was reachable from a clean state and the harness's instruments could
point at it. [findings/autotests.md](findings/autotests.md) item 27 has the
whole chain.

The steps as they were run, kept because the order and the corrections are
the reusable part:

1. ~~**The 13 `sceAtrac3plus` calls fail honestly.**~~ **Done 1 Sep, and
   corrected on the way** ([findings/autotests.md](findings/autotests.md)
   item 27). Refusing `GetAtracID` hung the game: its own open-failure path
   leaves the player thread spinning at priority 16 over the main thread.
   What shipped is a stand-in that opens the stream, answers the bookkeeping
   with hardware's numbers, and fails the decode — the one failure the game
   handles. The fault is gone: **0 bad accesses** on `new-game.pad`.
   - **1b. Done.** The wait that replaced the fault was ours too: the SAS
     mixer never ended a one-shot voice started in loop mode, and the game
     waits for its "decide" sound before leaving the title. Fixed to
     hardware's rule from `audio/sascore/vag.expected`; `SetGrain`,
     `GetGrain`, `SetOutputmode`, `GetOutputmode` registered with the codes
     the suite pins.
   - **1c. Done, and the feeder theory retracted.** PEEK showed the request
     counts and feeder words all zero; the gate was the player's buffered-PCM
     counter, fed from a `DecodeData` out-parameter the stand-in had not
     written. It now answers hardware's end-of-stream shape (`80630024`,
     samples 0, end 1, drained remainFrame). The sound system shut down and
     the original fault was reachable from a clean state.
   - **1d. Done — the cause.** TRACE build, `PSPRECOMP_WATCH` on the clipper's
     caller and clip routine, `PSPRECOMP_WATCHMEM` on the argument slot,
     `PSPRECOMP_VCMP_RING`: the clip stage doubled its output per plane
     because its `bvf` was emitted as `0 /* unhandled branch */`. Fixed in
     the emitter and interpreter with `psp_vfpu_cond`; `test_emit.c` pins it.
2. **`sceUtilityMsgDialog*` gets the savedata ratchet.** Four calls, same
   lifecycle (`src/hle/utility.c`). Unregistered, `GetStatus` is a permanent
   `NONE` — the non-terminating poll the savedata fix just removed, one
   subsystem over. `utility/msgdialog` is the oracle.
3. **Savedata writes `base.result`** — no-data on a load with nothing on ms0:.
   Still no filesystem; that is M4.
4. **The small ones.** `sceKernelDcacheWritebackRange` /
   `InvalidateRange` (no-ops), `sceUtilityLoadModule` / `UnloadModule`,
   `sceImposeSetLanguageMode`, `sceKernelStopUnloadSelfModuleWithStatus`,
   and the five `sceIo` directory calls over the existing host filesystem.
   Each removes a zero-return from the snapshot the boot host prints at the
   first bad access.
5. **Re-run the gate after each.** If the fault survives all of them, the
   three one-command steps the harness was built for, in order:
   `--stop 1` and read the `$ra` census and the zero-return snapshot;
   `--trace` with `PSPRECOMP_WATCH=0x0002E790` to separate "the caller passed
   garbage" from "this function computed it"; `PSPRECOMP_WATCHMEM` on the
   caller's `sp+1028` slot to name the write that put a float there.
6. **The VFPU is a suspect only if step 5 says so.** `cpu/vfpu/vector` has 33
   differing lines left and `prefixes` two. Check whether the instructions
   behind them occur in `psp_func_0002E790` or `0002E1D8` before touching
   either.

## M2 — A mission renders, in software ✅ 3 Sep

**Gate.** A recorded `scenarios/mission-1.pad` reaches a sortie; frame dumps
compared against PPSSPP reference shots, the way the logo was; `bad mem: 0`.

- **Done 1 Sep, first pass** ([findings/autotests.md](findings/autotests.md)
  item 28): the GPU suite's readback (`sceDmacMemcpy`) implemented, so the
  textured tests measure anything at all; texturing whenever enabled (texel
  0,0 without texcoords); the five texture functions with RGB/RGBA and
  doubling; the framebuffer alpha byte kept as the stencil; CLUT16/32; the
  one-axis-flipped sprite mapping. Sweep 129 -> 135. The game's frames did
  not change, so the smeared glyphs are none of those.
- **Done 2 Sep: the precision rules** (same item, second half). Screen
  positions are 12.4 fixed point; texture scale and offset are real registers
  and narrow texcoords are unsigned; bilinear weights are floored sixteenths
  and the blend truncates; texture dimensions saturate at 512. All four
  `gpu/filtering` precision tests exact, `textures/rotate` matching, `size`
  141 value-differences to 41, sweep 136. The game is unchanged in every
  count; the glyphs are still smeared, which now rules out six causes.
- **Next.** `gpu/filtering/nearest` and `linear` still differ on which texel a
  magnified sample lands on at half-texel offsets (262 and 335 values), and
  `gpu/textures/mipmap` at 190; that is the last sampling rule text could turn
  on. Then, in the order a mission needs them: skinning weights in the vertex
  decoder and a texture matrix (`gpu/texmtx`), the near-plane clipper and
  guard-band cull (`gpu/clipping`), and the `-inf` texture coordinates the
  settings panel reports. DXT only if the census names it.
- **A near-plane clipper and perspective-correct interpolation.** The camera
  is inside a large world; affine texturing skews on oblique geometry and
  `ge.c` currently drops any triangle touching the near plane whole.
  `gpu/clipping` is the oracle for the clipper.
- Speed is not a goal here. Headless runs are unpaced.

**Indexed draws (2 Sep).** The settings panel's fragmentary glyphs and the
hatched overlay were one bug: `GE_IADDR` was dropped and indexed vertex types
were read sequentially. Fixed; the panel and the option menu behind it render
exactly. Seven sampling fixes preceded it, each real, none this — see
autotests item 28's retraction and cause. Found by `PSPRECOMP_GE_WILDUV=1`,
which logs textured draws whose coordinates cannot be right.

**The clipper (2 Sep).** The "white pixels in lines" in the backdrop were
3D strips with vertices between the eye and the near plane, drawn as-is.
`gpu/clipping` gave the hardware's actual rules — with `DEPTH_CLIP_ENABLE`
**clear**, reject whole triangles with any vertex outside z/w ∈ [−1, 1]; with
it **set** the flag means *clamp*, so clip the near plane geometrically,
leave the far plane alone and pin the depth; and drop any triangle with a
vertex outside the 4096 guard band — and both tests now match on every value.
The same test found the rasterizer's hardcoded 480 bound; the scissor
registers are decoded and are the only bound now. Findings item 29.

**The clipper, in clip space (3 Sep).** The first clipper divided by w and
cut the near plane in NDC, which the oracle cannot distinguish from cutting
`z + w = 0` in clip space — every mixed-w vertex it poses sits exactly on the
plane. The hangar can: its wall pieces are drawn with the camera inside them,
so vertices behind the eye reach the GE at w < 0, and divided they land
mirrored with z/w inside the range, survive the NDC cut, and draw as shards
across the frame — the "vertex problems" in the option menu, and the black
floor. Cutting in clip space before the divide is the fix: the floor grid,
hazard stripes, pillars and doorway all appear. Findings item 34.

**Fog (3 Sep).** Decoded since item 33, applied now: `gpu/commands/fog`'s
768 channel values fit exactly one arithmetic, `(c·f + fog·(255−f) + 255) >> 8`,
and its Common rows give the coefficient, `(end − depth)·range` on the
eye-space depth with an infinite range reading as fully fogged. The oracle's
256 rounding rows draw through immediate-mode vertices (0xF0–0xF9), which are
decoded now too. fog.prx matches on every row; the mission's sky and far
field are the reference's light blue. Findings item 36.

**Blend and stencil (3 Sep).** `gpu/commands/blend`'s 64 rows fit one term,
`((c+1)·f) >> 8`, doubling as twice it clamped and inverse-doubling as the
factor 255−2a clamped at zero; the alpha byte every row reads is the
stencil REPLACE the test runs, so the stencil test and its operations now
exist in the rasterizer. Both blend oracles match on every value. Findings
item 37.

**Mipmapping (2 Sep).** Levels 1–7 were never decoded. The chain,
`TEX_LEVEL`'s three modes and bias, and the per-sixteenth blend now follow
`gpu/textures/mipmap`, which matches on every value (190 → 0). It did **not**
change the game: no draw in the settings scene carries a mip chain, and the
frame came back pixel-identical, so the backdrop's dotted trails were not
this. Findings item 30.

**Lighting (2 Sep).** `LIGHTING_ENABLE` was ignored, so 17.4M vertices of the
hangar scene were shaded with their raw vertex colour instead of the lit one.
Implemented from `gpu/commands/light` — a fixed (0,0,1) eye direction for the
half vector, `dot(L, D)` for the spot factor, and eye space throughout. It is
**not** why the hangar is dark: the game's own three lights cap the shaded
colour at 0x8C, so the missing brightness is in the compositing passes.
Next oracle there: `gpu/commands/blend`. Findings item 31.

**A mission, at last (2 Sep, night).** Sif recorded `scenarios/mission-1.pad`
— name entry, garage, mission select, a sortie, 108 seconds of play — the file
this gate has named since it was written. Its first replay showed every 3D
scene flat white with the 2D layer correct on top, and the pixel watch put
the cause in one line: a full-screen fade sprite whose vertex type carries
**no colour field**, which the decoder defaulted to opaque white. Hardware
gives a colourless vertex the material ambient colour and alpha (what
`sceGuColor` sets); with that, the mission renders. Findings item 32.

**Gate status.** The recording exists and reaches a sortie with `bad mem: 0`.
What remains is the comparison the gate specifies: frame dumps against the
PPSSPP capture at matching moments — the garage's AC, the main menu's mech,
the mission itself — and closing what they show. The first look said ours is
darker and less detailed than the reference (mean 38 against ~100), with
lighting reporting no lights on and fog absent entirely. Since the `vidt`
fix (3 Sep, findings item 33) the mission renders at mean 84 with 4,200
colours -- mech, buildings, smoke, lit horizon -- so the geometry gap is
closed and what is left is the passes. Fog is in (3 Sep, item 36) and the
mission's end frame sits at mean 92 against the reference's 89; the blend
factors and the stencil are in (item 37).

**Gate — passed (3 Sep).** Three frames against the capture, ours from
headless replays: the main menu's AC (`main-menu.pad`, recut to stop at 748),
the sortie launch's AC in the hangar (`garage.pad`, new), the mission at its
first chatter box (`mission-1.pad`). Layout agrees in the garage and the
mission; the main menu's camera orbits the AC, so its angle depends on the
second sampled, and everything else in the frame agrees. Brightness agrees to
within one level in all three: means 36/36, 39/38, 91/91. The composites are in
`reports/m2-gate/`. Findings item 38. What the comparison leaves is texture
-- affine interpolation and no dithering against a compressed capture -- and
speed, both M5's.

## M3 — Sound ✅ 3 Sep

**Gate — passed, in two halves.** Music and effects audible in a windowed run
on the garage screen; `audio/atrac` and `audio/sascore` worked targeted.

The measured half, 3 Sep (findings item 49): `garage.pad` replayed headless
under `PSPRECOMP_AUDIO_DUMP` writes **three channels of non-silent PCM** —
15.7 s at ZCR 0.075 on ch3, which is the shape of music, and 5.5 s and 4.0 s
at ZCR 0.25/0.29, which is the shape of effects. The same run calls
`__sceSasCore` 2,712 times and, under `PSPRECOMP_ATRAC_LOG=1`, opens three
tracks and runs 125 `DecodeData` with two `SetLoopNum`. So both generators are
running in the scene the gate names, and both are producing signal rather than
silence.

**The listening half, 3 Sep: Sif confirms the game's audio is fully audible
windowed and sounds correct, with nothing standing out as wrong.** That is the
half a dump cannot carry — a headless run has no speaker, and the numbers
above prove sound is *generated* and mixed, not that it comes out. The two
halves do different jobs and the milestone needed both: the ear is what made
the claim true, and the dump is what makes it re-checkable after every later
change.

`audio/sascore` is twelve of twelve byte-exact. `audio/atrac` matches on every
value in decode, setdata, addstreamdata and atractest; what still differs is
`sceAtracReinit`'s states and a streaming seek, neither of which this game
reaches — see the leftovers below.

- **ATRAC3+ through FFmpeg's `libavcodec`** — `find_library(avcodec)`,
  `PSPRECOMP_HAVE_FFMPEG`, dynamically linked, optional: the same shape as
  openh264 for sceMpeg, and the build and the headless host work without it.
  LGPL-2.1 goes in the README's licensing section beside openh264. Read the
  current libavcodec documentation for the `ATRAC3P` decoder's extradata
  contract before writing it; the RIFF `fmt` chunk of the game's `.at3` files
  is what feeds it. Implement the 13 calls to the streaming contract the game
  actually uses. The `.expected` files carry exact buffer-info numbers; read
  the layout out of them as `vpl/order`'s was.
- **SAS: done, all twelve files byte-exact** (3 Sep, findings items 44-48).
  `__sceSasSetVoicePCM` and `__sceSasGetAllEnvelopeHeights` were the only two
  of the game's 27 `sceSasCore` imports that were missing; from there the
  suite drove the rest. The ADSR curves and the parity rule that decides
  which phase takes which curve (item 45); the argument rules every setter
  enforces (item 46); the two clocks inside a voice, the filter table
  hardware reads past the end of, and `sceIoLseek32`, which had never been
  registered and made every VAG test read an empty file (item 47); then the
  guest-side struct hardware keeps up to date, the real `SetSimpleADSR`
  decode, and output mode 1's four mono blocks (item 48). What is *not*
  implemented, and is not measured by this corpus: reverb, and the noise
  generator. Reverb last, or never.

**ATRAC3+ (3 Sep).** libavcodec behind sceAtrac3plus, optional and dynamic,
the openh264 shape. The game holds each track whole and loops it forever;
decoded music comes out of the title and main-menu tracks headless. The
priming, first-decode, total, loop-point and ID rules are from the oracle
(findings item 39); decode, setdata, addstreamdata and atractest match on
every value once the runners let the tests open their data files. The host
mixes channels now instead of queueing them in turn. The intro movie's
ATRAC3+ substream goes through the same decoder (item 40), so cutscenes have
sound; their pops were ring starvation and their drift a picture clock
invented at 25 fps (items 40, 41), and the host's remaining 1.5% is paid by
dropping a late picture rather than showing it, which holds the two together
within a frame across the whole intro. A movie can now reach its end, which
nothing had done before: the player waits for the ring buffer to read empty
and deadlocks if it never does (item 42).

**What is left here, and why none of it blocks** (surveyed 3 Sep, item 49).
`_sceAtracGetContextAddress` is absent, and it is the one call `stream.prx`
needs before the streaming oracle will run at all — worth it only if a track
ever has to stream. `sceAtracSetSecondBuffer` is absent too, and both
spellings of `GetBufferInfoForRese[t]ting` are registered and refuse
deliberately; `ResetPlayPosition` is real but in-memory only. That whole
cluster is the seek tests, and `hle/atrac.c` records the reason to leave it:
**this game imports both reset calls and invokes neither**, so the work would
buy oracle rows and nothing the player can hear. The one cleanup worth doing
once — libavcodec's H.264 replacing openh264, leaving one optional dependency
instead of two — is small and contained (`CMakeLists.txt` L85–93, and three
sites in `hle/mpeg.c`); do it in M6's pass rather than paying a context switch
for it now.

## M4 — Saves ✅ 3 Sep

**Shared dialog implemented, 16 Sep; reviewed and restyled, 17 Sep:** slot
selection, confirmation/cancellation and selected deletion live in the shared
runtime and host; the overlay follows the PSP utility's layout (the save's own
PIC1 as backdrop, an icon column, the game's title/date/detail text, Cross and
Circle prompts) measured from real-hardware captures. Focused runtime,
software/GL and packaged UI checks pass. The [save-dialog plan](SAVEDATA-UI.md)
records evidence, the review's fixes and open items, and the remaining trilogy,
physical-controller and Deck acceptance checks.

**Gate — passed.** Save in the garage, quit, relaunch, load. `utility/savedata`
worked targeted.

Both halves are the game's, not the suite's. Sif saved from the garage in a
windowed run and the card took
`ms0:/PSP/SAVEDATA/NPUH10024ACLRSAVELIST00/`: 28,316 bytes of `SAVEDATA.BIN`,
a well-formed `PARAM.SFO` carrying the title, the pilot name and the AC name,
and the game's own `ICON0.PNG` and `PIC1.PNG`. A later launch loaded it and
came up in the hangar. The suite half is 8 of 14 tests byte-exact once the
`[r]`/`[x]` timing prefix is stripped, with the rest accounted for: free space
is the host's disk rather than a 16GB card (`getsize`, `sizes`), `idList` and
`bind` are written at a different point in the sequence (`idlist`, `filelist`),
`secureversion` measures the PGD crypto nobody attempted, and `loaddata` has
the one real divergence -- an empty `saveName` should fall back to the first
`saveNameList` entry and instead writes to the bare game directory. Findings
item 43.

- `ms0:` as a host directory, a PARAM.SFO writer, the savedata modes the game
  uses (measured from the HLE log, not the enum), the result codes, and the
  five `sceIo` directory calls from M1 made real.
- The original dialog auto-completed. Its list-save default and ignored focus
  have been replaced by explicit selection using the game's allowed slots.
- **Shared UI implemented (16 Sep).** Static inspection confirms list-save,
  list-load and latest-save focus setup in Last Raven, AC3 and Silent Line.
  The original temporary-card probe found false-success list-load and bulk
  list-delete behavior. Both now use a selection/confirmation lifecycle with
  error results and deletion of only the selected save.
- A better automatic default is insufficient for playing multiple saves.
  Keep PSP semantics and file operations in `tools/psprecomp/src/hle/utility.c`;
  put the overlay in new `host/save_dialog.c` / `.h`, with input/software
  presentation in `host/present.c` and GL composition in `host/render_gl.c`.
- **New gate:** save two distinct slots, relaunch and load each, cancel a save,
  decline overwrite, and delete only the selected slot. Validate all three
  titles, both renderers, real controller input, deterministic dialog tests
  and the packaged launcher-to-game path. Existing boot/garage replays do not
  exercise these decisions. Full sequence and acceptance matrix:
  [SAVEDATA-UI.md](SAVEDATA-UI.md).

## M5 — A GPU backend, and real time

**Gate — passed (4 Sep).** The M2 mission scene renders at native resolution
with 0.81 ms mean / 2.85 ms maximum measured draw-plus-blit time, comfortably
inside a 60 fps budget; the title itself presents new game frames at ~30 fps.
On a fixed 14,806-command display list, GL is pixel-comparable to the software
oracle at 122,818/130,560 exact pixels and 0.001168 normalized RMSE, with only
seven pixels differing by more than two channel levels (`test_raster` plus the
scenario frame dumps).

- Behind the `psp_render_backend` interface that already exists
  (`tools/psprecomp/docs/RENDERER.md`). The software path stays the oracle.
- **The API is decided: OpenGL 3.3 core on SDL2** (3 Sep). RENDERER.md had
  said SDL3 + Vulkan since 20 Jul and was the document being read; it now
  carries the decision and the reasoning. In short: nine of the interface's
  twelve entry points are per-draw state setters, which is GL's model and not
  Vulkan's; the old plan's first increment is what `host/present.c` already
  does in SDL2; and SDL3 would land on the audio callback M3's gate rests on.
  Vulkan later if it earns it -- the interface is what makes that cheap.
- **Prerequisites.** Runtime selection is **done** (3 Sep):
  `PSPRECOMP_RENDER` selects the backend and an unknown name is fatal.
  Display-list capture plus `host/gereplay.c` are also **done** (4 Sep), now
  including readable scene selection. The alleged empty-framebuffer copy was
  the first large list after a fade: its source buffers really were black, and
  its final destination-colour doubling pass correctly kept them black.
  `PSPRECOMP_GE_CAPTURE_MINCMDS` selects substantial work and the new
  `PSPRECOMP_GE_CAPTURE_MINMEAN` can additionally wait for a minimum visible
  RGB mean. Starting the hangar search at frame 300 with limits 5000 and 8
  captures one 14,806-command scene; software and GL replay the same primitive
  and vertex counts into readable frames. Its initial normalized RMSE of
  0.00572 falls to 0.001168 after the exact arithmetic work below.
  Still standing: `psp_render_raster_ns` is cumulative rather than per-frame,
  and `ge.c`'s pixel/depth counters are software-backend concepts that read
  zero under another backend.
- **Frame pacing on the vblank grid — done 4 Sep.** In a presented run,
  `GetVcount` and `GetAccumulatedHcount` are derived from the monotonic-backed
  guest clock rather than advanced by being read; the old read-driven counters
  let a cheap scene advance its own time simply by polling. Deterministic
  headless runs retain their synthetic read advance so a busy wait cannot
  deadlock the oracle. Presentation now finishes creating its SDL window, GL
  context and audio device before anchoring the real-time clock, so host/driver
  startup is not charged as the game's opening time. Every paced summary prints
  guest elapsed time beside wall elapsed time. On `hanger.pad`, null and GL
  reach the same 430th update at 14.769 s and 14.787 s respectively, with
  0.015/0.013 ms clock drift; before the startup barrier GL inherited roughly
  435 ms. The full GL mission reports 56.639 s guest over 56.639 s wall and
  0.018 ms drift, with its framebuffer byte-identical to the mip/cache baseline.
- **The backend lives in the host, and the context on the GE thread** (3 Sep,
  findings item 51). SDL2 is the host's by policy, so a backend needing a
  window cannot live in the runtime; `psp_render_register()` is the seam, and
  it refuses a backend missing any of the twelve entry points. The GE is
  driven by **exactly one host thread** -- measured, and now reported in every
  run -- so the SDL thread creates window and context and releases it, the GE
  thread claims it, and present swaps from there. No command queue. A GL
  backend must fail loudly if that thread ever changes. One consequence:
  GL needs a window even when hidden, so software-versus-GL comparison runs on
  a desktop, and the software path stays the CI oracle.
- **The framebuffer plan, from the census** (item 50). Own a GL colour buffer
  for the display pair -- 99.88% of drawing -- and present by blitting it,
  with no guest memory in the path. Switch to an FBO-backed texture for the
  one off-screen target, which is two switches a run. Read back into guest
  memory *lazily*: nothing forces a per-frame `glReadPixels`, but the
  project's own instruments (`score_frame` and `dump_frame_seq` in
  `display.c`, `dump_framebuffer` and `survey_vram` in boot.c, and the frame
  comparison that passed the M2 gate) all read guest framebuffer memory, and
  a backend that never writes it makes every one of them blind.
- **Build the 1x path, but know it is the 1x path.** See M7: the interface
  hands the backend screen-space vertices that `ge.c` has already transformed
  and quantized, which is the right input for matching the oracle and the
  wrong input for rendering at a higher internal resolution. Do not try to
  retrofit scaling onto it; M5's gate is a pixel comparison, and that gate is
  only meaningful at 1x. The adaptive-aspect target (`PSPRECOMP_ASPECT=window`,
  5 Sep) is still this path: 1x vertically, and horizontally the same
  PSP-precision geometry spread over the window's width by the viewport --
  what the present blit did before, rasterised at the target's resolution.
- **Mipmaps and LOD — done 4 Sep.** Cache identities include every active
  level's address, stride and dimensions; all levels are decoded and uploaded.
  Software and GL share the PSP's AUTO/CONST/SLOPE calculation, including its
  signed 1/16 bias, and GL performs the measured in-level and between-level
  filtering explicitly rather than inheriting OpenGL's different switch-over
  rule. `mission-1.pad` uploaded 47,779 chains / 95,558 extra levels with zero
  incomplete chains. Against the same deterministic software end frame, RMSE
  improved from 0.01532 to 0.01418.
- **Content-aware texture cache and cadence measurements — done 4 Sep.** Every
  scalar and bulk guest-memory write advances a 256-byte range generation;
  direct HLE writers, GE render-target readbacks and capture restores mark
  their ranges explicitly. A cache entry records the generations of all active
  mip levels and the reachable CLUT, taking an O(1) hit while the global write
  serial is unchanged and scanning only after some guest memory changed.
  Direct-colour entries ignore incidental CLUT state, and a 32-entry bounded
  probe retains the least-recently-used working set rather than thrashing its
  first collision. `mission-1.pad` fell from 232,385 uploads to 1,073
  (**99.5% fewer**) with 898 real dirty invalidations, 175 cold misses and only
  16 evictions; 159 of the 512 slots are resident at the end. Its final
  framebuffer is byte-identical to the
  pre-cache mip/LOD frame; the fixed `big.gcap` frame is also byte-identical to
  both its previous GL result and the software oracle. Texture binding,
  including generation checks, decoding and driver upload, costs 1.157 s over
  the 56.6 s mission; readback costs 0.832 s.

  Cadence is not scene-complexity dependent in the two measured paths. The
  mission renders 1,791 new frames over 56.6 s with 33 ms median / 34 ms p95
  intervals; the hangar has the same 33/34 ms steady intervals. The game makes
  two `sceDisplaySetFrameBuf` calls for each new frame, which the report keeps
  separate rather than mislabelling as 60+ fps. This is a stable ~30 fps game
  path, not a GL workload falling behind in the mission. Any remaining
  perceived game-speed difference belongs in the clock/game-timing pass rather
  than another texture optimisation.
- **GPU frame timing — done 4 Sep.** An eight-query nonblocking ring measures
  native-resolution draw work through the final blit without waiting for an
  unavailable result; a full ring is counted rather than stalled. The mission
  produces 1,791 samples at 0.80 ms mean, 0.8 ms p50, 2.1 ms p95 and 2.67 ms
  maximum, with no dropped measurements. The exact-list hangar costs 1.27 ms,
  and both its image and the mission end frame are byte-identical before and
  after the instrumentation. The observed 33--35 ms game-frame cadence is not
  GPU saturation: there is ample headroom inside a 16.7 ms rendering budget.
- **Exact fragment, blend and raster arithmetic — done 4 Sep.** GL now
  quantizes interpolated colour and texture-function results at the same RGBA8
  boundaries as the software oracle, applies the measured integer fog rule,
  honours the alpha-test mask, and rewrites masked source-alpha blends so the
  GE's separately truncated terms survive GL's combine-then-round pipeline.
  A 1/256-pixel vertical raster bias reconciles GL's lower-left half-open edge
  ownership with the PSP's top-edge rule after the Y flip; it removes the
  missing horizontal rows on half-pixel UI boxes. Implicit desktop-GL dithering
  is disabled until GE dither state is represented explicitly. On the fixed
  14,806-command hangar, exact pixels rise from 62,870 to 122,818 of 130,560
  and normalized RMSE falls from 0.005718 to 0.001168 (79.6%); only seven
  pixels differ by more than two levels. The full mission remains stable at
  zero bad accesses, +0.013 ms clock drift and 0.81 ms mean / 2.85 ms maximum
  measured draw-plus-blit time.

- **Expanded fixed-list regression coverage — done 4 Sep.** Six captures now
  cover the garage menu, launch, mission ground/smoke, and combat firing/boost
  input. `scripts/11-render-check.py` records them in one poll-stamped run and
  replays each twice per backend, requiring identical repeats and unchanged
  command counts. Saved capture hashes and software-output hashes anchor
  full-frame and lower-left-region regression checks. All six normalized
  RMSEs are between 0.002474 and 0.003105; complete results and commands are in
  `docs/RENDER-CHECKS.md`. Combat adds coverage absent from the earlier gate:
  each combat frame enables stencil on 801 draws and submits 8/9 lines, which
  were omitted at that checkpoint (addressed below). The full run also changes a scratch target's
  format after its first allocation; that history-dependent case needs its
  own regression. These are the next correctness targets, rather than evidence
  of complete PSP rendering from a small RGB error alone.

- **HUD lines and RGBA8888 stencil — done 4 Sep.** The missing targeting
  outline was eight skipped line segments, not a texture or game-logic bug.
  Both backends now render points/lines/line strips, including strip batch
  boundaries and transformed near-plane clipping. GL's stencil is synchronized
  with framebuffer alpha by lazy GPU passes; captures exercise 801 supported
  stencil draws with one import/export apiece. `scripts/12-render-tests.sh`
  passes 430 exact RGBA assertions on each backend and caught a software
  alpha-only-clear bug, now fixed. All six fixed-scene comparisons remain
  stable or improve, with no skipped lines or unsupported stencil in those
  scenes. Next: target format/size reuse across frames and reduced-bit-depth
  stencil (30 unsupported draws remain in the full replay); remaining blend
  modes/dithering and wider scene validation follow. The full 2210-poll run
  remains paced, with zero bad accesses and 0.98 ms mean GPU work.

## M6 — Ship shape

- Publish the fork and point `.gitmodules` at it (`FORK-NOTES.md` in the
  fork has the three commands). Until then a fresh clone cannot build.
- Windows: `src/os.c`'s Win32 half has never been compiled. `mingw-w64-gcc`
  is packaged on this machine and not installed; compile-check it, then a CI
  matrix.
- ~~Networking: the 27 `sceNet*` / `sceNetAdhoc*` imports registered to fail
  honestly, so the multiplayer menu cannot hang on a zero.~~ **Done 3 Sep**
  (`50ec2b6`). The refusal is split where hardware splits it: `sceNetInit`
  validates its pool arguments and succeeds vacuously, the adhoc and adhocctl
  inits refuse outright because radio firmware cannot exist here, and
  everything behind them reports the prerequisite it will never meet with its
  out-parameters untouched. The game calls none of them on any measured path,
  so this is preventive.
- Upstream: the PRs drafted in [upstream/README.md](upstream/README.md), in
  its suggested order.

## M7 — The PC port's own settings

**First delivery implemented 6 Sep: settings before launch, with saved presets.**
The immediate need is to stop remembering environment-variable recipes for
normal play. The [settings plan](SETTINGS-PLAN.md) inventories the existing
options, records their defaults and describes the implementation and checks.
Open `scripts/15-settings.sh`; [the settings guide](SETTINGS.md) covers presets,
navigation, storage and overrides. Validation is recorded in the settings plan.

- [x] **Shared configuration and saved presets.** One typed definition of
  each player option, consistent validation and effective-value reporting.
  Resolve defaults, the selected preset and explicit environment overrides
  in that order; do not save temporary overrides into the preset. Migrate
  graphics and controls consumers first, preserving direct boot defaults.
- [x] **Pre-launch screen.** Preset selection and management, Graphics,
  Controls and Advanced pages, reset defaults, Save & Play, Save and Cancel.
  Provide Classic, Controller and Mouse & Keyboard starting presets. The
  launcher passes an explicit config path and preset to the boot executable;
  replay and regression tools do not automatically load personal settings.
- [ ] **In-game access and live changes, later.** Input ownership, safe
  cross-thread updates and host redraw while paused need their own work.
  Existing settings cache values at startup; a widget alone cannot change
  them safely during play. Initially apply settings on the next launch.

**The environment inventory had grown.** The 6 Sep pre-migration audit found 74
distinct runtime `PSPRECOMP_*` option names in 14 host/runtime C files,
including the six options passed through named parsing helpers. Most are
diagnostics and need no ordinary menu row. New player options should use the
shared configuration layer; migrate diagnostic readers
incrementally rather than making all 74 a prerequisite for the screen.

**Window-resolution rendering is already available, independently of the
screen.** `PSPRECOMP_RESOLUTION=window` enables physical-resolution GL
rasterization, and `PSPRECOMP_ASPECT=window` independently enables the wider
view. Guest framebuffer layout remains PSP-compatible, including CPU writes,
readback and alpha/stencil semantics. The [resolution plan](NATIVE-RESOLUTION-PLAN.md)
and [validation results](RENDER-CHECKS.md#window-resolution-rendering) record
the implementation and remaining limitations. Expose these existing modes;
fixed scale factors and supersampling can follow separately.

**Higher frame rates remain research, not a ready settings row.**
The [framerate investigation](HIGH-FRAMERATE-RESEARCH.md) finds a faster loop
mode that also speeds up gameplay, and the [tick map](GAME-TICK-MAP.md)
investigates separating simulation from rendering. A correct enhancement
needs game-time validation beyond a faster present or a matching poll-based
replay. This work does not block saved settings or the launcher.

## The autotests policy

`scripts/08-autotest-sweep.sh` is a **regression map**. It is re-run after
any runtime change and diffed **per test** against the previous run — the
`join` in the script's header — and a test that moves the wrong way blocks.
The matching total is not a goal and is not raised for its own sake.

A suite is worked when a milestone names it — M2: `gpu/texfunc`,
`gpu/texcolors`, `gpu/clipping`; M3: `audio/atrac`, `audio/sascore`; M4:
`utility/savedata`, `utility/msgdialog` — or when a game bug points at the
library it covers.

Never worked: `font`, `jpeg`, `net`, the `rtc` calendar, `power/freq`,
`intr`, `mstick`, `ccc`, `dmac`, `video/psmfplayer`. The game imports nothing
from them.

The tests within a few lines of matching are not a goal either. One is picked
up only when a milestone already has that file open.

## The regression bar

Replaces the single headless number. Every row is re-run after a runtime
change; the expected values live in state.md's *regression checks* section
and are updated there when a milestone moves them.

| check | what it can see |
|---|---|
| `ctest --test-dir build/psprecomp -C Release` | the runtime's own unit tests |
| `scripts/05-oracle.sh 4000` | translation — must stay at its known two harness artifacts |
| `scripts/06-boot.sh` | the headless intro, default configuration: boot to the logo |
| `scripts/09-replay.sh scenarios/title-idle.pad` | the control: as far as the game goes on its own, no input |
| `scripts/09-replay.sh scenarios/skip-intro.pad` | the title menu, reached by pad |
| `scripts/09-replay.sh --decode scenarios/new-game.pad` | the M1 gate |
| `scripts/08-autotest-sweep.sh` | the conformance map, diffed per test |

## Not doing

- Rebasing the module to `0x08804000`. Emitted function names are addresses;
  it would rename every one for about thirty lines of tests. Parked in
  [findings/autotests.md](findings/autotests.md).
- Ad-hoc multiplayer beyond honest failure.
- Reverb, DXT, CLUT16/32, unless a census names them.
- Raising the sweep number.

## Keeping this file honest

When a gate is passed, move its numbers into state.md's regression table and
strike the milestone here with the commit that did it. When a milestone turns
out to be wrong — the wrong order, the wrong gate — change it here and say
why in the commit, the way state.md keeps its retractions. A plan that is
never corrected was never checked.
