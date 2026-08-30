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

The switch landed in `4462226`. Every doc commit after it recorded numbers
without saying which side of it they came from, so by the time anyone read them
there was no way to tell. **Label the configuration or do not write the number.**

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, and the frame loop runs. Both configurations stall at the same gate
by different routes.

### default — the failure path, force-stopped

`sceMpegGetAvcAu` refuses and the game prints its own diagnosis:

```
Fatal Error!!! : sceMpegGetAvcAu() is failed...ret=806101FE
```

```
entry:     returned
threads:   stopped by the host (sceKernelWaitSema)
bad mem:   0 accesses
disc read: 1,912,832 bytes
pixels:    33,423,361 drawn by the rasterizer
GE: 633 lists, 106,108 commands, 212 finishes
    texture 512x64 stride 352, clut8, modulate, swizzled  (153 clut loads)
    drawn   257 prims, 2,958 vertices, 611 triangle-strips, 257 sprites
    2,444 vertices in an unsupported format (transformed, or no position)
```

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

### The picture is still one flat colour, and that is now a different problem

The frame dumps as a single `rgb(0, 32, 32)` across all 130,560 pixels. It used
to be a single white, from nineteen untextured full-screen clears. It is no
longer that: real geometry with a real swizzled CLUT8 texture and 153 palette
loads now reaches the rasterizer. Something between "the GE executes the list"
and "the framebuffer holds a picture" is still wrong, and the **2,444 vertices
in an unsupported format** are the first place to look.

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

- **Deadline.** `psp_sched_drain` gives up after 60 seconds and prints
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
indistinguishable from success. See *Five ways to measure nothing*.

## The instruments, and what each can and cannot tell you

All are off by default and cost nothing when off.

| | |
|---|---|
| `PSPRECOMP_HLE_LOG=1` | Every firmware call, tagged with the calling thread, with arguments and result. The first thing to reach for. Unfilterable — see the volume note. |
| `PSPRECOMP_HLE_TRACE=<name>` | Dumps the guest function trace at every call to that firmware function. Exact name match, not substring. Up to 512 lines per hit, so aim it at a call the histogram has already shown to be cold. Needs `TRACE=1`. |
| `PSPRECOMP_WATCH=<hex addr>` | Argument registers on entry to one guest function, dereferencing the pointer-looking ones. Hooks `PSP_ENTER`, so it only ever fires on a **function entry**. Needs `TRACE=1`. |
| `PSPRECOMP_REACHED=<hex>[,...]` | Whether control ever arrived at each address. Covers every **label**, not just entries — see below. Needs `TRACE=1`. |
| `PSPRECOMP_SEMA=<substring>` | Narrates every wait, take and signal on matching semaphores, and unlocks the thread and signalled-uid censuses in the summary. See the volume note. |
| `PSPRECOMP_FINDPTR=<hex>` | Every address in the loaded module holding that value as a word. For pointers that only exist once the PRX is relocated. |
| `PSPRECOMP_PAD=start,cross` | Holds pad buttons for the run. There is no window and no gamepad. |
| `PSPRECOMP_FRAME=<path>` | Where to write the frame. Defaults to `frame.ppm`, and dumps the GE's render target rather than the scanned-out buffer. |
| `PSPRECOMP_MPEG_DECODE=1` | Demuxer and openh264 video path. Refused, loudly, in a build without openh264. |
| `PSPRECOMP_MPEG=1` | Narrates the movie path. Everything it prints is throttled except `RingbufferPut`, which is bounded by the disc read — safe in either configuration. |
| `PSPRECOMP_MPEG_DUMP=<path>` | The demuxed elementary stream, once it exceeds 1 MB. For checking against a decoder that is not ours. |
| `PSPRECOMP_MPEG_FRAME=<path>`, `PSPRECOMP_MPEG_FRAME_NO=<n>` | One decoded frame as a PPM. |
| `TRACE=1 ./scripts/04-emit-build.sh` | Rebuilds the generated C with tracing, into `build/host-trace` so it no longer destroys the plain build. `TRACE=1 ./scripts/06-boot.sh` runs it. |

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

### Five ways to measure nothing

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
- **A `SignalSema` on the semaphore SoundThread waits for.** `Movie Sync sema`
  (`0x0004001C`) is **never signalled in either configuration** — confirmed
  against the signalled-uid census, which records every uid ever passed to
  `sceKernelSignalSema`. Statically, no instruction in the module loads offset
  100 — where SoundThread reads it — and reaches `SignalSema`.
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

## Open work, in the order it is worth doing

**This list is the source of truth.** Anything worth picking up next session goes
here, in the repository, with enough context to act on without the conversation
that produced it.

1. **The picture does not reach the framebuffer.** *(default)* The GE executes
   633 lists, 257 prims and a real swizzled CLUT8 texture, and the frame is still
   a single flat `rgb(0, 32, 32)`. The strongest lead is in the summary: **2,444
   vertices in an unsupported format (transformed, or no position)**, up from 24
   before the queue fix. `read_vertex`/`vertex_layout` in `ge.c` decide that.
   Second lead: the dump takes the GE's last render target, which may not be the
   buffer the game intends to show.

2. **Movie Sync has no signaller, and that is the gate both configurations hit.**
   The frame consumer `0x40021` parks on it and never wakes; the movie therefore
   never advances and never ends; `user_main` waits on the movie thread forever.
   Two independent lines of evidence say nothing signals it — see the ruled-out
   entry.

   The chain below it is now measured rather than inferred. `0x00275864` is the
   *audio* pump; the strings its error path prints identify `0x002E4928` as
   `sceMpegGetAtracAu` and `0x002E48E0` as `sceMpegAtracDecode`:

   ```c
   if (this[92] - this[88] <= 0)                                  return 0;   // queue full
   if (sceMpegGetAtracAu(this[124], this[128], &this[4], &attr))  return 0;   // <- here
   buf = f_00275714(this);
   r = sceMpegAtracDecode(this[124], &this[4], buf, this[104]);
   if (r) { printf("Fatal Error!!! : sceMpegAtracDecode() is failed...ret=%08X", r);
            return -1; }
   if (this[104] == 0) f_00275744(this);      // the producer; this[104] is the init flag
   this[104] = 0;  return 1;
   ```

   `PSPRECOMP_REACHED` on the blocks, both configurations:

   | block | default | decoder |
   |---|---|---|
   | `0x00275864` entry | reached | reached |
   | `0x00275934` early return 0 | **reached** | reached |
   | `0x00275904` past both guards | not reached | **reached** |
   | `0x00275744` the producer | not reached | **reached** |

   So in the default configuration `sceMpegGetAtracAu` returns `0x806101FE` on
   its first and only call and the function returns at the second guard; **with
   the decoder on every block is reached, the producer included**, and
   `struct[88]` is incremented after all. Nothing guards the producer.

   Also measured, default configuration: `flag741` **is** set (`0x00273334`
   reached, and the store at `0x002733A4` is inside its entry block), the enqueue
   wrapper `0x00275864` runs, and `0x0013B02C` is live. The drain `0x00273E30`
   and its caller `0x0018BDB0` are not reached.

   ### The mistake that produced the question

   That table was first measured in the **default** configuration and used to ask
   "why does the producer not run" — a question that only means anything in the
   **decoder** configuration. In the default one the producer is *supposed* not
   to run: sceMpeg refuses playback and everything downstream correctly declines.
   The table was labelled; the conclusion drawn from it was not, which is the
   error this document exists to prevent, made while writing it. **Ask which
   configuration makes the question meaningful before choosing the one to measure
   in.**

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

### Two measurements worth taking before building further

- **What the rasterizer costs on a real scene.** Now urgent rather than
  hypothetical: the GE went from 1,900 commands to 106,108 in one fix, and
  everything from here is built on the software rasterizer. If a real frame costs
  tens of milliseconds, the GE needs GPU-backed display-list translation instead.
- **The behavioural oracle.** Scaffolded — see
  [autotests.md](autotests.md) and `scripts/07-autotests.sh`. The differential
  oracle validates *translation*, and everything left is *environment*, which is
  missing from both sides and so agrees perfectly. The GE queue bug is the proof:
  the oracle held at 3110/3108/2 throughout, because both sides called the same
  broken runtime.

## The patch series

Everything upstream-able lives in `patches/`, applied to the `tools/psprecomp`
submodule by `scripts/build-tools.sh`. **`scripts/verify-patches.sh` is the
check** — it applies the series to a pristine clone and compares git tree
hashes, so it sees untracked files the series creates. `--build` also configures,
compiles and tests the pristine clone, which is the other half of the bar.

Regenerating a patch in the middle of the series has two traps, and the first is
not hypothetical — it fired again while writing this:

- **Files a patch *creates* are untracked in the submodule**, so a plain
  `git diff` cannot see them and silently produces a patch with the file missing.
  Regenerating `0018` this way produced a **zero-line patch** and reported
  success. `git add -N` the file first.
- **A patch must be diffed against the state *after* its predecessors**, not
  against pristine HEAD, or it will clobber their hunks in a shared file. The
  recipe: clone the submodule, apply `0001`..`N-1`, commit that as a baseline,
  apply the old `N`, fold the new change in, and diff.

**Prefer appending a new patch to regenerating a middle one.** A change to a
region no existing patch owns — as the GE queue fix was — costs nothing to add at
the end and avoids both traps entirely.

## The regression checks, with the numbers they should produce

```bash
scripts/verify-patches.sh          # tree diff: byte-identical
ctest --test-dir build/psprecomp -C Release --output-on-failure   # 12/12
scripts/05-oracle.sh 4000
```

```
attempted: 3957 functions
compared:  3110      match: 3108      differ: 2      dispatch miss: 0
```

The oracle exits 1, because two divergences are the standing state. Exit 0 would
mean the sample missed them.

**The limit is attempts, and it has to be this big to be comparable.**
`scripts/05-oracle.sh 400` attempts 396 and compares 316, all matching, because
the two known divergences are outside that sample. A clean run at the wrong size
is not evidence of anything — the positional-sampling trap from *Five ways to
measure nothing* wearing a different hat.

Read `LastTest.log` for the current ctest result. `LastTestsFailed.log` persists
from whenever a test last failed and will happily name a test that passes today.
