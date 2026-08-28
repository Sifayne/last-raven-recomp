# Decision: target the PSP version, not the PS2 original

**Decided 2026-08-27. Settled — reopen only with new evidence.**

Armored Core: Last Raven exists as a PS2 original (2005) and a PSP port,
*Last Raven Portable* (2010). A static recompilation must pick one. This picks
the PSP version.

The reasoning is structural, not a judgement about which version is the better
game. The PS2 original is the definitive presentation — 60 fps target, higher
internal resolution, and a control scheme built for a Dual Shock. It is also
dramatically harder to recompile, for reasons that have nothing to do with how
good it is.

## The comparison

| | PS2 | PSP |
|---|---|---|
| Processors to recompile | EE + VU0 + VU1 + IOP | one Allegrex core |
| Vector unit | VU1 runs **its own microcode program** — a second ISA | VFPU is an ISA extension on the same core |
| OS boundary | games poke DMA and hardware registers directly | games call named `sceXxx` NIDs through an **import table** |
| GPU | Graphics Synthesizer — register-poke rasteriser, 4 MB eDRAM, no shader model | GE — fixed-function display-list processor |
| Reference implementation | PCSX2, whose EE/VU/GS code is hard to lift | PPSSPP, 15 years of HLE for every `sce` module |
| Tooling | `ran-j/PS2Recomp` — earlier stage, VU1 explicitly incomplete | `sp00nznet/psprecomp` — end-to-end pipeline that runs |

### The decisive one

The import table. A PSP game's requests to the operating system are a finite,
enumerable list sitting in the binary — 218 functions across 25 libraries for
this title, which `scripts/03-imports.py` reads directly. That gives a clean seam
to cut between game code and OS, and a measurable definition of "done" for the
HLE work.

PS2 games have no such seam. They talk to hardware. There is no list to
enumerate and no boundary to stub against.

### The second one

VU1. It is a separate processor running a separate program in a separate
instruction set, and it does all the geometry. Recompiling a PS2 game means
solving it twice. `PS2Recomp` lists VU1 microcode as incomplete, and that is the
wall a PS2 attempt would hit first.

By contrast — and this was not knowable in advance — Last Raven Portable barely
uses the PSP's vector unit at all: **331 VFPU instructions out of 741,848**. Its
geometry path was evidently carried over from the PS2 as scalar FPU code. The
subsystem most likely to sink the PSP effort turned out to be a rounding error.

## What this costs

- 480×272 source assets and a ~30 fps target
- A control scheme designed around the PSP's **missing second analog stick**

The second is the notable one, and in a recompilation it inverts: source-level
access means dual-analog camera control can be added, which is exactly what
*Zelda 64: Recompiled* did. That is a headline feature of the eventual port, not
a limitation of it — but it is real work, and it is not Phase 0's problem.

## Verdict

Confirmed by measurement. Phase 0 recorded 100.00% decode coverage with zero
unknown words and a clean compile-and-link of all 26,487 discovered functions.
See [findings/phase0.md](findings/phase0.md).

Nothing here argues the PS2 version is not worth doing. It argues it is a
different, much larger project — and that if a PS2 attempt ever happens, it
should start from a working PSP port rather than from nothing.
