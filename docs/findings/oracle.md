# Phase 1 — the differential oracle

**Status: two real emitter bugs found, fixed, and confirmed by measurement.**

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

## The two bugs

Both are the same defect at different depths: **a function whose last block
falls through into another function silently returns instead of continuing.**
No dispatch miss, no bad memory access — just less work done than the hardware
would do. That is the worst failure mode available, and it is exactly what a
differential oracle is for.

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

### Effect

Same 400 functions, same seeds, one variable changed at a time:

| | compared | agree | differ |
|---|---:|---:|---:|
| before either fix | 118 | 101 | **17** |
| after fix 1 | 118 | 110 | **8** |
| after fix 2 | 118 | 111 | **7** |

The 27-function `$v0` cluster is gone entirely.

---

## Corpus results

See `reports/05-oracle-full.txt` and `scripts/06-triage.py`.

Skip categories matter as much as the comparisons:

- **Traps** — an unimplemented instruction on one side, overwhelmingly VFPU.
- **Unbalanced `$sp`** — discovery splits on any pointed-at address, so an
  "entry" can land past the prologue that built the frame. A well-formed
  function restores `$sp`; anything else was never independently callable, and
  the comparison is meaningless rather than failing.
- **Interpreter hangs** — an HLE handler re-entered guest code through
  `psp_dispatch()`, landing in a recompiled function that does not return.
- **Host faults** — recompiled guest code runs on the host stack and every guest
  call is a real call, so a deep chain overflows it. Caught on an alternate
  signal stack; before that it killed both full runs at the same function.

---

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

Two hypotheses were tested and **disproved**, which is worth recording so they
are not retried: restoring all 32 MB of RAM instead of a 1 MB stack window
changed nothing; and the stdio-deadlock theory for the early hangs was wrong —
the real cause was HLE re-entry, and the fix was extending the watchdog to cover
the interpreter side.

## Known gaps

- **No regression test for either bug.** Reproducing them needs discovery to
  leave a fall-through target as a label inside a neighbouring function, and
  every synthetic shape tried got promoted to its own entry — a test that passes
  before *and* after the fix, which is worse than none. Pinning down
  `a_discover`'s ownership rules is the prerequisite. Noted in `test_emit.c`.
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
