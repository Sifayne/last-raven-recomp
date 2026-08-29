# Phase 1 — the differential oracle

**Status: goal reached. Four real emitter bugs found, fixed, and confirmed;
every divergence across the full corpus has a verdict.**

12,240 functions compared, **12,237 agree**. Of the 3 that do not, 2 are an
IEEE-754 NaN sign bit (not a bug) and 1 is documented as unresolved with a
reproduction.

Reproduce with `scripts/05-oracle.sh`, then `scripts/06-triage.py`. Measured
2026-08-27 against `NPUH-10024`, module `ACLR_App`.

---

## What the oracle is

psprecomp's `docs/ORACLE.md` describes a tier-1 interpreter oracle in the
present tense, but it was an **unchecked box in its Phase 5 roadmap** — `src/`
had no interpreter. Phase 1 wrote one.

| Piece | Where |
|---|---|
| Allegrex interpreter | `patches/0003` → `tools/allegrexrecomp/interp.{c,h}` |
| CLI | `allegrexrecomp interp <elf> [--from] [--trace] [--regs]` |
| Unit tests | `tests/test_interp.c` — 12 cases, hand-written encodings |
| Differential harness | `host/oracle_diff.c` |
| Triage | `scripts/06-triage.py` — clusters divergences by signature |

The property that makes it work: both sides share the decoder, CPU state,
memory model, semantic helpers **and HLE**. Only instruction sequencing differs.
So a disagreement localises to the emitter or to the interpreter's control flow,
and nowhere else.

---

## The four bugs

All three are the same class: **the emitted C silently does less than the
hardware does.** No dispatch miss, no bad memory access, no crash — just a
missing instruction. That is the worst failure mode available, and it is
exactly what a differential oracle is for.

### 1. Fall-through into a label is dropped

`emit.c` continued execution when the address after a function's extent was
another function's **entry**, and dropped it when the address was a **label
inside** another function.

Found as a `$v0` disagreement on 27 functions, all reached through `memset`
(`0x002A91DC`). Discovery splits memset's word-fill loop into its own function;
the loop's not-taken exit falls through into the byte-fill tail, which belongs
to the neighbouring body. The emitted loop just returned. So **memset skipped
its trailing 1–3 bytes and never ran the `jr $ra` delay slot that sets its
return value** — it returned whatever was already in `$v0`.

Confirmed outside the harness with a direct call: `$v0 = 0` before, `$v0 = $a0`
after.

### 2. …and the label it needs does not exist

Fixing (1) exposed the next layer. The continuation targets an address that has
no label and no dispatch entry, because labels are only emitted for addresses
something *branches to*. The dispatch missed and the continuation was lost
anyway.

At `0x00003630` the lost continuation was the epilogue itself:

```
0000366C  lw    $ra, 16($sp)
00003670  jr    $ra
00003674  addiu $sp, $sp, 32     ← delay slot restores the frame
```

Symptom: the function returns with `$sp` still holding its frame — visible in
the triage output as clusters of `$sp-0x10`, `$sp-0x20`, `$sp-0x40`.

The fix is a pre-pass over all functions before emission, marking every
fall-through target so its owner emits a label there. It has to be a pre-pass:
the function that *needs* the label is not the one that discovers it is needed,
and emission runs in address order.

**Worth noting for upstream:** psprecomp's HEAD commit message is *"shared-epilogue
+32 is correct, the -64 group is not explained."* The `$sp-0x40` cluster here is
that −64 group.

### 3. A return's delay slot is dropped when the analyzer does not own it

The delay slot was only emitted when discovery had assigned that word to the
same function:

```c
if (in.has_delay_slot && owned_by(an, a + 4, owner)) { ... }
```

A delay slot executes because the hardware executes it. Whether discovery
assigned the word to this function is an artifact of the analysis and has no
bearing on that. Where the two disagreed, the instruction was dropped.

This is the most damaging of the three, because of *which* instruction it
usually is. A MIPS compiler puts the stack restore in the delay slot of
`jr $ra`, so the dropped instruction is typically `addiu $sp, $sp, N` — the
function returns having never released its frame, and the damage lands in the
caller. `0x0000355C` emitted three instructions and silently discarded the
fourth:

```
0000355C  lw    $s0, 0($sp)
00003560  lw    $ra, 4($sp)
00003564  jr    $ra
00003568  addiu $sp, $sp, 16     ← never emitted
```

Ownership still decides whether the slot also needs a *labelled* copy, which is
a question about who can jump to it — and that ownership does answer.

### 4. An indirect call is treated as the end of a function

The terminal test read:

```c
last_terminal = in.is_return || in.is_indirect || (in.is_jump && !in.is_call);
```

`is_indirect` covers `jr` and `jalr` alike, and only one of them ends anything.
`jalr` is a **call**: it returns, and execution continues after its delay slot.
Marking it terminal suppressed the end-of-function continuation, so wherever a
function's extent ended just after an indirect call — exactly where discovery
splits — whatever followed was never emitted.

What followed was the epilogue. `0x002B0878` ended on `psp_dispatch(r_v1)` and
never reached the `lw $ra` / `jr $ra` / `addiu $sp, $sp, 16` three instructions
later. This was the cause of every remaining `$sp` cluster.

The decoder already draws the distinction — it sets `ends_block` for `jr` and
not for `jalr` — so this was the emitter failing to use information it already
had.

### Effect

Same 400 functions, same seeds, one variable changed at a time:

| | compared | agree | differ |
|---|---:|---:|---:|
| before any fix | 118 | 101 | **17** |
| after fix 1 (fall-through into a label) | 118 | 110 | **8** |
| after fix 2 (force the label to exist) | 118 | 111 | **7** |
| after fix 3 (delay-slot ownership) | 118 | **118** | **0** |

Fix 4 shows up in the corpus rather than this sample: it needs an indirect call
at the end of a function's extent, which none of the first 400 happen to have.

The 27-function `$v0` cluster and every `$sp` cluster are gone.

---

## Corpus results — every divergence has a verdict

All 26,462 discovered function entries, `reports/05-oracle-full.txt`:

| | |
|---|---:|
| attempted | 26,462 |
| **compared** | **12,240** |
| **agree** | **12,237 (99.98%)** |
| **differ** | **3** |

Skipped, and why each category is a skip rather than a failure:

| | count | |
|---|---:|---|
| trapped | 13,670 | an unimplemented instruction on one side, overwhelmingly VFPU |
| interpreter hung | 300 | an HLE handler re-entered guest code and landed somewhere that does not return |
| unbalanced `$sp` | 249 | the entry was not independently callable — discovery split past a prologue |
| import thunk | 25 | the firmware boundary, not guest code |
| host fault | 3 | recompiled code runs on the host stack; a deep guest chain overflows it |

### Verdicts on the remaining 3

**2 of 3 — NaN sign bit. Not a bug.** `0x00003630` and `0x001FC6A8` differ by one
stack word: `7FC00000` against `FFC00000`. Those are quiet NaNs differing only in
the sign bit. IEEE-754 leaves the sign of a NaN produced by an invalid operation
unspecified, and the two sides are compiled at different optimisation levels —
the interpreter inside `liballegrex_core` at `-O3`, the generated code at `-O0`.
Both results are equally correct.

**1 of 3 — unresolved.** `0x000FDE14` differs only in `$v0`
(`interp=00000000`, `recomp=001949A0`). Its three callees each agree when tested
in isolation, so the disagreement is state-dependent and only appears in the
composed chain. Resolving it needs instruction-level trace diffing rather than
another hypothesis; `allegrexrecomp interp --regs` produces one side of that
already.

```bash
build/host/oracle_diff game/extracted/ACLR_App.elf --from 0xFDE14 --verbose
```

## After merging shared blocks

Discovery used to split one function in two whenever walk order made two walks
share a block, which turned loop back-edges into C calls (see the commit *merge
functions that share a block instead of splitting them*). Merging them changed
what the oracle can even see, so the corpus numbers above are not comparable
line for line. A 5,000-entry sample of the merged build, `reports/05-oracle.txt`:

| | count |
|---|---:|
| attempted | 5,000 |
| **compared** | **1,481** |
| **agree** | **1,480** |
| **differ** | **1** |
| unbalanced `$sp` | 10 |
| host fault | 0 |

Two things moved, and one is the point of the change:

**Unbalanced `$sp` fell from 0.94% of attempts to 0.20%** (249/26,462 against
10/5,000). That is the same root cause: a block split out of the middle of a
function has no prologue, so calling it directly could never balance the stack.
The samples cover different parts of the corpus, so treat the ratio as
indicative rather than exact — but the direction is not in doubt.

**Host faults went from 3 to 0.** Those were host stack exhaustion, which is
what a loop emitted as recursion produces.

### The one divergence: 0x0001093C

Not a regression. It was previously reported as a spin (`SPIN 0001093C
pc=00010954`), and 0x00010954 is the head of its first copy loop -- the loop
whose back-edge used to be a nested call. It recursed twenty-four deep and
exhausted the budget instead of being compared at all. Merging made it
terminate, and only then could it disagree.

The disagreement is in the composed chain, not the call site. The `jal` at
0x000109CC emits its delay slot correctly (`$a1 = 1` before the call), and its
callee 0x00135D1C **agrees when tested in isolation**. That is the same shape as
the unresolved 0x000FDE14 above, and needs the same instrument:
instruction-level trace diffing rather than another hypothesis.

```bash
build/host/oracle_diff game/extracted/ACLR_App.elf --from 0x1093C --verbose
```

## Method notes, and why they are not incidental

Four *harness* defects were found and fixed along the way, each producing
divergences indistinguishable from codegen bugs. The base rate matters when
reading any remaining divergence:

1. **Unbounded work-list.** `--limit` counted successful comparisons rather than
   attempts, so a run ground through all 26,487 functions and looked like a hang.
2. **No reset between functions.** Each function inherited the previous one's
   writes; later ones read code as data and ended with MIPS instruction words in
   registers.
3. **Non-callable entries.** 36 of 175 early "divergences" were mid-function
   addresses. `0x24` is `sw $a0, 0($sp)`, nine instructions into the function at
   `0x0`.
4. **`--from 0x0` scanned everything**, because 0 was both a valid address and
   the "unset" sentinel.
5. **HLE state was never reset between runs.** The snapshot covered `psp_cpu`,
   RAM and the module image, but the firmware layer keeps state in ordinary C
   statics none of those reach — the allocator's block table above all. The
   interpreter's allocations persisted into the recompiled run, which then got
   *different addresses back from the same calls*. It showed up as
   `interp=0400C000 recomp=0400C200` against a VRAM base: entirely plausible,
   entirely an artifact.
6. **Seeded arguments pointed into `.text`.** Argument registers were seeded
   with small values (0..0x3FFF), and the module maps at 0 — so any function
   treating an argument as a pointer scribbled on its own code. At that point
   the two sides diverge *by construction*, because the interpreter fetches
   instructions from memory and runs the corrupted version while the recompiled
   C was fixed at compile time. Pointing the arguments into RAM instead removed
   five of the last eight divergences and raised comparability from 10,740 to
   12,240.

Two hypotheses were tested and **disproved**, which is worth recording so they
are not retried: restoring all 32 MB of RAM instead of a 1 MB stack window
changed nothing; and the stdio-deadlock theory for the early hangs was wrong —
the real cause was HLE re-entry, and the fix was extending the watchdog to cover
the interpreter side.

## Known gaps

- **Bugs 1 and 2 have no regression test.** Reproducing them needs discovery to
  leave a fall-through target as a label inside a neighbouring function, and
  every synthetic shape tried got promoted to its own entry — a test that passes
  before *and* after the fix, which is worse than none. Pinning down
  `a_discover`'s ownership rules is the prerequisite. Noted in `test_emit.c`.

  Bugs 3 and 4 **do** have tests (`test_return_delay_slot_not_owned`,
  `test_indirect_call_is_not_terminal`), each checked the way any regression
  test should be: revert the fix, confirm the test fails, restore it, confirm it
  passes.
- **One input per function.** Argument registers come from a fixed LCG seeded by
  address: reproducible, but one path through each function, not a range.
- **Caller-saved registers are not compared.** `$t*` and `$a*` are excluded by
  design; the two sides may legitimately leave different values there.
- **Guest re-entry from HLE is excluded**, not solved. Making the interpreter
  service `psp_dispatch` itself would close it.

## Next

1. Triage the remaining divergence clusters — `scripts/06-triage.py` groups them
   so each pattern is one investigation rather than N.
2. Let the interpreter service `psp_dispatch`, closing the re-entry exclusion.
3. Send `patches/0001`–`0005` upstream. Both emitter bugs are silent-truncation
   defects that affect every title, not just this one.
