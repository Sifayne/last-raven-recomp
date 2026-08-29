# The module is never relocated

Measured 2026-08-28, following the skip-category breakdown in
[skip-analysis.md](skip-analysis.md).

## What the profile showed

Recording the busiest address per budget-exhausted run, over 1,489 such runs:

| spin site | runs | share |
|---|---:|---:|
| **`0x00299BBC`** | **869** | **58%** |
| `0x00042468` | 46 | 3% |
| `0x0018CFD8` | 37 | 2% |
| `0x002A9214` | 32 | 2% |
| …a long tail | | |

One address accounts for well over half. The earlier guess — a few
non-terminating leaves poisoning thousands of callers — was right; an in-process
tally with a fixed 64-bucket cap had been hiding it behind a list of singletons.

## What that site is

```
00299B38  lui   $s0, 0x0
00299B3C  addiu $s0, $s0, 0        ; $s0 = 0
00299B40  or    $a2, $s0, $zero
00299B5C  lw    $t0, 4($a2)
00299B74  addiu $a2, $a2, 8
00299B78  lw    $t0, 0($a2)
00299B7C  bne   $t0, $zero, 0x00299B5C
```

A walk over an 8-byte-stride table, stopping at a zero entry. `$s0` is built by
a `lui`/`addiu` pair that both load **zero**, so the walk starts at address 0 and
scans `.text` — which is nonzero everywhere. It never terminates.

That `lui`/`addiu` pair is a HI16/LO16 **relocation** that was never applied.

## The module has 100,749 of them

| section | relocations |
|---|---:|
| `.rel.text` | 82,310 |
| `.rel.data` | 12,931 |
| `.rel.rodata` | 5,278 |
| others | 230 |
| **total** | **100,749** |

`readelf -r` reports "no relocations" because PRX uses a nonstandard section
type (`LOPROC+0xa0`), not because there are none.

None are applied. Every address literal that should point into the data segment
is currently 0.

## Why the earlier reasoning was wrong

`cmd_interp` carries a comment saying relocations are deliberately not applied,
because doing so would move addresses away from the ones the emitter used and
make the two sides incomparable. That is wrong, and the mistake is worth naming:
it conflates *relocating to a different load address* with *applying
relocations at all*.

PRX relocations encode a **segment index**. This module has two segments, at
`0x00000000` and `0x00331EE0`. A relocation against segment 1 should resolve to
`0x00331EE0 + addend` — a data address. Leaving it unapplied yields 0, which is
the first instruction of `.text`.

Mapping both segments at their linked addresses and applying the relocations
changes **only the data pointers**, which is exactly what is needed. Code
addresses stay where the emitter put them, so the two sides remain comparable.

## What this means

The dominant cause of non-termination is not missing HLE, and not the absence of
a thread scheduler. It is that **the module was never loaded properly**. Both
sides are equally affected — the recompiled C has the same literal 0 baked in —
so it never appeared as a divergence, only as runs that do not finish.

It also means the skip count was never a measure of how close the game is to
booting. Three separate causes have now been found behind it, in order of
discovery: seeded arguments that were really pointers into `.text`, seeded
lengths that were really addresses, and this. Each looked like the answer until
it was measured.

`BRINGUP.md` lists "implement minimal module loader to apply relocations" as a
Phase 2 item, and `psp_collect_pointer_seeds` already parses these same tables to
harvest function pointers — so the parsing exists and only the applying is
missing.

## Next

Apply relocations at load time, in `cmd_interp` and in `oracle_diff`, with both
segments at their linked bases. Then re-measure: the expectation is that the
`0x00299BBC` cluster disappears and the comparable set grows substantially.

That is a prerequisite for the boot milestone rather than a detour from it —
static constructors and a thread scheduler are both pointless while every global
pointer in the module reads as null.
