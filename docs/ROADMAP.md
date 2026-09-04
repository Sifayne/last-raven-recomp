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
stencil. Still affine, still undithered — that is what the M2 gate's frame
comparison had left over, and it is M5's. It costs 43–66 ns/pixel depending
on how textured the scene is: `mission-1.pad` spends 84.6 s of raster across
1,793 GE finishes, which is 47 ms of rasterizing per frame against a 16.7 ms
budget — **about 2.8× over, on the scene M5 has to hold at 60 fps**. It is the
reference any faster backend gets checked against, and it stays.

Sound: `sceSasCore` is a real synthesiser and all twelve `audio/sascore`
files are byte-exact; `sceAtrac3plus` decodes through libavcodec, and so does
the movie's audio substream. Reverb and the noise generator are the only
pieces not built, deliberately.

Saves: the game writes a real save to `ms0:` and loads it back — M4's gate,
passed 3 Sep.

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
- No dialog UI. The dialog auto-completes, as it does now.
- **One save slot, for now** (decided 3 Sep). "No dialog UI" above is about
  not drawing Sony's system dialog -- a list of slots with icons and a
  confirm prompt, firmware UI this project has no layer for. Choosing a slot
  is a separate matter that the phrase hid. Where the game names the slot in
  `param.saveName` we honour it and any number of slots work; AUTOSAVE
  already walks `saveNameList` for the first free one. The single slot is
  what is left when a caller leaves `saveName` empty and delegates the choice
  to the dialog: we take the first list entry every time.

  **Deferred, not dismissed.** The parameter block carries `focus`, which
  says which entry hardware's dialog would highlight -- first, latest,
  oldest, first empty, last empty. Reading it, together with the list,
  picks the slot the dialog would have defaulted to without drawing
  anything: about forty lines in `hle/utility.c`, where the offset is
  already defined and unread. Do it when a save appears that the game
  cannot reach, or before anyone calls this feature-complete.

  Whether this game is even affected is still open, though less so since
  Sif saved from the garage in a windowed run on 3 Sep: the game wrote
  `ms0:/PSP/SAVEDATA/NPUH10024ACLRSAVELIST00/` complete with 28K of save
  data, a well-formed PARAM.SFO naming the title, the pilot and the AC, and
  the game's own ICON0 and PIC1. So the writing half of the gate is met, by
  the game rather than by the suite. The slot-indexed name suggests the game
  numbers its own slots and passes each explicitly, which is the harmless
  case -- but a LISTSAVE whose list begins with that name produces the same
  directory, so it is not settled. Saving once more with
  `PSPRECOMP_SAVEDATA_LOG=1` prints the mode and whether `saveName` is set,
  and settles it in one line.

  **What is left of the gate: the load.** No scenario reaches it -- the
  garage recording starts a new game every time, so a replay only ever makes
  the boot free-space call. It wants a windowed relaunch that picks continue
  or load, or a recording that does.

## M5 — A GPU backend, and real time

**Gate.** The M2 mission scene at 60 fps at native resolution, and
pixel-comparable to the software path on a fixed set of display lists
(`test_raster` plus the scenario frame dumps).

- Behind the `psp_render_backend` interface that already exists
  (`tools/psprecomp/docs/RENDERER.md`). The software path stays the oracle.
- **The API is decided: OpenGL 3.3 core on SDL2** (3 Sep). RENDERER.md had
  said SDL3 + Vulkan since 20 Jul and was the document being read; it now
  carries the decision and the reasoning. In short: nine of the interface's
  twelve entry points are per-draw state setters, which is GL's model and not
  Vulkan's; the old plan's first increment is what `host/present.c` already
  does in SDL2; and SDL3 would land on the audio callback M3's gate rests on.
  Vulkan later if it earns it -- the interface is what makes that cheap.
- **Four prerequisites, none of them the backend.** The first is **done**
  (3 Sep): `psp_render_select` was called from `test_raster.c` and nowhere
  else, and is now wired to `PSPRECOMP_RENDER` in `host/boot.c`, with an
  unknown name fatal rather than a silent fall back to software. The other
  three stand: `psp_render_raster_ns` is cumulative and printed once per run,
  which cannot show 60 fps; the gate's "fixed set of display lists" has no
  capture or replay tool, since `09-replay.sh` replays controller input and
  `PSPRECOMP_FRAMES` dumps finished images; and `ge.c`'s pixel and depth
  counters are software-backend concepts that read zero under any other
  backend.
- Frame pacing on the vblank grid the clock already owns.

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
