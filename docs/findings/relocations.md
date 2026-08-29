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

## Why the earlier reasoning was half wrong

`cmd_interp` carried a comment saying relocations are deliberately not applied,
because doing so would move addresses away from the ones the emitter used and
make the two sides incomparable.

The first pass at this dismissed that comment. That was too quick — it is right
about the hazard and wrong only about the conclusion.

Applying relocations to **guest memory after loading** does exactly what the
comment warns about. The interpreter fetches instructions from memory, so it
sees the patched version; the recompiled C had its address literals baked in
when the emitter read the *file*, so it does not. The same instruction then
computes two different addresses. Doing this produced 108 divergences in 1,500
functions, every one of them manufactured by the loader.

Measured directly, before the fix:

```
/* 00299B38  lui $s0, 0x0 */
r_s0 = 0x00000000u;          <- generated C, unrelocated
```

against an interpreter that had just computed `0x00331EE0` from the same
instruction.

The resolution is neither "skip relocations" nor "relocate memory". It is to
relocate the **file image**, before analysis and emission, so the emitter and
the interpreter consume identical bytes. Both segments stay at their linked
addresses, so no code address moves and every `psp_func_XXXXXXXX` keeps its
name; only pointers into the data segment change, which is the entire intent.

After that change the same generated function reads:

```
/* 00299B38  lui $s0, 0x33 */
r_s0 = 0x00330000u;
```

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

## Result

Relocation happens in `load_and_discover`, so `emit`, `funcs` and `cover` all
consume the same relocated image. 102,615 relocations are applied.

The function that started this — `0x00299B30` — now runs to a clean return in
915,297 instructions. It was never an infinite loop: it is a real module
initialiser sorting 3,420 entries, and it only looked like a hang because its
table pointer read as null and the instruction budget was 200,000.

Full corpus, 25,555 discovered function entries:

| | before relocation | after |
|---|---:|---:|
| compared | 12,240 | **12,503** |
| agree | 12,237 | **12,480** |
| unexplained divergences | 1 | **0** |
| "trapped" | 13,670 | **6,582** |
| runs the watchdog had to kill | 300 | **0** |

The trap category halved, and what remains is now genuinely dominated by the
vector unit — `mtv`, `vrot`, `vcmov`, `vhdp`, `vcrsp` — which is what the earlier
write-up wrongly claimed of the pre-relocation number.

**Every divergence has a verdict.** 21 of 23 are dispatch misses: the interpreter
follows an indirect jump to whatever it computes and the recompiled side hands
the same address to `psp_dispatch`, which does nothing when discovery never
registered it. That is a gap in discovery's coverage, not in the code generated.

The other 2 — `0x0009F61C` and `0x001773A0` — are stores landing inside `.text`.
At `0x001773A0` the write is to `0x001773C0`, which is inside that same function.
Once code is overwritten the two sides diverge by construction: the interpreter
fetches instructions from memory and runs the modified version, while the
recompiled C was fixed at compile time. The oracle cannot compare
self-modifying code, and this is that.

The largest skip category is now **HLE re-entry at 6,254** — handlers calling
back into guest code. Making the interpreter service `psp_dispatch` itself would
move most of those into the comparable set, and is the obvious next lever.

Two adjustments came with it:

- **The instruction budget went from 200,000 to 4,000,000.** Real work happens
  now, and the old ceiling was reporting legitimate initialisers as hangs.
- **Guest re-entry from HLE is stopped at the source**, with a dispatch budget of
  one, rather than detected afterwards. Relocating made this urgent: guest code
  now reaches real thread-creation and callback paths, so handlers re-enter far
  more often, and native recompiled code has no instruction budget to stop it.
  Detecting after the fact cannot prevent a hang that has already happened —
  a 300-function run went from over ten minutes to nine seconds.

## Next

The boot milestone, now on a module that is actually loaded: static constructor
tables, `$k0`/reent setup, then a thread scheduler.
