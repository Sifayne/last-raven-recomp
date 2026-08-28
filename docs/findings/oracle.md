# Phase 1a — the differential oracle

**Status: built, validated, and producing comparisons. 10 divergences are open
leads, not confirmed bugs.**

Reproduce with `scripts/05-oracle.sh`. Measured 2026-08-27 against `NPUH-10024`,
module `ACLR_App`.

---

## What was built

psprecomp's `docs/ORACLE.md` describes a three-tier oracle, of which tier 1 —
an Allegrex interpreter sharing the toolkit's own decoder and runtime — was an
**unchecked box in its Phase 5 roadmap**. It did not exist. So this phase was
writing it, not wiring it up.

| Piece | Where | What |
|---|---|---|
| Interpreter | `patches/0003` → `tools/allegrexrecomp/interp.{c,h}` | Executes Allegrex on the shared `psp_cpu`, memory model, and `recomp_rt.h` helpers |
| CLI | `allegrexrecomp interp <elf> [--from] [--trace] [--regs]` | Runs and traces a module |
| Unit tests | `tests/test_interp.c` | 12 cases, hand-written encodings, no game data |
| Harness | `host/oracle_diff.c` | Runs each function both ways and compares |

The design property that matters: the two sides share **everything except
sequencing**. Decode, arithmetic, memory and HLE are common code, so they cannot
produce a false divergence. Only the emitter and the interpreter's own
control-flow handling can.

## Validation

The unit tests target precisely the part that is *not* shared — delay slots,
described in psprecomp's own notes as "the single most error-prone thing in MIPS
recompilation":

- a taken branch runs its delay slot
- a **not-taken plain branch also runs its delay slot**
- a **not-taken *likely* branch nullifies it**
- `jal` links to `pc+8`, past the slot
- `jr $t9` uses the value `$t9` held *before* the slot ran
- `bltzal` links even when not taken

Plus a drift guard: 70 real encodings covering everything `emit.c` translates,
asserting the interpreter implements the same set. If the emitter grows a case
the interpreter lacks, the oracle would start blaming the game for a hole in
itself; the guard fails first.

Writing those tests immediately caught three bad encodings in the test table
(`clz` is SPECIAL `0x16`, `wsbw` is BSHFL `sa=3`). They work.

## It runs real code

`allegrexrecomp interp` executes the module entry to a clean return in 62
instructions with **zero bad memory accesses**. The trace is legible and
correct: `lui $v0,0x505` / `ori $a0,$v0,0x10` builds `0x05050010` — the devkit
version from the `~PSP` header — on its way to `sceKernelSetCompiledSdkVersion`.
Calls into the import-stub region find the unlinked `jr $ra` and return.

The `$ra` sentinel survives a real stack spill and reload, which is the
mechanism that makes single functions testable in isolation.

## Results

400 functions attempted:

| Outcome | Count |
|---|---:|
| **compared** | **96** |
| — agree | **86** |
| — differ | **10** |
| skipped: reached an import | 58 |
| skipped: trapped on one side | 215 |
| skipped: `$sp` unbalanced (entry not independently callable) | 31 |

The skip categories are as important as the comparisons:

**Imports (58).** The recompiled stub calls `psp_hle_call()`; the interpreter,
running the *unlinked* module, finds a bare `jr $ra`. Guaranteed disagreement
that says nothing about codegen. **Routing the interpreter's stub hits into HLE
is the single highest-value next change** — it would move most of these 58 into
the comparable set.

**Traps (215).** Mostly VFPU with no implementation behind it, on both sides.

**Unbalanced `$sp` (31).** Discovery splits a function at any address something
points at, so an "entry" can land past the prologue that allocated the frame.
Entering there, the epilogue's `lw $ra, N($sp)` reads a slot no prologue wrote.
A well-formed function restores `$sp`; anything else was never independently
callable and the comparison is meaningless rather than failing.

## The 10 divergences are not yet findings

Stated plainly because it would be easy to misread the table above: **none of
the 10 has been confirmed as an emitter bug.**

Three separate *harness* defects were found and fixed during bring-up, each of
which produced divergences indistinguishable from real codegen errors:

1. **Unbounded work-list.** `--limit` counted successful comparisons, not
   attempts, so skipped functions were unbounded — the harness ground through
   all 26,487 functions and appeared to hang.
2. **No reset between functions.** Each function inherited the previous one's
   memory writes. Later functions read code as data; registers ended up holding
   MIPS instruction words, which looks exactly like a codegen bug.
3. **Testing non-callable entries.** Before the `$sp` filter, 36 of 175
   comparisons "differed" — almost all of them mid-function entries. `0x24`,
   the first, turned out to be `sw $a0, 0($sp)` nine instructions into the
   function at `0x0`.

Given a base rate that high, each remaining divergence needs individual triage
before it is called anything. One hypothesis was tested and **disproved**:
restoring all 32 MB of RAM instead of a 1 MB stack window changed nothing, so
incomplete state restoration is not the cause.

### The lead worth pulling first

`0x00003428`. The saved-`$ra` stack slots differ — interpreter `0x000034B8`,
recompiled `0x00003464` — so **the two sides took different branches**, not
merely computed different values. The function loads a global pointer
(`lw $a1, -2740($s1)`), dereferences it, and branches on bit 10 of the result
(`srl $a2, $a0, 10` / `andi $a2, $a2, 1`). The recompiled side also leaves `$s1`
= `0x00320000` where the interpreter restores it to its entry value, so one side
is not running the epilogue it should.

Reproduce:

```bash
build/host/oracle_diff game/extracted/ACLR_App.elf --from 0x3428 --verbose
```

```bash
build/psprecomp/tools/allegrexrecomp/allegrexrecomp interp game/extracted/ACLR_App.elf --from 0x3428 --regs --budget 200
```

## Known limits

- The interpreter has no HLE. Firmware calls are unlinked `jr $ra` returns.
- No thread scheduler on either side; this is single-threaded execution only.
- Argument registers are seeded with small pseudo-random values from a fixed
  LCG. Deterministic and reproducible, but it exercises one input per function,
  not a range.
- Callee-saved registers, `$v0`/`$v1`, `$sp`/`$fp`/`$gp`, `hi`/`lo`, all RAM and
  the module image are compared. Caller-saved `$t*`/`$a*` are not — the two
  sides are entitled to leave different garbage there.
- The recompiled side gets a 2-second wall-clock watchdog, because unlike the
  interpreter it has no instruction budget and a runaway loop never returns.

## Next

1. Route the interpreter's import-stub hits into `psp_hle_call`, so both sides
   cross the firmware boundary the same way. Unlocks ~58 more comparisons per
   400 and is the prerequisite for comparing anything that does real work.
2. Triage `0x3428`, then the other nine.
3. Send `patches/0001`–`0003` upstream. The interpreter fills a roadmap gap the
   author had already scoped, and the label-ordering fix in `0002` is a bug they
   would want.
