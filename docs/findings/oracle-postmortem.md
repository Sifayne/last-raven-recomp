# The oracle: what it found, and when to stop

Phase 1 is closed. This records the outcome and, more usefully, the reason for
stopping — the signal flattened well before the work felt finished.

## What it found

Five things that were really wrong, in the order they surfaced:

| | |
|---|---|
| **Fall-through into a label dropped** | `memset` skipped its trailing bytes and never set its return value |
| **The label it needed did not exist** | the lost continuation was the epilogue |
| **A `jr $ra` delay slot dropped when unowned** | that is where the stack restore lives |
| **`jalr` treated as terminal** | an indirect call returns; the function was truncated before its epilogue |
| **PRX relocations never applied** | 102,615 of them; every cross-segment pointer read as null |

All five fail the same way: **silently**. No crash, no dispatch miss, no log
line. Four of them make the generated C do less than the hardware; the fifth
makes every global pointer null. Nothing else in the toolchain would have
noticed, which is the entire argument for building a differential oracle.

Two carry regression tests (`test_return_delay_slot_not_owned`,
`test_indirect_call_is_not_terminal`), each verified by reverting the fix and
confirming the test fails first.

## Final numbers

All 25,555 discovered function entries:

| | |
|---|---:|
| compared | 12,479 |
| agree | **12,449** |
| differ | **2** |
| differ, dispatch miss (discovery gap, not codegen) | 28 |

The 2 are stores landing inside `.text` — at `0x001773A0` the write is to
`0x001773C0`, inside that same function. Once code is overwritten the two sides
diverge by construction: the interpreter fetches instructions from memory and
runs the modified version, the recompiled C was fixed at compile time. The
oracle cannot compare self-modifying code.

### Servicing HLE re-entry did not increase coverage

Worth recording plainly, because it is not what was expected of it:

| | before | after |
|---|---:|---:|
| compared | 12,503 | 12,479 |
| skipped: HLE re-entry | 6,254 | **0** |
| skipped: trapped | 6,582 | 12,863 |

The 6,254 discarded functions did not become comparisons. They became *traps* —
they now run far enough to hit a real unimplemented instruction instead of being
refused at the door. Coverage is flat; what changed is that the reason for
skipping is now a fact about the module rather than a limitation of the harness.

That is worth having, but it is an integrity win, not a coverage win, and the
earlier expectation that it would "move most of those into the comparable set"
was wrong.

### A remaining asymmetry, counted

**29,165 re-entries were refused** for exceeding a nesting depth of 64. Some
guest callbacks therefore did not run on the interpreter side while the
recompiled side ran them. In practice this produces no false divergences — only
2 functions differ, and both are explained — but the executions are not fully
faithful and the number is reported rather than hidden. Raising the limit from 8
to 64 removed the refusals from the first thousand functions and not from the
corpus, so the deep nesting is real, not an artifact of the constant.

## What it cost

Roughly **twelve harness defects against five real findings**, and the ratio got
worse over time, not better:

unbounded work-list · no state reset between functions · testing non-callable
entries · `--from 0x0` treated 0 as "unset" · HLE state never reset · seeded
arguments pointing into `.text` · seeded lengths that were really addresses ·
instruction budget too low · then too high · profiling overhead · a nesting
counter that leaked on every abandoned run · a nesting depth limit set
arbitrarily low

Every one of them produced results indistinguishable from codegen bugs. The
discipline that made the difference was refusing to report a divergence without
first trying to prove it was the harness — three of the four emitter bugs were
found only after an earlier "finding" had already been withdrawn.

## Why it stopped here

The last emitter bug was the fourth. Everything after it — relocations aside —
was harness repair. Comparability rose, the divergence count did not move, and
no new defect appeared.

That is the signal to stop. An oracle earns its keep by finding bugs, not by
maximising the size of its comparable set, and the marginal round had stopped
finding any.

The harness knobs are also, honestly, arbitrary. The instruction budget, the
watchdog intervals and the nesting depth were each tuned by measurement rather
than derived from anything, and each retuning changed the headline number
without changing the conclusion. A metric that moves when you adjust a constant
is not the metric to optimise.

## What it cannot say

The oracle establishes that the translation is faithful. It says **nothing**
about whether the game runs, and cannot: both sides execute the same guest code,
so anything missing from the execution environment is missing from both, and
agrees perfectly. Static constructors, a thread scheduler, the 122 unimplemented
firmware functions — all invisible here, all still required.

That is the next phase, and it starts from a better place than it would have:
the module is correctly loaded and the code is known to be correctly translated,
so a failure to boot is now attributable to what is missing rather than to what
is wrong.

## Reproducing

```bash
scripts/05-oracle.sh 30000        # full corpus
python3 scripts/06-triage.py      # cluster whatever diverges
```

Add `--profile` for a per-instruction PC histogram — it is off by default
because it costs an increment per interpreted instruction, and it has already
answered the question it was built for.
