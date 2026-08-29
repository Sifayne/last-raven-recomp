# Where this is, and how to find things out

Written at a session boundary. The commits carry the reasoning for individual
changes; this is the part that lives between them — what the game currently
does, which instrument answers which question, and what has already been ruled
out so it is not investigated twice.

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, the intro movie ends, and a frame loop runs to a steady state it
never leaves:

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
1,717,502 `sceDisplaySetFrameBuf` and 1,717,501 `sceGeListUpdateStallAddr`
calls — and 21 GE lists, 19 prims, 2,350,081 pixels *in total*. It presents the
same nineteen clears about 1.7 million times. Whatever would produce geometry
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
    uid 0x00040021  prio 16  blocked on sceKernelWaitSema
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
| `PSPRECOMP_PAD=start,cross` | Holds pad buttons for the run. There is no window and no gamepad. |
| `PSPRECOMP_FRAME=<path>` | Where to write the frame. Defaults to `frame.ppm`, and dumps the GE's render target rather than the scanned-out buffer. |
| `TRACE=1 ./scripts/04-emit-build.sh` | Rebuilds the generated C with function-entry tracing. Slow to build; needed by the watch and by any trace dump. |

The boot summary also reports, without any flag: the firmware-call histogram,
the GE state (framebuffer, texture, vertex type of geometry that actually
*draws*), a VRAM survey of which 64K blocks hold data, and the live thread list
with what each is parked on.

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

1. **The frame loop presents forever without submitting geometry.** The game is
   not stuck; it repeats. ~1.7M frame presents against 21 GE lists for the whole
   run. Thread `0x40021` (entry `0x0027594C`) is parked on a semaphore — find
   which, and who was supposed to signal it. Start with `PSPRECOMP_HLE_LOG=1`,
   which tags every call with its thread. Note the 28,637 unimplemented
   firmware calls in the same run, `0xDBA6C4C4` among them: an unimplemented
   call returns 0, 0 is `SCE_KERNEL_ERROR_OK`, and this project has been bitten
   by that four times already.
2. **Blocks promoted to function entries by the pointer-seed scan, whose
   back-edges stay C calls.** `a_scan_data_pointers` accepts any word that lands
   in `.text` and decodes, so data pointing into the middle of a function
   becomes an "entry"; both merge sites in `analyze.c` are guarded on
   `!entry_map[ti]`, so those addresses block their own merge and the loop
   back-edge ships as a call. Give seeds provenance — hard (entry, exports,
   `jal` targets, table targets) against soft (pointer guesses) — and guard on
   hard only. The metric is the oracle's unbalanced-`$sp` skip bucket, not its
   agreement count. This is the deepest thing still known-broken in the emitter.
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
