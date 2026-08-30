# Where this is, and how to find things out

Written at a session boundary. The commits carry the reasoning for individual
changes; this is the part that lives between them — what the game currently
does, which instrument answers which question, and what has already been ruled
out so it is not investigated twice.

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, and a frame loop runs to a steady state it never leaves. The intro
movie does **not** end — see the open work below; that is the thing holding
everything else up.

```
entry:     returned
threads:   still alive
bad mem:   0 accesses
disc read: 1,912,832 bytes
pixels:    2,350,081 drawn by the rasterizer
```

The frame is a single flat colour, and that is correct: the only geometry
reaching the rasterizer is nineteen untextured full-screen quads — screen
clears — drawn with no vertex colour, which defaults to white. There is nothing
else to draw yet.

**It does not stop. It repeats.** Over a 60-second run the game issues
2,309,200 `sceDisplaySetFrameBuf` and 2,309,199 `sceGeListUpdateStallAddr`
calls — and 21 GE lists, 19 prims, 2,350,081 pixels *in total*. It presents the
same nineteen clears about 2.3 million times. Whatever would produce geometry
never runs.

**The renderer is not the blocker**, and neither is the scheduler. Work on
textures, sampling or the block transfer will not change the picture while
nothing is submitted to draw.

### The run ends on a deadline, not on a deadlock

The boot host's `psp_sched_drain` gives up after 60 seconds and prints the live
thread list. That report is easy to misread as a deadlock — it is not one, and
mistaking it for one cost a session:

```
psprecomp: guest threads still running after 60s; 5 alive, not waiting further
    uid 0x00000000  prio 32  running
    uid 0x00040000  prio 32  blocked on sceKernelWaitThreadEnd
    uid 0x00040001  prio 16  running
    uid 0x00040021  prio 16  blocked on sceKernelWaitSema(Movie Sync sema)
```

Two slots say `running` because that is what the timeout path does: a guest
thread cannot be unwound from outside, so the main context stops waiting for it
and takes the token back, leaving the runaway thread genuinely still running.
`sched.h` documents it. **A real deadlock prints `deadlock -- N thread(s)
alive, none runnable`, and this game has never printed it.** Check for that
string before concluding anything about the scheduler.

## The instruments, and what each can and cannot tell you

All are off by default and cost nothing when off.

| | |
|---|---|
| `PSPRECOMP_HLE_LOG=1` | Every firmware call, tagged with the calling thread, with arguments and result. The first thing to reach for. |
| `PSPRECOMP_HLE_TRACE=<name>` | Dumps the guest function trace at every call to that firmware function. Answers "which of the game's loaders called this". |
| `PSPRECOMP_WATCH=<hex addr>` | Prints the argument registers on entry to one guest function, and dereferences the pointer-looking ones. Needs a `TRACE=1` build. |
| `PSPRECOMP_SEMA=<substring>` | Narrates every wait, take and signal on the semaphores whose name contains it, tagged with the calling thread. A signal also dumps the guest trace. Answers "is nobody signalling this, or is it signalled too early" — which the thread dump cannot. |
| `PSPRECOMP_PAD=start,cross` | Holds pad buttons for the run. There is no window and no gamepad. |
| `PSPRECOMP_FRAME=<path>` | Where to write the frame. Defaults to `frame.ppm`, and dumps the GE's render target rather than the scanned-out buffer. |
| `TRACE=1 ./scripts/04-emit-build.sh` | Rebuilds the generated C with function-entry tracing. Slow to build; needed by the watch and by any trace dump. |

The boot summary also reports, without any flag: the firmware-call histogram,
the GE state (framebuffer, texture, vertex type of geometry that actually
*draws*), a VRAM survey of which 64K blocks hold data, and the live thread list
with what each is parked on.

### Three ways to measure nothing

Each of these produced a confident number that meant nothing. All three were
believed before they were checked.

- **A capped oracle run used to be positional.** `--limit` bounds *attempts*,
  and the old work-list walked `.text` from the bottom testing whatever the
  dispatch table resolved — mostly interior labels. 5,000 attempts reached the
  first ~256KB of 3.03MB, so two runs across an emitter change that reclaimed
  171 functions produced **byte-identical output files**. Fixed since: the
  work-list is the entry list and a capped run strides the whole module.
- **Counting `UNBALANCED` log lines counts the print cap.** `psp_trace_sp`
  stops at 24 sites, `psp_trace_sp_call` at 16, and both saturate here. The
  boot summary reports the real totals now.
- **The unbalanced-return total is not a before/after metric.** It counts
  returns, so it tracks how many frame-loop iterations fit in the drain window.
  Two runs of the *same* build differed by 5,688. Use distinct sites and leaks.

### The trace ring is not a call stack

It records function *entries in order*. A loop calling a four-function chain
116 times looks identical in that output to 116-deep recursion. Counting
entries says nothing about depth, and reading it as a stack cost three turns of
wrong diagnoses on one fault.

**When the question is structural — how deep, who called whom — use gdb.** One
backtrace settled what the ring could not:

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
out-parameters is worse than one that fails honestly. When a game does
something inexplicable, check what it was last told.

## Ruled out — do not re-investigate

- **`sceKernelDelayThread` causing the deadlock.** It yields, which leaves the
  thread READY and always runnable. It cannot contribute to an all-blocked
  state.
- **Stack exhaustion in the MovieReadThread fault.** 64MB did not help; the
  gdb backtrace showed a stack depth of three.
- **The `$k0`/reent gap as the cause of an allocator failure.** Only twelve
  sites in the whole 2.1M-line module read `r_k0`, and none are on that path.
- **`psp_mem_ptr`'s bounds logic.** It was never wrong. The memory was being
  freed underneath it by the boot host's teardown.
- **The GE block transfer as how textures arrive.** Implemented, and never
  used — the command does not appear in the stream, and the game imports no
  `sceDmac` either. How textures reach memory here is still unknown.
- **`sceDisplayGetFramePerSec` as the reason the game draws nothing.** It was
  28,470 of the 28,482 unimplemented calls in a run and returns a float on the
  frame path, so a zero looked certain to poison the frame timing. Implemented:
  the picture did not change by a single pixel.
- **The `sceKernelWaitSema cannot be satisfied` report as a blocker.** It is
  teardown noise *after* the 60s drain deadline — drain gives up, the main
  context takes the token back, and the still-running thread's next wait then
  finds main `RUNNING` rather than `READY`. It fires once, after the
  `still running after 60s` line. Check the order before reading anything into
  it; stderr is unbuffered and stdout is not, so the two interleave misleadingly
  in a redirected log.
- **Returning a clean end-of-stream from sceMpeg to end the movie.** The game
  already ends it. `SCE_MPEG_ERROR_INVALID_VALUE` makes the AU-fetch wrapper at
  `0x002750C0` print its own `Fatal Error!!!` and return 0; the caller's
  `beql $v0, $zero` is then taken, sets the "movie done" flag at offset 746,
  and proceeds into the teardown chain at `0x00273804` / `0x00273838` /
  `0x0027394C`. `NO_DATA` would restore the fifteen-million-query spin — see
  the header comment in `mpeg.c`, which has the guest disassembly. **The stall
  is downstream, in the teardown, which never signals `Movie Sync`.**
- **A `SignalSema` on the semaphore SoundThread waits for.** Two independent
  lines of evidence say the movie's sync semaphore is never signalled by
  anything, anywhere. At run time the uid census records every uid ever passed
  to `sceKernelSignalSema` and `Movie Sync` (`0x0004001C`) is not among them.
  Statically, no instruction in the module loads offset 100 — where SoundThread
  reads it — and reaches `SignalSema`; the offsets that do feed its `$a0` are
  {0, 4, 8, 20, 40, 52, 128, 132, 140, 176, 396, 668}, and 100 is absent.

  What the surrounding structure does say: `struct[88]` is a queue depth,
  incremented by the producer at `0x00275770` and decremented by the consumer at
  `0x00275844`, and **both of those signal `struct[96]`, not `struct[100]`**. So
  the producer half of that queue is a path the game never reaches, rather than
  a signal we are dropping.
- **A colour-conversion or decode-mode stage as the missing display step.** The
  game imports **none** of `sceMpegAvcDecodeMode`, `sceMpegAvcCsc`,
  `sceMpegAvcDecodeYCbCr`, `sceMpegAvcCopyYCbCr` or `sceMpegAvcQueryYCbCrSize`.
  All 23 of its sceMpeg imports were identified by SHA-1 and they are the plain
  path only — init, create, ring buffer, regist, the queries, `InitAu`,
  `GetAvcAu`, `GetAtracAu`, `AvcDecode`, `AtracDecode`, `AvcDecodeStop`, delete,
  finish. So `sceMpegAvcDecode` writing into the buffer the game passes **is**
  the whole display mechanism, and registering `sceMpegAvcDecodeMode` would be
  dead code.
- **Skipping the intro movie with `PSPRECOMP_PAD`.** The game does read the
  pad — holding `start,cross` visibly changes which threads park where — but
  the skip path routes back into the movie subsystem rather than around it:
  `0x40001` stops running the frame loop and waits on `Movie Start sema`
  instead. The output is identical, 21 GE lists and 19 prims either way. Useful
  negative result, because it also shows the movie machinery has **two** gates,
  `Movie Start sema` and `Movie Sync sema`. Satisfying only the one the game
  happens to be sitting on would move it to the other.
- **The scheduler as the reason the game does not progress.** Measured three
  ways: the current build, the scheduler with its token fix reverted, and the
  fully original semantics. All three are functionally identical — same 19
  prims, same 2,350,081 pixels, same five live threads, and `deadlock -- `
  printed zero times in all three. The scheduler had a real bug (see the patch
  series note below) and fixing it changed nothing the game does.

## Open work, in the order it is worth doing

**This list is the source of truth.** It used to live only in an agent's task
tracker, which does not survive a session — one session opened by reconstructing
it from transcripts, and inherited a stale premise doing so. Anything worth
picking up next session goes here, in the repository, with enough context to act
on without the conversation that produced it.

1. **The game never leaves its intro-movie state.** It is not stuck; it
   repeats — ~2.3M frame presents against 21 GE lists for the whole run. The
   blocked-thread dump now names the object, and that settles it:

   ```
   uid 0x00040021  entry 0x0027594C  prio 16  blocked on sceKernelWaitSema(Movie Sync sema)
   ```

   Nothing signals it. The movie *teardown* has been walked and it runs to
   completion — `0x00273804` signals `Movie Start`, `0x00273838` reaches
   `sceMpegAvcDecodeStop` (implemented, returns OK), and `0x00274420` marks the
   movie stopped with `struct[140] = 2`. Confirmed against the trace ring, not
   inferred. Nothing in that chain touches `Movie Sync`, and the one teardown
   step that would wait on a semaphore, `0x0027394C`, is skipped.

   So the game's *failure* path stops the movie cleanly by its own lights and
   leaves the frame consumer parked forever. That thread is woken by the frame
   producer, and at shutdown by whatever sets its quit flag — neither happens
   here. Nothing in this chain is a psprecomp bug: every call behaves correctly.
   We are driving the game down an error path its authors never expected to be
   taken, because on hardware `sceMpegGetAvcAu` does not fail.

   Which makes the choice explicit, and it is a real fork: make the game take
   its *normal* completion path (sceMpeg succeeding and reporting the stream
   ending), decode for real, or find another way past. The pad is already ruled
   out.

   `sceDisplayGetFramePerSec` was the obvious suspect and is **not** the cause:
   28,470 of the run's 28,482 unimplemented calls, returning zero on the frame
   path, and implementing it changed nothing at all. It is implemented anyway;
   unimplemented calls are down to 12, all cold.
2. **Shared epilogues split into pseudo-functions.** Partly fixed; the
   remainder is below. What the `$sp` reports are really about, now that the
   instrument can say so:

   There are **zero** stack leaks. Every imbalance is a *positive* delta, which
   `psp_trace_sp` identifies as a continuation holding an epilogue without its
   matching prologue — an artifact of discovery, not corruption. The reported
   address is the **return instruction**, not the function, so the list is of
   return sites and one function with two returns appears twice.

   Two of the six hot sites were a single shared epilogue at
   `0x002B5884`–`0x002B5898` cut into pieces. Neither merge site could see it:
   both fire on a *transfer* into a claimed block, and this arrives by
   **fall-through**, which broke the walk silently. Merging on fall-through —
   both into an unwalked soft entry and into an already-claimed block — took it
   from 52 sites / 7.0M returns to **50 sites / 4.5M returns**.

   Seed provenance (soft/hard) landed with it and is correct — a pointer guess
   should not veto a merge — but be clear that on its own it changed **nothing**
   measurable: same 52 sites, same addresses, same deltas.

   **The rest was an emitter bug, now fixed.** A gdb backtrace found it:

   ```
   #0  psp_at_002B57E4      <- label thunk
   #1  psp_body_002B5784    <- called from itself
   #2  psp_func_002B5784
   ```

   The body contains `psp_dispatch(r_a2)`, the lowering of a guest `jr $a2`,
   and at run time `r_a2` is `0x002B57E4` — a label *inside that same
   function*, already in its own entry switch. Dispatch resolves it to a thunk
   that calls the body afresh, so a computed jump within a function re-enters
   it as a call instead of jumping to its label. The re-entry runs
   `PSP_SP_ENTER()` with the frame already allocated, then hits the shared
   epilogue and releases 16 — which is every `+16` in the report, and why the
   leak count is zero.

   A computed jump now re-enters the body's own `switch (_entry)` when the
   target is one of this function's labels, and only falls back to
   `psp_dispatch` for a genuine cross-function transfer.

   The last of it was the same thing one level up: **a switch case belongs to
   the function whose `jr` selects it**, and nothing could establish that. The
   walk stops dead at a computed jump, so the owning function never reaches its
   own cases; the cases surface later when the table is resolved, are walked as
   functions, and no branch, jump or fall-through ever connects them back.
   Every other merge is the walk noticing a collision — this one has to be
   stated outright, against final ownership. Only soft targets are folded, so a
   table of genuine handlers (whose entries are `jal` targets, hence hard) is
   untouched.

   ```
   session start                    52 sites   7,067,959 returns
   fall-through merge               50         4,509,431
   computed jump jumps              37           889,348
   switch cases join their function 36             1,159
   ```

   Zero leaks at every step — that was never the problem. `entry`, `bad mem`,
   `disc read` and `pixels` are unchanged throughout, and the oracle holds at
   2 divergences with **dispatch miss 0**, which is the number that would report
   a `jr` whose target stopped resolving.

   What is left is 36 sites of 85 hits or fewer, ~1,200 in total. Nothing in it
   is hot, and none of it is a leak.
3. **The two composed-chain oracle divergences.** Each callee agrees in
   isolation; the disagreement only appears in the chain. Needs
   instruction-level trace diffing, not another hypothesis — which means giving
   the recompiled side a per-instruction register dump to match
   `allegrexrecomp interp --regs`.
4. **The guest's panic message.** The abort chain is `sceKernelStdout` ->
   `sceIoWrite` -> `abort()`, but no text reaches stderr. Worth having before
   the HLE push, for the same reason as the 0-is-OK pattern above.
5. **`$k0` thread control block and reent.** Bring-up completeness, not a live
   blocker — only twelve sites in the whole module read `r_k0`.

### Two measurements worth taking before building further

Neither is on the critical path today, and both could redirect months of work:

- **What the rasterizer costs on a real scene.** Everything from here — texturing,
  blending, lighting — is built on the software rasterizer. If a mid-poly frame
  costs tens of milliseconds, the GE needs GPU-backed display-list translation
  instead, and that is a rewrite of whatever is stacked on top by then. Capture
  one real display list and time it.
- **A behavioural oracle.** The differential oracle validates *translation*, and
  everything left is *environment* — the HLE, the scheduler, save data. Anything
  missing from the execution environment is missing from both sides and agrees
  perfectly, so the current instrument is structurally blind to exactly the work
  that remains. `pspautotests` are small PSP programs with real-hardware expected
  output; run through both the interpreter and the recompiled module they give
  ground truth where the oracle cannot.

## The patch series

Everything upstream-able lives in `patches/`, applied to the `tools/psprecomp`
submodule by `scripts/build-tools.sh`. After changing anything under
`tools/psprecomp`, regenerate the affected patch and check the whole series
still applies to a pristine checkout **and builds green there** — a stale build
directory will happily report 12/12 for code you did not build.

Regenerating a patch in the middle of the series has two traps. Files a patch
*creates* are untracked in the submodule, so a plain `git diff` cannot see them
and silently produces a patch with the file missing — `git add -N` first. And a
patch must be diffed against the state *after* its predecessors, not against
pristine HEAD, or it will clobber their hunks in a shared file. The recipe:
clone the submodule, apply `0001`..`N-1`, commit that as a baseline, apply the
old `N`, fold the new change in, and diff.

The check that actually proves it is not `ctest` but a tree diff: apply the
whole series to a pristine checkout and compare every file the series owns
against the working tree. Byte-identical is the bar. Green tests only
approximate it.
