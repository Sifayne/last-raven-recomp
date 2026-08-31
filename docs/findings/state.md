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
> work; the run reports 93,354,668 today.

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

- **Deadline.** `psp_sched_drain` gives up after 60 seconds —
  `PSPRECOMP_DRAIN=<seconds>` widens it — and prints
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
| `PSPRECOMP_PAD=start,cross` | Holds pad buttons for the run. There is no window and no gamepad. |
| `PSPRECOMP_PAD_PRESS=start,15,0.5` | Presses a button at a wall-clock moment — down at `delay` seconds, up `duration` (default 0.5) later. A held button never reads as *pressed*, because a press is a transition. Headless only; in a windowed run the SDL layer owns the pad. |
| `PSPRECOMP_FRAME=<path>` | Where to write the frame. Defaults to `frame.ppm`, and dumps the GE's render target rather than the scanned-out buffer. |
| `PSPRECOMP_MPEG_DECODE=1` | Demuxer and openh264 video path. Refused, loudly, in a build without openh264. |
| `PSPRECOMP_DRAIN=<seconds>` | Widens the scheduler drain past its 60-second default, for runs that are supposed to still be going — a movie, for one. |
| `PSPRECOMP_WINDOW=1` | An SDL2 window, the gamepad and audio out. Implies real-time pacing. The frame is published at `sceDisplaySetFrameBuf` — the flip — because this game never asks for a vblank; hanging the hook off one publishes nothing, which looks exactly like a broken renderer. Closing the window stops the run through the scheduler, so the end-of-run summary still prints. Needs SDL2 at build time; without it the host still builds and says so when asked for a window. |
| `PSPRECOMP_REALTIME=1` | Real-time pacing without a window, so a wall-clock measurement of a real scene is honest. Ignored when `PSPRECOMP_WINDOW` is set, which already implies it. |
| `PSPRECOMP_MPEG=1` | Narrates the movie path. Everything it prints is throttled except `RingbufferPut`, which is bounded by the disc read — safe in either configuration. |
| `PSPRECOMP_MPEG_DUMP=<path>` | The demuxed elementary stream, once it exceeds 1 MB. For checking against a decoder that is not ours. |
| `PSPRECOMP_MPEG_FRAME=<path>`, `PSPRECOMP_MPEG_FRAME_NO=<n>` | One decoded frame as a PPM. |
| `TRACE=1 ./scripts/04-emit-build.sh` | Rebuilds the generated C with tracing, into `build/host-trace` so it no longer destroys the plain build. `TRACE=1 ./scripts/06-boot.sh` runs it. |
| `scripts/07-autotests.sh <dir>` | One pspautotests directory at the real instruction budget, against recorded hardware output. What you work a suite with. |
| `scripts/08-autotest-sweep.sh` | All 432 tests, reduced budget and a per-test wall timeout, into `reports/08-sweep.tsv`. A map, not a verdict: the reduced budget truncates tests that legitimately run long. Rank by the differing-line column — one line away is one bug, five hundred is an unimplemented library. |

**`scripts/05-oracle.sh` now refuses to run against stale generated C.** It
rebuilds the interpreter every time but links C emitted whenever
`04-emit-build.sh` last ran, so editing the emitter and re-running only the
oracle compares two different programs and reports it as `differ` — which reads
exactly like a codegen regression. It cost one this session. Run
`scripts/04-emit-build.sh` after touching `emit.c`, `decode.c`, `decode.h` or
`recomp_rt.h`.

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

## Open work, in the order it is worth doing

**This list is the source of truth.** Anything worth picking up next session goes
here, in the repository, with enough context to act on without the conversation
that produced it.

**Start at item 8.** The numbering is chronological, not a priority order — 1
and 3 are closed and 0 is a status entry — and 8 is the piece with the clearest
next step and the most behind it.

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
   This is the next substantial piece, and the diagnosis is already done — do
   not re-derive it.

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
- **The behavioural oracle.** Scaffolded — see
  [autotests.md](autotests.md) and `scripts/07-autotests.sh`. The differential
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
compared:  3073      match: 3071      differ: 2      dispatch miss: 0
```

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
