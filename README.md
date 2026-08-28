# last-raven

A static recompilation of **Armored Core: Last Raven Portable** (PSP) — turning
the game's Allegrex MIPS code into C ahead of time, linked against a native
runtime, to produce a real PC executable rather than an emulated one. Same model
as N64Recomp / *Zelda 64: Recompiled*.

**Status: Phase 0 complete — feasibility measured, verdict GO. The game does not
run.** See [docs/findings/phase0.md](docs/findings/phase0.md) for the numbers and
what is still missing.

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

Stage 04 emits ~2.1M lines of C, compiles it, and links it against the runtime.
It takes a couple of minutes and produces a link probe, not a playable game.

## Layout

| Path | What |
|---|---|
| `scripts/` | the Phase 0 pipeline, one stage per file |
| `host/` | the native host. Currently just a link probe |
| `docs/` | decisions and findings |
| `patches/` | fixes to vendored tools, applied by `build-tools.sh` |
| `tools/psprecomp` | submodule — the recompiler and runtime (MIT) |
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

Three bugs found during bring-up are carried in `patches/` — a `libm` link
failure and two emitter codegen bugs. They belong upstream in
[sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp); the patches are
kept here so a fresh clone reproduces the same build in the meantime.
