# A behavioural oracle: pspautotests through the interpreter

Written as a working harness with known seams — what it does and does not do
is listed below, and the list is load-bearing.

## Why at all

The differential oracle validates *translation*: the same instructions go
through the Allegrex interpreter and the recompiled C, and a disagreement
localises to the emitter. Everything left on the project is *environment* —
the HLE, the scheduler, save data. Anything missing from the execution
environment is missing from **both sides**, so the oracle agrees perfectly and
is structurally blind to exactly the work that remains. The intro-movie
blocker is an environment gap: some firmware answer on hardware starts a
producer that never starts here, and no amount of interpreter-vs-recompiled
comparison can see it.

`pspautotests` (pspdev/pspautotests) are small PSP programs with assertions
against documented behaviour, printed on real hardware as PASS/FAIL per check.
Run through *our* side, a FAIL names the firmware call whose answer is wrong —
ground truth where the current oracle cannot see.

## What exists now

`scripts/07-autotests.sh <dir-of-elfs>` runs each ELF through
`allegrexrecomp interp --dispatch`, which loads any ELF/PRX, applies
relocations, binds import thunks to HLE, and runs from the module entry under
an instruction budget. HLE re-entry is **served**: a firmware handler that
starts a thread or dispatches into guest code runs that code interpreted,
nested, charged against the run's budget (`psp_interp_service_dispatch`; the
dispatch hook and the spawn hook in interp.c). Reports land in
`reports/07-<name>.txt`.

**No toolchain is needed, and the repository name here was wrong.** The tests
live in `hrydgard/pspautotests`, not `pspdev/pspautotests`, and the `.prx`
binaries are committed there alongside `.expected` files holding real-hardware
output. Clone it into `game/` (gitignored) and point the script at a test
directory:

```bash
git clone --depth 1 https://github.com/hrydgard/pspautotests game/pspautotests
scripts/07-autotests.sh game/pspautotests/tests/cpu/vfpu
```

Nothing is fetched by the script and nothing from it is committed — the same
policy as game data, for the same reason.

## Where the seams are

Stated plainly, because a harness that silently runs less than it looks like
it runs is the whole class of bug this project keeps writing down:

1. **Threads run to completion at their start point.** That is sequential
   semantics, not scheduling: no interleaving, no preemption, and a thread
   that blocks part-way has nothing to be resumed *into* — the nested run
   ends one way or another and the starter carries on. Most pspautotests
   mains run their checks and return, which is exactly the shape this
   serves. Tests that depend on real concurrency are out of reach until the
   interpreter runs on the boot host's scheduler.
2. **The tests emit now.** Seven of the eight `cpu/vfpu` tests produce output
   and are compared against real hardware. Three run to their own
   `sceKernelExitGame`. This took four fixes, and the shape of the first is
   worth keeping, because the earlier diagnosis in this file was wrong in a
   way that reads as right:

   - **`$gp` was never loaded.** A module with a small-data area addresses it
     as an offset from `$gp`, and nothing in the instruction stream says what
     `$gp` is — the value lives only in the module info header, which the
     loader did not read. Every such access went to around address 0. The
     `read32 at 0xFFFF800C` this file previously listed as a mysterious bad
     access is exactly `lw -0x7FF4($gp)` with `$gp` zero, and the "code word
     used as an address" at `0xAFB40070` was the same cause downstream, not a
     separate bug. Armored Core is built `-G0` and never names `$gp`, so this
     was invisible for the whole project; the differential oracle could not
     have caught it either, because `oracle_diff.c` excludes `$gp` from
     comparison. The 15 relocations the loader reported as skipped were
     `R_MIPS_GPREL16`, which correctly need no patching — but reading
     "skipped" as "mis-loaded" is what led to the register that was never set.
   - **Opcode `0x1C` was decoded as an unknown VFPU op.** It is SPECIAL2, and
     it is where Allegrex puts `mfic`/`mtic`. The interpreter and emitter
     already implemented both; only the decoder disagreed. A test calling
     `mfic` from newlib's lock path reported `unimplemented VFPU instruction`,
     which sends the search to the vector unit for an interrupt-controller op.
   - **`IoFileMgrForUser 0x54F5FB11` is `sceIoDevctl`.** The two calls are the
     harness probing for an emulator with `devctl("kemulator:", …)`. Answering
     "no such device" is the answer, not a stub: it is what a real PSP does,
     and it is how the test learns to take the hardware path.
   - **Nothing read `psp_exit_requested()`.** A test that called
     `sceKernelExitGame` ran on until the instruction budget stopped it, which
     reports as a hang rather than as a program that finished.

   Where the output goes matters: the harness redirects its own stdout to
   `host0:/__testoutput.txt` — usbhostfs, how a test talks to the PC it is
   tethered to — and `iofilemgr` resolves a device to `<cwd>/<device>/<path>`.
   The file *is* the output; reading the console instead is what made every
   test look silent. The script runs each test from `reports/hostfs/`, because
   the default working directory would put `host0:` on top of `host/` — the
   boot host's own source directory.
3. **Output comparison is against the file, not the console.** The script
   diffs `reports/hostfs/host/__testoutput.txt` against `.expected` and reports
   `MATCHES hardware`, `differs: N line(s)`, or `NO OUTPUT`. It removes the
   file before each run, so a test that emits nothing cannot inherit its
   predecessor's verdict.
4. **`matrix` matches hardware exactly**, and `gum` is four lines away.
   All 50 of `matrix`'s lines are byte-identical to real PSP output: every
   `vtfm`, `vhtfm`, `vmmul`, `vmidt`, `vmscl` and transpose case.

   Where `cpu/vfpu` finished. Every remaining difference is closed — two are
   the transcendental tables ruled out on licence grounds (18), one is
   upstream's own committed data (9) — so there is no open VFPU gap that is
   ours:

   | test | stopped | ours / expected |
   |---|---|---|
   | `matrix` | ExitGame | **MATCHES hardware** |
   | `gum` | ExitGame | **MATCHES hardware** |
   | `colors` | ExitGame | **MATCHES hardware** |
   | `vavg` | ExitGame | **MATCHES hardware** |
   | `convert` | ExitGame | **MATCHES hardware** |
   | `vector` | ExitGame | 5329 / 5329 lines, **8** differ — `vasin` ULP, closed |
   | `prefixes` | ExitGame | 26 / 26 lines, **1** differs — `vsin` ULP, closed |
   | `vregs` | ExitGame | 30 / 48 lines — stale test data, see 9 |

   Six bugs so far, none of which the differential oracle could see, because it
   runs the same decoder and the same `psp_vfpu_*` helpers on both sides:

   - **`lv`/`sv` decoded the wrong register field.** Arithmetic carries `vt` as
     a contiguous 7 bits at 22..16; load/store cannot, because 22..21 are the
     top of the *base register*, so it puts the low five at 20..16 and the top
     two at 1..0. Reading the contiguous field mixes the base register into the
     register number: `lv.q R000, 0($a1)` becomes 0x60 instead of 0x20 -- same
     matrix and column, row 2 instead of row 0, four lanes rotated by two.
   - **A matrix register names a sub-matrix.** `M022` is the 2x2 at column 2,
     row 2; both base offsets were being forced to zero.
   - **`vmmul` and `vtfm` indexed their matrix operand transposed.** `vmmul`
     reads its first operand transposed *twice* -- the assembler sets the bit,
     the hardware indexes by [output row][summation] -- and the two cancel.
   - **`vhtfm` was not implemented.** It has no opcode of its own: it is `vtfm`
     with the vector one element narrower than the instruction's order.
   - **`mtv`/`mfv` were decoded but implemented nowhere.** The integer/vector
     moves. Also split from `mtvc`/`mfvc`, which are a different register file
     and were being folded in with them.
   - **`vrot` bypassed the register mapping**, indexing `psp_cpu.v[]` directly
     and stepping lanes by one. Lanes are 32 apart and the row offset in bit 6
     was dropped, so pspgl's `glRotatef` wrote into registers nobody read.

   **This was also the game's logo bug.** With the load/store field fixed the
   `FROM SOFTWARE` logo moved 68 pixels right and is centred (x 69..417,
   midpoint 243 on a 480-wide screen). The earlier note about a 68 in `m[9]`
   instead of `m[12]` was lanes rotated by two seen from the far end of the
   chain, which is why chasing the write that produced it never reached a cause.

5. **The user heap was a megabyte too small, and the guest did not check.**
   `sceKernelAllocPartitionMemory` refused `gum`'s single 0x01500000 request
   against a 0x01400000 heap. The floor was a fixed 0x08C00000, a guess meaning
   "above the module" -- but a PRX linked at address 0, which every one of these
   is, is not in user RAM at all, so there was nothing to step around. The
   allocator is now told the module's real extent.

   Worth keeping for the shape: the guest formatted into the null it got back,
   over its own code at address zero, so an out-of-memory presented as a wild
   pointer and a run that eventually executed ASCII. `gum` went from 2 lines and
   58 bad accesses to 45 lines and none.

6. **The operand prefixes are implemented.** They were the largest remaining
   gap and the one blocking the most tests. `prefixes.prx` went from 38
   differing lines to **1**.

   A prefix instruction rewrites the operands of the *next* VFPU instruction --
   swizzling lanes, taking absolute values, substituting one of eight
   constants, negating, saturating the result, masking lanes out of the write
   -- and nothing in that instruction's own encoding says so. They were left
   out deliberately for a long time, with a pending prefix making the next op
   report and skip rather than compute a number that ignored it. That policy
   was right, and it was also what left pspgl's `glRotatef` returning an
   identity: it builds `(cos, sin)` and `(-sin, cos)` as two `vmov.p` whose
   only prefix content is a lane negation.

   Three details were worth getting from a hardware-validated source rather
   than reasoning about:

   - **Identity, not "unset".** The hardware restores 0xE4 (swizzle x,y,z,w)
     for the source prefixes and 0 for the destination after *every* VFPU
     instruction, so "no prefix" and "the identity prefix" are one state. Every
     op consumes all three, including matrix ops that ignore them -- one left
     set would apply to whatever came next.
   - **Absolute value and negation are bit operations**, not `fabsf` and unary
     minus. The sign of a zero and of a NaN is observable, and the test prints
     both.
   - **The [0,1] clamp substitutes its bound**, so -0.0 saturates to +0.0,
     while the [-1,1] clamp on the next line of the same test leaves -0.0
     alone. That pair is what shows it is about the bound rather than zero.

   The one line still differing is `-NaN + -1/3`: hardware canonicalises the
   result to a positive NaN where x86 propagates the operand's sign. That is
   FPU NaN propagation, not prefixes.

7. **`viim`/`vfim` wrote the wrong register.** The immediate loads take their
   destination from the vt field; the low seven bits, where every other VFPU op
   keeps vd, are part of the immediate. `vfim v84, 1/90` encodes as 0xDFD421B0
   and we were naming v48.

   pspgl converts degrees to quarter-turns with exactly that instruction, so
   `glRotatef(180, ...)` multiplied its angle by an untouched register and got
   zero. `vrot` then produced cos=1, sin=0 -- a clean identity, no error
   anywhere, and indistinguishable from the prefix gap above until that one was
   closed.

8. **`gum` matches hardware too.** Two more fields read from the wrong bits,
   both in the decoder and both silent.

   - **A quad load/store takes one bit for the register, not two.** The single
     forms (`lv.s`/`sv.s`) put the top two bits of `vt` at 1..0; the quad forms
     put *one* there, because a quad always starts at row 0 and has no use for
     the row bit -- bit 1 is a cache write-through hint. Reading it as part of
     the register makes `sv.q C700` store from row 2, so pspgl's `glGetFloatv`
     returned a matrix with every value correct, transposed and rotated by two.
   - **`vrot`'s control field was overwritten after decoding.** It is a 5-bit
     field at bits 20..16, not an immediate, and the decoder sets it correctly
     inside the opcode switch -- then a `default:` arm after the switch assigns
     `SIMM16(word)` to the same place. For `glRotatef` (0xF3A434B4) that turned
     0x04 into 0x14, and bit 4 is "negate the sine". The rotation came out with
     the right magnitude and the wrong handedness.

   The second was found with a new instrument rather than by reasoning.
   `PSPRECOMP_VDUMP=<hex pc>` prints the VFPU register file when execution
   reaches an address; `--regs` covers the GPRs and nothing else, and three
   rounds of deducing a matrix backwards from what a run eventually printed had
   produced two confident wrong answers. Reading the file at the instruction
   showed `vrot` writing a correctly-sized, wrongly-signed sine in one step.

   A replay harness disagreeing with the real run is what pointed at the
   decoder at all: calling the same ops in the same order from C produced the
   hardware answer, so the ops were right and the *operands* were not.

9. **`vregs` is not ours to fix: the binary and the expected output are from
   different builds.** Both differences were chased to the committed test data,
   and our execution of the `.prx` is exactly right.

   - The eight lines containing `inf` -- four of `Upgrade`, four of `Combine`
     -- come from one fill value that is `1e+07` in `vregs.prx` and `100.0f` in
     `vregs.cpp`. The test converts its fill values to half-precision at run
     time, and `float_to_half_fast3` quite correctly clamps 1e7 to
     half-infinity, which comes back as `inf` and poisons everything computed
     from it. We execute that conversion right, instruction for instruction.

     It is *index 8 of two* 16-float arrays, at file offsets 0x19F20 and
     0x19F60 -- the second array being the variant seeded with infinities and
     NaNs. Set both to 100.0f in a copy of the binary and re-run: all 30 lines
     we emit match hardware byte for byte, zero differing. That is the whole of
     our side of this test.
   - The 18 missing lines are three sections that the binary does not contain.
     `main` at 0x17D0 makes exactly five calls -- FillAllVectorRegs, TestDouble,
     TestDoubleSwizzle, TestUpgrade, TestCombine -- and returns. `TestVscl`,
     `TestReuse` and `TestSwizzle2` are in the source and in the `.expected`,
     and not in the `.prx`. Nothing we emit builds a `vscl` word into the code
     buffer because nothing in the binary asks for one.

   So `differs: 34 line(s)` for vregs is a statement about upstream's committed
   data, not about this project. Left in the table as measured rather than
   suppressed -- a hand-maintained exclusion list would hide a real regression
   the first time one landed -- but it should not be read as work outstanding.

   Both halves were re-measured after the condition-code work, on the chance
   that something since had changed what was left. Neither moved: `main` still
   makes exactly five `jal`s, and the patched binary still matches on all 30
   lines. Re-running a closed finding is cheap; assuming it is still true is
   how a stale conclusion survives.

   Worth keeping as method: the same two moves settled this and the gum bugs.
   Replay the sequence in isolation and see whether it agrees with hardware,
   which separates "this function is wrong" from "this function is being told
   the wrong thing"; and where the disagreement is in the data, patch the datum
   in a copy of the binary and re-run, which turns a hypothesis into a
   measurement.

10. **`colors` and `vavg` match hardware.** Four of the eight now do. Two
    groups of instructions and one piece of machine state:

    - **Unaligned quad load/store** (`lvl.q`/`lvr.q`, `svl.q`/`svr.q`). A quad
      is 16-byte aligned, so a vector straddling two blocks takes a pair of
      instructions: one fills the lanes at and below the address, the other the
      lanes at and above, and each leaves the rest alone. Both tests hit these
      long before their own subject matter -- they are in the C library's
      block copy, 16 and 32 uses respectively -- which is why neither got as
      far as printing anything interesting.
    - **`vfad`, `vavg` and the colour packs** (VFPU4 rs=2). `vfad` sums the
      lanes and `vavg` averages them; both are a dot product against a constant
      vector on hardware, which is worth writing that way because it keeps one
      property a loop loses: `vavg` of a *single* lane is zero, since the
      constant for size 1 is 0 rather than 1. `vt4444`/`vt5551`/`vt5650` pack
      four 8888 pixels into four 16-bit ones and are the only VFPU ops that
      read the register file as integers.
    - **A fresh thread's float and vector registers are NaN, not zero.** The
      PSP fills all 32 COP1 and all 128 VFPU registers with 0x7F800001 for a
      new thread context. That is directly observable: `vavg` writes one lane
      with `vavg.p S000` and stores four with `sv.q C000`, so the other three
      print whatever the register file came up with -- `nan` on hardware,
      `0.000000` from a zeroed file. Every one of vavg's 24 lines turned on it.

      The general-purpose half of the same reset -- 0xDEADBEEF in every GPR --
      is deliberately not copied. Nothing measured needs it, and seeding every
      register with something that looks like a plausible pointer would turn
      "the guest used an uninitialised register" from a null-page fault into a
      wild write.

    The decode test's example of a "still genuinely unmapped" encoding had to
    move again, for the second time: it was opcode 0x35, which is now the
    unaligned quad load. The comment above it already recorded the same thing
    happening to 0x34. Choosing that example by what the dispatch actually
    leaves unmapped, rather than by what happened to be missing on the day,
    would have avoided both.

11. **`convert` matches hardware; `vector` is a long tail.** Five of the eight
    now match. What convert needed:

    - **The packed-integer conversions** (VFPU4 rs=1): `vuc2i`/`vc2i`/`vus2i`/
      `vs2i` unpack narrow integers to 32-bit, `vi2uc`/`vi2c`/`vi2us`/`vi2s`
      pack back down, and `vf2h`/`vh2f` do the same for half floats. Each
      changes the *width* of the vector as a function of the input width and
      the variant, not of the instruction's own size field.
    - **`vf2i` and `vi2f` carry a scale exponent**, and the versions we had
      ignored it. They were entries in the unary table -- `f2iz` and `i2f` --
      which multiply by nothing, so they were right for the scale of 0 a plain
      cast emits and silently wrong for every other. They also lacked the four
      rounding modes, the INT_MIN/INT_MAX saturation, and NaN going to INT_MAX.
    - **Round-half-to-even.** `vf2in` uses C's `round` nowhere: hardware turns
      0.5 into 0 and 2.5 into 2, and `round` gives 1 and 3.
    - **Two half-float edge cases**, both pinned by hardware output. Expanding
      a half whose exponent is all ones does *not* shift the mantissa up --
      `vh2f` of 0x7F80 gives 0x7F800380, not 0x7FF00000. And narrowing a NaN
      saturates the mantissa to 0x7FFF rather than producing the 0x7E00 the
      software algorithm gives.

    The half conversions now live in recomp_rt.h with one copy, because the
    decoder needs them for `vfim`'s immediate and the runtime needs them for
    `vh2f`, and two copies of a float conversion is exactly what the oracle
    cannot see -- it runs the same helper on both sides.

12. **`vdiv` is VFPU0 sub-opcode 7, and we had it at 4.** Found by `vector`,
    which stops on a sub-opcode 7 we decoded as unknown.

    This one had been "corrected" the wrong way once already: an earlier commit
    moved the decoder *and* its test from 7 to 4 together, reasoning that the
    test had been agreeing with a wrong decoder rather than catching it. The
    reasoning was right in shape and wrong in direction, and changing both in
    one commit is what let it stand -- a test edited alongside the code it
    checks has stopped being evidence.

    The evidence this time is outside both: Armored Core's `.text` contains
    VFPU0 sub-opcodes 0, 1 and 7 and no 4 at all; `vector.prx` contains 0, 1, 2
    and 7 and no 4; and the published table has 3..6 invalid. So the game has
    had seven `vdiv` instructions decoding as unknown-VFPU for its whole life.

    Fixing it changes the picture: the logo's letter edges go from speckled to
    clean and 940 more pixels are drawn, with the same 633 GE lists and 0 bad
    accesses. The oracle does not move -- 312 compared, 0 differ -- because
    both sides read the same decoder, which is why a *test* had to find it.

13. **What `vector` still needs.** It reaches 180M instructions and stops on an
    unimplemented op rather than on the budget, having emitted 1313 of 5329
    lines. Implemented on the way: `vsbn`, the VFPU9 family (`vsrt1`-`vsrt4`,
    `vbfy1`/`vbfy2`, `vocp`, `vsgn`) and `vrexp2`, which was decoded and had a
    runtime entry but had never been listed in either dispatch table. What is
    left is a genuine tail across several families, not one blocker.

    The suite's budget went from 100M to 800M for this. It is a cap and not a
    cost -- a test that finishes stops on its own -- and at 100M vector was
    reported as "instruction budget exhausted" when the real answer was an
    instruction we do not implement. A misleading reason is worse than a slow
    run, because it sends you to look at the wrong thing.

14. **All eight tests now run to completion, and `vector` is 98.5% of the way.**
    It emits all 5329 of its lines with 81 differing, from 1313 emitted and a
    trap. Everything it needed, in the order it asked:

    - `vrexp2` -- decoded, with a runtime entry, and never listed in either
      dispatch table.
    - `vsbn`, and the VFPU9 family (`vsrt1`-`vsrt4`, `vbfy1`/`vbfy2`, `vocp`,
      `vsgn`), which combine a vector with a swizzled copy of itself.
    - `vsge`, `vslt`, `vscmp` -- comparisons that write 1.0/0.0 into a register
      rather than into the condition codes. Decoded since forever, implemented
      nowhere.
    - `vhdp`, `vcrs`, `vdet`, and `vcrsp.t`/`vqmul.q` -- one encoding whose
      meaning is chosen by operand width.
    - `vwbn`, which occupies eight consecutive rs values because the exponent
      it applies lives in rt.
    - `vcmov`, which the differential oracle had been reporting against the
      *game* as an unimplemented op for as long as there has been a report.
    - `mfvc`/`mtvc`, and with them a control-register file. The prefixes and
      the condition codes stay where the rest of vfpu.c keeps them rather than
      being copied into an array, so a prefix written through `mtvc` is the
      same prefix the next instruction consumes -- which is exactly what the
      test does.
    - **Eight of `vcmp`'s sixteen conditions were missing.** The upper half test
      the first operand's *class* -- zero, NaN, infinity, and their negations --
      rather than comparing two values, and they had all been falling into a
      `default` that answered 0. That alone was 576 of vector's lines.
    - `vcst` took its constant index from the wrong field: `vs` where the
      hardware uses `rt`. It read past the end of the table and returned 0 for
      every constant in it.
    - `vcrsp` flushes infinities to zero before computing its third lane, and
      only that lane. It looks arbitrary; `inf * 0` in the dot would be a NaN
      and the hardware answers with the finite part.

    **`vector.expected` is CRLF where every other `.expected` in the directory
    is LF.** Left alone that decided the whole test: all 5329 lines "differed",
    every one of them by a byte the guest never emitted. The comparison strips
    CR from both sides now.

15. **What is left in `vector`, and why it stops here.** 81 lines in three
    groups:

    - **26 lines of `vsqrt`, `vrsq` and `vasin`** -- the hardware's
      transcendental approximations, which are table-driven and not reproducible
      from libm. The same class as the VFPU's sine, which is the one line still
      differing in `prefixes`.
    - **20 lines across `vdot`, `vhdp`, `vdet`, `vcrsp`, `vfad`, `vavg`** -- all
      of them the dot-product unit's own arithmetic. The hardware accumulates in
      extra precision with a shared exponent and round-to-odd, and has explicit
      rules for infinities and NaNs; a plain sum of products differs on exactly
      those inputs. This is a self-contained piece of work worth doing on its
      own terms rather than as a footnote.
    - **25 lines of `checkCompare`**, which look like one more condition-code
      case rather than a family.

    None of it is a blocker for anything: the ops all execute, and the
    differences are in the last bits of results involving infinities, NaNs and
    transcendentals.

16. **The dot-product unit, reproduced rather than approximated.** Every
    reduction in the VFPU is one circuit, and a sum of products is not it. It
    computes all four products to 24+2 bits with round-to-odd, aligns them to
    the largest exponent by truncation, sums those *exactly* as integers, and
    rounds once at the end -- against a plain `a0*b0 + a1*b1 + ...` which
    rounds four times, in a different order. Infinities are resolved before any
    of that: `inf * 0` and `inf - inf` are NaN, and anything else containing an
    infinity is that infinity whatever the finite terms would have contributed.

    `vdot`, `vhdp`, `vdet`, `vcrsp`/`vqmul`, `vfad` and `vavg` all go through
    it now, which is how the hardware builds them -- each is the same unit with
    a different second operand, synthesised from a forced prefix. All twenty of
    vector's dot-family differences went with it, and the five tests that
    already matched still do.

17. **The square roots were edge cases, not precision.** 28 of the 36
    "transcendental" lines turned out to need no table at all. `vsqrt` and
    `vrsq` classify their argument before computing anything, and the classes
    are not what a C library does:

    | input | `vsqrt` | `vrsq` |
    |---|---|---|
    | zero or denormal, either sign | +0 | ±inf, sign preserved |
    | negative | NaN | *negative* NaN |
    | +inf | +inf | +0 |
    | NaN | NaN | NaN |

    `psp_fsqrt` is a general helper -- it answers 0 for a negative and NaN for
    an infinity, which is right for a rasteriser and wrong for this unit -- so
    the rules went into the VFPU path rather than into it. Ordinary values
    already agreed to the last bit; only the edges did not.

18. **What is left in `vector`: 33 lines, and neither group is an instruction.**
    (25 of them closed by 19; the 8 below are all that remain.)

    - **8 lines of `vasin`, at two inputs out of 256.** For x = ±0.828125 the
      exact answer is 0.621184528, which correctly rounds to 0.621185 -- what
      we print. Hardware prints 0.621184, so its result is the float *below*
      the correctly-rounded one: the approximation is a unit in the last place
      out, and we are right by accident of being exact. Every formulation of
      the real maths gives our answer; reproducing hardware's means carrying
      its tables, which for asin alone are about 1.3 million entries. They are
      an exhaustive characterisation of the silicon, not something derivable.

      **Decided, not open: the tables are not coming in.** They are PPSSPP's
      generated output and PPSSPP is GPL-2.0-or-later, so carrying them puts a
      licence question over this tree -- for nine lines total, on inputs where
      our answer is the *more* correct one. The same goes for the sine behind
      the last line of `prefixes`. If it ever needs revisiting, the way in is
      to characterise the hardware independently, not to copy the result of
      someone else having done it.

      What that costs is worth stating plainly: `vsin`, `vcos`, `vasin`,
      `vrcp`, `vexp2` and `vlog2` will stay a unit or so off the hardware on
      some inputs, and any test that prints their bits will show it. Nothing
      that consumes them as geometry will notice.
    - **25 lines of `checkCompare`, which are one bit** -- resolved; see 19.

`nest refused` in a report is the bounded-execution signal: a callback or
thread start beyond the nesting limit did not run, so the result is not a
faithful execution of that test.

## Not this tool's job

- Building the PSP toolchain or the tests. `pspautotests` upstream documents
  that; it is out of scope here.
- Recompiling the tests through the emit pipeline. That is the eventual shape
  (the differential oracle ran the same module both ways), but running them
  interpreted comes first: if a test cannot execute, recompiling it proves
  nothing about the environment.

19. **The VFPU condition codes come up set, not clear.** The last group in
    `vector` was 25 lines that all had the same shape: `checkCompare` runs one
    `vcmp.t`, then reads condition bits 0..3 with four `vcmovt.s`. A triple
    compare writes bits 0, 1, 2 and the any/all pair -- it does not touch bit
    3 -- and hardware reads bit 3 back as 1 having never written it.

    Two readings fitted that equally and the note above said so: either the
    initial state is non-zero, or `vcmp` writes all four lane bits regardless
    of size. They are distinguishable, and the tie was broken by the reset
    value rather than by picking one. The condition codes power on at `0x3F`
    -- all six bits set -- so bit 3 is simply never written by this test and
    reads its initial 1. The other reading would have required `vcmp.t` to
    write a lane that does not exist.

    The prediction was falsifiable and was checked before the doc was written:
    exactly the 25 `checkCompare` lines, and nothing else in the suite. That
    is what happened -- `vector` went 33 -> 8 and the other seven tests did
    not move. `checkCompare2`, which clears the codes with `mtvc` before
    comparing, was unaffected, which it must be.

    Set in `psp_cpu_reset_fp` because that is the hook every host path already
    calls at thread start; `psp_vfpu_reset` reaches only the unit tests. The
    control registers next door got their documented power-on values at the
    same time -- revision `0x7772CEAB`, and the eight `RCX` words the
    random-number generator seeds from. Nothing reads those yet. They are set
    because the register beside them turned out to be observable and there is
    no reason to assume these are not.

    Worth noting what this was not: not an instruction, not arithmetic. Every
    VFPU instruction involved was already correct. The bug was one register's
    value at the moment before any of them ran, and no amount of looking at
    `vcmp` or `vcmov` would have found it.

20. **Past `cpu/vfpu`: the whole suite, surveyed.** `cpu/vfpu` is eight tests.
    The tree has **432 with recorded hardware output**, and nothing had ever
    looked at the other 424.

    `scripts/08-autotest-sweep.sh` runs all of them: same comparison as
    07, but with a per-test wall timeout and a reduced budget, writing a TSV
    instead of a report. That split is deliberate — 07 is for working a suite
    at the real budget, and a survey where an unknown fraction of the tests
    exercise unimplemented subsystems needs a timeout more than it needs
    precision. **Read the sweep as a map, not a verdict**: the reduced budget
    truncates tests that legitimately run long, and `cpu/vfpu/vector` reports
    3281 differing lines there against 8 at the full budget. Anything
    interesting gets re-run through 07 before a line of code is written.

    The useful column is the third one. A test differing by one line is one bug
    away; a test differing by five hundred is an unimplemented library. Ranking
    by it is what turned 400+ failures into a short list worth reading.

    What the survey says, and it is better news than the headline: **384 of 432
    run to completion** and call `sceKernelExitGame`, with 44 stopped only by
    the sweep's reduced budget. Exactly **two** hit an invalid instruction and
    none hit an unimplemented VFPU op, so decode coverage is essentially done.
    Almost everything that differs, differs because a firmware library is
    missing, not because the CPU is wrong.

    Where it stands after the fixes in 21 and 22 — **32 of 432**, up from 14:

    | | matching | of |
    |---|---|---|
    | `gpu` | **22** | 88 |
    | `cpu` | 8 | 16 |
    | `misc`, `string` | 2 | 8 |
    | `threads` | 0 | 127 |
    | `audio` | 0 | 58 |
    | `video` | 0 | 30 |
    | everything else | 0 | 105 |

    `threads` is the largest untouched block and the one the seam list at the
    top of this file already predicts will not do well: threads run to
    completion at their start point, which is sequential semantics rather than
    scheduling. That is a real limit of the harness, not 127 separate bugs, and
    it will not move until the interpreter runs on the boot host's scheduler.

    **The sweep's first version scored its own fixes as failures.** It reported
    `NOOUTPUT` for an empty result *before* comparing — but several `gpu` tests
    check a framebuffer rather than text and their `.expected` is empty, so
    silence is the correct answer. Nineteen tests went from wrong to right and
    the tally did not move, because the classifier called the correct answer a
    failure. `07-autotests.sh` had the same ordering and the same latent bug;
    every `.expected` in `cpu/vfpu` is non-empty, which is the only reason it
    never showed. Compare first, then interpret emptiness.

21. **The COP1 conversion family, and the FPU control registers.** `cpu/fpu`
    was the first thing the survey turned up, and it stopped dead on an
    unimplemented instruction.

    Three of the five float-to-integer ops were decoded and implemented
    nowhere — `round.w.s`, `ceil.w.s`, `floor.w.s`. They differ from
    `trunc.w.s` only in rounding mode, so all five now share one helper that
    takes the mode explicitly.

    `cvt.w.s` was aliased to `trunc.w.s`, which is right only when FCR31's RM
    field happens to say round-toward-zero. **The PSP comes up in
    round-to-nearest**, so the alias was wrong by default rather than in a
    corner.

    The saturation was wrong too, and silently: a C cast of an out-of-range
    float to `int` is undefined, and on x86 yields `0x80000000` for
    *everything* out of range, including large positives where MIPS answers
    `0x7FFFFFFF`. The range check is done in float against 2^31 so that it does
    not depend on the conversion it is guarding.

    `sqrt.s(+inf)` answered NaN. `psp_fsqrt` is a general helper that returns
    NaN for an infinity — correct for a rasteriser — so the rule went into the
    COP1 path rather than into the helper, the same split the VFPU square roots
    got in 17.

    Separately: **the FPU control registers were one variable.** All 32
    addresses aliased to `fcr31`, so `cfc1 $t, $0` returned whatever had last
    been written to the status register. There are two real ones — `fcr0` is a
    read-only revision word reading `0x00003351`, and `fcr31` is the
    control/status; everything else, including 25..28 which MIPS32 defines as
    FCCR/FEXR/FENR, reads zero and ignores writes.

    FCR31 is not fully writable either, and `cpu/fpu/fcr` measures exactly
    which bits are: RM, flags, enables, cause, FCC and FS are kept; FO, FN,
    FCC1-7 and `0x001C0000` read back as zero. Mask `0x0181FFFF`, nothing
    preserved outside it, reset `0x00000E00`.

    ```
    cpu/fpu/fpu   342 -> 6 differing lines   (and now runs to completion)
    cpu/fpu/fcr    36 -> 12
    ```

    **Both now match hardware exactly** — see 24, which closed the rest.

22. **One missing firmware call was worth nineteen tests.**
    `sceDisplayGetFrameBuf` was not implemented, and an unregistered firmware
    call returns without touching its out-parameters, so the caller reads back
    whatever its stack held. pspautotests' screenshot helper asks for the pixel
    format, gets stack garbage, and prints `ERROR: Invalid format 2928` once
    per scanline where hardware prints nothing.

    This is the failure state.md names as the shape of most bugs here — a call
    that looks like it succeeded while writing nothing — and it is worth
    recording how cheap the fix was against how it presented: nineteen `gpu`
    tests, each differing from hardware by one enormous line, all of it one
    absent function.

23. **The near misses that are not bugs.** Three `cpu` tests differ by one or
    two lines and none of the three is worth fixing as stated:

    - `cpu/cpu_alu/cpu_branch` prints `jalr: non-ra: 00000420` where hardware
      prints `08804420`. That is the **module load address**, not `jalr`: we
      load test modules at 0 and hardware at `0x08800000`. Every PC-relative
      behaviour in the test agrees; this is the one line that prints an
      absolute address.
    - `cpu/icache/icache` differs only in that hardware's `.expected` has no
      trailing newline. A capture artifact, like the CRLF in `vector`.
    - `cpu/lsu/lsu` emits one extra blank line at the very end. Per-test, not
      systemic — `cpu/vfpu/matrix` ends byte-exact — so it is one stray
      separator, not our stdout adding a newline.

    Recorded so the next reader does not spend the afternoon on them. The
    ranking that surfaced them is still the right ranking; it just needs the
    top of the list read with judgement.

24. **FCR31 made to *do* something, and `cpu/fpu` finished.** The register was
    storing state that changed nothing. Three things read it now, and both
    tests go to zero differing lines: `fpu` 342 -> **0**, `fcr` 36 -> **0**.

    **The rounding mode reaches the arithmetic.** One `mul.s` gives four
    different answers under the four modes, and only round-to-nearest was
    implemented — as the host's default, silently.

    Not via `fesetround()`. Honouring it requires `#pragma STDC FENV_ACCESS
    ON`, which GCC does not implement, so an optimiser is free to hoist
    arithmetic across the mode change and the generated C is built at -O2. A
    wrong answer from a compiler reordering is worse than slightly slower
    arithmetic. Instead the operation is computed in double and rounded once,
    in the requested direction, by stepping one ULP when the nearest result
    landed on the wrong side.

    That is exact, and not by luck: binary64 is wide enough to hold a binary32
    add, subtract or multiply with nothing lost, and for division it carries 53
    bits against the 2p+2 = 50 needed to decide a binary32 quotient. So none of
    the four suffers a double-rounding error. Overflow then needs no special
    case either — an exact value past FLT_MAX rounds to +inf under RN, and one
    ULP down from +inf is FLT_MAX, which is what RZ and RM are meant to give.

    **Flush-to-zero (FS, bit 24)** turns a denormal result into zero with the
    sign kept.

    **The exception flags.** FCR31 carries the same five bits twice — Cause at
    12..16, rewritten every operation, and Flags at 2..6, sticky until software
    clears them — which is why the update is one shift each. The encoding was
    measured rather than assumed; `fcr`'s four situations pin it exactly:

    | situation | fcr31 | reads as |
    |---|---|---|
    | `sqrt(-1)`, `0/0`, `NaN*NaN` | `0x00010040` | V |
    | `FLT_MAX * FLT_MAX` | `0x00005014` | O and I |
    | `1.0 / FLT_MAX` | `0x0000300C` | U and I |
    | `1.0 / 3.0` | `0x00001004` | I |

    Two of the rules are worth stating because the obvious version is wrong.
    An infinite result is only *overflow* if the operands were finite, and the
    double distinguishes them — it holds `FLT_MAX * FLT_MAX` without
    overflowing. And a denormal result is only *underflow* if it is also
    inexact; one that is exactly representable has lost nothing.

    `sqrt.s` got its own path, since `psp_fsqrt` is shared with the rasteriser
    and answers 0 for a negative — right there, wrong here. Its exactness is
    decidable without an exact square root: the correctly rounded `r` makes
    `r*r` a 24x24-bit product, exact in a double, so if that reproduces the
    operand nothing was lost.

    **It costs nothing measurable**, which was not obvious in advance — every
    FP operation now goes through a double and a store to a global. Checked
    rather than assumed: the game is bit-identical (633 GE lists, 106,108
    commands, 93,354,668 pixels) and the decoder path is 11,240 lists against
    11,222 before, which is noise. The rasteriser uses host floats directly and
    never touches this path, and the game's own maths is mostly VFPU.

    Left undone, deliberately: the rounding mode does not reach `psp_fsqrt`'s
    iteration, and the conversion ops do not raise flags. No test covers
    either, and inventing untested behaviour is worse than a stated gap.

25. **`threads`: four gaps closed, and the suite still 0 of 127.** Reporting
    this as a win would be wrong. The aggregate moved -- 8,870 differing lines
    to 8,057, and `threads/semaphores/create` from 74 to 7 -- and **not one
    test crossed to a full match.** What the work actually bought is a clear
    account of why.

    Four real gaps, each measured:

    - **The module entry thread had no priority.**
      `sceKernelGetThreadCurrentPriority` answered 0 when the scheduler held
      nothing. On hardware `module_start` runs on a loader-created thread at
      0x20, and 0 is not an absence -- it is a real and very urgent priority.
      `threads/mutex/unlock2` checks this on its first line and refuses to run
      at all when it is wrong.
    - **NULL names were accepted and attributes unchecked.** Hardware rejects
      a null name with `0x80020001` and any attribute at `0x200` or above with
      `0x80020191`. Measured: `create.expected` accepts `0x1ff` and refuses
      `0x200`.
    - **The three `Refer*Status` calls did not exist.** Same failure as the
      missing `sceDisplayGetFrameBuf` in 22 -- the create tests printed
      `attr=167767488, init=1308` where hardware writes zeros, because an
      unregistered call returns without touching its out-parameter.
    - **`sceKernelTerminateThread` did not exist**, which is why the
      rescheduler thread below survived being killed.

    **And one rule, which the tests measure directly.** pspautotests'
    `checkpoint` helper terminates a rescheduler thread between every two
    checks, prints, and restarts it; the thread sets a flag, and each line is
    tagged `[x]` if the flag is clear and `[r]` if set. That thread is created
    *at its creator's own priority*, and a PSP reschedules at
    `sceKernelStartThread` only for a thread that **outranks** the starter. So
    on hardware it never runs and every line reads `[x]`.

    Running every started thread to completion inside `StartThread` is not a
    conservative approximation of that. It is the opposite behaviour, and it
    tagged every line in the suite `[r]`. Outranking threads still run nested;
    everything else is parked, cancelled if the guest terminates it, and
    drained when the top-level run ends.

    ### Why none of it is enough, and what actually blocks the suite

    `hle_WaitEventFlag` ends an unsatisfiable wait like this:

    ```c
    warn_block("sceKernelWaitEventFlag");
    psp_ret(SCE_KERNEL_ERROR_WAIT_TIMEOUT);
    ```

    **A wait that cannot be satisfied returns a timeout instead of blocking.**
    The threads suite is largely a test of blocking and wakeup *ordering*, and
    there is nothing here to order. On top of that, five of the object types it
    exercises have no implementation at all -- `fpl`, `vpl`, `mbx`, `msgpipe`
    and `lwmutex`; `grep -c MsgPipe` over threadman.c returns 0.

    So the remaining work is making waits block and implementing five kernel
    object types, which is a larger piece than the gaps above and not a
    continuation of them.

    ### Two things recorded against interest

    Eight tests got *worse* in line count. Six are `msgpipe`/`fpl`, where the
    output is garbage either way and reordering merely shuffles it -- but
    `threads/events/events` went 16 to 22 and that one *is* implemented, so it
    is a genuine regression from the parked-thread model.

    A drain-on-block hook was built to fix exactly that: park a thread, and
    give it its turn when the starter blocks, which is when hardware would run
    it. **It was removed again.** Measured across all 432 tests it changed
    nothing -- byte-identical results, 8,057 lines both ways -- because no wait
    path reaches `psp_sched_block` to trigger it. It was correct-looking, dead,
    and untestable until waits block. Shipping it with a comment claiming it
    mattered would have been worse than not writing it.


26. **`threads`: waits block, and the suite opens.** 0 of 127 to **33**, and
    8,057 differing lines to **2,861** — and **no test in the suite is silent
    any more**. Across all 432 the oracle went 35 to **66** matching and 80,466
    to 74,228 differing lines. What follows is the part that is reusable — the
    numbers are in the commits.

    ### The architecture question 25 left open, and why it was forced

    25 ended by naming two pieces of work: make waits block, and implement five
    object types. It did not say that the first one has a prerequisite, and it
    does: **the two thread models are exclusive, not alternatives.**

    `interp.c`'s `spawn_hook` runs a started thread nested to completion inside
    its starter's frame, so a thread that blocks has no context to be resumed
    into. `psp_sched_block` then finds nothing runnable and the wait must fail.
    Making waits block while keeping that model turns every one of them into a
    deadlock report — which is exactly why the drain-on-block hook 25 records
    measured as byte-identical. It was correct and unreachable.

    So: real threads. And the interpreter is not a special case for `sched.c`,
    it is *the* case `sched.c` was built for. Its register file **is** `psp_cpu`
    — the same global the scheduler saves into a slot and restores on a switch —
    and its control flow is its host C stack, which is the invariant `sched.h`
    says host threads exist to hold. `thread_main` already builds a thread's
    register file and calls `psp_dispatch(entry)`, and `psp_dispatch` consults
    the dispatch hook *before* the lookup table, so **`sched.c` needed no change
    at all**. `run_thread_now` turned out to be a re-implementation of
    `thread_main` minus the ability to stop.

    The oracle's baseline is protected by construction rather than by
    measurement: `psp_interp_service_dispatch` still installs only the dispatch
    hook, and the new `psp_interp_service_threads` decides what happens to a
    *spawn*. `oracle_diff` never calls the second one.

    ### The third cause 25 did not know about, and it was the load-bearing one

    pspautotests' `checkpoint` prints `[x]` or `[r]` on **every line** depending
    on whether the kernel rescheduled during the operation just performed —
    about 2,900 lines across the suite. Where a switch happens is therefore
    measured directly, one character at a time, and two things put it in the
    wrong place:

    - **The timeslice counted somebody else's calls.** One global counter of
      firmware calls, so a switch happened every 64 calls made by *anybody*.
      Now guest microseconds, stamped per thread by the handoff.
    - **A firmware call cost 100µs of guest time.** The clock had one constant
      serving two questions. The *read* tick is sized against a loop counting
      microseconds and is right at 100µs; the tick charged to every other call
      exists only so time cannot stop, and its size had never been asked about
      separately. 100µs is two orders of magnitude more than a kernel call
      costs, and `threads/semaphores/fifo` asks for a 200µs timeout and then
      makes three kernel calls — so the timeout expired between a line's text
      and its newline. At 1µs the checkpoint finishes first, which is what
      hardware does.

    `fifo` also settles a question no amount of reasoning would have: **a later
    caller does not take a count a parked waiter is ahead of it for.** With a
    count of 1, a thread asking for 5 parks; a second asking for 1 parks behind
    it rather than taking what is sitting there; and only when the first times
    out and *leaves the queue* does the second get it. Eight lines, both rules.

    ### Two correct rules the game does not survive

    Both are measured, neither is shipped, and the pattern is the finding.

    - **The argument block is copied onto the thread's stack.** A one-byte start
      of the global `0x4567` reads back on hardware as `0xFFFFFF67` — one byte
      of data with the stack's 0xFF fill above it — which no reading of the
      original address can produce. Implementing it takes Armored Core from 633
      GE lists to **3**. Narrowed: performing the copy is harmless, handing the
      thread the copy's *address* is what breaks it, and copying 256 bytes
      instead of four does not help. The game starts three workers in a row from
      one shared slot, rewriting the word between each — so with the original
      pointer all three read the last value and with copies each reads its own,
      and the correct behaviour is the one it does not survive.
    - **A thread's priority change reaching the scheduler** did the same thing,
      *and that one had a findable cause.* `sceAudioOutputPannedBlocking` paid
      its playback backlog only when a host audio sink was registered; headless,
      a call with "Blocking" in its name returned instantly and the audio thread
      had been spinning since audio was implemented — 11 million outputs in 60
      seconds against the ~2,600 a paced thread makes. Survivable only while it
      shared a priority with everything else. The game lowers its main thread
      from 16 to 40 on its second firmware call; the moment that took effect the
      audio thread outranked it, and **a yield cannot give way to a lower
      priority**. A blocking output now waits the time its samples take whether
      or not anything is listening, and the priority change ships.

    **A spinning thread is not harmless because nothing has outranked it yet.**
    That is the reusable half. The argument-block case is still open and is the
    same shape: something upstream is wrong, and correctness elsewhere exposes
    it rather than causing it.

    ### Three instruments that lied, and one that did not exist

    - **`scripts/06-boot.sh` did not build the runtime library.** It compiled
      the boot host, linked, and printed a fresh set of numbers — measuring the
      *previous* library whenever the caller had not run cmake first. It cost
      two wrong attributions in one afternoon: the same change declared harmless
      and then harmful, on runs that had neither. Fixed; it builds now.
    - **`test_hle.c`'s `call5` wrote the fifth argument to `$sp+16`.** That is
      plain o32 and not what a PSP stub does — `hle.h` has carried the
      disassembly and the allocator bug that established `$t0` for a long time.
      Every `call5` had been putting the argument somewhere `psp_arg(4)` does
      not read, invisible because the only value ever passed was 0 and `$t0`
      also starts at 0.
    - **The interpreter's own guest stack was inside the allocator's heap.** Top
      of RAM, and the allocator's high end is also top of RAM, so the first
      `sceKernelCreateThread` returned a stack containing it. Invisible until
      `StartThread` began painting fresh stacks with 0xFF, at which point 125 of
      the 127 threads tests went from "returned" to "invalid instruction" in one
      step.
    - **`sceKernelSuspendDispatchThread` did not exist**, and
      `threads/scheduling/dispatch` is an entire test of it. Its NID was found
      the way the rest should be: hash the candidate names from the pspautotests
      sources and match them against the NIDs the run reports as unimplemented.

    ### What the attribute check cost, and the rule behind it

    `b8da5c8` added an attribute range check to `sceKernelCreateSema`,
    correctly, and applied the same predicate to `sceKernelCreateEventFlag`,
    where it is false. Hardware refuses bit `0x100` for an event flag and
    accepts bit `0x200` — `PSP_EVENT_WAITMULTIPLE` — which is the exact
    opposite of the semaphore. Armored Core creates one with attribute `0x200`,
    was told `ILLEGAL_ATTR`, used the error code as a uid, and the run went to
    **0 GE lists and 123,606 bad memory accesses**.

    Two things about it are worth keeping. **It was already broken at session
    start**, and was attributed to the scheduler work until reverse-applying
    both patches reproduced 123,606 exactly — the rule state.md already states,
    which cost a run anyway. And the instrument that found it is also
    state.md's: grep an `HLE_LOG` capture for every return of the form `= 0x8…`.
    Six calls in 22 million lines return an error and `CreateEventFlag` is two
    of them; nothing else pointed at it, because the failure surfaced 200
    instructions later as a write to `0xAFB10028`, which is the instruction word
    `sw $s1,0x28($sp)` being used as an address.

    ### Where the suite stands, and what to do next

    | subsystem | before | after | |
    |---|---|---|---|
    | `lwmutex` | 922 | **0** | all 8 match |
    | `fpl` | 404 | **38** | 4 of 9 match |
    | `alarm` | 90 | **29** | 2 of 4 match |
    | `mutex` | 419 | **68** | 3 of 10 match |
    | `semaphores` | 210 | **76** | 4 of 10 match |
    | `vtimers` | 306 | **104** | 2 of 12 match |
    | `mbx` | 671 | **127** | 2 of 9 match |
    | `tls` | 453 | **186** | 1 of 6 match |
    | `msgpipe` | 1001 | **193** | 2 of 10 match |
    | `vpl` | 1138 | **269** | 5 of 11 match; see below |
    | `scheduling` | 734 | 436 | dispatch suspend/resume |
    | `events` | 300 | 214 | |
    | `threads` | 1178 | 1046 | 4 of 16 match |

    **Every object type the suite exercises now exists.** What is left is
    accuracy rather than absence, and it is concentrated: `threads`, `vpl`,
    `scheduling` and `events` are two thirds of the remainder.

    ### The suite was not reproducible, and most of these numbers were samples

    **Read this before trusting a threads number from earlier in this item.**
    Three identical runs of `threads/refer` gave 59, 56 and 64 differing lines,
    with outputs of 67, 72 and 26 lines. Every threads total quoted above was
    one sample of a varying quantity, and the ones that read as exact — a test
    "matching hardware" on a single run — were the luckiest sample of several.
    Two full sweeps now agree byte for byte.

    The cause is worth the space because nothing in the design permits it. The
    token serialises execution, selection is priority-then-round-robin, and the
    clock is virtual; the *game* run is bit-identical across runs and always
    was. What broke it was `sceKernelTerminateThread` calling only
    `psp_sched_cancel_spawn` — which reaches a hook installed **only for the
    model without real threads**, so under the scheduler the call did nothing
    at all. pspautotests' `checkpoint` terminates and restarts one thread on
    every line, so each restart spawned another host thread for the same uid, a
    few hundred checkpoints ran the 130-slot table out, and *which* spawns
    failed depended on how many earlier host threads had happened to exit.

    **Reusing a dead slot naively is unsafe.** The dead thread's pthread may
    still be parked in `await_turn_locked`, which refuses to proceed only while
    the slot reads DEAD; hand that slot to a new thread and the old one's wait
    *succeeds*, so two host threads run guest code at once against the single
    global `psp_cpu`. `threads/create` went to **131,929,071 bad memory
    accesses** the moment reuse was allowed on the state alone.

    ### Retracted: slot exhaustion was not what silenced `threads/terminate`

    This section previously named safe slot reuse as the largest remaining
    piece and said the leak was what silenced that test. Both were wrong, and
    what settled it was building the thing rather than arguing about it: reuse
    *was* implemented safely — an `exited` barrier set under the lock
    immediately before each host thread's final unlock, on all eight paths a
    guest thread leaves by, with a `pthread_join` before the slot is handed
    out — and `threads/terminate` stayed exactly as silent while ten differing
    lines appeared elsewhere. Reverted.

    The real cause was one line of the test: **a thread cannot terminate
    itself**, `Current: 80020197`, exactly as it cannot suspend itself. The
    test does that once per block, and once terminate actually worked, obeying
    it killed the thread that was going to flush the checkpoint buffer. A rule
    that reads like a nicety, silencing a whole file.

    The slot leak is still real and is still worth fixing eventually. It is
    simply not what anything is blocked on, and a plausible cause that survives
    only because nobody tested it is the thing this file exists to prevent.

    Two process notes from the same afternoon, both of which produced a number
    that was believed before it was checked:

    - **A scripted text replacement silently did nothing.** An earlier commit
      had renamed the function it was searching for, so the edit was a no-op
      that reported success, and the measurement that followed described code
      that did not exist. Grep for the *new* text after any scripted edit.
    - **A single run of a nondeterministic test was reported as exact.** The
      fix is the same as everywhere else in this file: measure twice.

    ### Dispatch suspended is a stronger rule than it looks

    threads/scheduling/dispatch is an entire test of one thing, and the rule is
    not "a call that would block fails". It is that **every potentially
    blocking call is refused outright** while dispatch is suspended: a
    semaphore that has just been signalled is refused, and so is a wait whose
    count is illegal, which would otherwise report ILLEGAL_COUNT. The check
    therefore sits above argument validation, in all eleven blocking entry
    points.

    Suspend and resume do **not** nest — suspending an already-suspended
    dispatcher is an error, and so is resuming with anything that is not a
    state a suspend returned. The test's own `dispatchCheckpoint` reads the
    current state by suspending and immediately resuming, which only works
    because both halves fail cleanly when it is already off.

    ### `sceKernelReferThreadStatus` was wrong in five fields at once

    Worth listing because four of the five would each have looked like a
    plausible implementation:

    - **`attr` is not what the caller passed.** Hardware ORs in `0x800000FF`.
    - **`status` is the kernel's enumeration**, not the thread manager's own; a
      created thread reports 16.
    - **`exitStatus` is never zero.** DORMANT for a thread never started,
      NOT_DORMANT for one still running — it has not exited, so there is
      nothing to report — and its real status only once it has finished.
    - **`currentPriority` is the initial one until the scheduler has a slot.**
      Reporting the "no slot" sort-last sentinel put `0x7FFFFFFF` there, and a
      sentinel is not a priority.
    - It writes **as many bytes as the caller says it has room for**, which is
      the third object to want that rule after the alarm and the mutex.

    threads/create 258 → **0**.

    ### `vpl/order` is done, and it said what the 32 bytes were

    That test does not check totals. It casts a pointer into the pool and walks
    the kernel's own structures, printing every node's address, `next` and size
    at each step — so the layout, the placement and the order of operations are
    all observable. Its own header declares them, and the 32-byte
    `VplAccounting` at the base of the allocation **is** the 32 bytes of pool
    overhead the create test reports. The two were measured a week apart and
    are the same fact.

    Everything else fell out of reading the expected output as a *sequence*
    rather than as lines. Allocation carves from the **top** of the first free
    block that fits; an allocated block's `next` is the pool's `start`, which
    is what a free uses to tell the two apart; a free coalesces both ways and
    leaves the head at the node preceding the returned block; and the pool is
    allocated from the **high** end, which is the only way the test's
    `addr3 + 0x18` reaches the middle of three pools.

    The bug that survived the first pass is the reusable part: the free list is
    ascending **and circular**, so the predecessor of a block below every free
    node is the *terminator*, where the list wraps. Searching only for a node
    below the freed address — what a non-circular list wants — put every such
    block in the wrong place and left 57 lines that read as a coalescing fault.

    278 differing lines to zero. **And the barge rule was re-measured rather
    than carried over**: it had been established against the old host-side
    allocator, so it was A/B'd again against this one — 269 differing lines
    with it, 331 without. It still wins, and `vpl/priority`'s remaining 230 is
    a wait-and-wake ordering difference rather than that rule.

    ### Eight object types, seven attribute masks

    | type | legal attribute bits |
    |---|---|
    | semaphore | `0x1FF` |
    | event flag | `0x2FF` minus `0x100` |
    | mutex | `0xBFF` |
    | lwmutex | `0x3FF` |
    | vpl | `0x43FF` |
    | mbx | `0x5FF` |
    | fpl | `0x41FF` |
    | tlspl | `0x41FF` |

    Seven distinct masks across eight types. The mutex refuses bit 10 and
    accepts bit 11; mbx does the reverse. An event flag refuses bit 8 where
    every other type accepts it. A vpl takes bit 9 where an fpl does not, and
    the two masks differ by exactly that bit. **Read the capture for the type
    you are implementing, not the one next to it** — the one time this project
    assumed two objects shared a rule it took the game from 633 GE lists to 0.

    The single pair that *does* agree, fpl and tlspl, was checked against its
    own capture like the rest. It happens to be the only one that could have
    been guessed, and there was no way to know that in advance.

    ### What the seven types cost, and the four shapes they came in

    Roughly a file each, no scheduler changes, and the work was reading rather
    than designing. What made each hard was different:

    - **`lwmutex` — state the guest owns.** All eight tests match. The count,
      owner and attributes live in a 32-byte workarea and the uncontended lock
      is arithmetic on guest memory with no kernel involvement. The tests prove
      it by running every case twice, once against a hand-forged workarea with
      a zero uid, which locks and unlocks perfectly while `Refer` on it answers
      NOT_FOUND. Three consequences that had to be separated: a uid of **zero**
      is a legal user-space lwmutex while a **non-zero uid that resolves to
      nothing** is a deleted one; a memcpy of a live workarea can be *locked*
      but not *deleted*; and the pre-6.00 `sceKernelTryLockLwMutex` answers one
      code for all fifteen of its failures where the `_600` export gives the
      specific one.
    - **`mbx` — a circular list threaded through the guest's own messages.**
      The kernel stores no copy: the queue is a linked list in the message
      headers, and it *loops* — one message reports `next=ITSELF`, two report
      `OTHER` and `FIRST`. A NULL-terminated list would differ on every line.
    - **`vpl`, `fpl`, `msgpipe` and `tlspl` — arithmetic that is the
      contract.** A vpl's overhead is 32 bytes per pool and 8 per allocation
      with 8-byte alignment, and `round_up(size, 8) - 32` reproduces all eleven
      pool sizes the create test prints. An **fpl has neither** — nothing is
      rounded and nothing is reserved, which had to be checked rather than
      inherited from the pool next to it. A msgpipe has no framing at all
      despite the name: 256 bytes into a 0x1000 pipe leaves `free=f00`. And an
      fpl's free list is a **queue**, not a lowest-first search: after freeing
      the first block the next allocation lands *above* the second, which the
      test states in words.
    - **`alarm` and `vtimer` — a handler called from no thread.** Nothing
      blocks on either. threads/alarm prints `alarmHandler called on thread -1`
      and means it: the handler asks which thread it is on and the answer is
      neither of the two that exist. A deadline is noticed at a firmware call,
      because that is the only place guest time is observed to move, and the
      handler's return value is a *rescheduling interval* — which is how a
      guest writes a periodic timer with one call and no thread.

    Three rules that are distinctions rather than behaviours, each of which a
    reasonable implementation would have flattened:

    - A **negative** length is a different mistake from one merely bigger than
      the pipe, and gets a different code.
    - A pipe created with **no buffer** answers FULL rather than too-big, even
      to requests that are obviously too big.
    - A wait that **timed out** writes `bytes = 0` where an argument failure
      leaves the caller's value alone. The tests pre-seed `0x1337` to tell them
      apart.

    Two more of the same kind, from the later types:

    - **A `Refer*Status` writes exactly as many bytes as the caller says it has
      room for.** `alarm/refer` sweeps the size field one byte at a time: at 1
      the `size` field alone reads back as 20, because one byte of the
      little-endian 0x14 is the whole of it. Writing all 20 regardless differs
      on every line of the sweep; writing none differs on the rest.
    - **A 64-bit read signals failure with all-ones.** Its whole return value
      is the number, so there is no room for an error code, and returning the
      ordinary `0x800201BE` in the low word reads back as a plausible time.

    And one crash worth recording because of where it hid: `mpp_take` updated
    the ring cursor *before* checking the length, so a zero-length receive on a
    zero-size pipe — both legal, both in the tests — took a remainder by zero.
    It cost 45 lines of `tryreceive.expected`, which simply stopped mid-file
    with no other symptom.

    ### Two absences that were subtractions

    Both are the pattern state.md names as the shape of most bugs here, hiding
    in an arithmetic rather than in a return value.

    **`sceKernelTotalFreeMemSize` and `sceKernelMaxFreeMemSize` did not
    exist.** An unimplemented call returns zero, and a test measuring what an
    operation *consumed* takes the difference of two of them — so
    `(allocated N bytes)` read 0 for every create in `tls/create`, 120 lines of
    it, while saying nothing about memory at all.

    **`vpl`'s free path returned the allocator's `ILLEGAL_MEMBLOCK`**
    (`0x800201A9`) where both pools use `0x800201B6` for a pointer that is real
    but is not the start of one of their live blocks. Found by writing `fpl`
    next to it; it would have gone on looking like a vpl-specific gap.

    ### `mutex` was the first, and it said what the next one would cost

    419 differing lines to 68 in one file, `src/hle/kernlock.c`, with three
    tests matching exactly. Nothing about it needed a scheduler change; it needed
    the captures read carefully, and two of its rules are not what a mutex
    usually is.

    **Its attribute mask is `0xBFF`** — bit 10 refused where bit 11 is accepted.
    That is now three object types with three different attribute rules
    (semaphores take the low nine bits, event flags refuse `0x100` and accept
    `0x200`), and the one time this project assumed two of them shared a rule it
    cost the game 633 GE lists. **Read the capture for the type you are
    implementing.**

    Two corrections fell out that were invisible with only one object type to
    look at, and both are now shared: a *zero* timeout is a deadline that has
    already arrived rather than the shortest future one, and a `Refer*Status`
    call offering zero bytes gets zero back and nothing written.

    Three of them have observable internals and will not be guessed into place:

    - **`lwmutex` state lives in guest memory** — a 32-byte workarea
      `{count, thread, attr, numWaitThreads, uid, pad[3]}` that the tests dump
      raw, including the three pad words, and cross-check between
      `ReferLwMutexStatus` and `ReferLwMutexStatusByID` with a `memcmp`.
    - **`vpl` tests probe the allocator's layout** — a 0x1000 pool reports
      `poolSize=00000FE0`, so 32 bytes of overhead, with an 8-byte header per
      allocation and exact `freeSize` after every operation.
    - **`mbx` threads its queue through a guest-owned `SceKernelMsgPacket`**,
      poisoned with `0xDEADBEEF` before sending so that the test can classify
      what the kernel wrote as `NULL`/`DEAD`/`ITSELF`/`FIRST`/`OTHER`. The
      pointer topology is part of the contract.

    All three are readable straight out of the `.expected` files. None of them
    needs a scheduler change: `vpl`, `mbx` and `lwmutex` predate `checkpoint()`
    and carry no `[x]`/`[r]` column at all, which is 28 of the 127 reachable on
    object-type work alone.

    ### `sceKernelGetThreadmanIdList`, and the census it performs on us

    148 differing lines to 2. It is one call, and it is the only test that
    walks the *whole* object table, so it doubles as an audit of every type
    added above: `threadmanidlist` asks for each of the fourteen id types plus
    the four thread-state pseudo-types and prints what came back.

    The valid set is exactly **1–14 and 0x40–0x43**, and everything else —
    `-1`, 0, 15–24, 0x44–0x48, 0x80, 0x100, 0x200, 0x1000, 0x100001,
    0x80000000 — answers `0x800201BB` and leaves the caller's count word
    untouched. Untouched is the observable part: the test seeds it with
    `-1337` and reports only whether it changed, so a call that fails and
    still writes zero is a differing line even though the error code matched.
    A **negative** size is a second failure of the same shape (`0x800200D3`);
    zero is not, and neither is a NULL buffer, and neither is a NULL count.

    Two things the signature does not say:

    - **The count is how many exist, not how many fitted.** The return value is
      the number written. They differ exactly when the buffer is short, and the
      test checks the pair against each other and against a scan of the buffer.
    - **Type 1 counts dormant threads too.** The state pseudo-types partition
      only the live ones; 0x43 is dormant, and a thread that has never been
      started and a thread that has finished are both dormant.

    Sleeping (0x40) and delaying (0x41) are separate types, and the scheduler
    has one `PSP_SCHED_SLEEPING` for both — the distinction only exists at the
    call that made it, so `psp_thread` now records which one parked it.

    Rather than export five files' private tables, each module registers a
    lister and answers for the types it owns. The count is threaded through as
    a running total that keeps counting past the buffer, which is the same
    total-versus-written split the call itself reports.

    The two lines left are one `[x]`/`[r]`: the rescheduler thread happens to
    run during the 45-line type sweep. Two consecutive runs are byte-identical,
    so it is a fixed offset in our interleaving rather than a race.

    The audit found one absence, and it is the reason the id list is worth
    having: `sceKernelTerminateDeleteThread` was not registered. The test
    creates a thread in each of four states and cleans it up with that call
    between sections, so the counts climbed 1, 2, 3, 4 across a file whose
    every expected line reads 1. An unimplemented call returns zero, which
    reads as success, so nothing said the cleanup had not happened — the id
    list is what made a leak visible at all.

    It is not terminate followed by delete. `threads/terminate` runs the same
    ten cases through both and they part on the dormant ones:

    ```
    sceKernelTerminateThread        Created: 800201a2   Finished: 800201a2
    sceKernelTerminateDeleteThread  Created: 00000000   Finished: 00000000
    ```

    There is nothing to stop but there is still something to free. The id
    checks are terminate's, including the one that forbids a thread from
    ending itself.

    ### `threads/terminate`: three calls compared, five rules found

    18 differing lines to 3. The test is built as a comparison -- it runs ten
    thread states through `sceKernelTerminateThread`,
    `sceKernelTerminateDeleteThread` and `sceKernelDeleteThread` and prints all
    thirty answers -- and comparison is what makes it productive: each rule
    shows up as one column differing from the other two.

    **Delete refuses what it would have to stop.** It is the only one of the
    three that answers `800201a4` to a thread that is ready, waiting, suspended
    or running, including the caller itself, which is what an id of 0 names
    here. Ours accepted all of them.

    **A thread that finishes inside `sceKernelStartThread` was marked runnable
    again on the way out.** `psp_sched_spawn` performs the reschedule-at-the-
    system-call for a thread that outranks its starter -- it has done so all
    along, with a comment saying why -- so a short thread can be *finished*
    before that call returns. `hle_StartThread` then assigned `TH_READY` over
    the `TH_DORMANT` its own end hook had just written. Moving the bookkeeping
    to before the spawn fixed `Finished:` in two tests. Worth naming the shape:
    the missing piece was not a rule but an ordering, and the code that was
    wrong looked like initialisation.

    **Terminating a thread ends it.** Ours stopped the thread and left everything
    waiting on it parked for the rest of the run. All the ways a thread reaches
    an end now go through one function, and they differ only in the status left
    behind -- `0x800201AC` for terminate, which is not a wake code but the
    *exit status*: `threads/refer` reads it back out with
    `sceKernelReferThreadStatus` (`exit=800201ac`) and `threads/threadend` gets
    the same value out of `sceKernelWaitThreadEnd`, which returns the exit
    status. Two observations, one value. A thread freed out from under a wait
    answers the same, and `threads/threadend` puts that on consecutive lines
    with `80020198` for a thread that was already gone when the wait began.

    The three lines left are one artifact, and it is measured rather than
    guessed: `checkpoint()` calls `sceKernelGetSystemTimeWide`, which costs
    `PSP_READ_TICK_US` -- 100us of virtual guest time. Two checkpoints exhaust
    the test's `sceKernelDelayThread(200)`, so the main thread resumes and exits
    before the third line is written. Hardware spends microseconds there. The
    same granularity is what `sched.c` already records about the `[x]`/`[r]`
    column.

    ### The two rules `threads/change` needed, and the corpse in the way

    Correcting the start ordering above cost `threads/change` 98 lines before it
    gained any, which is the useful kind of regression: it had been passing for
    a reason that was not the rule. Three findings came out of the file, and the
    last one is not in threadman at all.

    **`sceKernelStartThread` distinguishes three bad ids where we had one.**
    Zero is `80020197`, an id naming nothing is `80020198`, and a thread that is
    already running is `800201a4` -- `threads/start.expected` prints them on
    consecutive lines as `NULL`, `Deleted`/`Invalid` and `Twice`/`Current`. The
    third is the load-bearing one: the restart in `threads/change`'s loop only
    happens when the thread is dormant, and everything downstream depends on it.

    **Starting forgets the priority.** A restarted thread comes back at the
    priority it was created with: `0x08 priority: 00000000` followed by
    `After restart: Current=30, init=30`.

    **A priority change reschedules like a start.** Raising *another* thread
    above the caller hands it the CPU at that instruction, which
    `psp_sched_spawn` has always done for a spawn and `hle_ChangeThreadPriority`
    did only for the caller's own priority. The test proves it without ambiguity:
    it sweeps a ready thread through every legal priority, and the four values
    higher than the caller's are exactly the four where ` - testThread` appears
    *before* the line reporting the change that caused it -- the thread ran to
    completion inside the call.

    That left one thing that no threadman rule could reach. **Slots are never
    reused** -- deliberately, and `psp_sched_spawn` records the 131,929,071 bad
    memory accesses that reuse cost -- so a thread that is terminated and started
    again owns *two* slots carrying one uid, and `slot_of` returned the first,
    which is the corpse. Its priority, its state and its place in every scan.
    Making a live slot win the lookup is not reuse and needs no join; the dead
    slot is still the answer when it is the only one, because a finished thread
    is a thing callers legitimately ask about.

    Across the suite: 2,585 differing lines to 2,515, 34 matching. Seven tests
    improved -- `start` 71 to 53, `threadend` 38 to 24, `terminate` 18 to 3,
    `change` 156 to 140, `suspend` 6 to 2, `refer` 52 to 50, `threads` 4 to 0 --
    and `tls/free` went 23 to 26, which is a reshuffle rather than a new fault:
    threads now run at different points, so different lines of a test that has
    real tls bugs in it line up.

    ### `threads/change` finished: one attribute bit, and a scheduling rule

    140 differing lines to 2, in two changes that have nothing to do with each
    other beyond the file that found them.

    **`sceKernelChangeCurrentThreadAttr` owns exactly one bit.** Ours applied
    whatever it was handed. The test sweeps all thirty-two bits through both
    arguments, twice, and gets `80020191` for thirty-one of them in each
    direction -- only `PSP_THREAD_ATTR_VFPU` (`0x4000`) is the thread's own
    business. The attribute readback on the next line is what makes the sweep
    airtight: it holds still across every refused call, so a refusal that
    quietly applied the change would show. 128 of the 140 lines.

    **A displaced thread is not a finished one.** The remaining lines were
    `[x]`/`[r]`, which this file has repeatedly declined to chase -- and four of
    the five turned out to be a real rule rather than the clock. A PSP puts a
    thread that was preempted at the *head* of its priority queue; it never
    stopped being the one that should run at that priority. A thread that yields
    goes to the tail. `psp_sched_yield` served both, so a starter displaced by
    the thread it had just made more urgent came back *behind* an
    equal-priority thread that was merely waiting -- which is pspautotests'
    rescheduler thread, and which tags the checkpoint `[r]`.

    Splitting the two -- one flag on the slot, one tie-break in the handoff --
    took `threads/change` to 2 and fourteen tests with it, none regressed:
    `scheduling/scheduling` 58 to 46, `msgpipe/data` 67 to 55, all five
    `callbacks` tests, `threadend` 24 to 18, `scheduling/dispatch` 369 to 363.
    Suite: 2,515 differing lines to 2,321. The game is unchanged at 633 GE
    lists, which is the check that mattered: `sched.c` says the slice is what
    stops Armored Core's disc-read poster starving its equal-priority workers,
    and this changes who wins exactly that race.

    The one line left is the evidence that the rest of the column really is the
    timer. The test makes the *same call twice in a row* --
    `sceKernelChangeThreadPriority(sceKernelGetThreadId(), 0x20)` on a thread
    already at 0x20 -- and hardware tags the first `[x]` and the second `[r]`.
    No rule about that call can produce two different answers to it.

    ### `threads/start` matches hardware, and the held-back rule is released

    53 differing lines to 0. Four things, and the third is the one this file
    has been carrying an apology for.

    **The kernel writes four words of its own into every thread stack** -- the
    k0 area at the top plus one at the bottom -- and the test checks them by
    hand after reading the stack address back out of
    `sceKernelReferThreadStatus`:

    ```
    stack[0]         == thread id
    stackEnd[-16]    == thread id
    stackEnd[-14]    == stack base
    stackEnd[-2..-1] == 0xFFFFFFFF
    ```

    The last pair look like the 0xFF fill and are not, which is how the test
    separates them: it creates a thread with `PSP_THREAD_ATTR_NO_FILLSTACK` and
    finds them still there. The top **0x100 bytes** are reserved for that area.

    **Three stack attributes are acted on, not merely recorded** -- `0x100000`
    no-fill, `0x200000` clear-on-delete, `0x400000` low-stack -- and each is
    measured by scribbling a known pattern over the memory first and reporting
    what survived. An attribute that was stored and ignored shows up as a
    *missing* line rather than a wrong one, which is the kind of absence this
    file keeps finding.

    **The argument block copy is no longer held back.** It was declined with a
    measurement -- 633 GE lists to 3 -- and the note guessed at the reason:
    "that points at something else being wrong upstream rather than at the
    rule." The guess was right. The game starts three workers in a row from one
    shared argument slot, rewriting the word between each; with the original
    pointer all three read the last value, with copies each reads its own. The
    upstream fault was the missing preemption ordering: those workers are
    started *above* their starter's priority, so each now runs at its start,
    before the slot is rewritten. Each reads its own value either way, and the
    game survives the correct behaviour because the scheduler became right.

    Where the copy lands is measured rather than chosen: `top - 0x100 -
    roundup(len, 16)`, and the 0x100 is the k0 area above, so the two
    measurements confirm each other.

    **A thread stack is the size that was asked for.** We raised anything under
    0x1000 to 0x1000, which is observable twice: `ReferThreadStatus` reports the
    size back verbatim, and the argument-block offsets are measured from the
    stack *base*, so a stack 0x800 too big moves every one of them by 0x800.

    Suite: 2,321 differing lines to 2,252, 36 matching.
    `semaphores/semaphores` -- which measures the copy alongside the two
    argument rules already implemented -- goes to 0 with it, and
    `scheduling/scheduling` 46 to 36.

    ### `threads/threadend`, and the callbacks that were never delivered

    18 differing lines to 2. Two small corrections and one absent feature.

    **`sceKernelWaitThreadEnd` refuses three ids it accepted.** Zero does not
    mean "the current thread" here the way it does for a priority change, and a
    thread cannot wait for itself -- both `80020197`, where an id naming nothing
    is `80020198`. And a thread that was *never started* will never end, so the
    wait is refused outright with `800201a2` rather than timing out. A thread
    that has *finished* is dormant too and returns its exit status, so the test
    is "was it ever started", which the very next line of the file checks.

    **A thread's status comes from the scheduler.** `ReferThreadStatus` built it
    out of threadman's own field, which records that a thread was started and
    never that it parked, so a thread inside `sceKernelSleepThread` read back
    READY. The test isolates it to one line by printing the same thread twice
    with nothing between but a start -- `before start status=00000010`,
    `after start status=00000004` -- on a thread whose whole body is a sleep.

    **Callbacks were registered and never delivered.** The old comment was
    honest about it and wrong about the consequence: it argued that reporting
    zero from `sceKernelCheckCallback` was "accurate rather than a stub" because
    nothing here raises a callback. `sceKernelNotifyCallback` raises one, and it
    was not registered at all.

    The handler's three arguments are pinned by a single line of
    `callbacks/notify`, which fires it 10002 times:

    ```
     * cbFunc hit: 00002712, 00000001, 00000000
    ```

    0x2712 is 10002 -- the accumulated count, entered *once* -- then the *last*
    notify argument and the common pointer given at create. The intermediate
    arguments are not kept. Two more rules come free with it: a callback is
    delivered only to the thread that created it, and `sceKernelCheckCallback`
    reports *whether* anything ran rather than how many (`With 2 pending:
    00000001` after two handlers).

    `sceKernelDeleteCallback` answered OK and deleted nothing, which is the kind
    of stub that stays invisible until something downstream starts working:
    with notify implemented, a notify on a deleted callback succeeded.

    Suite: 2,252 differing lines to 2,195. Six tests improved, none regressed --
    `threadend` 18 to 2, `callbacks/count` 22 to 10, `notify` 28 to 18,
    `check` 13 to 6, `delete` 12 to 6, `cancel` 9 to 3. `cancel` first went
    *up* by two, which was an accidental match lost rather than a fault: it
    reads the notify count back, and a count that was always zero happened to
    agree until it became real.

    The two lines left are one `[x]`/`[r]`.

    ### `callbacks/notify`: a CB wait is a wait a notify can end

    18 differing lines to 2. Delivering callbacks on the way into a wait --
    which is what the previous commit implemented -- is only half of what the
    suffix means, and the wrong half for the tests built to measure it.

    `callbacks/notify` puts threads in `sceKernelSleepThreadCB` and **never
    wakes them by name at all**. The handler lines they print are the only
    evidence those threads ran. So a notify raised by another thread has to end
    the wait long enough to run the handler, after which the wait resumes; a
    real `sceKernelWakeupThread` ends it for good. The two are told apart by a
    flag set at the notify.

    Three more rules came out of the same file:

    - **A handler returning non-zero deletes the callback.** Measured from the
      outside, without asking about the callback: a sleeper whose handler
      returns 0x1337 answers `Notify #1: OK` and then
      `Notify #2: Failed (800201a1)`, with nothing between but the handler
      running. Every other handler in the file returns 0 and survives.
    - **Delivery is re-entrant.** A handler may notify itself and then call a CB
      wait, which delivers the notify it just raised. Guarding against re-entry
      silences the second hit. What stops it running away is that the count is
      cleared *before* the dispatch.
    - **`scePowerRegisterCallback` fires the callback as it registers it.** The
      test says so in the line that calls it -- `(causes notify)` -- and proves
      it by reading the count back as 2 after a single manual notify.

    And one absence with a long reach: **`sceKernelGetThreadExitStatus` was not
    registered**, so it answered zero. That is not a harmless zero. The
    pspautotests thread wrappers use it to decide whether a worker is alive, so
    a live thread read as "exited cleanly" and was torn down silently instead of
    being terminated and announced -- whole lines missing from several files.
    `threads/exitstatus` went 18 to 0 with it.

    Suite: 2,195 differing lines to 2,144, 37 matching. Two tests got noisier,
    and in both the fault is older than the change that exposed it:
    `msgpipe/data` 55 to 63, whose workers block on a zero-buffer pipe and never
    complete either way, and `fpl/cancel` 3 to 4, sitting on top of a real bug
    the file already showed -- `sceKernelCancelFpl` releases its waiters with
    `800201b5` where hardware uses `800201a9`.

    The two lines left in `notify` are one `[x]`/`[r]`, and this one is squarely
    the virtual clock: the test makes **10000** notify calls in a row, and at
    `PSP_CALL_TICK_US` each that is 10ms of guest time against a 5ms timeslice,
    so our rescheduler thread runs twice where hardware's does not run at all.

    ### A message pipe is a byte stream, and `msgpipe/data` says so four times

    63 differing lines to 0. Our transfer was all-or-nothing: a waiter either
    got everything it asked for or nothing. A pipe is a byte stream, and the
    test is built to make the difference unmissable -- it runs the *same*
    scenario four times, varying one thing each time, and the four runs
    triangulate the model:

    | block | buffer | attr | what it isolates |
    |---|---|---|---|
    | Using a buffer | 0x100 | 0 | the baseline |
    | Without a buffer | **0** | 0 | identical output with nowhere to store |
    | Using receive priorities | 4 | 0x1000 | which queue that bit orders |
    | Using send priorities | 4 | 0x100 | the other queue, and the asymmetry |

    Four rules, each of which one block pins:

    - **A waiting receiver is filled in pieces**, into its own buffer, and keeps
      waiting for the rest. Three receivers wanting four bytes each, senders
      offering three at a time, and each receiver finally reads a word assembled
      across separate sends: `msg1`, `msg2`, `msg3`.
    - **With the buffer drained, a sender hands bytes straight to a receiver.**
      The no-buffer block produces byte-for-byte the same output as the 0x100
      one, which it cannot do through storage.
    - **The two queues take their order from different bits** -- 0x100 for
      senders, 0x1000 for receivers. We passed the raw attribute to both, so
      receivers were ordered by the sender's rule.
    - **A full-wait sender does not part-fill the buffer; an ASAP one does.**
      This is the subtle one, and the send-priority block reads it out directly:
      a sender arriving at a pipe with one byte free leaves that byte alone, so
      the receiver that comes next takes it from a *more urgent* sender instead
      -- `msgs`, not `msg1`. Had the first sender dribbled its byte in, the
      whole rest of the block would decode differently.

    One consequence worth stating separately: **whoever satisfies a waiter is
    the only one who knows how many bytes moved**, because an ASAP waiter can be
    released with less than it asked for. So the byte count is written by the
    releaser rather than by the woken thread, and a pipe deleted under a waiter
    still reports what that waiter had got -- `received = 00000000` where an
    untouched word would hold the test's 0x1337.

    The rewrite livelocked the first time. The pump keeps the calling thread in
    the queue so it sits in the right place in the release order, and I set the
    "something moved" flag on *reaching* that thread rather than on moving
    bytes, so the loop re-picked it forever. `msgpipe/receive` went from a diff
    to no output at all, which is the shape a hang takes here.

    Suite: 2,144 differing lines to 2,051, 38 matching. `data` 63 to 0, `send`
    24 to 12, `receive` 22 to 12, `delete` 8 to 4, `trysend` 16 to 12.

    ### `msgpipe/create`: the seventh attribute mask, and two ceilings

    28 differing lines to 2, in three findings, only one of which is about
    message pipes.

    **The attribute mask is `0x51FF`**, and the sweep settles it in two lines:
    `0x3FF` is refused and `0x51FF` is accepted, so the legal set is the low
    nine bits plus the two queue-order bits. That is now seven object types with
    seven different masks -- sema `0x1FF`, event flag `0x2FF` minus `0x100`,
    mutex `0xBFF`, lwmutex `0x3FF`, vpl `0x43FF`, mbx `0x5FF`, fpl and tlspl
    `0x41FF`, msgpipe `0x51FF`. **Read the capture for the type you are
    implementing.**

    **A request too big to round up was allocated anyway.** In the allocator,
    not in msgpipe:

    ```c
    uint32_t rounded = (size + 0xFF) & ~0xFFu;
    if (!rounded) rounded = 0x100;
    ```

    `0xFFFFFFFF + 0xFF` truncates to `0xFE`, masks to 0, and the zero-size guard
    on the next line turns it into a **256-byte** allocation that succeeds. A
    caller asking for four gigabytes got a handle and a buffer three orders of
    magnitude smaller than the one it thinks it has. Every caller of
    `psp_sysmem_alloc` had it; the test that asks for `0xFFFFFFFF` is the one
    that noticed.

    **Two ceilings, and the second was invisible behind the first.** The test
    ends by creating 1024 pipes and checking they all succeeded. We failed at
    64 (the pipe table), and then at 507 (the allocator's block table, 512
    entries, one per allocation). Both were guesses about what a game needs, and
    the tests are the only thing here that has ever named a number. A capacity
    failure tells you about exactly one limit at a time.

    ### `msgpipe/tryreceive`: a poll is not a peek, and a wake needs a reason

    16 differing lines to 4.

    **A poll reaches past the buffer.** `sceKernelTryReceiveMsgPipe` decided
    from what was stored, so on a pipe with no buffer it always failed -- even
    with a sender blocked on the other side holding exactly what was asked for.
    Two lines of the test say otherwise, both against `buffer=0`:
    `Partial packet: OK (bytes=128)`, `Complete packet: OK (bytes=256)`.

    Deciding from the buffer was a shortcut taken for a real reason: a poll
    moves all-or-nothing and cannot take bytes back from a thread it has already
    handed them to. The way out is to *ask instead of try* -- capacity is
    knowable, so the poll computes it, decides, and only then joins the queue,
    by which point completing is certain.

    **A wake does not say what happened, and two outcomes need opposite
    answers.** The test deletes a pipe immediately after a poll that satisfied
    two senders. Those senders were released, their bytes moved, their counts
    written -- and then the pipe vanished before they next held the CPU. Our
    post-block path looked the pipe up first, found it gone, and reported
    `800201b5`; hardware answers `00000000`, because bytes already moved are
    moved. But a sender genuinely *abandoned* by that same delete does get
    `800201b5`, on the next line of the same file, so "was I woken" cannot
    decide it and neither can "does the object still exist".

    So the waker leaves a reason behind in the scheduler slot. This is general
    rather than msgpipe's: `fpl/cancel` has the same shape, where a cancel must
    answer `800201a9` and a delete `800201b5`, and it is still open.

    The two lines left are both the virtual clock -- one `[x]`/`[r]`, and one
    timeout readback of `10ms remaining` against hardware's `8ms`, which is the
    same thing measured in milliseconds.

    ### `msgpipe/cancel`, and a constant that had been named on a guess

    8 differing lines to 0. The gap was the one flagged as open one commit
    earlier, and the mechanism to close it was already there: cancelling an
    object is not destroying it, and the waiters turned out of it answer
    `800201a9` where a delete's answer is `800201b5`. Only the waker knows
    which, so it says so.

    Worth recording what the value turned out to be. `0x800201A9` was already in
    `hle.h`, named `SCE_KERNEL_ERROR_ILLEGAL_MEMBLOCK` -- and **never used**.
    That name was a guess made before the pool types were written, and the
    captures overruled it at the time: a pool's "not a live block of mine" is
    `0x800201B6`, which is why `ILLEGAL_MEMBLOCK_PTR` exists next to it. The
    guess was left behind rather than removed, and sat there looking like
    knowledge. It is `WAIT_CANCEL`.

    The timeout readback came right with it -- `timeout = 8ms remaining`,
    matching hardware exactly -- which is worth noting against the two
    neighbouring tests where that readback is still 2ms out: the millisecond
    difference there is not a fixed offset in the clock, it is a consequence of
    a wait taking a different path.

    ### The same rule, for every object type that has a Cancel

    `fpl/cancel`, `vpl/cancel` and `mbx/cancel` all went to 0 together, and the
    diff each one showed was the same two lines msgpipe had shown: `800201a9`
    from hardware, `800201b5` from us. Four object types, one rule, and the
    fifth -- `mutex` -- moved 24 to 20 with the same change and has other
    problems underneath.

    Worth noting as method rather than as a finding. The mechanism was built
    for message pipes and the rule was stated in the commit that built it, with
    the other types named as still open. Generalising it afterwards took the
    reason constants out of `kernobj.c` and into `waitq.h`, added a
    `psp_waitq_cancel_all` next to the existing `release_all`, and inserted the
    same six lines into four waits. Nothing was rediscovered. That is the
    argument for writing down the shape of a fix at the time even when only one
    caller needs it.

    Threads suite at this point: **42 of 127 matching, 1,967 differing lines**,
    from 0 of 127 and 8,057 at the start of this work.

    ### `mutex/cancel`: a re-arm, and a sentinel that is not zero

    20 differing lines to 0, and the second of the two findings reached four
    other files.

    **`sceKernelCancelMutex` re-arms the mutex.** It is not just a release: it
    sets the count, and the test sweeps that count against a mutex it never
    locks by any other means. The ceiling is a lock's -- one, when the mutex is
    not recursive -- and exceeding it leaves the object *and* the caller's
    wait-count word untouched. A negative count is not a count; it means
    unlocked, as zero does.

    **An unheld mutex reports its owner as `-1`.** The tests read that field
    through `info.lockThread == -1 ? 0 : 1`, so the zero we wrote for a free
    mutex printed as *locked* -- an error that inverts a boolean rather than
    perturbing a number, which is why it was worth four other tests
    (`mutex/create` 18 to 2, `refer` 2 to 0, `unlock` 10 to 6).

    It also forced a small correction of an assumption: the owner cannot be
    derived from the uid alone, because the main context's uid is **0**, so a
    mutex held by it is indistinguishable from a free one by that field. The
    count is what says whether it is held. Worth remembering wherever else a
    uid of zero is treated as "nobody".

    ### `mutex/unlock`: the one rule an unlock has that a lock does not

    6 differing lines to 0, and one line of code. Unlocking requires *holding*
    it, and we checked only that the mutex was held by somebody.

    The test isolates it the way this suite usually does -- two adjacent lines
    with one thing different between them:

    ```
    Locked 1 => 1: L1 L2 OK (thread=00000000)
    Locked 0 => 1: L1 L2 Failed (thread=00000000, main=800201C5)
    ```

    Same call, same count, same thread doing the unlocking. In the first, main
    created the mutex already locked, so main owns it; in the second it was
    created free and a worker thread took it first. Only the owner differs.

    The code is `800201C5`, which is the same one an unlock of a *free* mutex
    gets -- so the kernel does not distinguish "nobody has it" from "somebody
    else has it". Both are "you do not have it".

    ### The clock: a read is a firmware call like any other

    This one was named in three separate commits as "the virtual clock" and
    left alone each time, because it looked like an accuracy limit rather than
    a bug. It was a bug, and it was one line.

    A clock read charged **100us** of guest time where every other firmware
    call charged 1. The argument for that, written down when the constant was
    introduced, was about *our* throughput: a guest polling the clock in a loop
    should finish in a sane number of iterations. It was paid for in fidelity.
    pspautotests' `checkpoint()` reads the clock once, so a checkpoint cost
    100us, and a test that delays 200us and prints two lines had its delay
    expire inside the printing.

    The throughput it protected turns out not to need protecting. With reads
    charging one microsecond the game's output is **byte-identical** -- same 633
    GE lists, same 93,354,668 pixels -- so nothing it does polls the clock long
    enough to notice, and no test started timing out. So the separate read tick
    is gone rather than retuned.

    `threads/terminate`, `threads/threadmanidlist` and `msgpipe/create` went to
    0; `alarm/set` 22 to 12, `fpl/create` 8 to 4, `tls/create` 60 to 56,
    `msgpipe/tryreceive` and `trysend` 4 to 2 each. Suite 1,919 to 1,890, 45
    matching to 48.

    The lesson is about the shape of the mistake rather than the constant. Two
    questions -- "how long does a kernel call take" and "how long does reading
    the clock take" -- had different answers only for as long as one of them
    was unmeasured, and the wrong one was defended by an argument about
    performance that nobody had checked. The same file already records the
    identical correction being made to the *other* constant, from 100 to 1, for
    the same reason. It was made once and not carried across.
