# Where this is, and how to find things out

Written at a session boundary. The commits carry the reasoning for individual
changes; this is the part that lives between them — what the game currently
does, which instrument answers which question, and what has already been ruled
out so it is not investigated twice.

## What the game does today

It boots. Constructors run, `module_start` returns, the disc is read through
async I/O, the intro movie ends, and a frame loop runs to a steady state:

```
entry:     returned
bad mem:   0 accesses
disc read: 1,912,832 bytes
pixels:    2,350,081 drawn by the rasterizer
```

The frame is a single flat colour, and that is correct: the only geometry
reaching the rasterizer is nineteen untextured full-screen quads — screen
clears — drawn with no vertex colour, which defaults to white. There is nothing
else to draw yet.

**The renderer is not the blocker.** The game stops issuing geometry because it
reaches a false deadlock (task #1), not because anything about the GE is
missing. Work on textures, sampling or the block transfer will not change the
picture until that is fixed.

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

## Open work, in the order it is worth doing

1. **Task #1 — two slots marked RUNNING at once.** The false deadlock that
   stops the game. Everything else is downstream of it. The task carries the
   suspect path and a cheap assertion that should localise it in one run.
2. **Tasks #22 / #24 / #32 are one bug.** Blocks promoted to function entries
   by the relocation-pointer seed scan, whose back-edges stay C calls.
   `merge_shared` skips them precisely because they are known entries. This is
   the deepest thing still known-broken in the emitter.
3. **Task #25 — the two composed-chain oracle divergences.** Each callee agrees
   in isolation; the disagreement only appears in the chain. Needs
   instruction-level trace diffing, not another hypothesis.

## The patch series

Everything upstream-able lives in `patches/`, applied to the `tools/psprecomp`
submodule by `scripts/build-tools.sh`. After changing anything under
`tools/psprecomp`, regenerate the affected patch and check the whole series
still applies to a pristine checkout **and builds green there** — a stale build
directory will happily report 11/11 for code you did not build.
