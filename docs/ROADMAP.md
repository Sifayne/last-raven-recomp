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

Translation is finished — the differential oracle agrees on every function it
can compare, and nothing on the critical path is codegen. The environment is
where the work is: 154 of the module's 218 firmware imports are implemented
(`reports/03-imports.txt`). Of the 64 missing, 27 are ad-hoc networking, 13
are ATRAC3+ — all of the game's music — and the rest are small.

Measured 1 Sep, decoder on, from `scenarios/`: the intro plays, circle skips
it, the title menu renders headless (7,506 GE lists, 0 bad accesses), and by
the end of the day NEW GAME runs through to the game's sound-settings panel
(18,006 GE lists, a million textured draws, 0 bad accesses) — M1's gate. The
fault behind it was a codegen bug in the VFPU condition branches, reached
only after three audio lies were removed; see M1 below. The full table is in
[findings/state.md](findings/state.md) under *pad-driven*.

Known from that run and belonging to M2: the panel's glyphs are smeared
(affine texturing), and the GE reports non-finite texture coordinates from
the new geometry.

The renderer is a software rasterizer that is exact where it is implemented
and a placeholder where it is not: one texture function of five, no clipper,
affine interpolation. It costs 12.9 ms of a 16.7 ms frame on the intro movie.
It is the reference any faster backend gets checked against, and it stays.

Sound: `sceSasCore` is a real synthesiser (VAG decode, ADSR, 32 voices, SDL
output) with seven no-op entry points. `sceAtrac3plus` does not exist.

Saves: the savedata dialog runs its lifecycle and completes; nothing is read or
written, and the result code is never filled in.

## M1 — Past New Game ✅ 1 Sep

**Gate — passed.** `scripts/09-replay.sh --decode scenarios/new-game.pad`
reaches its `stop` at poll 6000 with `bad mem: 0`, 70 of 70 events delivered,
18,006 GE lists; the displayed frame is the game's initial sound-settings
panel. `title-idle.pad` and `skip-intro.pad` unchanged.

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

## M2 — A mission renders, in software

**Gate.** A recorded `scenarios/mission-1.pad` reaches a sortie; frame dumps
compared against PPSSPP reference shots, the way the logo was; `bad mem: 0`.

- **Let the GE census drive it, not the test list.** `PSPRECOMP_GE` already
  reports which texture functions and formats the stream used. Implement what
  the mission scene uses; do not implement what it does not.
- Likely needed: the four texture functions besides modulate (`render.c`
  hardcodes modulate at both call sites; `gpu/texfunc` is the oracle), the
  16-bit texture formats (`gpu/texcolors`, eight lines each), and CLUT16/32
  or DXT only if the census names them.
- **A near-plane clipper and perspective-correct interpolation.** The camera
  is inside a large world; affine texturing skews on oblique geometry and
  `ge.c` currently drops any triangle touching the near plane whole.
  `gpu/clipping` is the oracle for the clipper.
- Speed is not a goal here. Headless runs are unpaced.

## M3 — Sound

**Gate.** Music and effects audible in a windowed run on the garage screen.
`audio/atrac` and `audio/sascore` worked targeted, per the policy below.

- **ATRAC3+ through FFmpeg's `libavcodec`** — `find_library(avcodec)`,
  `PSPRECOMP_HAVE_FFMPEG`, dynamically linked, optional: the same shape as
  openh264 for sceMpeg, and the build and the headless host work without it.
  LGPL-2.1 goes in the README's licensing section beside openh264. Read the
  current libavcodec documentation for the `ATRAC3P` decoder's extradata
  contract before writing it; the RIFF `fmt` chunk of the game's `.at3` files
  is what feeds it. Implement the 13 calls to the streaming contract the game
  actually uses. The `.expected` files carry exact buffer-info numbers; read
  the layout out of them as `vpl/order`'s was.
- **SAS:** `__sceSasSetVoicePCM` (missing), `SetSL` and `SetADSRmode`
  (no-ops), real `SetSimpleADSR` curves (it currently ignores its arguments),
  grain and output mode. Reverb last, or never.

## M4 — Saves

**Gate.** Save in the garage, quit, relaunch, load. `utility/savedata` worked
targeted.

- `ms0:` as a host directory, a PARAM.SFO writer, the savedata modes the game
  uses (measured from the HLE log, not the enum), the result codes, and the
  five `sceIo` directory calls from M1 made real.
- No dialog UI. The dialog auto-completes, as it does now.

## M5 — A GPU backend, and real time

**Gate.** The M2 mission scene at 60 fps at native resolution, and
pixel-comparable to the software path on a fixed set of display lists
(`test_raster` plus the scenario frame dumps).

- Behind the `psp_render_backend` interface that already exists
  (`tools/psprecomp/docs/RENDERER.md`). The software path stays the oracle.
- Which API is decided then, not now. OpenGL 3.3 core through SDL2 is the
  low-risk first; Vulkan later if it earns it.
- Frame pacing on the vblank grid the clock already owns.

## M6 — Ship shape

- Publish the fork and point `.gitmodules` at it (`FORK-NOTES.md` in the
  fork has the three commands). Until then a fresh clone cannot build.
- Windows: `src/os.c`'s Win32 half has never been compiled. `mingw-w64-gcc`
  is packaged on this machine and not installed; compile-check it, then a CI
  matrix.
- Networking: the 27 `sceNet*` / `sceNetAdhoc*` imports registered to fail
  honestly, so the multiplayer menu cannot hang on a zero.
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
