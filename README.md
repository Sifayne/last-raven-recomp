# last-raven

A static recompilation of **Armored Core: Last Raven Portable** (PSP) — turning
the game's Allegrex MIPS code into C ahead of time, linked against a native
runtime, to produce a real PC executable rather than an emulated one. Same model
as N64Recomp / *Zelda 64: Recompiled*.

**Status: Phase 0 complete — feasibility measured, verdict GO. The game boots
and reaches its frame loop; the intro movie is the current blocker.** Phase 0's
numbers are in [docs/findings/phase0.md](docs/findings/phase0.md); where things
stand right now, and how to measure them, is
[docs/findings/state.md](docs/findings/state.md).

Phase 1 added the differential oracle: an Allegrex interpreter sharing the
toolkit's decoder, runtime and HLE, so a disagreement with the recompiled C
localises to the emitter. It found **four silent-truncation bugs** in the
emitter, each one making the generated C do less than the hardware does — a
dropped fall-through, a missing label, a dropped `jr $ra` delay slot (where the
stack restore lives), and an indirect call treated as the end of a function.

Across all 26,462 discovered functions: **12,240 compared, 12,237 agree.** Of
the 3 that do not, 2 are an IEEE-754 NaN sign bit and 1 is documented as
unresolved. See [docs/findings/oracle.md](docs/findings/oracle.md).

## This repository contains no game data

No ROM, no ISO, no assets, no firmware, no decryption keys — and it never will.
You supply your own dump of media you own. `game/` is gitignored in its entirety,
as is all emitted C, since that is a derivative of the binary and is meant to be
regenerated rather than committed.

## Requirements

- CMake ≥ 3.16, a C/C++ compiler, `make`, Python 3
- `zlib` and `openssl` (for `pspdecrypt`)
- Your own dump of the game as `.iso` in `game/`

## Quick start

```bash
git clone --recurse-submodules <this-repo> && cd last-raven
```

```bash
scripts/build-tools.sh
```

Then drop your dump in `game/` and run the pipeline in order:

```bash
scripts/00-identify.sh && scripts/01-extract-decrypt.sh && scripts/02-analyze.sh
```

```bash
python3 scripts/03-imports.py | tee reports/03-imports.txt && scripts/04-emit-build.sh
```

Then the oracle, which needs stage 04's objects:

```bash
scripts/05-oracle.sh 400
```

Then the boot host, which loads the module, runs its constructors and threads
against a scheduler, a software GE and the HLE:

```bash
scripts/06-boot.sh
```

Stage 04 emits ~2.1M lines of C, compiles it, and links it against the runtime.
It takes a couple of minutes. The boot host runs the game today: it reaches its
frame loop and stalls on the intro movie — see
[docs/findings/state.md](docs/findings/state.md) for exactly where and why.

## Layout

| Path | What |
|---|---|
| `scripts/` | the pipeline, one stage per file — 00–06 bring-up, `07-autotests.sh` the behavioural-oracle scaffold, `verify-patches.sh` the patch-series check |
| `host/` | the native host: `boot` (module load, scheduler, GE rasterizer, HLE) and the link probe |
| `docs/` | decisions and findings |
| `patches/` | fixes and additions to vendored tools, applied by `build-tools.sh`, checked by `verify-patches.sh` |
| `tools/psprecomp` | submodule — the recompiler, runtime and interpreter (MIT) |
| `tools/pspdecrypt` | submodule — decryption, for the mode-9 path psprecomp lacks |
| `game/`, `reports/` | gitignored working directories |

## Why PSP and not PS2

Short version: the PS2 needs two instruction sets recompiled (VU1 runs its own
microcode program) and offers no clean game↔OS boundary, while PSP games call
named firmware functions through an enumerable import table. Long version with
the full comparison: [docs/DECISION-target.md](docs/DECISION-target.md).

## Licensing of vendored tools

`psprecomp` is MIT and is **linked into** anything this project builds.

`pspdecrypt` is **GPL-3.0** and is deliberately only ever invoked as a separate
process by `scripts/01-extract-decrypt.sh`. That keeps it at arm's length. Do not
link its code into the host or copy routines out of it without deciding what that
means for this project's own license first.

## Upstream

`patches/` carries changes to `psprecomp`, applied by `build-tools.sh` and
verified to apply to a pristine checkout in order — `scripts/verify-patches.sh`
is that check, and `--build` compiles and tests the pristine clone too.

The first three are upstream bugs with a fix that belongs in
[sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp); they are kept
here so a fresh clone reproduces the same build in the meantime:

| Patch | What |
|---|---|
| `0001` | `libm` was never linked — invisible on MSVC, a hard failure on Linux |
| `0002` | Two emitter codegen bugs: an invalid float literal, and a label-ordering hazard |
| `0003` | The interpreter oracle, filling an unchecked box in upstream's Phase 5 roadmap |

Patches `0004` onward are this project's bring-up — HLE additions (threads,
scheduler, disc filesystem, sceMpeg), discovery fixes, and the diagnostics the
findings documents lean on. They are driven from this repo's needs rather than
upstream's, and each patch's own diff carries the reasoning.
