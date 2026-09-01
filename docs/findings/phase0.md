# Phase 0 — feasibility spike

**Verdict: GO.**

Measured 2026-08-27 against `NPUH-10024` (US PSN SKU), module `ACLR_App` v1.1.
Reproduce with `scripts/00-identify.sh` … `04-emit-build.sh`.

Every number here is per-build. A different SKU is a different binary — see
[Which disc](#which-disc).

---

## The six metrics

| # | Metric | Result | Baseline |
|---|---|---|---|
| 1 | Decode coverage | **100.00%** — 758,015 / 758,052 words, **0 unknown** | 92% on psprecomp's own bring-up title |
| 2 | Function discovery | **26,487 functions**, 97.8% of `.text` reached | — |
| 3 | VFPU | **331 / 741,848 instructions (0.04%)**, 127 functions (0.5%) | 0.08% trap rate upstream |
| 4 | Import surface | **218 functions / 25 libraries; 96 implemented (44.0%), 122 missing** | psprecomp registers 112 NIDs total |
| 5 | Threading | **Required.** `sceKernelCreateThread` imported; `ThreadManForUser` is the largest single library | psprecomp has **no scheduler** |
| 6 | Emit → compile → link | **PASS.** 2,123,791 lines of C, compiles at `-O0` in ~45s, links to a 25 MB executable | psprecomp's stated bar for a retail module |

Metric 1 is the headline. A clean 100% with zero unknown words means the
Allegrex decoder has no gaps on this binary — the single most likely way for a
recompilation to be impossible, ruled out.

---

## What makes this title favourable

**The VFPU is almost unused.** 331 vector instructions in a 758,000-word 3D
action game. Last Raven Portable is a PS2 port, and its geometry path appears to
have been carried over as scalar FPU code rather than rewritten for the PSP's
vector unit — `lwc1` alone is 2.35% of all instructions. The VFPU was the
subsystem most likely to sink this, and it is a rounding error here.

**Indirect control flow resolved completely.** 55 jump tables resolved to 615
targets with **0 unresolved computed-jump sites**. Jump tables were the flagged
risk for function discovery and they cost nothing.

**Ad-hoc multiplayer is separable.** `sceNetAdhoc` + `sceNetAdhocctl` + `sceNet`
+ `sceWlanDrv` = 27 imports, none implemented — but all of it serves ad-hoc
multiplayer. It can be stubbed to failure for a single-player bring-up and the
work deferred indefinitely.

---

## What the work actually is

122 missing firmware functions, and the shape matters more than the count:
these are mostly *whole absent subsystems*, not scattered gaps.

| Library | Missing | What it is |
|---|---:|---|
| `sceMpeg` | 23 / 23 | PMF video — the intro and cutscenes |
| `sceNetAdhoc` + `ctl` + `sceNet` + `sceWlanDrv` | 27 / 27 | ad-hoc multiplayer — **deferrable** |
| `IoFileMgrForUser` | 14 / 23 | file I/O — needed to load anything |
| `sceAtrac3plus` | 13 / 13 | ATRAC3+ audio — `bgm/` is all `.at3` |
| `sceUtility` | 11 / 11 | system dialogs, save data |
| `ThreadManForUser` | 7 / 28 | threading |
| `sceSasCore` | 6 / 27 | hardware voice mixing |
| `sceUmdUser` | 6 / 6 | disc access |
| others | 15 | display, power, ctrl, impose, suspend, sysmem |

Fully covered already: `sceGe_user` (10/10), `sceAudio` (9/9), `StdioForUser`,
`Kernel_Library`, `LoadExecForUser`.

Two blockers are **psprecomp-wide, not game-specific** — they block every title
and are the right place to contribute upstream rather than work around here:

1. **No thread scheduler.** Confirmed required, not merely suspected.
2. **The GE does not rasterise.** It counts transformed geometry. Nothing
   renders until that changes.

---

## Upstream bugs found and fixed

Three, patches `0001`-`0003`, now the first three of our commits on the fork.

**`0001-cmake-link-libm-on-unix.patch`** — `src/vfpu.c` calls `sinf`/`cosf`/
`powf`/`logf`/`asinf` but the library never linked `libm`. Invisible on MSVC,
where those live in the CRT; a hard link failure on Linux. Fixed on the library
target as `PUBLIC` so consumers inherit it.

**`0002-emit-fix-float-literal-and-label-ordering.patch`** — two codegen bugs,
which together were the *only* 3 errors in 2.1M generated lines:

- `%.9g` drops the decimal point on whole values, so the `f` suffix landed on
  what C reads as an integer constant. The emitter produced `psp_vimm(0, -1f)`,
  which is not a literal. Also handles inf/NaN halves, which have no literal
  form at all.
- **A label-ordering bug.** `is_label[fn->addr]` was set *after* the pass that
  propagates delay-slot labels to the following address. When a function's entry
  is itself the delay slot of the instruction that pulled in an earlier block
  (`fn->start < fn->addr`), that pass read the flag before it was set and never
  marked the landing address — while emission still emitted `goto L_<addr>`
  against a label nothing declared. Moving the assignment before the pass fixes
  it and recovers one interior entry (32,067 → 32,068).

The second is worth reporting upstream: it is an ordering hazard, not a typo,
and upstream's own comment shows they hit the same class of bug once before.

---

## Caveats — read before trusting the numbers

- **Nothing has executed.** Metric 6 is compile-and-link. The link probe
  deliberately does not start the game. "It links" is a long way from "it runs",
  and further still from "it renders."
- **213 instructions (0.03%) inside discovered code do not decode**, even though
  the linear `cover` pass reports zero unknown. The two passes use different
  denominators: `cover` walks the `.text` extent, `funcs` walks only what
  discovery reached. The likely reading is that discovery pulled data into some
  function extents — consistent with the **234 functions flagged "scattered"
  (suspect boundaries)**. Worth confirming before trusting per-function output
  in those regions.
- **10,128 functions have no `jr $ra`** — tail calls, or discovery losing the
  trail. Upstream treats this as normal; it has not been verified here.
- **Missing NIDs are unnamed.** `scripts/03-imports.py` reports the NIDs it
  cannot match, but naming them needs a NID table (PPSSPP has a complete one).
  Worth importing before the HLE work starts, so the missing list reads as
  function names instead of hashes.
- Measured in a single session on one machine. Re-run before betting months on
  it.

---

## Which disc

The dump analysed is **`NPUH-10024`**, the US **PSN digital** SKU — *not* the
UMD retail SKU (`ULUS-10493`) the plan originally assumed. `PARAM.SFO` declares
`PSP_SYSTEM_VER 5.50`, but the EBOOT is signed with tag `0xD91612F0`, a
6.00–6.20-era key: it was re-signed for digital distribution. A UMD dump is a
different binary with different addresses.

`scripts/00-identify.sh` warns if the disc in `game/` is not this one.

Decryption is `decrypt mode 9`, which **psprecomp cannot do** — it implements
the KIRK primitives but not the mode-9 transform that builds a CMD1 header from
a `~PSP` header (its `docs/DECRYPT.md` says so and recommends the alternative).
The pipeline uses `pspdecrypt` instead. Verified by size: the plaintext is
exactly the 4,192,292 bytes the `~PSP` header declares.

---

## Recommended next step

Not the renderer, and not the missing HLE.

**Stand up the oracle first.** psprecomp ships a three-tier differential testing
setup (`docs/ORACLE.md`) — built-in interpreter, PPSSPP headless, and
`pspautotests` against real-hardware output. Two codegen bugs surfaced here from
*compiling alone*; the ones that matter will not announce themselves that
politely. Getting the interpreter oracle running before writing HLE means every
subsequent bug is localised to the emitter by construction, instead of being
debugged through a game that does not boot.

Then follow `docs/BRINGUP.md` in its stated order — foundational codegen, then
static constructor tables (upstream's #1 blocker: global state silently never
initialised), then the thread scheduler.
