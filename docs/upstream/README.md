# Upstream contribution — draft, not sent

Draft text for [sp00nznet/psprecomp](https://github.com/sp00nznet/psprecomp).
**Nothing here has been posted.** Review and send it yourself, or ask for
changes first.

Five patches sit in `patches/`, applied by `scripts/build-tools.sh`. They apply
to a pristine checkout in order and the suite stays green (11/11, including two
new tests).

| patch | what | send as |
|---|---|---|
| `0001` | `libm` never linked | small PR, no discussion needed |
| `0002` | **four emitter codegen bugs** | one issue + one PR, or four issues |
| `0003` | the interpreter oracle | issue first — it fills a roadmap slot, and the author may have a design in mind |
| `0004` | `psp_hle_set_quiet`, drop a leftover debug line | small PR |
| `0005` | two regression tests | folds into the `0002` PR |

The four codegen bugs are the valuable part. All four are **silent truncation**
— the generated C does less than the hardware, with no crash, no dispatch miss
and no log line. They affect every title, not just the one they were found on.

Suggested order: send `0001` and `0004` first as trivially reviewable fixes,
then the `0002`+`0005` bundle, and open `0003` as a discussion rather than a
drop-in PR.

---

## Issue 1 — `libm` is not linked, so the build fails on Unix

`src/vfpu.c` calls `sinf`, `cosf`, `powf`, `logf` and `asinf`, but the
`psprecomp` target never links `m`. On MSVC these live in the CRT so the build
is fine; on Linux `test_vfpu` fails to link:

```
/usr/bin/ld: libpsprecomp.a(vfpu.c.o): in function `psp_vunary':
vfpu.c:(.text+0x1b49): undefined reference to `logf'
```

Fixed on the library target as `PUBLIC` so consumers — the tests, and any game
host linking the runtime — inherit it. Patch: `0001`.

---

## Issue 2 — four emitter bugs that silently drop instructions

Found with a differential oracle (see Issue 4) on a retail module, then each one
confirmed by re-measuring: on a fixed 400-function sample, disagreements went
**17 → 8 → 7 → 0** as they were fixed one at a time. Across all 26,462
discovered functions, 12,240 comparable functions now agree except 3, and those
3 have explanations (two are an IEEE-754 NaN sign bit; one is open).

### 2a. Fall-through into a *label* is dropped

`emit_function` continues execution when the address after a function's extent
is another function's **entry**, and drops it when the address is a **label
inside** another function.

`memset` is split exactly that way: the word-fill loop becomes its own function
and its not-taken exit falls into the byte-fill tail, which belongs to the
neighbour. The emitted loop just returned — so **memset skipped its trailing
1–3 bytes and never ran the `jr $ra` delay slot that sets its return value**.
Confirmed by calling the recompiled function directly: `$v0 = 0` before,
`$v0 = $a0` after.

### 2b. …and the label it needs does not exist

Fixing 2a exposes the next layer: labels are only emitted for addresses
something *branches to*, so the continuation targets an address with no label
and no dispatch entry, the jump misses, and the continuation is lost anyway.

Fixed with a pre-pass marking fall-through targets before emission. It has to be
a pre-pass — the function that needs the label is not the one that discovers it
is needed, and emission runs in address order.

Possibly relevant to the HEAD commit message *"shared-epilogue +32 is correct,
the -64 group is not explained"*: the `$sp-0x40` group is what this and 2d
produce.

### 2c. A return's delay slot is dropped when the analyzer does not own it

```c
if (in.has_delay_slot && owned_by(an, a + 4, owner))
```

A delay slot executes because the hardware executes it; whether discovery
assigned that word to this function is an artifact of the analysis. Where the
two disagreed, the instruction vanished — and a MIPS compiler puts the stack
restore in the delay slot of `jr $ra`, so what vanished was typically
`addiu $sp, $sp, N`:

```
0000355C  lw    $s0, 0($sp)
00003560  lw    $ra, 4($sp)
00003564  jr    $ra
00003568  addiu $sp, $sp, 16     <- never emitted
```

Ownership still decides whether the slot needs a labelled copy; that question is
about who can jump to it, which ownership does answer.

Regression test: `test_return_delay_slot_not_owned`.

### 2d. An indirect call is treated as the end of a function

```c
last_terminal = in.is_return || in.is_indirect || (in.is_jump && !in.is_call);
```

`is_indirect` covers `jr` and `jalr` alike, and only one of them ends anything.
`jalr` is a call: it returns, and execution continues after its delay slot.
Marking it terminal suppressed the end-of-function continuation, so wherever an
extent ended just after an indirect call — where discovery routinely splits —
whatever followed was never emitted. What followed was the epilogue.

The decoder already draws this distinction: it sets `ends_block` for `jr` and
not for `jalr`. The emitter was not using information it already had.

Regression test: `test_indirect_call_is_not_terminal`.

**Note on test coverage:** 2c and 2d have regression tests, each verified by
reverting the fix and confirming the test fails. 2a and 2b do not — every
synthetic shape tried got promoted to its own entry by discovery, giving a test
that passes before *and* after, which is worse than none. `test_emit.c` records
why. Reproducing them needs `a_discover`'s ownership rules pinned down.

---

## Issue 3 — `psp_hle_call` logs on every unimplemented call

Two problems for a batch caller. At oracle volume the logging is slower than the
work being measured. Worse, a harness that puts a wall-clock watchdog around
recompiled code has to `longjmp` out of a signal handler, and if that handler
interrupts an `fprintf`, stdio's stream lock is still held — the next `fprintf`
blocks on it forever, a hang no timeout escapes because the timeout caused it.

`psp_hle_set_quiet()` suppresses the message and keeps the history behind
`psp_hle_dump_recent()`. The patch also drops a leftover debug line that traced
one hardcoded NID (`0x237DBD4F`) on every call. Patch: `0004`.

---

## Issue 4 — the interpreter oracle (Phase 5), implemented

`docs/ORACLE.md` describes the tier-1 interpreter in the present tense, but
`ROADMAP.md` Phase 5 has it unchecked and `src/` has no interpreter. This is an
implementation of it, offered as a starting point rather than a finished design
— you may well want it structured differently.

- `tools/allegrexrecomp/interp.{c,h}` — executes Allegrex on the shared
  `psp_cpu`, memory model and `recomp_rt.h` helpers. Only sequencing differs
  from the recompiled path, so a divergence localises to the emitter.
- `allegrexrecomp interp <elf> [--from] [--trace] [--regs]`
- `psp_interp_set_imports()` routes thunk hits into `psp_hle_call`, so both
  paths cross the firmware boundary identically.
- `tests/test_interp.c` — 12 cases, hand-written encodings, concentrated on
  delay slots since that is where the two paths stop sharing code.

Known limits, stated plainly: no HLE re-entry (a handler calling
`psp_dispatch()` lands in recompiled code, which the harness detects and skips),
single-threaded only, and `a_half_to_float` was moved from `emit.c` into
`decode.c` so both consumers share one implementation.

The differential harness itself (`host/oracle_diff.c`) lives in the game repo
rather than here, since it links generated code. Happy to move it if you would
rather it were upstream.
