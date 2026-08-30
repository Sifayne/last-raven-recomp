# Where this is, and how to find things out

Written at a session boundary. The commits carry the reasoning for individual
changes; this is the part that lives between them — what the game currently
does, which instrument answers which question, and what has already been ruled
out so it is not investigated twice.

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, and a frame loop runs. **The decoder is opt-in
(`PSPRECOMP_MPEG_DECODE=1`), and every number below is labelled with its
configuration** — the two runs stall at the same gate by different routes, and
an unlabelled number already rewrote this summary wrongly once.

### Default — decoder off: the failure path, force-stopped

`sceMpegGetAvcAu` refuses, and the game prints its own diagnosis:

    Fatal Error!!! : sceMpegGetAvcAu() is failed...ret=806101FE

The teardown chain runs, but the run does **not** end cleanly, which corrects
an earlier reading of it. Measured from the HLE log of a full default run:

- thread `0x40024` (entry `0x00274090`, prio 18) signals **Movie Start**
  (`0x0004001E`) exactly once;
- the movie controller (`0x40001`) consumes it, loops, and waits on Movie
  Start a **second** time — `sceKernelWaitSemaCB`, no timeout;
- nothing can ever signal it: the frame consumer (`0x40021`) is still parked
  on `Movie Sync`, which receives **zero** signals in the whole run;
- `wait_deadlock` stops the run and names the call. Since patch 0021 the
  summary says so instead of reading as success:

```
entry:     returned
threads:   stopped by the host (sceKernelWaitSema)
bad mem:   0 accesses
disc read: 1,912,832 bytes
pixels:    2,350,081 drawn by the rasterizer
```

**`threads: all finished` used to be printed here, and it was wrong** — a
force-stopped run also leaves zero live threads, because `psp_sched_stop_all`
marks them dead. That is the fourth entry in *Three ways to measure nothing*.

The frame loop meanwhile presents the nineteen-clear frame 422 times
(`sceDisplaySetFrameBuf` 422, `sceGeListUpdateStallAddr` 421, 21 GE lists, 19
prims, 2,350,081 pixels in total). An earlier note said the game "does not
stop; it repeats" and reported 2,309,200 presents: that was measured on the
build pinned to the 60-second drain deadline by the starvation bug. The delay
fix (cb06a25) let the run end early, and the repeating behaviour went with the
deadline it lived on.

**The renderer is not the blocker**, and neither is the scheduler. Work on
textures, sampling or the block transfer will not change the picture while
nothing is submitted to draw.

### Decoder on — real frames, same gate

The movie machinery runs for real: the ring buffer fills through the game's
own callback (the first put carries `00 00 01 BA`, a program-stream start
code), the demuxer and openh264 produce real pictures, and `sceMpegAvcDecode`
writes them into the buffer the game passes — checked against ffmpeg at the
byte level; the header comment in `mpeg.c` has the details. Then it stalls:

- `sceMpegGetAvcAu` is called 5 times and `sceMpegAvcDecode` 4 in a 60-second
  run, then never again — the decode loop's frame queue has reached capacity
  (open item 1);
- thread `0x40024` signals Movie Start **~7.4M times** in that run while
  `AvailableSize` reports 0 free — the whole stream demuxed, almost none of it
  decoded;
- **Movie Sync receives zero signals in this configuration too.** An earlier
  summary claimed both never-signalled semaphores now fire; only Movie Start
  does. The claim was wrong and this line replaces it;
- the run reaches the 60-second drain deadline rather than a force-stop:

```
psprecomp: guest threads still running after 60s; 5 alive, not waiting further:
    uid 0x00000000  entry 0x00000000  prio 32  blocked
    uid 0x00040000  entry 0x00253324  prio 32  blocked on sceKernelWaitThreadEnd
    uid 0x00040001  entry 0x002615B0  prio 16  sleeping
    uid 0x00040021  entry 0x0027594C  prio 16  blocked on sceKernelWaitSema(Movie Sync sema)
    uid 0x00040022  entry 0x00274090  prio 18  ready
    uid 0x00040024  entry 0x00274398  prio 17  running
```

Both configurations therefore stall at the same gate: **Movie Sync has no
signaller anywhere**, in either run. The frame consumer parked on it is what
keeps the movie from advancing, and the movie from ending, in both.

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
deadline — the slot is marked `SLEEPING` with `now + usec`, `handoff_locked`
wakes expired sleepers before choosing, and when nothing is runnable but
something sleeps it advances the clock to the earliest deadline rather than
hanging. The duration is honoured; only its relation to wall time is not.

**Ready-but-never-running is the signature.** A thread parked on something names
what it waits for; a starved one names nothing, which reads like an idle thread
rather than a stuck one.

### How a run ends, and how to tell the endings apart

Three endings, three signatures. Reading one as another has cost sessions.

- **Deadline.** `psp_sched_drain` gives up after 60 seconds and prints
  `still running after 60s` with the live list. Not a deadlock — a run still
  going round. The decoder run ends here. Slots saying `running` are what the
  timeout path does: a guest thread cannot be unwound from outside, so the
  main context stops waiting for it and takes the token back, leaving the
  runaway genuinely still running. `sched.h` documents it.
- **Force-stop.** `wait_deadlock` finds a no-timeout wait that nothing can
  ever satisfy, prints the live list itself, and marks every thread dead. The
  default run ends here. The summary names the call since patch 0021:
  `stopped by the host (sceKernelWaitSema)`.
- **Deadlock.** `deadlock -- N thread(s) alive, none runnable`. This game has
  never printed it. Check for that string before concluding anything about
  the scheduler.

`threads: all finished` is reserved for a run where every thread ended on its
own. Before patch 0021 it was printed for force-stops too — see *Three ways
to measure nothing*.

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
| `PSPRECOMP_MPEG_DECODE=1` | Enables the sceMpeg video path: ring buffer, demux, openh264. Off by default — without it `GetAvcAu` refuses and the game takes its error path. Which is why every measurement above is labelled. |
| `PSPRECOMP_MPEG=1` | Narrates the movie path: ring-buffer puts, the first packet in hex, decoded-frame counts. |
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
- **`threads: all finished` counted force-stops as finishes.** `stop_all`
  marks every thread dead, so a run killed at an unsatisfiable wait also
  reported zero live threads — indistinguishable, in the summary, from a run
  that ran to completion. The default run's summary said *all finished* for
  most of a session while the game sat parked on a semaphore nobody would ever
  signal. The stop reason (patch 0021) is the difference.

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
- **The `sceKernelWaitSema cannot be satisfied` report — *as it was*.** It
  used to be teardown noise *after* the 60s drain deadline: drain gave up, the
  main context took the token back, and a still-running thread's next wait
  found main `RUNNING` rather than `READY`. It fired once, after the
  `still running after 60s` line. **In the default run today it is the
  terminal event itself, before any deadline**: the controller's second wait
  on Movie Start, with the frame consumer parked on Movie Sync and no third
  thread able to signal either. Do not apply the old ordering lesson to that
  one — but the stderr/stdout interleave warning still stands: stderr is
  unbuffered, stdout is not, and a redirected log mixes them.
- **Returning a clean end-of-stream from sceMpeg to end the movie.** The game
  already ends it. `SCE_MPEG_ERROR_INVALID_VALUE` makes the AU-fetch wrapper at
  `0x002750C0` print its own `Fatal Error!!!` and return 0; the caller's
  `beql $v0, $zero` is then taken, sets the "movie done" flag at offset 746,
  and proceeds into the teardown chain at `0x00273804` / `0x00273838` /
  `0x0027394C`. `NO_DATA` would restore the fifteen-million-query spin — see
  the header comment in `mpeg.c`, which has the guest disassembly. **The stall
  is downstream, in the teardown, which never signals `Movie Sync`.**
- **A deep or surprising call chain into the decode step.** gdb says it is flat:
  `psp_func_002750C0` is called directly from `psp_body_00274398`
  (MovieDecodeThread) and nothing else, every time. It runs **exactly 60 times**
  in a 60-second run and then stops being called, while the thread stays alive
  in the census — so it blocks rather than exits, and the loop terminating is
  the thing to explain.
- **A `SignalSema` on the semaphore SoundThread waits for.** Two independent
  lines of evidence say the movie's sync semaphore is never signalled by
  anything, anywhere. At run time the uid census records every uid ever passed
  to `sceKernelSignalSema` and `Movie Sync` (`0x0004001C`) is not among them.
  Counted directly in the HLE log: **zero `sceKernelSignalSema` calls on
  0x0004001C, in both configurations.** Statically, no instruction in the
  module loads offset 100 — where SoundThread reads it — and reaches
  `SignalSema`; the offsets that do feed its `$a0` are
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

1. **The movie decoder fills its frame queue and nothing drains it.**
   (Decoder configuration. Measured on today's build: `GetAvcAu` 5 calls,
   `AvcDecode` 4, then never again; Movie Start signalled ~7.4M times; Movie
   Sync signalled 0 times.) The decode loop spins half a billion times a run
   without ever fetching an access unit, and the branch that turns it back is
   two instructions before the fetch:

   ```
   0027516C  lw   $a0, 172($s0)      queue capacity
   00275170  lw   $a1, 168($s0)      frames outstanding
   00275174  subu $a0, $a0, $a1
   00275178  blez $a0, 0x00275268    full -> return 1, go round again
   00275198  jal  sceMpegGetAvcAu    never reached
   ```

   `struct[168]` is incremented on every successful decode at `0x00275250` and
   decremented in exactly one place, `0x00273E7C` inside `psp_func_00273E30`,
   which then signals Movie Lock. That function is reached from MovieDecodeThread's
   *teardown* loop at `0x00274410` — not its playback loop — and from a wrapper
   at `0x00273A50` whose single caller is `psp_body_002729C0`, on the display
   side. So during playback the counter only ever climbs.

   The decoder is not starved and the semaphores are not stuck. The consumer
   simply never runs, so the queue reaches capacity and stays there.

   The consumer's callers are ordinary `jal` sites, and **none of them ever
   runs**. Breakpoints on `psp_func_0018BDB0`, `psp_func_00272278` and
   `psp_func_00273E30` — the drain itself — are hit zero times in a full run.

   The chain above the drain is `psp_body_002729C0` ← `0x00272264` / `0x00272280`
   in `psp_body_00272224` / `psp_body_00272278` ← `0x0013B040` / `0x0018BE48`.
   `0x0013B02C` is the target of no transfer at all and is probably a
   pointer-scan artifact rather than live code; `0x0018BDB0` is called from
   `0x001B1770` and `0x001B8BA4`, and both guard on a null field before
   reaching the drain.

   The drain methods themselves live in the movie's own range, but the calls
   that reach them come from `0x0013B040` and `0x0018BE48`, in the game's update
   code — and that code never runs. Breakpoints confirm it: `psp_func_001B134C`
   and `psp_func_0027ECD4` are hit zero times, while `user_main`
   (`0x00253324`) is entered exactly once and then blocks in
   `sceKernelWaitThreadEnd` for thread `0x40001`, the movie controller.

   So the game cannot proceed until the movie thread ends; the movie cannot end
   until its frame queue drains; and the drain is only called from code that
   runs once the game has proceeded. Whatever breaks that on hardware, we are
   not providing it — and it is not reachable from inside sceMpeg.

   The display side never gets far enough to dispatch it. SoundThread wakes on
   Movie Sync and then guards on two fields before doing anything:

   ```
   0027597C  lw    $a1, 72($s1)
   00275984  sltiu $a1, $a1, 1        struct[72] == 1 ?
   0027598C  bne   $a1, $zero, ...    yes -> away
   00275990  lw    $a0, 88($s1)
   00275994  blez  $a0, 0x00275A24    depth <= 0 -> straight back to the wait
   0027599C  jal   0x002732C8         sceKernelWaitEventFlag -- never reached
   ```

   `sceKernelWaitEventFlag` does not appear in the firmware histogram at all, so
   that guard is what turns it back: `struct[88]`, the display queue depth, is
   zero. **Two different counters are involved and only one of them moves.** The
   decode loop is held up by `struct[168]` reaching `struct[172]`, while the
   depth the display waits on is never incremented — the producer that would do
   it, at `0x00275770`, is not being reached either.

   That producer has exactly one caller, `0x00275864`, which the decode step
   calls only when a byte flag is set:

   ```
   00275140  lbu $a0, 741($s0)
   00275144  beq $a0, $zero, 0x00275164   flag clear -> skip the enqueue
   0027514C  jal 0x00275864               only reached when it is set
   ```

   `flag741` is set to 1 by an initialiser at `0x002733A4`, inside
   `psp_body_00273334`, which is called by `jal` from `0x00272608`.

   So the whole chain is ordinary calls, and the live question is which of these
   callers runs and which does not — not how they are reached.

1. **The game never leaves its intro-movie state.** Both configurations stall
   with the frame consumer parked on Movie Sync — the default run then dies at
   the controller's second Movie Start wait, the decoder run spins to the
   deadline. The blocked-thread dump names the object:

   ```
   uid 0x00040021  entry 0x0027594C  prio 16  blocked on sceKernelWaitSema(Movie Sync sema)
   ```

   An earlier reading said the movie *teardown* runs to completion and stops
   the movie cleanly by its own lights. The teardown does run — `0x00273804`
   signals `Movie Start`, `0x00273838` reaches `sceMpegAvcDecodeStop`
   (implemented, returns OK), and `0x00274420` marks the movie stopped with
   `struct[140] = 2`, all confirmed against the trace ring — but it is not a
   clean stop: the controller afterwards waits on Movie Start a second time,
   nothing signals it, and the host force-stops the run (see *How a run
   ends*). Nothing in the teardown chain touches `Movie Sync`, and the one
   teardown step that would wait on a semaphore, `0x0027394C`, is skipped.

   So the game's failure path leaves **two** unmet dependencies, not one: the
   frame consumer's Movie Sync, and the controller's second Movie Start. On
   hardware neither arises, because `sceMpegGetAvcAu` does not fail and the
   pipeline keeps moving — we are driving the game down an error path its
   authors never expected to be taken.

   Which makes the choice explicit, and it is a real fork: make the game take
   its *normal* completion path (sceMpeg succeeding and reporting the stream
   ending), decode for real, or find another way past. The pad is already ruled
   out. Note the *normal* path already is the decode path — the fork is really
   "finish the decode pipeline so the movie ends by itself" against "fake a
   stream end well enough to satisfy the teardown", and the second Movie Start
   wait shows that faking the stream end alone would not even satisfy the
   failure path.

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
4. **The guest's panic message.** Partly fixed by patch 0020: the synchronous
   write path already reached stderr (the movie's `Fatal Error!!!` line is the
   proof), but the *async* variant returned BADF for fds 1 and 2 — no
   descriptor slot — and dropped exactly the write a panic path makes right
   before `abort()`. That hole is closed and pinned by `test_stdio_async`. No
   run has yet reached the abort itself (the default run stops at the
   unsatisfiable Movie Start wait first), so whether any text still fails to
   arrive is untested; next run that hits an abort, check the log for what
   reached stderr.
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
approximate it. `scripts/verify-patches.sh` runs that check — add `--build`
to compile and test the pristine clone too.
