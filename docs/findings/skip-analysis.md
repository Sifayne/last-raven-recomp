# What the 13,670 skipped functions actually are

Measured 2026-08-28. Reproduce with `scripts/05-oracle.sh` — the breakdown is
printed as "why runs stopped early".

## The claim this corrects

The Phase 1 write-up described the skipped bucket as "an unimplemented
instruction on one side, overwhelmingly VFPU". That was asserted, not measured,
and it was wrong — Phase 0's own numbers rule it out. There are **331 VFPU
instructions in the entire binary**, touching 127 functions. At most ~1% of
13,670 skips could possibly be VFPU.

## What it is instead

Over a 2,000-function sample, of 1,009 skipped:

| reason | count | share |
|---|---:|---:|
| **instruction budget exhausted** | **960** | **95%** |
| budget, spinning on NID `0x8F2DF740` | 20 | 2% |
| invalid instruction | 17 | 2% |
| VFPU (`vcmov`, unnamed) | 7 | <1% |
| control transfer in a delay slot | 4 | <1% |

So the category is not "we cannot translate this instruction". It is **"this run
did not finish"** — a different problem with a different fix.

## Why it propagates

The interpreter follows calls, so a function is only as terminating as its whole
call graph. `0x00000190` is ten instructions long and cannot loop:

```
00000190  jal   0x00001118
00000194  addiu $a0, $a0, -3744
...
000001B0  jr    $ra
000001B4  addiu $sp, $sp, 16
```

It still exhausts 200,000 instructions, because something below `0x00001118`
does not return. A small number of non-terminating leaves poisons a large number
of callers, which is why the count is in the thousands rather than the dozens.

## One identified driver

`NID 0x8F2DF740`, imported from **`ModuleMgrForUser`** and **not implemented**.
One run called it 7,864 times consecutively. The shape is the expected one for
an unimplemented firmware call: `psp_hle_call` returns 0 forever, the guest's
wait condition never becomes true, and it spins.

That accounts for the 20 runs where a single NID dominates. **It does not
account for the other 960** — those exhaust the budget without repeating any one
firmware call, and what each of them is individually waiting on has not been
established.

## Hypotheses tested and rejected

Recording these so they are not retried:

- **"The seeded length argument is huge."** Making all of `$a0`–`$a3` RAM
  pointers meant a `memset(dst, val, len)` got a length of ~0x08400000 and
  looped ~138 million times. Real, and fixed by seeding `$a0`/`$a1` as pointers
  and `$a2`/`$a3` as small counts — but it moved the count only from 970 to 960,
  so it was not the bulk.
- **"They are all spinning on unimplemented firmware."** Only 20 of 980 repeat a
  single NID enough to say so.

## What this means for sequencing

The wall is not the vector unit and not codegen. It is that the program has no
working execution environment: firmware that returns 0 instead of doing
something, and no scheduler to run the thread `module_start` creates. That
points at the boot milestone — static constructors, `$k0`/reent, thread
scheduler — rather than at a VFPU push.

The honest limit: this establishes the *mechanism* (propagation through the call
graph) and *one* driver. It does not establish what the majority are each
waiting on. Raising the budget and recording where execution actually sits when
it runs out would answer that, and is the obvious next measurement.

## Also fixed here

The interpreter reported a control transfer inside a delay slot as
"unimplemented VFPU instruction (jal)", because such opcodes fell through to the
VFPU trap. Nonsense on its face, and it sends the reader to the vector unit for
what is almost always data being decoded as code. It now has its own status.
