# Phase 1 — the differential oracle

**Status: four real emitter bugs found, fixed, and confirmed. Every divergence
of the *pre-merge* corpus had a verdict; the merged build's 9 do not yet.**

Latest full-corpus run (2026-08-29, merged build, 15,858-entry discovery):
**12,441 compared, 12,432 agree, 9 differ** — and the 9 are 4 distinct
signatures, one of which accounts for six of them. None triaged yet.

The earlier pre-merge run reported 12,240 compared / 12,237 agree / 3 differ,
with verdicts on all three: 2 an IEEE-754 NaN sign bit (not a bug) and 1
unresolved with a reproduction. Those verdicts stand for that build; they do not
carry over to the 9 above.

Reproduce with `scripts/05-oracle.sh` — an argument caps attempts, `0` runs the
whole entry list — then `scripts/06-triage.py`. Measured 2026-08-27 against
`NPUH-10024`, module `ACLR_App`.

Read [What a limited run covers](#what-a-limited-run-covers) before treating any
capped run as a statement about the module. Until 2026-08-29 a capped run was
positional, and the numbers it produced do not mean what they look like.

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

> **Counts below move whenever `psprecomp` is rebuilt.** Discovery changed three
> times in the ten minutes around 2026-08-29 15:00 — entry counts of 16,494,
> 16,665 and 15,858 in succession — as *reunite shared epilogues* landed. Every
> table here is therefore a dated measurement against a named build, not a
> standing fact. Re-measure with `scripts/05-oracle.sh 0`; it takes about six
> and a half minutes and prints its own work-list size in the header.
>
> The full run below is against **15,858 entries, which is the state that
> commit settled on** — so it describes current `HEAD`, not a superseded build.

### 2026-08-29, merged build, 15,858-entry discovery

The first full-corpus run over the function-entry list after the work-list
change. Build fingerprint: 15,858 entries, 59,174 dispatch registrations.

| | | |
|---|---:|---|
| attempted | 15,833 | 15,858 entries less 25 import thunks |
| **compared** | **12,441** | **78.6% of attempts** |
| **agree** | **12,432 (99.93%)** | |
| **differ** | **9** | 4 distinct signatures — see below |

| skipped | count | |
|---|---:|---|
| trapped | 3,374 | an unimplemented instruction on one side, mostly VFPU |
| unbalanced `$sp` | 16 | the entry was not independently callable |
| import thunk | 25 | the firmware boundary, not guest code |
| recomp hung | 2 | interpreter returned; recompiled C did not |
| interpreter hung | 0 | |
| host fault | 0 | |
| unregistered | 0 | every discovered entry was registered by the emitter |

The 9 divergences are **4 signatures**, not 9 independent findings:

| addresses | signature |
|---|---|
| `0x001457A0`, `0x00145A14`, `0x001955B4`, `0x00195654`, `0x00195748`, `0x001957FC` | one stack word, `interp=005E2C2F recomp=003323A0` — identical on all six |
| `0x0001093C` | the known one, below |
| `0x00133A18` | `module[00421158]`, `interp=80202000 recomp=00000000` |
| `0x001407B4` | `module[0032055C]` plus one stack word |

The six-address cluster is one defect reported six times. None are triaged yet,
and all are leads rather than bugs — see the base rate below.

Also new and unexplained: the stop-reason tally lists `recomp: sll` twice and
`recomp: sra` once. The *recompiled* side calling `psp_unimplemented` for
ordinary shifts is not a shape that has appeared before and is worth a look.

### Superseded: the pre-merge full run

The table this section used to carry — 26,462 attempted, 12,240 compared, 3
differ, 249 unbalanced `$sp`, 13,670 trapped — was measured against a
pre-merge discovery with a different entry list, and `reports/05-oracle-full.txt`
on disk is a *different, later* pre-merge run again (25,555 attempted, 12,479
compared, 2 differ + 28 dispatch-miss, 204 unbalanced). Neither is comparable
line-for-line with the run above. Kept only as a record that the numbers moved.

### Verdicts on the 3 divergences of the pre-merge run

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
line for line. The merged-build run in `reports/05-oracle.txt`:

| | count |
|---|---:|
| attempted | 5,000 |
| **compared** | **1,481** |
| **agree** | **1,480** |
| **differ** | **1** |
| unbalanced `$sp` | 10 |
| host fault | 0 |

**That table describes the bottom 8% of the module, not the module.** It was
produced before the work-list change below, by a `.text` walk that spent its
5,000 attempts on the first ~250 KB of a 3.03 MB `.text`; the highest address it
mentions anywhere is `0x0003D7B0`. The giveaway is `import thunk: 0` — the
stubs sit at `0x002E4490`, 2.9 MB in, and the run never got near them.

So the two conclusions previously drawn from it do not follow. "Unbalanced `$sp`
fell from 0.94% of attempts to 0.20%" (249/26,462 against 10/5,000) and "host
faults went from 3 to 0" each set a whole-corpus run against the low end of one,
over different populations — entries in the first case, mostly interior labels
in the second. The *mechanism* behind both is still sound and argued
independently: a block split out of the middle of a function has no prologue, so
calling it directly could never balance the stack, and a loop emitted as
recursion is what exhausts the host stack. These particular ratios are just not
evidence for it.

The comparison that *does* hold is between the two full entry-list runs, which
measure the same population the same way:

| | pre-merge full | merged full (2026-08-29) |
|---|---:|---:|
| attempted | 25,555 | 15,833 |
| unbalanced `$sp` | 204 (**0.80%**) | 16 (**0.10%**) |
| host fault | 0 | 0 |
| recomp hung | 9 | 2 |
| comparability | 48.8% | **78.6%** |

An eightfold drop in unbalanced `$sp`, on runs that are actually comparable.
Host faults were already 0 before the merge in that run, so the "3 to 0" claim
was never supported by it — the 3 came from an older run still.

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

## What a limited run covers

`--limit` bounds **attempts**, not comparisons. That part is deliberate —
bounding comparisons makes the loop unbounded whenever most functions are
skipped, which is the common case here. What it bounds attempts *over* was the
problem.

There are two possible work-lists, and they are not the same question:

| work-list | what it is | size (2026-08-29) |
|---|---|---:|
| `--funcs LIST` — the default, generated by `scripts/05-oracle.sh` | function entries, from `allegrexrecomp funcs <elf> --list` | ~16,000 |
| `--text-walk` — opt-in | every `.text` address the dispatch table resolves | ~59,000 |

Both counts drift with every `psprecomp` rebuild; the ratio does not. The run
prints the exact work-list size in its header, which is the number to trust.

The walk used to be the default and it is the wrong default. Roughly five of
every six addresses it attempts are interior labels rather than entries. A
label sits past the prologue that set up the frame, so entering there is not a
call, the comparison is meaningless, and the `$sp`-balance check discards it only
after paying to run it. The walk answers how much of the dispatch table is
reachable at all, which is a real question and the one it was written for. It is
not the codegen question.

**And a capped walk is positional.** At ~7.8% table density, 5,000 attempts
reach about `0x0003E000` and stop. That is not a rounding error in coverage: two
runs of `scripts/05-oracle.sh 5000` straddling an emitter change that reclaimed
171 functions produced **byte-identical output files**, because every function
the change touched sat past the window. Identical files read as confirmation,
which makes this worse than a wrong number.

The entry list is now strided rather than taken as a prefix, so a capped run
spans the whole address range and says so in its own header:

```
worklist: 16494 function entries from reports/05-funclist.txt
sample:   300 of 16494, every 55 across 0x00000000..0x0031FD3C
```

The same budget as a prefix would have covered `0x00000000..0x00009298` — 37 KB,
1.2% of `.text`. `--prefix` still asks for that, for bisecting a region or
reproducing an older report.

Measured against the 16,494-entry discovery (superseded since — the point here
is the method, and the ratio survives the rebuild), `--limit 5000` striding by 4:

| | strided entries | walk, prefix (`reports/05-oracle.txt`) |
|---|---:|---:|
| selected | 4,124 | — |
| attempted | 4,118 | 5,000 |
| **compared** | **3,142 (76%)** | **1,481 (30%)** |
| agree | 3,140 | 1,480 |
| **differ** | **2** | **1** |
| unbalanced `$sp` | 4 | 10 |
| import thunk | 6 | 0 |
| span | `0x0`–`0x003204D0` | `0x0`–~`0x3E000` |

Three things to take from that. Comparability goes from 30% to 76%, because the
walk was spending most of its budget on addresses that could not be compared
even in principle. `unbalanced $sp` falls for the same reason, though **not to zero** —
4 entries in 4,118 still fail the balance check, so a handful of addresses on
the entry list are not independently callable either, and the label/entry split
does not account for all of them. And the run costs **80 seconds**; a measured
`scripts/05-oracle.sh 0` over the whole list came in at **6m29s**, so a full
corpus pass is a coffee break rather than an overnight job.

The stride rounds up, so `--limit N` selects at most N and usually fewer: 5,000
over ~16,000 strides by 4 and selects ~4,100. Rounding down would exhaust the
budget at nine tenths of the list and reintroduce the positional bias in
miniature.

### What the old default could not reach

Every divergence in the merged full run except `0x0001093C` sits between 1.25 MB
and 1.66 MB into `.text` — `0x00133A18`, `0x001407B4`, and the six-address
`0x00145xxx`/`0x00195xxx` cluster. All of them are past the ~250 KB a capped
walk covered, so the old default could not have reported any of them at any
limit anyone actually ran.

They reproduce individually, so none is an artifact of sampling order:

```bash
build/host/oracle_diff game/extracted/ACLR_App.elf --from 0x133A18 --verbose
```

That is the concrete cost of the positional sample: not a slightly worse number,
but a whole region of the module that never got looked at.

## Method notes, and why they are not incidental

Seven *harness* defects were found and fixed along the way. Most produced
divergences indistinguishable from codegen bugs; the last one did the opposite
and hid them. The base rate matters when reading any remaining divergence:

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
7. **A capped run sampled positionally, and the default work-list was the wrong
   one.** `--limit` walked `.text` from the bottom and stopped when the budget
   ran out, so `scripts/05-oracle.sh 5000` reported on ~8% of the module while
   presenting itself as a corpus result. It is the only defect here that makes
   the harness *quieter* rather than noisier: two runs across a change that
   reclaimed 171 functions came out byte-identical, which reads as "no
   regression" and meant "did not look". Fixed by making the function-entry
   list the default work-list and striding it. See above.

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
- **Interior labels are not tested by default any more.** The entry list holds
  function entries only. Several addresses that turn up in `$sp` investigations
  are labels rather than entries — `0x00165450`, `0x00167784` and `0x0029ADF0`
  are each interior to a larger entry (`0x00165198`, `0x00167694`, `0x0029ACA8`
  respectively; the first two are that entry's last eight bytes, i.e. its
  epilogue). That is the intended trade, because entering a function past its own
  prologue was never a call — but it does mean those literal addresses stop
  appearing in the report. The containing entries are on the list and exercise
  them as part of a real call; `--text-walk` still attempts them directly, and
  `--from ADDR` reaches any single one.
- **The stride samples entries, not code.** Every entry gets equal weight
  regardless of size, so a strided run is uniform over the function list and
  slightly under-weights the large functions where more instructions live. Use
  `--limit 0` when the question is coverage rather than a spot check.

## Next

1. Triage the 4 signatures from the merged full run, starting with the
   six-address `interp=005E2C2F recomp=003323A0` cluster — six reports, almost
   certainly one defect, so it is the cheapest one to resolve per finding.
   Then `recomp: sll` / `recomp: sra` in the stop tally: the recompiled side
   hitting `psp_unimplemented` on ordinary shifts is a new shape.
2. Reconcile with the capped run in *reunite shared epilogues*, which reports
   "2 divergences before and after". That was ~4,000 attempts; the full 15,833
   here reports 9. The extra 7 are very likely older than that change rather
   than caused by it — every one sits past where a capped run reaches — but
   "unchanged" is a claim about the corpus made from a quarter of it, and the
   full run is now cheap enough that it does not have to be.
3. Run `scripts/06-triage.py` over the full report — it groups divergences by
   signature, which is how the six-address cluster was spotted by eye above.
4. Let the interpreter service `psp_dispatch`, closing the re-entry exclusion.
5. Send `patches/0001`–`0005` upstream. Both emitter bugs are silent-truncation
   defects that affect every title, not just this one.
