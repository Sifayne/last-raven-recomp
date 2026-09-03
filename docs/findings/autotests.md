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

    ### `events/poll`, and a check order that was written down backwards

    74 differing lines to 10. `sceKernelPollEventFlag` was not registered, so
    it answered OK, wrote nothing and cleared nothing -- all 37 lines wrong.

    It is the wait's immediate path, so writing it surfaced two rules the wait
    had only half of: **`PSP_EVENT_WAITCLEARALL` (0x10)** clears the whole
    pattern where `WAITCLEAR` clears only the bits waited for (the test shows
    the pair on consecutive lines from the same starting pattern --
    `cur=FFFFFFFE` against `cur=00000000`), and both clear modes at once is
    `ILLEGAL_MODE`. And **a poll counts as a waiter** for the single-waiter
    rule even though it never parks.

    The poll's own rule: a poll that finds the pattern absent still writes the
    *current* pattern to the out word, where an argument error leaves the
    caller's `0xDEADBEEF` alone. "Not yet" is an answer, not a refusal.

    Then the part worth recording as method. `hle_WaitEventFlag` carried a
    comment asserting the check order, citing the capture -- and the order in
    the comment was **backwards**. Both files show `Wrong (0x04) 0x00000000`
    answering `ILLEGAL_MODE`, not `EVF_ILPAT`, so the mode is checked before
    the pattern. The comment got the "arguments before the handle" half right
    and the relative order of the two argument checks wrong, and it had been
    read as settled ever since. Correcting it moved `events/wait` 32 to 24 and
    `events/events` 18 to 6 for free -- two tests that were never looked at.

    A comment citing evidence is worth more than one that doesn't, and it is
    also more dangerous: it stops the next reader from re-deriving the thing.

    ### The scheduling harness, and a census worth repeating

    Eight of the test groups share a harness that starts a worker, has it spin
    on a wait that can only end when main deletes the object under it, and
    prints a six-letter trace of which thread was last active at each step.
    Hardware's is `A1B1C2E1D1F2`; ours was `A1B1D2C2E1F1`. A different letter
    *order* is the harness saying the two threads interleaved differently, and
    it says so in every test that uses it.

    The cause was one unregistered call. **`sceKernelWaitEventFlagCB` did not
    exist**, and an unimplemented call returns zero -- which is exactly what a
    *successful* wait returns. So the worker's loop ended on its first
    iteration and the worker was never waiting while the rest of the test ran.

    That prompted a census rather than another single fix: list every
    `sceKernel*CB` the suite calls, diff against what we register. Two were
    absent entirely and six more were registered straight to their non-CB
    handlers -- they waited correctly and delivered nothing, the suffix being
    the whole difference between the two calls. Worth repeating for other
    suffixes.

    One more rule came out of the same trace: **a wait released by a delete
    reports a pattern of zero**, because there is no longer a flag to have one.
    The harness reads that word after deleting the flag under its waiter.

    `events/clear`, `events/set`, `events/poll`, `events/delete` and
    `events/events` all went to 0; `wait` 24 to 2, `create` 12 to 2, `refer` 8
    to 2, `cancel` 36 to 30. Suite 1,806 to 1,719, 48 matching to 53.

    Two tests got noisier and both are the fix exposing new behaviour rather
    than breaking old: `vpl/allocate` 9 to 14, whose own harness expects `E2`
    where the events one expects `E1` -- so the worker is supposed to run at a
    point ours does not -- and `callbacks/callbacks` 4 to 6, where deliveries
    now happen often enough that each notify arrives on its own and the
    accumulated count never reaches 2. Both are about *when* delivery happens,
    which is the next question in this area.

    ### When a callback is delivered: one moment, found by elimination

    Every CB wait delivered on the way *in*, unconditionally. The real point is
    neither end of the call, and two tests fix it between them.

    `callbacks/callbacks` notifies a callback and calls `sceKernelLockMutexCB`
    three times, printing four consecutive lines:

    ```
    Lock 0 => 5: 800201BD                    illegal count
    cbHandler called: 00000002, ...          <- two arrive at once
    Lock 0 => 1: 00000000                    succeeded
    Lock 1 => 1: 800201C8                    already held
    ```

    The count of **two** proves that a call refused on its arguments delivers
    nothing: the first call's notify was still pending. So delivery is *after*
    validation.

    Moving it to the way out, on success, is wrong twice over.
    `sceKernelWaitThreadEndCB` returns the awaited thread's **exit status** --
    an arbitrary number, 5 here -- so "succeeded" cannot be read from the
    return value at all; and a CB wait that *times out* delivers. The question
    is whether the call reached its wait, not how it ended.

    And it is not the way out either: `threads/threadend` tags the handler's
    line `[x]` and the wait's result `[r]`, so a reschedule falls between them
    and the handler ran *before* the wait blocked.

    That leaves one moment -- past the argument checks, not yet blocked -- and
    every waiting call passes through it exactly once, in `psp_wait_deadline`.
    Worth noting that the `[x]`/`[r]` column, which this file has treated as
    noise throughout, is what settled the last step.

    One correction fell out: `sceKernelLockMutex` took its deadline *before*
    refusing a non-recursive relock, so that refusal counted as reaching the
    wait and delivered. The check moved up with the other argument checks --
    an ordering that read as arbitrary until the delivery point landed between
    the two positions.

    `callbacks/callbacks` matches hardware; `vpl/allocate` 14 to 4, against 9
    before this area was touched at all. Suite 1,719 to 1,703, 54 matching.

    ### A deadline that has already arrived is not a wait

    `vpl/allocate` asks for a block that is not there with a timeout of **0**
    and tags the line `[x]`: no reschedule happened. Ours tagged it `[r]`,
    because a zero timeout was being turned into "the earliest deadline that
    exists" and then parked on -- which hands the CPU away and takes it back,
    and that round trip is what the tag reports.

    There is no interval in which anything could change, so there is nothing to
    schedule around. `psp_sched_block_until` now answers EXPIRED without
    switching away when the deadline is already behind it.

    The rule was written down as a deliberate choice when zero timeouts were
    first handled -- "the shortest real one there is ... so that it expires
    through the ordinary path" -- which is right about the *result* and wrong
    about the route, and the difference is only visible in a column that says
    whether a switch happened. `msgpipe/receive` and `msgpipe/send` came with
    it, 8 to 6 each.

    What is left in `vpl/allocate` is a single line and it is calibration, not
    a rule: the harness's worker retries on a **5us** timeout, and hardware's
    `sceKernelAllocateVpl` takes long enough for one of those to expire inside
    it where our one-microsecond firmware call does not. The 5us is chosen by
    the test to sit on that boundary. Nothing in the captures says what a vpl
    allocation costs, so tuning to it would be fitting the constant to the
    test.

    ### A threshold that is in the data and is not a rule

    `msgpipe/receive` sweeps a wait's timeout one microsecond at a time against
    an empty pipe, and the `[x]`/`[r]` column changes at a single place:

    ```
    [x] 0us   [x] 1us   [x] 2us   [r] 3us   [r] 4us   [r] 5us  ...
    ```

    That reads exactly like a kernel deciding a handoff costs more than a wait
    of two microseconds is worth. It is the cleanest-looking measurement in the
    suite, it made `msgpipe/receive` and `msgpipe/send` match, and it is wrong.

    Refusing to park below the threshold took **forty tests to no output at
    all**. pspautotests' own workers spin on 1us waits at a priority above the
    main thread; a wait that never parks never yields, so the thread that was
    going to release them never runs again. The `[x]` at 1us therefore does not
    mean "did not park" -- it means "parked, and nothing else managed to use
    the microsecond". Which of those it is remains open.

    Two things worth keeping from it beyond the finding. The sweep reports a
    test with no output as zero differing lines, so a naive count of
    zero-difference rows called that run **68 matching** -- the best number of
    the session, produced by breaking a third of the suite. The verdict column
    exists for exactly this and is the thing to count. And this is the second
    time in this file that an empty output has read as success; the first is
    recorded near the top as a trap, which did not stop it happening again.

    ### `mbx/receive`: a line of code that was right for the wrong reason

    A received message keeps the `next` it had. `mbx_pop` overwrote it with the
    packet's own address, under a comment quoting the capture:

    ```c
    /* The message leaves pointing at itself, which is what a receiver sees:
     * `GOT: "hi 0" (next=ITSELF)`. */
    psp_write32(head + MSG_NEXT, head);
    ```

    The quote is real and the conclusion does not follow. `hi 0` is the
    *Single standard* case -- one message in the box -- and a ring of one
    already points at itself, so the write was invisible there. The test runs
    three cases in a row and only the middle one separates them:

    ```
    Single standard:      next=ITSELF     ring of one
    Multiple standard #1: next=FIRST      the other message, now first
    Multiple standard #2: next=ITSELF     ring of one again
    ```

    Deleting the line is the whole fix. `mbx/receive` and `mbx/poll` both match
    hardware, and `mbx/priority` came down 40 to 30. Suite 1,697 to 1,647, 56
    matching.

    That is now the third comment in this file found citing evidence for a
    claim the evidence does not support -- after the event flag's check order
    and the zero-timeout route. All three were right about *something*, which
    is what made them look settled. The pattern to watch for is a comment whose
    example has only one case in it.

    ### A mailbox holds its *last* message, not its first

    `mbx/send` is the nastiest test in this directory and the only one that can
    show this: it sends messages, then reaches into the guest-owned packet
    header of the one it just sent and rewrites `next` behind the kernel's
    back, then reads the mailbox status. Two of its cases are decisive:

    ```
    next = itself   ->  the walk starts at that message
    next = NULL     ->  first=NULL, with count still 2
    ```

    Neither is reachable if the kernel keeps a head pointer, because the guest
    cannot touch a head pointer. Both fall out of `first = last->next`: the
    mailbox stores its **last** message and derives the head. Our head-pointer
    version could not be made to produce either.

    The restructure cost two bugs of my own, both worth naming because they are
    the same bug twice. Inserting in front of the head and appending after the
    last write the *same two links* -- in a ring, `last -> msg -> head` is both
    -- so the links cannot say which happened and only the search that produced
    them can. And the FIFO append read the head *after* overwriting the link
    the head is derived from, which is a hazard the head-pointer version simply
    did not have. Deriving a value instead of storing it moves where the
    ordering constraints live.

    `mbx/receive`, `poll`, `cancel` and `delete` match hardware; `refer` 10 to
    4, `send` 29 to 24, `priority` 30. Suite 1,647 to 1,636.

    ### The rest of `mbx/send`: what the kernel checks and what it does not

    24 differing lines to 0, in four rules, and the interesting one is the case
    the kernel *does not* mind.

    The test breaks the ring three ways from the guest side and receives from
    it. Two are refused, with different codes:

    ```
    next = NULL                     no head to take            800200D3
    two messages, head is the last  taking it would empty      800201C9
    ```

    The third -- a two-node ring whose head is not the last, built by pointing
    a queued message at a packet that was never sent -- is served without
    complaint, and that is what pins the rest of the model. It returns the
    never-sent packet, drops the count to zero, and *still reports a non-null
    first*: `count=0, first=OTHER`, with a walkable message behind it. So the
    head is `last->next` unconditionally, and the box is empty when the message
    leaving **is** the last one -- which is not the same as the count reaching
    zero, and only a tampered ring can tell them apart.

    The fourth rule is `Send twice`: a packet belongs to one queue at a time,
    and sending the same one again is refused with the count left at 1.

    Six of the eight `mbx` tests now match hardware. Suite 1,636 to 1,612, 57
    matching.

    ### `mbx/priority`: "never joined the queue" is observable

    30 differing lines to 0, by deleting one write.

    A message sent while a receiver is already waiting goes straight across
    without joining the queue. Ours said so in a comment and then wrote a
    ring-of-one into the packet anyway -- harmless-looking, since the message
    is being handed over rather than stored.

    It is not harmless, because the packet is the *guest's* memory. The test
    poisons every packet with `0xDEADBEEF` before sending and reads that value
    back out of the delivered message:

    ```
    GOT: "hi 2" (next=DEAD, prio=15)
    ```

    `DEAD` is the poison surviving. A message that had been queued carries
    `ITSELF`, and the difference between the two is the whole question of
    whether it was ever in the queue. Seven of the eight `mbx` tests now match
    hardware; suite 1,612 to 1,582, 58 matching.

    Three of this directory's fixes have now been the removal of a write to
    guest memory that we had no business making -- the received packet's
    `next`, the delivered packet's `next`, and the mailbox's head. Structures
    the guest owns are observable in a way internal ones are not, and every
    field written into them is a claim being made about hardware.

    ### `mbx/refer`, and the last two lines of the directory

    Two rules, one of them already written down elsewhere in the same file.

    **A `Refer*Status` offering zero bytes writes nothing.** The mutex, thread,
    alarm and pool versions all follow it -- it is recorded above as one of the
    corrections that fell out of implementing `mutex` -- and the mailbox's was
    written before that and never revisited. `Size 00000000 => 00000000`
    against `=> 00000034` for every other value the sweep tries, including -1.

    **A send is refused by a ring the guest has already broken.** Where a
    *receive* from such a mailbox has two distinguishable failures, a send has
    exactly one observable effect: the test breaks the ring, sends another
    message, and reads the count back unchanged.

    All eight `mbx` tests now match hardware, from 4, 2, 8, 24, 16, 10, 29 and
    40 differing lines when this directory was first opened. Suite 1,582 to
    1,578, 59 matching.

    ### The `Refer` zero-size census

    Not a test but a sweep across an established rule: list every
    `sceKernelRefer*Status` we implement, check each for "a caller offering
    zero bytes gets zero back and nothing written". Thirteen calls, nine had
    it.

    Three did not -- sema, event flag, callback -- and each is pinned by its
    own test printing `00000000 => 00000000` against `=> 00000034` or
    `=> 00000038` for every other size. All three predate the commit that first
    wrote the rule down, which is exactly why: it was established while
    implementing `mutex` and applied forward, never backward.

    The fourth apparent gap was a fault in the census itself.
    `sceKernelReferLwMutexStatus` looks unguarded because the guard lives in
    `lw_refer`, the helper it and `ReferLwMutexStatusByID` both delegate to.
    Pattern-matching a function body does not see through delegation -- worth
    remembering the next time one of these sweeps looks conclusive.

    A fourth call had a different bug the census surfaced anyway:
    `ReferCallbackStatus` answered the generic `UNKNOWN_UID` where a callback
    has its own id error, the same one notify and cancel already used.

    `semaphores/refer`, `events/refer` and `callbacks/refer` all match
    hardware. Suite 1,578 to 1,566, 62 matching.

    The method is worth repeating. Every rule in this file that was established
    for one object type and applied forward is a candidate: the types written
    before it will lack it, and their tests will say so without anyone having
    opened them.

    ### The guest-owned structure writes: a census that found nothing

    Recorded because a clean result is worth as much as a dirty one, and
    because the next person to notice the pattern should not have to redo it.

    Three of the `mbx` fixes above were the *removal* of a write into memory
    the guest owns -- the received packet's `next`, the delivered packet's
    `next`, and the mailbox's head. That is a class, so it deserves a sweep:
    every `psp_write*` in the HLE that is not to a documented out-parameter,
    which is thirty-nine sites in four groups.

    | group | sites | pinned by |
    |---|---|---|
    | thread stack k0 area | `threadman.c:418-421` | `threads/start`, which reads `stack[0]` and `stackEnd[-1,-2,-14,-16]` by hand |
    | lwmutex workarea | `kernlock.c:467-630` | all 8 `lwmutex` tests, which dump it raw including the pad words and `memcmp` the two Refer calls against each other |
    | mbx ring links | `kernobj.c:1109-1154` | 8 of 9 `mbx` tests, which poison every field with `0xDEADBEEF` first |
    | vpl in-pool accounting | `kernobj.c:190-273` | `vpl/order`, which walks the kernel's own structures printing every node's address, `next` and size |

    Every group is covered by a test that reads the memory back, and every one
    of those tests matches hardware. There is nothing to fix.

    Two limits on what that means, so the result is not over-read. The census
    finds writes we *make* and cannot find writes we *fail* to make -- the
    mailbox's derived head was the second kind, and only a tampering test could
    expose it. And it says nothing about the values, only that something checks
    them; a field written wrongly in a way no test reads is invisible to both.

    ### `tls`: a wait that was never entered, and an owner that outlived nothing

    Two rules, and the second is the one that makes thread-local storage
    thread-local.

    **`sceKernelGetTlsAddr` waits.** A pool with nothing free answered NULL.
    `tls/priority` makes a pool of *one* block, takes it from the main thread,
    starts three threads that each ask for one, and reads the pool back:
    `totalBlocks=00000001, freeBlocks=00000000, wait=2`. Two of the three are
    parked inside the call. The struct already had a waiter queue and
    `sceKernelFreeTlspl` already released it; nothing had ever joined.

    **A block goes back to the pool when its thread ends.** The same test
    measures it without ever freeing successfully: its workers take a block,
    delay, and then free it with `sceKernelFreeFpl` -- the wrong call for the
    type, which fails with `8002019d` every time, on hardware and here. The
    block still reaches the next waiter, so what returned it was the worker
    exiting.

    That second one is the sort of rule a test can only show by getting
    something else wrong. A test that freed correctly would have proved
    nothing.

    `tls/priority` 28 differing lines to 12, `delete` 20 to 12, `free` 21 to
    16. Suite 1,566 to 1,537.

    All six lines left in `tls/priority` are the same line: the pool's base
    address, `09d35700` on hardware against `09d00000` here. Every ordering,
    release and count matches. That residue is the memory-map difference that
    also holds `threads/create` at 254, and it is the only thing between this
    test and hardware.

    ### The HLE is psprecomp's Phase 4, and it did not build on Windows

    Not a test finding. A census of a different kind, prompted by asking where
    this work belongs.

    `psprecomp/ROADMAP.md` **Phase 4 — the HLE library** lists threads,
    semaphores, event flags, mutexes, callbacks, memory partitions, timers,
    `sceIo`, `sceCtrl`, `sceDisplay`+`sceGe`, `sceAudio`/`sceSas` and import
    resolution. Every box unchecked. `ARCHITECTURE.md` lists the HLE alongside
    the CPU and the GE as a first-class component, and the README calls runtime
    libraries part of the product. Measured:

    ```
    upstream src/hle:   2,350 lines,  8 files
    ours:              12,237 lines, 18 files
    created by the series: clock, ctrl_replay, kernlock, kernobj,
                           ktimer, mpeg, sched, umd, waitq
    ```

    So the HLE belongs exactly where it is, and this work *is* that phase. It
    was already integrated on the project's own terms -- our files sit in its
    `CMakeLists.txt` source list, three suites were added to its `tests/` under
    that directory's no-game-data rule, and the licence posture is the one its
    README picks a fight over.

    One thing did not fit, and it was ours:

    ```
    upstream psprecomp uses pthreads:  nowhere
    sched.c:  86 sites   clock.c: 2 sites   render.c: 1 site
    ```

    The README says "MSVC on Windows; gcc/clang elsewhere". We introduced an
    entire POSIX threading dependency into a codebase that had none, so the
    runtime did not build on a platform the project claims. `find_package
    (Threads)` does not save it: that links a library, it does not make
    `pthread_mutex_t` a type.

    Fixed by `include/psprecomp/os.h` + `src/os.c` -- five primitives, two
    backends. Two details worth keeping:

    - **A POSIX condition variable's timed wait is on `CLOCK_REALTIME` by
      default**, so a deadline computed from a monotonic clock expires at an
      unrelated moment, or never. The attribute has to be set explicitly, which
      is why a statically-initialised condvar needs an init call after all.
    - **The Windows monotonic clock overflows the obvious arithmetic.**
      `counter * 1e9 / frequency` exceeds 64 bits within seconds of uptime at a
      10 MHz counter, so it is computed as seconds plus remainder.

    The Windows half is **written and not run** -- there is no MSVC or mingw
    here. `mingw-w64-gcc` would compile-check it. The POSIX half is verified the
    usual way: the threads suite is byte-identical across the change, 62
    matching and 1,537 differing before and after.

    ### Provenance: sourcing the facts outside PPSSPP

    Twelve comments in six files cited PPSSPP. psprecomp's README makes the hard
    MIT boundary the thing that distinguishes it from the other PSP recomp
    project, so a Phase 4 contribution carrying those citations puts a
    maintainer in the position of having to adjudicate them. Reading all twelve,
    they were two different things:

    **Six were corroboration.** We derived the rule from our own evidence and
    noted that PPSSPP agrees. `iofilemgr.c` spends a paragraph on the bug we
    found -- a read of 64 bytes at offset 58144 where the game asked for 64
    *sectors* -- before mentioning them. `sysmem.c` cites them **to disagree**:
    the circulating name for a NID hashes to `0xC28A2329`, not the NID, so it
    cannot be the exported symbol. `threadman.c` we had re-confirmed this
    session from `threads/threadend`. Nothing was taken; the comments just read
    as though it had been. Reworded to lead with the evidence.

    **Four were genuine lookups**, and three of those resolve to a better source
    than the one they had:

    - **GE command numbers** -- PSPSDK's `src/gu/guInternal.h`, BSD-licensed, and
      the SDK that *emits* them, so it is the definition rather than a reading
      of one. Values match exactly; only the names differ (`TEX_ADDR0`,
      `CLUT_BUF_PTR`).
    - **The block-transfer commands** -- same header, `TRANSFER_SRC` 0xb2
      through `TRANSFER_SIZE` 0xee, with the field layout being what
      `sceGuCopyImage` writes into them.
    - **The volatile memory region** -- not a fact to look up at all. The call
      reports address and size through out-parameters and the guest uses what it
      is handed, so the placement is ours to choose; what constrains it is that
      it be mapped and sit below the user heap, which uofw documents as starting
      at 0x08800000.
    - **sceMpeg's constants** -- the one with no better source, and the honest
      answer is that sceMpeg has no published specification. What validates them
      here is end-to-end rather than by citation: the game queries a size,
      allocates it, and the movie plays. A wrong value does not misbehave
      subtly; the allocation is the wrong size and playback never starts.

    One reference survives, in `interp.h`, and it is the *correct* kind: it
    names PPSSPP as the tier-2 external oracle, "separate processes -- never
    linked". That is what `docs/ORACLE.md` prescribes and what the README
    advertises as the legitimate use.

    Worth keeping as a distinction. "Where did this number come from" and "what
    makes it right" are different questions, and a comment that answers only the
    first has borrowed someone's homework even when nothing was copied.

     ### `tls/get`: four rules about a pool, and one about uids

     48 differing lines to **6**, and the directory 144 to 74. Four of the
     rules are cheap once seen, and each is a thing the obvious implementation
     gets wrong in a way that looks reasonable:

     - **The index a pool reports is a slot, not a count of the living.** The
       test makes two pools, deletes the first and makes a third; hardware
       gives the third index 0. Counting survivors answers 1.
     - **Blocks are handed out round-robin.** Take and free from a pool of
       three, four times over, and the offsets are +0000, +0010, +0020, +0000.
       First-free answers +0000 every time — and is indistinguishable from
       correct until the pool has more than one block *and* something is freed
       between the takes.
     - **The option struct's alignment is a block stride, not a pool
       placement.** One-byte blocks with alignment 0x100, 1 and 0 are spaced
       0x100, 4 and 4 apart: it rounds each block up, anything below four is
       four, and a pool created with no options behaves as though it asked for
       four. The reported `blockSize` stays what was asked for, so the stride
       is not observable through `Refer` — only through the addresses.
     - **A block is zeroed when it is handed out.** The evidence is the pair,
       not the single line: scribble 0xCC, free, take again and the read is
       zero; scribble again and take *without* freeing and it is still 0xCC.
       The second take is the already-ours early return, which is what places
       the clearing in the allocation branch rather than at the top of the
       call.

     ### The six lines left in `tls/get`, and what they would cost

     `sceKernelGetTlsAddr` does not validate its uid. It indexes an object
     table, and a PSP uid encodes that index as `uid >> 3`. Every case fits:
     with two pools alive at slots 0 and 1, uid 0 and uid 1 both answer the
     first pool's base, 0xF answers the second's, and 0x10, 0x17, 0x18, -1 and
     0xDEADBEEF all fail. `ReferTlsplStatus` on the same uids answers
     `800201D0`, so the laxity is this one call's, not the object table's.

     Not implemented, and deliberately. Our uids come from a global counter
     with no table index in them, so reproducing this means either a fallback
     that tries `index == uid >> 3` when the exact match fails — which fits
     every line of this test and is not the mechanism, since it would be dead
     code the moment uids did encode an index — or giving every kernel object
     a uid derived from its slot in a shared table. The second is the real
     fix, it is a change to every object type at once, and it is worth doing
     for a better reason than six lines of one test.

     ### `tls/create`: the same argument, checked against a different table

     40 differing lines to **0**. The directory is 144 to 44, and two of its
     six now match.

     - **Partitions 8 and 9 are `ILLEGAL_PARTITION` to a tlspl and
       `ILLEGAL_PERM` to a vpl or an fpl.** All three take the same argument in
       the same position and `vpl/create.expected` and `fpl/create.expected`
       disagree with `tls/create.expected` about two of its values, so the
       shared helper was wrong for one of its callers. Only the 1..7 window is
       common. Partition 5 is the value no tlspl test covers, and it keeps
       vpl's answer for want of anything better.
     - **The option alignment must be a power of two**, refused with that same
       partition code — which is what an fpl already does with the same field,
       and remains the least guessable thing about either.
     - **What bounds a pool is memory.** 0x1000 blocks of 0x100 bytes is a
       megabyte and succeeds; the 64-entry owner table that made it fail was
       ours. The two refusals either side of it are a distinction worth
       keeping: 0x10000 blocks does not fit in *memory* and answers NO_MEMORY,
       0x1000000 blocks is exactly 2^32 bytes and does not fit in a *word*, so
       it answers ILLEGAL_MEMSIZE. Checking the overflow and letting the
       allocator answer for the rest gets both without a table of sizes.
     - **Sixteen pools, and the seventeenth answers `0x800201D1`.** Observable
       twice, since the index a pool reports is its slot: the loop fails at 16
       and the last success reports index 15.

     PSPSDK's `pspkerror.h` does not name `0x800201D1` — its TLS entries stop
     at the kernel-side trio, `ILLEGAL_KTLSID`, `KTLS_FULL` and `KTLS_BUSY` at
     `0x800201C0..C2`. The user-side family sits one row down in the same
     shape, which is corroboration for calling this one `TLSPL_FULL`; what
     actually pins it is the seventeenth create.

     ### `tls/free` and `tls/delete`: ownership is checked in one place only

     Free does not care who holds the block. It is called twice in a row and
     answers OK both times, and from a thread that never asked while another
     thread holds the pool's only block — OK again, with `freeBlocks` still
     zero, so it did not take anyone else's. The uid is validated; ownership is
     not.

     Delete does care, and about a different thing than the obvious one. A
     one-block pool with two threads *queued behind it* deletes cleanly; a
     two- and a three-block pool whose spare blocks went to threads that never
     returned them both refuse with `0x800201D2`. So the count is of blocks
     lent out, not of waiters, and the caller's own block does not count
     towards it. Same row as `TLSPL_FULL` and the same shape as the
     kernel-side `KTLS_BUSY`.

     And a block is wiped at **both** ends of its life. `tls/free` writes 0xCC
     over its block, frees it, and reads zero back without asking for it
     again — so the free did that. `tls/get` dirties a block *after* a free
     and reads zero after the next hand-out — so the allocation does it too.
     Either rule alone explains one of the two tests and gets the other wrong.

     ### What is left in `tls`, and it is mostly one thing

     144 differing lines to **34**, two of six matching. Of the 34:

     - **~24 are one number** -- every "got result" line reports a block
       address, and hardware's pool sits at 0x09d357xx-0x09d35axx where ours
       sat at 0x09d00000. That turned out to be its own finding, below.
     - **6 are `tls/get`'s uid family** — recorded above, and deliberately not
       implemented.
     - **2 are a sub-microsecond ordering tie in `tls/delete`.** A worker whose
       1000µs hold expires and a worker just started both become runnable at
       the same priority within about a microsecond of each other, and
       hardware runs the older one first. Left alone on purpose: the last time
       a rule was fitted to a difference this small — the 2µs park threshold in
       `msgpipe/receive` — it matched one test and took forty others to
       NOOUTPUT.
     - **2 are one `[x]` where hardware says `[r]`**, on a thread that sleeps
       and is woken. Not investigated.

     ### A module outside the partition still occupies it

     Filed as the memory-map problem, which it was not. The allocator's own
     comment said a PRX linked at address 0 "lands outside the partition
     entirely, so there is nothing to step around" and handed out user RAM
     from 0x08800000. True of us; not true of the machine being modelled,
     whose loader put the module in the partition before the test ran.

     What made it measurable is that the difference is not a constant. Three
     tls tests print a block address, and the address is the guest's heap end,
     and the heap starts where the module stops:

     | test | module image | implied heap start | address printed |
     |---|---|---|---|
     | `delete.prx` | 0x31700 | 0x08835700 | 0x09d35700 |
     | `priority.prx` | 0x31700 | 0x08835700 | 0x09d35700 |
     | `free.prx` | 0x319C0 | 0x08835A00 | 0x09d35A00 |

     Three binaries, three different sizes, and the gap between the image and
     the heap is **0x4000 in all three** -- the loader's own bookkeeping. A
     single constant would have fitted one of them and been wrong about the
     other two, which is what makes this a rule rather than a fudge.

     Measured over the whole suite, both directions: **99 to 100 matching,
     72,288 to 71,886 differing lines, and not one test worse.** Five moved --
     `tls/priority` to matching, `tls/delete` and `tls/free` 8 to 2, and
     `video/pmf` and `video/pmf_simple` by 8 and 370 lines, which were not
     predicted and are the reason a global change gets a global measurement.

     The reservation has to stay honest about its size. The floor here used to
     be a flat megabyte and that megabyte broke `gum.prx`, which asks for a
     single 0x01500000 block against a 0x01400000 heap, gets NULL, does not
     check it, and formats into its own code at address zero.

     ### The ten-line band: sixteen tests, eight findings

     `threads` 64 of 127 to **80**, the whole suite 100 to **116 of 432**, no
     test worse anywhere. Ranked by lines, these tests were all within ten of
     matching, and what they had in common was that each was one rule rather
     than one subsystem.

     Two of the eight paid for themselves several times over:

     - **Caps of our own, read as hardware's.** Semaphores, event flags,
       callbacks, mutexes, mailboxes and vtimers were capped between 32 and
       128, and every `Create 1024` case in the suite reported `Failed at 128`
       against them. Five tests matched on the constant alone. The headroom
       above 1024 matters: at exactly 1024 callbacks stopped at 1023, because
       the process already held one.
     - **A deleted object still writes back its timeout.** Every wait reported
       how much of its timeout was left except the path where the object
       vanished underneath it — the early return there reads as a clean
       bail-out, but the timeout word belongs to the caller, not the object.
       Eight sites, one shape, four tests matched.

     The other six are each their own rule:

     - `RotateThreadReadyQueue` refuses a priority a user thread could not
       hold, and zero — meaning "my own level" — is always allowed.
     - An event-flag wait with a **zero** timeout reports no pattern at all,
       where one that ran out reports the pattern it did not get. The test
       seeds the word with 0xDEADBEEF and reads it back untouched from the
       zero case and 00000000 from the 5ms case on the next line.
     - A callback handler may be null and may be nonsense but may not be
       negative: 0x07ADBEEF is accepted and 0xDEADBEEF is not, and the only
       thing between them is the top bit.
     - A callback does not outlive the thread that created it.
     - A null vtimer uid is `ILLEGAL_VTID`, which PSPSDK names at 0x800201BF —
       and which of that and `UNKNOWN_VTID` a call answers is **per call**.
       Start, stop, sethandler and cancelhandler take the first; delete,
       gettime, getbase, refer and settime take the second. The tests disagree
       on purpose, so only the two measured here were changed.
     - Signalling a semaphore past its maximum is refused, not clamped, and
       nothing moves.

     And one that had to be measured twice. `vtimers/stop` reads a base of 0
     after three start/stop cycles, which looked like "the base is not set by
     starting". Making that change matched the test **and cost `sethandler`
     four lines and `cancelhandler` two** — because `sethandler` prints
     `base=0` beside every `active=0` and a real reading beside every
     `active=1`. The base is set by the start and cleared by the *stop*. Both
     versions matched the test that prompted the change; only one was right,
     and the directory-wide run is what said which.

     `semaphores/poll` was the other one worth writing down, because its three
     answers come in an order nothing would suggest. An empty semaphore says
     `SEMA_ZERO` whatever it was asked for; then the count is settled, before
     the uid is even a question, so `PollSema(NULL, 0)` is `ILLEGAL_COUNT`
     where `PollSema(NULL, 1)` is `UNKNOWN_SEMID`. The least certain part is
     that a semaphore at zero *with a waiter* answers `ILLEGAL_COUNT` and not
     `SEMA_ZERO` — one observation supports it and nothing contradicts it, and
     the comment in the code says so.

     ### The content half of the band: four more, and a name that hid a bug

     116 to **118 of 432**, `threads` 80 to **82 of 127**, nothing worse.

     - **A vtimer handler is handed two clock *pointers*, not one clock.** The
       signature takes `SceKernelSysClock *` for both `elapsedScheduled` and
       `elapsedReal`; passing the schedule's low and high words in a1/a2 put a
       small integer where an address belonged. `vtimers/vtimer` dereferences
       the second argument and prints what it finds, which is how a wrong
       calling convention showed up as a garbage number rather than a crash.
       `sethandler` dropped eight lines with it.
     - **A vtimer cannot be deleted from inside a handler**, and the code says
       why: `ILLEGAL_CONTEXT`, which is about where the call was made from
       rather than what it was made on.
     - **`fpl`'s option alignment spaces its blocks out**, exactly as a
       tlspl's does. This one is worth the warning: `fpl/tryallocate` measures
       16 bytes between blocks in three sections and 32 in the fourth, all
       with the same `blockSize=0x10`. Reading only the failing lines makes it
       look like a property of the block size. The fourth section is the one
       that passes `opt.alignment = 32`.

     And a bug that existed only because of a name. Adding
     `SCE_KERNEL_ERROR_ILLEGAL_CONTEXT` at 0x80020064 -- PSPSDK's value --
     produced a *second* definition of that name in the header, because
     0x80020066 was already carrying it. The later definition quietly won, and
     the vtimer's refused delete answered the dispatch code. 0x80020066 is
     `CPUDI`; it is now called that, and its two existing callers say so.
     Nothing warned: two `#define`s of one name are only an error if the
     bodies differ textually, and these were both plain integers.

     ### Ordering rules, and a cap that hid as an allocation failure

     118 to **120 of 432**, `threads` 82 to **84 of 127**, nothing worse.

     - **`fpl` and `vpl` never got the cap the other types got.** It read as
       `Failed at 0` rather than `Failed at 64`, because the earlier sections
       of `fpl/create` leave their pools alive — so the loop had no slot to
       start from, and the failure looked like memory rather than
       bookkeeping. The same miscounting made the alignment sweep's 4096 case
       report `NO_MEMORY` for a 64K pool.
     - **An unmapped pointer is refused before `sceKernelFreeVpl` looks at the
       uid.** A null uid with a good pointer is `UNKNOWN_VPLID`; a null uid
       with 0xDEADBEEF is 0x800200D3. The argument the kernel objects to first
       is not the first argument.
     - **A waiter absorbs a signal that would otherwise overflow.**
       `semaphores/signal` refuses +2 on an idle 0/1 semaphore and allows the
       same +2 on one with a thread queued for 1, so the overflow test has to
       look past the queue before it refuses.

     Two things found and deliberately not acted on:

     **`vpl/create`'s pool arithmetic** — and this one was wrong when first
     written here. `poolSize` is the requested size rounded up to 8, less 0x20
     of overhead, which is exact from 0x31 upward. The five smaller sizes all
     report 0x0FE0, and reading that as "a pool below a floor becomes a whole
     0x1000 page" is modelling test noise: `schedfVpl` refers into an
     **uninitialised stack struct** and prints what the previous iteration
     left in it. The comment above `vpl_pool_size` in kernobj.c had already
     established this. Those five lines cannot be matched and should not be.

     **The error names in `hle.h` do not all belong to their numbers.**
     0x800200D2 is `ILLEGAL_ARGUMENT` and not `ILLEGAL_PARTITION` (0x800200D6);
     0x800200D3 is `ILLEGAL_ADDR` and not `ILLEGAL_SIZE` (0x800201BC, which is
     in the header under the invented name `ILLEGAL_SIZE_MPP`). This is the
     same trap that produced the `ILLEGAL_CONTEXT` bug above, still loaded: a
     future correct constant would collide with a wrong name and lose
     silently. Left as a separate audit because it is a rename across 34 call
     sites and wants doing against the whole published list at once, not
     three entries at a time.

     ### Finishing the band: a register nothing pointed at, and a byte

     120 to **125 of 432**, `threads` 84 to **86 of 127**, nothing worse.

     - **A thread starts with `$k0` pointing at its control block.** The block
       was already being written into the top 0x100 bytes of every thread
       stack — thread id at +0xC0, stack address at +0xC8, 0xFFFFFFFF at +0xF8
       and +0xFC — correctly, and *nothing pointed at it*. `threads/k0/k0`
       takes `$k0` straight out of the register and walks the structure. It is
       initial register state like `$sp`, so it belongs with the spawn, which
       is why `psp_sched_spawn` grew an argument.
     - **`sceKernelGetThreadId` answers nothing inside a handler.** ktimer.c's
       header had said a handler runs on no thread since it was written; the
       id call did not know.

     And then a byte. **Ten of the 435 `.expected` files end without a final
     newline that the guest did print** — `threads/k0/k0`'s last statement is a
     `printf` ending in `\n`. That is a property of the recording, not of the
     PSP, and it is exactly the artifact the CRLF strip already in
     `07-autotests.sh` exists for. Normalising it matched **five** tests
     outright: `cpu/icache`, `display/display`, `loader/bss`, `threads/k0` and
     `threads/mutex/mutex`, and took two lines off `intr/intr`.

     This is a loosening of the oracle and worth being plain about. It is
     narrow — a missing trailing newline is added, empty files are left empty
     so the framebuffer tests still read as silence — and it had to go into
     **both** scripts, because a sweep and a per-directory run that disagree
     about the same test are worse than either verdict alone. That mistake was
     made first: `07-autotests.sh` was fixed, the sweep still said 10 lines,
     and the two answers stood side by side for one run.

     Two tests classified as content turned out not to be. `vpl/allocate`'s
     `E2` against our `E1` is an interleaving marker from the scheduling
     harness, and `callbacks/cancel`'s missing line is a callback never
     delivered. Both belong with the checkpoint-column work rather than ahead
     of it, which means the band and that investigation are not as cleanly
     separable as the classification suggested.

     ### The end of the band, and a regression the numbers nearly hid

     125 to **127 of 432**, `threads` 86 to **88 of 127**.

     `fpl/allocate` and `fpl/tryallocate` finished the fpl directory on one
     finding: **a waiter handed a block is not a waiter whose pool vanished.**
     Both were woken the same way, so the waiter told them apart by asking
     whether the pool still existed — and `fpl/allocate` deletes the pool
     immediately after the free that hands a block over. The block is not
     taken back by that delete. A message pipe already drew this distinction;
     nothing else does, and the other five object types still have the bug
     with no test that catches it.

     ### What the sweep caught, and what nearly stopped it being caught

     The same sweep reported `utility/msgdialog` going from 139 differing
     lines to **no output at all** — a test in a directory nobody had touched.

     The cause was the *first* change of the band, twenty commits earlier:
     raising the object caps to 2048. `vtimer_tick` runs at every firmware
     call by design, so its scan went from 32 iterations to 2048 on the
     hottest path in the runtime, and the test stopped finishing inside its
     budget. A correctness change with no correctness consequence, paid for in
     time.

     Three things about finding it are worth keeping:

     - **It was invisible in the headline.** Matching went *up* in that sweep.
       Only the per-test join against the previous run showed a test moving
       the wrong way, and NOOUTPUT is scored separately from MATCH so nothing
       about 127 looked wrong.
     - **The first two bisection steps were worthless.** `git revert
       --no-commit` followed by `git checkout -- .` left `hle.h` modified, so
       two "reverts" were built against a mixed tree and both said the bug was
       still there. The give-away was a checkout that *failed* with "local
       changes would be overwritten" and a test that ran anyway. A `git reset
       --hard` between steps is what made the search mean anything.
     - **The fix is the same shape as an existing one.** sched.c already
       describes its slot table as "a high-water mark rather than a census".
       These tables are too: freed slots stay in range and are never
       compacted, so one past the highest slot ever handed out is the correct
       bound. Callbacks, semaphores and event flags got it on principle;
       vtimers is the one that was measured.

     ### Why we rescheduled where hardware did not

     127 to **128 of 432**, `threads` 88 to **89 of 127**, differing lines
     71,762 to 71,738.

     Two hypotheses were on the table, both framed as a single number with a
     physical meaning, and **both were wrong**. Neither cost a sweep, which is
     the point worth keeping.

     The first was that a firmware call costs more than one microsecond of
     guest time, so msgpipe's `1us:` and `2us:` waits would find their deadline
     already passed. It is not reachable by any value of `PSP_CALL_TICK_US`.
     The tick is charged at *call entry* (`psp_hle_call`), and the handler then
     computes its deadline as `psp_clock_peek() + usec` from that
     already-advanced clock before comparing it against the same unchanged
     value. The cost moves the deadline and the comparison point by exactly the
     same amount: it is a relative offset, and the guard can never fire for a
     nonzero timeout however expensive a call is made.

     The second was that the 5000us quantum fires mid-sequence. It does not
     fire at all. Instrumented to report every time it actually switches, the
     slice fired **zero** times in `mutex/unlock2`, `threads/change`,
     `threads/threadend`, `msgpipe/receive`, `msgpipe/send` and
     `threads/suspend` — and twice in the whole `threads` suite, both in
     `callbacks/notify`, which is how the probe was shown to be alive rather
     than broken.

     What it actually is, and it was already written down here: **giving way to
     a thread you just woke is a preemption, not a yield.** `psp_sched_wake`
     sets `urgent` on a strict `priority <` and nothing else, so a caller that
     switches on it did not volunteer — it was displaced. sched.c has described
     the difference since `threads/change` first showed it, and
     `sceKernelStartThread` was wired to `psp_sched_preempt` accordingly; the
     28 wake sites were left calling `psp_sched_yield`, which sends the caller
     to the *tail* of its own priority queue. pspautotests creates its resched
     thread at exactly the main thread's priority, so an equal-priority thread
     waiting there overtakes the caller on the way back — and that overtaking
     is the entire thing a checkpoint measures.

     `threads/mutex/unlock2` is the whole difference in one line. The switch
     trace before and after, on `Unlocked, ran: 4`:

         better(p16) -> resched(p32)     [r], ours
         better(p16) -> user_main(p32)   [x], hardware

     One test moved the wrong way, and it is worth being exact about why it is
     not a counter-example. `threads/scheduling/scheduling` went 36 to 38
     differing lines on a **single** flipped line — a one-line change re-aligns
     the diff around it — and that line is inside
     `testNoThreadSwitchingWhenSuspendedInterrupts`:

         int intr = sceKernelCpuSuspendIntr();
         checkpoint("  sceKernelWakeupThread: %08x", sceKernelWakeupThread(sleepingThid));

     The woken thread is priority 0x10 against the main thread's 32, so it does
     outrank it — but interrupts are suspended and hardware must not switch.
     `sceKernelCpuSuspendIntr` is bookkeeping only here (`g_intr_enabled` in
     misc.c is set and never read), so we take the switch either way and get
     the line *ordering* wrong in both versions. The old `[r]` was produced by
     a switch that should not happen at all: right in the column, for a reason
     hardware does not have. Gating dispatch on interrupt masking is a separate
     finding and the test is 38 lines from passing regardless.

     ### A vblank wait is a wait, and one vblank releases everybody

     `threads/scheduling/scheduling` 38 to **22** differing lines. No test
     matches that did not before, and none got worse.

     Breaking that test down first was what made it worth touching at all. Its
     38 lines were **three** independent causes, not one: 4 lines of interrupt
     masking, 16 of vblank, and 18 in the two `testSimpleScheduling` sections.
     The last of those is the deep one — hardware takes upwards of 500us to
     create and start a thread, enough that thread 0's `sceKernelDelayThread(500)`
     expires while the main thread is still starting thread 1, where we take
     about six. That is `PSP_CALL_TICK_US` arriving from the opposite direction
     to the hypothesis refuted above, it is global, and it is the parameter that
     has already cost this project a session. The test does not go green without
     it, so the test was not the target; the vblank half was.

     Two things were wrong with `hle_WaitVblank`, and they are separable.

     **It yielded instead of waiting.** A yield marks the caller READY and hands
     off, and the handoff picks the most urgent READY thread — which is the
     caller again whenever it outranks everything else. The test's threads are
     priority 0x18 against the main thread's 32, so thread 0 ran its whole
     four-iteration loop before thread 1 started its first. `psp_sched_delay`
     already existed for exactly this and says so in its own comment, written
     when the game's movie threads sat READY and never ran; the vblank handler
     had never adopted it.

     **It advanced the clock per caller.** Three threads waiting on one vblank
     each added a frame, so the guest saw three frames of guest time pass for
     one frame of scanout. The fix is to make the boundary a shared absolute
     moment — `psp_clock_next_frame`, on the grid the clock already owns —
     rather than a per-caller duration. Everyone parks on the same moment and
     the existing idle path in `handoff_locked` releases them together, because
     it already wakes *every* slot whose deadline has arrived. Nothing in the
     scheduler had to change. `psp_clock_frame` is gone: adding a frame is no
     longer an operation anything wants.

     The frame counter is now counted per vblank rather than per waiter, which
     is what it was always documented to mean. It is deliberately *not* derived
     from the clock, which would look tidier and would destroy it: ticks advance
     the clock, so a derived counter would keep climbing for a game that is
     stuck, and telling looping from stuck is the entire reason it exists.

     The risk here was the game, not the suite — this is the frame path, and
     the boot is byte-identical across it: 633 GE lists, 106,108 commands,
     93,354,668 pixels, 0 bad memory accesses. The payoff was narrower than
     hoped, and worth recording as such: `display/vblankmulti` was the reason to
     expect more, and it did not move. It tests `sceDisplayWaitVblankStartMulti`
     and whether `vcount` advances by exactly one across a wait — and
     `hle_GetVcount` still advances the counter on *read*, which is a separate
     infidelity with its own comment. `intr/vblank` did not move either. The 16
     lines in `threads/scheduling` are the whole measured result.

     ### A dialog that never appears still has to say so

     `NOOUTPUT` 11 to **3 of 432**. Eight `utility/savedata` tests went from
     producing nothing at all to running to completion.

     None of them was failing. They were *hanging*. An unimplemented firmware
     call returns zero (`psp_hle_call`), and zero from a dialog's InitStart
     reads as "started successfully" — so the harness settled into the poll loop
     every dialog caller has, waiting for a status nothing would ever set. Its
     loop is bounded at four hundred thousand iterations with a 2ms delay
     between them, which is not a hang in principle and is one in practice: each
     test hit the ten-second thread-drain timeout and dumped core. The eight of
     them together now take **three seconds**.

     sceMpeg already reached this conclusion and wrote it down — refuse
     outright rather than report nothing, because a caller that waits out
     "nothing" never stops. The same shape, one subsystem over.

     **The first attempt was wrong in an instructive way.** Reporting the dialog
     as cancelled the moment it started does end the loop, and it made six tests
     that already produced output *worse*: `filelist` 86 to 92, `sizes` 90 to
     96. The diff said why. Hardware runs the whole documented lifecycle even
     for a dialog that does nothing — `INIT`, `VISIBLE`, an `Update`, `QUIT`,
     then `FINISHED` after ShutdownStart, settling to `NONE` — and the tests
     print every status transition. Skipping three of them loses three lines per
     trial. Walking the states as a ratchet, advancing on each poll because
     being asked is the only thing that could drive it here, reproduces the
     sequence exactly. Four of those six then came out *below* where they
     started: `filelist` 80, `getsize` 74, `idlist` 75, `sizes` 84.

     **Differing lines are not monotone in correctness, and this is the clean
     example.** `autosave` reads as a regression, 37 to 40. It went from seven
     lines of output to all thirty-four, and from two lines matching hardware to
     fourteen. A longer output has more lines available to differ, so the count
     rose while the test got substantially better — the same trap as `NOOUTPUT`
     scoring zero, one level up. When output *length* changes, count matching
     lines; the third column only means something between runs of the same
     shape.

     The game calls savedata too, and the run changed: **633 GE lists to 639**,
     106,108 commands to 106,762, 93,354,668 pixels to 93,875,406, still 0 bad
     memory accesses. The frame comparisons are identical to the pixel — 0, 751
     and 9,020 of 130,560 against the corner — so nothing is drawn differently.
     It gets further per instruction budget because it is no longer spinning on
     a dialog that could not finish. The reference figures in state.md move with
     it.

     Nothing here touches ms0:, PARAM.SFO or the save layout, and the eight
     tests are nowhere near matching. This is the shape of the conversation, not
     savedata.

     ### `cpu_branch`: one line, and it is the module base

     Parked, and filed next to `threads/create` because it is the same
     question. Worth writing down mainly to stop it being picked up again as
     the cheap cpu-level fix it looks like.

     `cpu/cpu_alu/cpu_branch` differs on one line out of nine:

         jalr: non-ra: 00000420      ours
         jalr: non-ra: 08804420      hardware

     The `jalr` is correct. Every other line in that test is an ordinal — which
     branch went first, whether the link register was written before or after
     the delay slot — and all eight pass, including the three that check the
     link register's *ordering*. This is the only line in the file that prints
     an absolute address, and the difference is exactly 0x08804000: the base a
     PSP loads a user module at.

     Our loader puts each segment where it was linked, and says so —
     "a relocation against segment 0 adds zero and every code address stays
     put". For a relocatable PRX that linked address is zero, so the module runs
     based at zero and a captured code address is short by the base.

     Rebasing is not a loader change. **Emitted function names are addresses** —
     `psp_func_00299C5C` — so the base is the project's naming convention, the
     oracle's identity for a function (`mapped: 0x00000000 + 6162720 bytes`,
     102,615 relocations against it), and the address in every report and
     finding written so far. The game's own ELF is relocatable and based at zero
     too, so this is not a test-only path: it would move Armored Core, rename
     every generated function, and invalidate the emitted C.

     One line is not worth that. `threads/create`'s 28 lines are the same cause
     — its `entry=`/`gpReg=` USER classification depends on the module living in
     the user partition — so the two are one question, and it is worth about 30
     lines whenever something else makes rebasing necessary anyway.

     ### `ctrl/ctrl`: Read waits for a sample, and Peek does not

     128 to **129 of 432**. Two findings, and the first one moves nothing on its
     own.

     `sceRtc` was not implemented at all, and `ctrl/ctrl` measures with it:

         sceRtcGetCurrentTick(&tick0);
         for (n = 0; n < 5; n++) sceCtrlReadBufferPositive(&pad_data, 1);
         sceRtcGetCurrentTick(&tick1);
         printf("%d\n", (tick1 - tick0 > 5000));

     Unimplemented, the call returned zero and never wrote either variable, so
     the subtraction was of two pieces of uninitialised stack. Only the tick
     counter is added here: the rest of sceRtc is calendar work and
     `rtc/arithmetic` alone differs by more than a thousand lines. A tick is a
     microsecond, which the tests state rather than a header — `rtc/rtc` delays
     2000us between two reads and checks the difference is at least 2000. Both
     users take differences, so our epoch of "since the module started" is not a
     problem.

     The second is the real one. **Read waits for a controller sample it has not
     already been given; Peek takes whatever is there.** That is the whole
     difference between the two, and the registration here said as much while
     pointing them at the same handler -- "nothing samples here, so the two are
     the same call". That was true until the vblank grid existed to sample
     against. Now it does, and the test measures the difference three ways: five
     Reads span four vblanks and answer 1, `ReadLatch` answers 0, five Peeks
     answer 0 against a `< 5000` threshold. We had the second and third right
     and the first wrong.

     Waiting *unconditionally* would have been the expensive mistake. A frame
     loop is `WaitVblank(); ReadBufferPositive();`, and the sample it wants
     arrived at the vblank it just waited out -- charging another frame would
     halve the frame rate of every game that reads the pad. So what is tracked
     is when the next unread sample falls due, and a read that already has one
     does not wait. The game confirms it: unchanged at 639 GE lists, 106,762
     commands, 93,875,406 pixels, same frame comparisons to the pixel.

     One process note, because it nearly cost the finding. An intermediate run
     reported `ctrl/ctrl` matching with only the clock added, which would have
     meant the sampling change was unnecessary. It was a stale binary — the
     result came from a build that had not finished. Re-checked against a build
     confirmed to have succeeded, the clock alone still prints 0. This is the
     same rule as never trusting a ctest result printed after a failed build,
     and it applies to attribution runs just as much.

27. **New Game: a fault, then two hangs, each a firmware lie -- and the
    third is still ours.** The 30 Aug fault behind NEW GAME reproduced on
    1 Sep to the register (state.md, *pad-driven*), and removing lies from
    the path in the order the roadmap gave has moved the stop three times in
    one afternoon. What follows is what each move measured.

    ### The honest refusal the game does not survive

    All 13 `sceAtrac3plus` imports were unimplemented, so every one returned
    zero and wrote nothing: zero from `GetAtracID` is a valid ID, and the
    game's music pump advances a ring cursor by a sample count it is never
    handed. The first repair -- `GetAtracID` fails with NO_ATRACID, every
    other call refuses a bad ID -- hung the game before the title screen:
    **336 pad polls, 890 million semaphore operations, a black screen.**

    Read from the emitted C: the player thread (`CSoundAtrac3Player`,
    priority 16) ticks in a lock / pump / unlock loop whose only throttle is
    a delay taken when the player is idle or has PCM buffered, and the game's
    own open-failure path leaves the player *enabled* with `remainFrame`
    still zero -- the one state in which the pump does nothing and delays
    nothing. A `SetData` failure lands in the same state. Over a priority-40
    main thread, a priority-16 spin is a hang. On hardware neither call ever
    fails, so the game has no working failure path for either.

    What it does handle is a decoder that opens and then cannot decode: a
    negative return from `DecodeData` takes its stop path. So `atrac.c` is
    now that -- IDs two per codec as `ids.expected` shows, the RIFF header
    parsed with `setdata.expected`'s codes, `remainFrame` -1 for a whole
    file and frames-present-minus-one for a partial one (all six sizes in
    `getremainframe.expected`), `GetStreamDataInfo`'s count ending on a
    frame boundary (0x7800 of free buffer is hardware's 0x76B0), `SetLoopNum`
    refused on a file with no `smpl` chunk -- and `DecodeData` failing. The
    title screen came back: 7,506 GE lists, 0 bad accesses, the menu.

    ### The fault is gone, and the wait behind it was a sound effect

    With the stand-in, NEW GAME no longer faults -- **0 bad accesses** where
    there were 1.2 billion -- and the run parks at the same poll, 2567. An
    HLE log of the transition (`--drain 75`, stderr capped at 1.5 GB)
    showed the main thread finishing 67 frames of fade and then looping on
    two locks -- `Sound Player Sema` and the `CSoundSasPlayer`'s
    `CommandThread Sema` -- with a 100us delay, forever. Not the ATRAC
    player: the SAS mixer.

    `__sceSasGetEndFlag` said why. Before the press it alternated between
    voice 0 playing and all voices ended, as menu sounds came and went.
    At the press the game keyed on one more voice -- the same 46,560-byte
    VAG sample it had already started for START -- and from then on every
    one of 86,816 polls reported both voices still playing. The game waits
    for that "decide" sound to finish before it leaves the title screen.

    Our `decode_block` never finished it. With the voice's loop argument
    set it restarted from byte 0 at the buffer end and at a block flagged
    7. Hardware does neither: `audio/sascore/vag.expected` plays 16-block
    samples in loop mode 1, and a sample of plain blocks **ends when its
    buffer does**; only a block flagged exactly 3 keeps it playing. The same
    test also shows the check is on exact values, not bit 0 -- it plays
    `music.vag` with its file header in front, so block 0's flag byte is the
    'A' of "VAGp", and the voice is still playing a grain later. (An earlier
    version of the fix tested bit 0 and moved `vag` 422 -> 442; that is how
    the header block was noticed.)

    The same test exposed `__sceSasSetGrain` as unregistered: the tests set
    a grain of 512 and ours stayed at 256, so one core call rendered half a
    sample and the buffer end was never reached. Registered, with
    `GetGrain`, `SetOutputmode` and `GetOutputmode` (the last called three
    times by this game at boot), and the codes `sascore.expected` and
    `vag.expected` pin: grain outside 64..2048 or off a multiple of 32
    `80420001`, voice count `80420002`, output mode `80420003`, sample rate
    `80420004`, null or unaligned core `80420005`, voice index `80420010`,
    a sample size that is zero or not a multiple of 16 `80420014`.
    `audio/sascore/vag` 422 -> **404**, every `Ended` line matching; eight
    of the twelve `sascore` tests moved down, `sascore` itself 149 -> 55;
    the title screen and the headless bar unchanged. One test moved up:
    `audio/reverb/volume` 59 -> 68. It builds its voice with
    `__sceSasSetVoicePCM`, still unregistered, and measures the output
    level; with the grain stuck at 256 part of its buffer went unwritten
    and the level check happened to land "near 1x". With the grain honoured
    the whole grain is written, silently, and reads zero. An accidental
    match lost, not a fault gained -- it passes once `SetVoicePCM` exists.

    ### What is left, and it is ours again

    With the sound effect ending, the main thread runs the fade, issues two
    seeks, three async reads and five polls, and then sits in the sound
    system's shutdown loop, `psp_func_00267298`: run the sound managers'
    update, sleep one frame, repeat while the sum of three request-list
    counts (`[sys+16] + [sys+544] + [sys+1072]`) is above zero. The
    per-slot locks are gone from the loop now -- the effect requests
    drained -- and no `sceAtrac` call is made after the press, so what
    remains is the stop of the title track it opened before it.

    That request completes through `psp_func_0026A900`: the player idle
    (`[player+40]` set and `[player+76]` below 8192 -- both true after our
    stop path) **and** the stream-feeder object's busy word clear
    (`arr2[idx][+0]`, cleared by `psp_func_0026C104` via `0026BC24`).
    Nothing on the decode-error path clears it, and only a normal run-out
    of the track does. **Next:** read who calls `0026BC24` and when, and
    take a TRACE build with `PSPRECOMP_WATCH=0x00267298` to name `sys`, then
    `PSPRECOMP_PEEK` on the three counts and the feeder word at the stop.
    The durable answer is probably a stand-in that *plays* -- silence, at
    the right sample count -- so that every path the game takes is the
    normal one; M3 needs that bookkeeping regardless.

    ### The pattern, again

    Three stops at one poll, three different firmware lies: an import that
    answered zero and wrote nothing, a refusal the game's own error path
    could not survive, a voice that never ended. Each was invisible until
    the one before it was removed, and each was found the same way -- the
    call histogram to see the shape, the HLE log to see the order, the
    `.expected` files to see what hardware does.

    ### Retracted: the feeder word. What the stall was, measured

    The section above named the stream feeder's busy word as the remaining
    gate. Read at the stall with `PSPRECOMP_PEEK` -- the sound system is
    reachable from the global word at `0x0031F5D8`, the player and feeder
    tables from `0x0031F5E8` and `0x0031F5F0` -- all three request counts
    were **zero** and both feeder words were **zero**. Player 0 had its end
    flag set and its buffered-PCM counter at +76 holding `0x23B43080`. The
    game adds `DecodeData`'s sample count to that counter *before* it looks
    at the return code, and the stand-in wrote nothing back on failure, so
    it added the stack's leftovers. The idle predicate's `< 8192` could never
    be true. The unwritten out-parameter, one level down.

    Hardware writes all three out-parameters on a decode with nothing left
    -- `stream.expected` line 1108: `80630024=sceAtracDecodeData error:
    samples: 00000000, finish: 00000001, remainFrame: -2` -- so the stand-in
    now reports the stream fully decoded at its first decode, with the
    drained `remainFrame` for its kind. The sound system shut down, and the
    game reached **the fault it faulted on before any of this**: `read32 at
    0x461CC570`, `ra 0x0002E2F8`, every register as on 30 Aug, with zero bad
    accesses before it and no audio lie left on the path. The fault was
    never about audio. Three lies had merely been standing in front of it.

    ### The cause: a branch the emitter never took

    With the fault reachable from a clean state, the roadmap's three
    one-command steps ran on a TRACE build. `PSPRECOMP_WATCH=0x0002E1D8` --
    the clipper's caller, entered once in the run, right before the fault --
    showed sane arguments: a model pointer, a stack buffer, and a vertex-type
    word of `0x1400013D` in the fifth register, a legitimate stride-20
    s16-position type. `PSPRECOMP_WATCHMEM` on the caller's `sp+1028`, the
    slot that word is stored to, saw it written by the setup routine and then
    **overwritten four times by the per-plane clip routine**
    `psp_func_0002E370`, the last value `0x4612ED37` -- the float the vertex
    decoder faulted on. The clip stage ran off the end of its output buffer
    into the caller's frame.

    `PSPRECOMP_WATCH=0x0002E370` on the six calls gave the count going into
    each plane: **3, 6, 12, 24, 48, 96.** A triangle against six planes can
    produce at most nine vertices; ours doubled at every plane, which means
    both stores ran on every edge. `PSPRECOMP_VCMP_RING` showed the compares
    answering correctly. The emitted C for the branch that skips the first
    store read:

        /* 0002E3C4  bvf        0x0002E3D8 */
        { int _c = (0 /* unhandled branch */);
          if (_c) goto L_0002E3D8; }

    `branch_cond()` in the emitter had no case for `bvt`, `bvf`, `bvtl` or
    `bvfl` -- the branches on a VFPU condition code -- and emitted them as
    never taken, with no trap and no diagnostic. The decoder named all four
    and marked them as branches, so labels and delay slots were right; only
    the condition was missing. The interpreter's `branch_taken()` had the same
    gap, so **the two translations agreed on never taking them and the
    differential oracle could not see it**; no pspautotests source uses the
    branches either. Fifteen sites in this module. The game was the only
    instrument, and it needed three lies removed from in front of it before
    it could point.

    Fixed with a `psp_vfpu_cond(cc)` helper beside `psp_fpu_cond`, the code
    index being bits 18..20 of the word, used by both sides;
    `test_vfpu_branch_condition` pins both senses. Oracle 316/316 unchanged.
    This is the fifth silent emitter bug and is drafted for upstream in
    [../upstream/README.md](../upstream/README.md), 2e.

    ### M1's gate passes

    `scripts/09-replay.sh --decode scenarios/new-game.pad` runs to its
    `stop` at poll 6000 (t=94.8s) with **0 bad accesses** and 70 of 70 events
    delivered: 18,006 GE lists, 50,813,097 commands, 1,035,722 primitives,
    1,002,057 textured 3D draws, CLUT4 textures appearing for the first time,
    5,601,280 bytes read. The displayed frame at the stop is the game's
    initial **sound settings** panel -- BGM, SE and Voice volume sliders
    between MIN and MAX, a Default button, a status line -- which is the first
    screen a new game shows. From 1.2 billion bad accesses at poll 2567.

    Two things seen on the way that are M2's: the glyphs on that panel are
    smeared, which is the affine texturing the roadmap already names; and the
    GE summary reports texture coordinates of `-inf..inf`, so something in
    the new geometry hands the rasterizer non-finite UVs. Also
    `sceIoGetstat failed: ms0:/PSP/SAVEDATA/NPUH10024DATAINSTALL` -- the game
    looked for its install data and was told honestly there is none (M4).

    Title screen 7,506 GE lists and headless 639 unchanged, both 0 bad
    accesses; sweep 129 of 432 unchanged.

    **Four stops at one poll.** An import answering zero and writing nothing;
    a refusal the game's own error path could not survive; a voice that never
    ended; a decoder that wrote nothing back on failure -- and underneath all
    four, the codegen bug that was there from the first run. Every one of the
    audio findings was real and needed fixing. None of them was the fault.

28. **M2, first pass: the GPU suite was blind to texturing, and what it saw
    once it could see.** M2's gate is a mission rendering, and its first
    frames past the title -- the sound-settings panel -- showed smeared
    glyphs and a screen-wide hatched sheet with texture coordinates of
    `-inf..inf`. The roadmap says to let the suite drive the renderer, so
    the textured GPU directories were run first. Every one of them printed
    the test's own preset fill at every sampled pixel.

    ### The readback

    The GPU tests clear the framebuffer and read pixel (0,0) back through
    `sceDmacMemcpy`. It was unimplemented: it returned zero and copied
    nothing, so the test's buffer still held the 44444444 it had written,
    and every textured test in the corpus reported "nothing drawn" whatever
    the GE had done -- 811 calls in one test. The suite was blind to
    texturing by the readback, not by the rasterizer. Implemented with the
    contract `dmac/dmactest.expected` gives (size checked before pointers:
    zero size 80000104, null with a length 80000103), and the three cache
    range operations the same tests call registered as the no-ops the flat
    memory model makes them. With that, the suite measured texturing for the
    first time, and four things were wrong at once.

    ### Four things, each with its test

    - **Texturing without texture coordinates.** `gpu/texfunc` draws sprites
      with `GU_COLOR_8888 | GU_VERTEX_32BITF` -- no texcoords -- over a solid
      texture and reads the texture's colour back. Our GE required texcoords
      in the vertex before binding a texture, reasoning that geometry without
      them would otherwise be painted with a stale texture sampled at texel
      zero. That is what hardware does. The guard is gone; a vertex without
      coordinates samples (0,0). A game that wants flat geometry disables
      texturing, and this one does: its frames did not change.
    - **The texture functions.** Only MODULATE existed, hardcoded at both
      call sites, so DECAL, BLEND, REPLACE and ADD all drew as MODULATE.
      All five now match `gpu/texfunc` on every value, with the RGB/RGBA
      flag (under RGB the fragment alpha is the vertex's -- measured through
      the tests' blended lines) and colour doubling ("Half x2 + Half"
      saturates). The five files still differ by 540-1568 lines each: every
      remaining line is the `[x]`/`[r]` reschedule column, hardware
      rescheduling during a draw-and-readback where we do not. A scheduler
      matter, recorded here so it is not read as a rendering one.
    - **The framebuffer's alpha byte is the stencil.** Hardware reads
      44ffffff back from a 44444444 fill after every draw; an ordinary draw
      does not write it, a clear does when its stencil bit is set. Three
      `gpu/texcolors` tests matched the moment this was right. The raster
      unit tests asserted the old behaviour and now observe a sampled alpha
      the only way hardware allows, through a blend.
    - **16- and 32-bit palette indices.** `gpu/clut/shifts` and `masks`
      index the palette with whole 16- and 32-bit texels; the formats were
      refused, so the quads drew flat white. Added: `masks` matches, `shifts`
      164 -> 12, `offset` 394 -> 162, `address` 44 -> 24.

    And one more from `gpu/filtering/precisionnearest2d`: **a sprite with
    exactly one axis flipped runs u along y and v along x.** The corners the
    GE generates take u from the vertex that gave them their y and v from
    the one that gave them their x; TR->BL reads texel (0,1) at the top-left
    pixel where the standard mapping reads (1,0). All four orientations
    match now.

    Sweep 129 -> **135 of 432**: `clut/masks`, `rendertarget/depal`,
    `simple` and the three 16-bit `texcolors` crossed; `rendertarget/copy`
    3,906 -> 1,706. Two moved the other way: DXT5 by eight lines, a format
    this game does not use, and `clipping/guardband` 16 -> 20, where hardware
    culls triangles entirely outside its guard band and we now draw them
    textured -- the clipper item, unchanged in substance.

    ### What it did not change, and what that says

    The game: title screen 7,506 GE lists, headless 639, New Game 18,006,
    all 0 bad accesses, frames the same to the eye (the title frame's
    corner count moved 127,050 -> 126,299 with the alpha byte). The smeared
    glyphs on the settings panel are exactly as they were. So they are not
    the texture function, not the palette, not the alpha. What remains open
    in the suite is what they are: `gpu/filtering`'s precision tests differ
    on sub-pixel and half-texel rules -- hardware draws a through-mode sprite
    starting at x=1 from pixel 0, and picks a different texel at a
    half-texel offset -- and `nearest` differs on 269 of 549 value lines the
    same way. Text at 1:1 is the case those rules decide. **Next:** derive
    hardware's through-mode coverage and sampling rule from
    `precisionnearest2d` and `nearest`, then look at the glyphs again.

    Also open from the same runs, in the order they matter to a mission:
    `gpu/textures/size` -- in 3D mode a 16-bit texture coordinate of 32768
    reads as the far end of the texture on hardware and the far edge pixel
    is drawn; `gpu/texmtx` -- vertices with skinning weights are not stepped
    over by the decoder, and there is no texture matrix at all;
    `gpu/clipping` -- no near-plane clipper and no guard-band cull. The
    hatched sheet's `-inf` coordinates are unchanged and unexplained.

    ### The precision rules, and what they were worth

    Item 28 ended by naming sub-pixel coverage and half-texel selection as
    what the filtering tests still measured. Four rules came out of them, each
    read off hardware's numbers rather than assumed, and all four precision
    tests are now exact on every value.

    **Screen positions are 12.4 fixed point.** The GE truncated them to whole
    pixels, so every edge and every texel boundary moved by up to a pixel.
    `gpu/filtering/precisionnearest2d` places a two-pixel sprite at
    x = -i/16 for i in 0..15 and reads which texel each pixel gets; whole
    pixels answered one of those sixteen cases and lost the other fifteen.
    `psp_vertex` now carries `PSP_SUBPX` units per pixel, both fillers work in
    them, and a pixel is covered when its centre -- `16*i + 8` -- lies inside
    the primitive, which is `(v + 7) >> 4` at each end. Both nearest-precision
    tests went to exact, and `gpu/primitives` stayed at all thirteen matching.

    **Texture scale and offset were never decoded**, and narrow texture
    coordinates are unsigned. `precisionnearest3d` scales by 0.5 and its texel
    boundary landed at a quarter of the sprite; `gpu/textures/size` draws with
    16-bit coordinates from 0 to 32768, which read as signed is -1.0, so the
    right-hand edge sampled texel 0 instead of the last one. Both fixed in
    `read_uv_model`; `size` 141 value-differences to 41, `rotate` to zero.

    **Bilinear weights are sixteenths, floored, and the blend truncates.**
    `precisionlinear2d` stretches two texels over 256 pixels: a continuous
    filter starts changing colour at pixel 64 by one step, and hardware first
    changes it at pixel 72 by 0x10 -- sixteen levels, and the ramp does not
    begin until a full sixteenth has accumulated. The blend then truncates:
    a quarter of the way from 00 to ff is 0x3f, not the 0x40 rounding gives,
    and the halfway point is 0x7f. Both linear-precision tests went to exact
    and `linear` 488 to 335.

    One epsilon is ours, not hardware's, and is marked as such in the code.
    Our interpolated coordinate carries a few ULP of error; at a 1:1 blit that
    puts it just below a texel boundary, the weight becomes fifteen sixteenths
    on the texel below, and truncation drops a whole texel -- linear stopped
    agreeing with nearest at the one scale where they must agree, which
    `test_raster` catches. A thousandth of a texel absorbs it, four orders of
    magnitude below the sixteenth being measured.

    **Texture dimensions saturate at 512.** `gpu/textures/size` asks for 1024
    up to 8192 and every one reads texel 511 at its far edge.

    Sweep 135 -> **136 of 432**, `textures/rotate` crossing. The game is
    unchanged in every count -- headless 639 GE lists, title 7,506, New Game
    18,006, all 0 bad accesses -- and the frames are the same picture, with
    the FromSoftware logo and the title screen slightly crisper at the glyph
    edges where the coverage rule moved.

    **The glyphs on the settings panel are still smeared.** Six rendering
    findings in, that is now a strong negative result: it is not the texture
    function, the palette, the alpha, the sub-pixel coverage, the filter
    weights or the coordinate range. What is left in the suite that text
    could turn on is `gpu/filtering/nearest` and `linear`, which still differ
    on 262 and 335 values about *which* texel a magnified sample lands on at
    half-texel offsets, and `gpu/textures/mipmap` at 190. That, and the
    unexplained `-inf` texture coordinates the panel's own draws report.

    ### The glyphs are not a sampling problem, and the overlay was texgen

    Six rendering fixes in, the settings panel's text was still wrong, so the
    next move was to stop inferring from tests and look at the game's own
    draws. Two instruments had to be corrected first, and both had been
    answering plausibly about the wrong thing:

    - **The texture dumper capped at 16 distinct textures.** The title screen
      alone binds that many, so every dump taken to look at a later menu
      contained the title's textures. At 256, the New Game run yields 49, and
      the files now carry each texture's address, size and format so a dump
      can be matched against the GE summary.
    - **The draw log counts down from the first draws of the run**, which is
      the wrong end of a question about a menu three screens in.
      `PSPRECOMP_GE_TEXDRAW=<hex>` asks it the other way round: show me the
      draws that bind this texture, with each vertex's position and
      coordinates.

    The glyph atlas is texture 45, 512x512 CLUT4, and it **decodes perfectly**
    -- Latin, Greek, Cyrillic, kana and kanji, every glyph crisp. The draws
    are perfect too: each glyph is a pair of triangles spanning 5x13 pixels
    with texture coordinates spanning exactly 5x13 texels, a 1:1 blit. So the
    text is neither the texture, nor the palette, nor the sampler, nor the
    filter, nor the coordinates. It is something in the fill, and it is the
    open question.

    What the same log *seemed* to explain was the hatched sheet, and this is
    recorded as a retraction because the claim went into a commit message.
    Interleaved with the glyph draws are full-screen quads binding the glyph
    atlas with texture coordinates of -512 and NaN, and the GE summary's
    `u -inf..inf` comes from them. The first reading was texture coordinate
    *generation*: `TEX_MAP_MODE` was never decoded, so a game using the
    generation matrix would leave the vertex field uninitialised, and that is
    exactly what these looked like. It was implemented (PSPSDK: `TEX_MAP_MODE`
    0xC0, `TGEN_MATRIX_NUMBER` 0x40, `TGEN_MATRIX_DATA` 0x41), the panel frame
    came back without streaks, and the commit said texgen had removed them.

    Sif reported the streaks still there, going away only after confirming
    the dialog. Two measurements settled it. The scenario's panel frame before
    and after the texgen change is **pixel-identical** -- 0 of 130,560
    differing -- so the change made no visible difference at all; the earlier
    frame had simply been taken after the confirm. And with the texture-enable
    bit, map mode, vertex type and colour added to the draw log, every one of
    those full-screen quads has **texturing off**, map mode 0, and a black
    colour with a fading alpha (`FD000000`, `F9000000`, `F3000000`...). They
    are the fade overlay. The NaN is in a field the draw never reads.

    Texgen stays: it is a real register the game sets elsewhere and
    `gpu/texmtx` measures it (`source` 40 value differences to 15, `uvs` 4 to
    2; the two normal sources fall back to position because the vertex
    decoder reads no normals). It just was not this.

    **Correction (later the same day).** The paragraph below reads as though
    the mip chain explained the dotted trails. It did not. With the chain
    implemented the New Game frame came back **pixel-identical** -- 0 of
    130,560 differing -- and the instrument added with it says why: not one
    draw in that scene carries a mip chain (`TEX_MODE`'s top level is 0
    throughout), so the mip path never engages. Mipmapping was a real gap and
    `gpu/textures/mipmap` now matches hardware on every value, but the trails
    were something else. What follows is the reasoning as it stood; it is
    kept because the *shape* of the argument -- minified fine detail
    point-sampled from one level -- was sound, and only its application to
    this scene was wrong.

    **The real cause, and it was one bug.** An every-120th-frame sequence
    (`PSPRECOMP_FRAMES`) showed the hatch in full at frames 44-45, between the
    New Game press and the settings panel: a single huge triangle covering
    the lower-left of the screen, filled with high-frequency diagonal stripes,
    the signature of a textured draw whose coordinates run hundreds of texture
    widths across one primitive. `PSPRECOMP_GE_WILDUV=1`, which logs textured
    draws with coordinates far outside the texture or not finite, fired 24
    times -- and every hit was a **glyph batch**. The first four vertices of
    each were the perfect 5x13 quad already seen; somewhere in the remaining
    146 was garbage.

    The vertex type of those batches is `0x99F`, and bits 11..12 of it are
    `01`: **8-bit indexed**. `GE_IADDR` was decoded and dropped -- `case
    GE_IADDR: break;` -- and `VT_INDEX` was defined and never read, so every
    indexed draw in the game fetched its vertex array in order as if the index
    list were 0,1,2,3... For a quad drawn as 0,1,2,0,2,3 that is right for the
    first triangle and wrong for everything after, the error growing with
    every glyph until the read runs off the end of the 100-vertex array into
    whatever follows. Which is exactly what the panel showed: the first glyph
    whole, the rest progressively misassembled, and the tail one enormous
    triangle with coordinates from unrelated memory, painted as stripes.
    Fixed with a `vertex_addr()` that fetches through the 8- or 16-bit index
    list. Zero wild draws, and both the settings panel and the option menu
    behind it render exactly: every glyph, every label, the caution box.

    Seven rendering fixes chased this in the wrong place, and it is worth
    saying why. Each of them was real -- the tests measured them and the
    hardware numbers agree -- but none was *this*, because the symptom
    (fragments) suggested sampling and the sampling was fine. The instrument
    that found it did not look at textures or filters at all: it asked which
    draws had coordinates that could not be right, and followed the draw. The
    lesson is the one this file keeps relearning: when a fix does not change
    the symptom, stop fixing and measure the symptom directly.

    **The pattern is worth keeping.** Every one of the seven rendering
    findings so far came from a test measuring something the game does; this
    one came from the game measuring something no test covers, and it took
    fixing two instruments before the game could say it. The tests found the
    rules; the game found the gap.

29. **The clipper, the scissor, and what the GE does with w** (2 Sep). With
    the text fixed, Sif pointed at the background: "white pixels in lines
    that feel wrong". The final frame's draw log (`PSPRECOMP_GE_DRAWLOG`
    now takes a `_SKIP` count so it can be aimed at the end of a run) showed
    the 3D backdrop's strips with screen bounding boxes like x -377752..-1093:
    vertices between the eye and the near plane, small positive w, projected
    to positions in the hundreds of thousands. The old rule dropped a
    triangle only when a vertex had w <= 0 and drew everything else as-is,
    and the slivers those triangles left were the pixels.

    `gpu/clipping` -- two tests, forty data points -- fit one model, and it
    is not the textbook one. **The hardware divides by w first, whatever its
    sign, and applies its rules in NDC.**

    - A triangle with every vertex at w <= 0 draws nothing (`Flat W=0: 0`,
      `Flat W=-1: 0`). Mixed signs just divide: `Linear W 1->-1->-1` lights
      the same 16,384 pixels as `1->1->2`, because (-w,-w,-w,w) is the same
      NDC point for either sign. There is no eye-plane clip.
    - With `DEPTH_CLIP_ENABLE` (0x1C) clear, near and far *reject*: any
      vertex with z/w outside -1..1 drops the whole triangle. guardband's
      `TRIANGLE_OUT_NEG_Z` has one vertex at -1.2 and two inside and is
      DRAW=0; `Flat W=0.001 (noclamp)` is 0 because its other vertices sit at
      z/w = 499.
    - With the flag set the hardware clamps rather than rejects, and what it
      does is exact: it clips the near plane geometrically, in NDC, and the
      far plane not at all. `Z outside near` (one vertex at z = -2) lights
      171 of 255 pixels on the wide edge -- the cut at t = 1/3 -- and
      `Z outside both` 192, the cut at t = 1/4, while `Z outside far` alone
      keeps all 255. On this hardware the flag means clamp, not clip, and
      the GU library's `GU_CLIP_PLANES` name is the wrong way round.
    - The guard band: a triangle with **any** vertex outside the 4096-square
      box placed by `OFFSET_X/Y` is not drawn, with or without the flag.
      `TRIANGLE_OUT_NEG_X` puts one vertex at x = -1809 against a -1808 edge,
      leaves the other two well inside, and reads DRAW=0. (This was later
      relaxed to "all three beyond the same edge" on a theory about the
      hangar's missing walls, and the sweep caught it the same day: the rule
      is per-vertex, and the hangar was dark for other reasons. Recorded
      because the wrong version reached a commit message.)

    Two wrong models preceded the right one, and both were *measured* wrong
    within minutes rather than argued about: Sutherland-Hodgman against the
    near plane gave `Z outside near (noclamp)` 171 pixels where hardware
    gives 0, and a clip-space -w <= z <= w reject gave `Linear W 1->1->-1` 0
    where hardware gives 16,384. The `.expected` numbers are cheap to test
    against and there is no reason to hold a theory for longer than that.

    And a bug the same test found on the way: the rasterizer's bounds were a
    hardcoded 480x272 -- `sw_tri`'s bounding box was clamped at 479 --
    and the scissor registers (`SCISSOR1/2`, 0xD4/0xD5) were never decoded.
    homogeneous draws into a 512-wide target with a 512-wide scissor and lost
    its last 32 columns (`X outside: 478` for 510, `X outside (right): 0`
    for 128). The scissor is now a backend call and the only bound.

    **Both clipping tests match hardware on every value.** homogeneous's
    remaining 92 differing lines are all the checkpoint column.

30. **Mipmapping, and the dots in the backdrop** (2 Sep). With the clipper in,
    the background band Sif pointed at was pixel-for-pixel unchanged -- the
    clipper was right and measured, and it was not this. A map of every pixel
    brighter than 40 outside the panel showed the thing itself: trails of
    single pixels, one per row and seven columns apart along straight
    diagonals, their colour grading along the line. The draw log aimed at the
    final frame (`PSPRECOMP_GE_DRAWLOG_SKIP`) put the 3D backdrop's strips
    over them: hangar wall panels, CLUT8, 64x64 and 128x128, full of fine
    seams and bright rivets, stretched over strips thousands of pixels tall,
    sampled with linear-mipmap-linear minification. Twenty-fold minification
    of a one-texel seam, point-sampled from level 0 because levels 1..7 were
    never decoded, is exactly a trail of dots.

    `gpu/textures/mipmap` gives the rule in its own numbers, each level of
    its test texture filled with 0x10 times the level:
    - **AUTO**: level of detail = log2 of the texel-per-pixel ratio, the
      *larger* axis ("Minify 4x W", width only, lands on level 2), plus the
      bias, a signed count of sixteenths from `TEX_LEVEL` (0xC8); floored,
      capped at `TEX_MODE`'s top level, never below 0.
    - **CONST**: the bias alone. **SLOPE**: the slope register (0xD0) plus
      the bias -- a slope of 2.0 reads level 2, which log2 would not give.
      The undefined mode 3 measures exactly like CONST.
    - With a mip-linear filter the next level is blended in by the fraction,
      exact to the sixteenth: bias +07 at 1:1 reads 07 between 00 and 10;
      +87 (−7 9/16) at 256x reads 07 again; "Magnify" (¼) at +70 reads 50.
      The blend is per channel, `a + (b − a) · f / 16`, truncated.
    - A filter without mipmapping stays on level 0 whatever the ratio.
      Within a level the min filter applies when minifying and the mag
      filter when magnifying. Mip-nearest's rounding is unmeasured -- the
      test was compiled with the linear variant -- and rounds half up.

    The level is chosen once per primitive from its own texture gradient
    (the affine map from pixels to texels, solved on two edges for
    triangles; the corner ratio for sprites), not per pixel. 190 value
    differences to **0; the test matches hardware.** One trap on the way:
    `scripts/07-autotests.sh` takes a directory, and handed a `.prx` it
    prints an error and leaves the previous `.got` in place, so two "runs"
    of the test measured nothing and the third measured the stale file.

31. **Lighting, and what it does not explain** (2 Sep). Sif's PPSSPP
    reference for the settings screen shows a full hangar behind the menu --
    grey walls, floor grating, hazard stripes -- where ours is nearly black
    with faint dotted trails. `LIGHTING_ENABLE` (0x17) was set for 17.4
    million vertices of that scene and never read: with lighting on the
    vertex colour is not a colour, it feeds whichever material components
    `MATERIAL_COLOR` selects, and the shaded colour is computed from the
    lights. We used the raw field.

    Implemented from `gpu/commands/light`, whose two-pixel boxes carry a
    known normal and a red ambient, green diffuse, blue specular light, so
    every reading decomposes:

    - The light's ambient always contributes, and attenuation and the spot
      factor scale it with everything else ("Diffuse 0.5 - Spot A + D:
      7f3f00" halves the ambient and quarters the diffuse).
    - Diffuse is max(N.L, 0), or `pow(N.L, specular coefficient)` for a
      powered-diffuse light.
    - Specular is `pow(N.H, coefficient)` with **H = normalize(L + (0,0,1))**
      -- a fixed eye direction, not the view position. With N.L = 0 the test
      reads 0xb5, and 1/sqrt(2) is exactly what a fixed +Z eye gives. It
      contributes when N.L >= 0 and not below ("Diffuse 0.0" 0xb5, "Diffuse
      -0.5" 0x00).
    - The spot factor is **dot(L, D)** -- the *vertex-to-light* direction
      against the spot direction, not the usual dot(-L, D): with the light
      overhead and a direction of +Z the test reads a full-strength spot,
      and the conventional sign would switch it off.
    - A directional light ignores attenuation and reads its position field
      as a direction.

    A fixed (0,0,1) eye direction is only meaningful where the viewer looks
    down -Z, so lighting is done in **eye space** and the light positions
    are transformed by the view matrix. The test cannot tell -- its view
    matrix is identity -- and the game turned out not to either, which is a
    reminder that agreeing with a test is not the same as being right.

    **And it is not the cause of the dark hangar.** Dumping the game's own
    light state at the wall draw settles it arithmetically. Three
    directional lights, diffuse only, no specular; global ambient and
    emissive zero; `MATERIAL_COLOR` = 3 so ambient and diffuse both come
    from the vertex colour, which is 0x80. Their ambients sum to 0.20 and
    their diffuses to 0.90, so the shaded colour cannot exceed

        0.5 x (0.20 + 0.90) = 0.55, or 0x8C

    however the surfaces face -- and the same ceiling holds if the material
    bits select the registers rather than the vertex, which is the one part
    of the encoding the test does not pin down. Modulated against a 0x70
    texel that is at most 0x26, and the one doubling pass in the composite
    (measured: 0x10110D became 0x20221A exactly) takes it to 0x4C. The
    reference's walls are near 0x90. **So the missing brightness is in the
    compositing passes, not in the geometry, the textures or the lighting**
    -- the pixel watch shows the wall then tinted by an alpha blend, a
    reverse-subtract against a fixed colour, and that doubling. The oracle
    for those is `gpu/commands/blend` (128 lines) and `blend565` (140).

    One obstruction to note: `gpu/commands`'s own harness does not survive
    our GE. Its tests draw a row of small boxes and read them back at the
    end, and a full-screen black clear appears between one box and the next,
    so only the last box is still on screen when the readback happens --
    `cull` reads 0x00000000 where hardware has 0x001f1fff, and `light` reads
    black or white for all but its first box. The first box is drawn
    correctly and exactly (`ffff00`, hardware's value), which is what makes
    the lighting above measurable at all. The mechanism behind the extra
    clears is not identified yet; until it is, those two tests are not
    usable as oracles beyond their first reading.

32. **Into a mission: the white screen was a decoder default** (2 Sep, night).
    Sif played past everything anyone had recorded -- name entry, main menu,
    garage, mission select, a sortie -- and recorded it: `scenarios/
    mission-1.pad`, 108 seconds of play, 1,751 polls, the file M2's gate has
    named since the roadmap was written and nobody had made. The screencast
    showed what was wrong: every 2D screen right, and every 3D scene either
    empty (the garage's AC panel) or flat white (the mission -- 96% pure
    `FFFFFF`, 80 distinct colours in the whole frame, the dialogue box
    rendering correctly on top).

    The obvious reading was "3D geometry is dropped", with skinning as the
    prime suspect: `vertex_layout` refuses weighted vertex types and discards
    the draw. It was not asserted, and the first headless replay said why
    not: **no dropped-geometry line at all**, and 1.16 million textured 3D
    draws, 21.7 billion textured pixels. The scene was being drawn, heavily.
    The white was on top of it.

    The pixel watch (`PSPRECOMP_PIXWATCH`) at (240,100) read the last frame
    like a script: particles with a 32x32 texture, two additive passes, a
    pass sampling the *other framebuffer* as a texture (a bloom), building
    the pixel to about 0x62 -- then

        2d: sprites  x -80..560 y -184..456  vaddr 004E0A9C
            rgba FFFFFFFF  vtype 800102  tex no

    an untextured full-screen sprite, alpha 0xFF, alpha-blended: opaque
    white. Its alpha was 0xFF in all 1,103 frames it was drawn, so not a
    slow fade but a stuck one. And `vtype 0x800102` says why: **bits 2..4
    are zero -- the vertex has no colour field.** The 0xFF was never the
    game's; it was `read_vertex`'s default for a colourless vertex,
    `0xFFFFFFFF`.

    Hardware does not default to white. A colourless vertex takes the
    **material ambient colour and alpha**, registers 0x55 and 0x58 -- which
    is what PSPSDK's `sceGuColor` writes (it is `sceGuMaterial` with every
    component selected). That is how a game animates a fade without touching
    a vertex: one register per frame, then a quad with no colour. Both
    registers were already decoded for lighting; the fix is a
    `current_colour()` that both vertex paths default to. The mission
    renders: night sky, smoke, a lit horizon, 1,160 distinct colours where
    there were 14.

    Two workflow lessons the same evening. The first replay ran thirty
    minutes for a 108-second recording: I chose the bound by guessing rather
    than from the number in the file, and the scenario had no `stop`, so it
    ran on for 70,000 polls drawing white after the last input at 1,716. It
    now stops at 1,790 and takes 87 seconds. And the mission costs 56 ns a
    pixel, 16x slower than real time -- your worry about the software
    renderer, quantified; the docs' M5 conclusion stands, and it is separate
    from every correctness question above, which is why a white screen was
    never going to be "the renderer is slow".

33. **The dark hangar was a collapsed rotation: `vidt` took the wrong lane**
    (3 Sep). The hangar rendered nearly black (mean 10) behind a correct 2D
    menu, and the mission ran dark (mean 38 against ~100) with no lights on.
    Blending was suspected and acquitted: the pixel watch traces every
    compositing pass arithmetically correct, the vertex colours and textures
    are genuine game data (PEEK, and WATCHMEM shows the tile colours are
    never rewritten), and the scene is frozen, not fading (1,070 extra polls
    reproduce the pixel exactly). The `gpu/commands/blend` oracle did find
    real bugs on the way -- the doubling factors saturate instead of
    doubling (codes 6-9 clamp at 255), products round where hardware
    truncates, and the stencil byte is unmodelled -- but all are ±2 LSB or
    alpha-channel, not a 9x darkness. The `[r]`/`[x]` line prefixes are a
    scheduling artifact (did the reschedule thread run?), not renderer
    output; strip them before diffing.

    The cause was upstream of every pixel: per-object world matrices arrived
    as `[X,X,X,T]` -- three identical columns, rank 1 -- which folds the room
    onto a line and mangles every normal it touches. The uploader
    (`002B752C`) is faithful (WATCH shows healthy sources uploading
    healthy), the composer chain multiplies healthy inputs, and the seed it
    all reduces to is a scratchpad matrix reading `[(1,0,0,0)x4]`. That
    scratchpad is staged by three rotation builders (`002B0020/007C/00D8`),
    each verified healthy in turn -- until the second, whose output already
    has column 1 equal to column 0. Its rows are built by `vrot` (correct)
    and `vidt`, and `vidt` took bits 6-7 of vd as the lane, which reads 0
    for every lane register the builders use (v0..v3). So two of the four
    rows came out `(1,0,0,0)` and every rotation collapsed. The lane is
    vd & 3 -- the element field this file's own register addressing uses
    everywhere else -- under which all three builders produce orthonormal
    X/Y/Z rotations (checked by hand, including the look-at builder's
    homogeneous row at vd=39). No pspautotests suite covers `vidt`
    (vector.prx dies at line 4452 of 5329, before the vrot cases, so both
    were unverified); the game was the only oracle that could say it.

    With one line changed the hangar renders -- walls, grating, doorway,
    light shafts -- and the mission     reaches mean 84 with 4,200 colours: mech, buildings, smoke, lit horizon, chatter box on top. M1's gate reproduces
    exactly (18,006 lists, stop at 6000, 0 bad) behind a wider drain, the
    title runs are bit-identical in lists, the sweep holds at 137 with no
    per-test movement, and the oracle sample agrees 316/0. What is left is
    in the passes, not the geometry: fog is decoded and counted but never
    applied (the mission's far field wants its light-blue fog), and the
    blend-factor findings above. The ghostly foreground mech is depth func 1
    (always) meeting submission order, not missing geometry.

34. **The shards in the hangar: the near plane was cut after the divide**
    (3 Sep). With the rotations fixed (item 33) the option menu's hangar
    rendered its right wall and, where the floor should be, black with a
    few dark triangles the size of the screen -- one of them wearing the
    floor's hazard stripe. The mission's ground had the same disease. The
    summary already said where: 117,060 vertices a run "all behind the
    eye" and 73,404 "outside the guard band", in a room of 917,000, and
    the draw log put the camera inside the geometry. The wall pieces are
    16-bit positions (vertex type `0x13D`) filling a ±1 cube under a
    world matrix of scale 17 and a translation 12-17 units away, so every
    piece has vertices behind the eye, and the game submits them
    unclipped: its own VFPU clipper (the one M1 fixed) is not run for the
    room.

    The clipper from item 29 divided by w first and cut z/w >= -1 in NDC.
    That reading of `gpu/clipping` is not wrong about anything the test
    measures -- every mixed-w vertex it poses sits at z = -w exactly, on the
    plane, so cutting `z + w = 0` in clip space before the divide gives the
    identical forty values, and "Linear W 1->-1->-1" lights the same
    16,384 pixels either way because (-w,-w,-w,w)/w is one point for either
    sign of w. What the test cannot pose is a vertex behind the eye off the
    plane. Divided, it lands mirrored through the screen centre, and for
    the game's projection (near 1, far infinite: z' = -z - 2, w' = -z) its
    z/w is 1.4 -- inside the range, so the NDC cut kept it and the triangle
    drew as a shard, or more often reached the guard band and was dropped
    with the rest of the floor. In clip space z + w is, for any standard
    projection, an affine function of eye z that is positive in front of
    the near plane and negative behind the eye, so one Sutherland-Hodgman
    pass on that plane handles both; attributes lerp with the same t,
    which is exact for the cut vertex. A vertex that passes with w < 0 is
    still divided, as hardware does -- that is what the "1->-1->-1" row
    measures -- and "all w <= 0 draws nothing" stays as a separate rule,
    since the test's degenerate projections need it and a real one never
    triggers it.

    With the cut moved, the hangar has its floor grid, hazard stripes,
    pillars and doorway, laid out as in the PPSSPP frame at t=12 s of the
    capture, and the same run reports 7,944 vertices cut at the near plane
    (was 84) and 12,300 added by splits (was 4,780). `gpu/clipping` still
    matches on every value: `guardband` exactly, `homogeneous` modulo the
    `[r]`/`[x]` prefix from item 33. Lists and commands are unchanged
    (1,296 / 1,613,365), as they must be -- this touches nothing the game
    can observe. The mission gets its ground back the same way: the frame at
    the first chatter box has continuous sand to the horizon, the camp's
    tents, trucks and soldiers, the smoke column and the sky, laid out as
    in the PPSSPP frame at t=77 s -- and the translucent AC across the
    foreground is in the reference too, the game's own effect, not a depth
    bug. Still missing there is the light-blue fog. The three replays are
    unchanged in what the game does: title-idle 5,406 / 874,060,
    skip-intro 7,506 / 2,280,256, mission-1 5,376 / 19,007,445 with
    `stop` at 1790, all at 0 bad accesses; M1's gate holds at `stop` 6000,
    18,006 lists, 6,003 finishes, 0 bad (305 s of raster, so give it a
    drain of several hundred seconds); ctest 13/13.

    The sweep holds at 137 MATCH with one row moved: `utility/msgdialog/
    dialog.prx` went from DIFFER 139 to NOOUTPUT, and NOOUTPUT is the
    verdict to distrust. It is a host segfault -- `psp_read32` handed a
    non-null pointer for guest `0x09FEF9F4`, a heap address just under the
    top partition, from `exec_simple` on a guest thread -- deterministic at
    the full budget, and it reproduces on the fork's committed HEAD with
    every uncommitted change stashed, so it is neither the clipper nor
    item 33's work. It is not new either: the 2 Sep commit records the
    same DIFFER-to-NOOUTPUT move, and item 33's sweep had it back at
    DIFFER, so the crash comes and goes between runs.
    `cpu/vfpu/vector.prx` dies the same way at 800M (the "line 4452"
    death), and under gdb it does not crash but parks with two guest
    threads alive after the 10 s wait, so a race is in it somewhere. Both
    are one bug to chase in `psp_mem_ptr`'s bounds against the interp's
    mapping, and it gates the only oracle for `vidt`/`vrot`.

    Two traps: `scripts/07-autotests.sh` must be given an **absolute** test
    directory -- a relative one silently reports every test as NO OUTPUT and
    empties its `reports/07-*.got`, which is how this session lost the
    previous vector.prx comparison -- and a `git stash` in the fork to
    bisect leaves HEAD's binaries in `build/` after the pop; rebuild before
    measuring anything else.

35. **vector.prx was never dying on an instruction: the interp's drain
    deadline, and a teardown that raced the threads it gave up on** (3 Sep).
    Item 34 left `utility/msgdialog/dialog.prx` and `cpu/vfpu/vector.prx`
    segfaulting the interpreter in `psp_read32` and guessed at
    `psp_mem_ptr`'s bounds. Wrong guess. Every backing store there is a
    plain calloc, so a pointer inside RAM can only fault after the RAM is
    freed -- and the run's own stderr had said when: "guest threads still
    running after 10s; 2 alive, not waiting further". The interp's
    `psp_sched_drain` gives the test's threads ten wall seconds after
    module_start returns, sized for the sweep's per-test timeout, and on
    the deadline it takes the token back by fiat and returns. The thread
    that held it does not know; between firmware calls nothing looks at
    the token, and a VFPU test between two printfs has no firmware calls
    for seconds. main.c then printed its summary and called
    `psp_mem_free`, and the thread's next load was from freed memory. The
    guest's output file is written in chunks, so the crash left vector.prx
    cut mid-line at 4452 of 5329 -- which the 07 runner's own comment, and
    item 33, had read as an unimplemented instruction past that point.
    Under gdb the timing shifted and the thread parked first, which is
    why the crash "came and went".

    Three pieces. `psp_os_thread_join` in os.c, both halves, since the
    threads were always joinable and nothing joined them.
    `psp_sched_join_all` in the scheduler, walked without the lock (the
    threads take it on their way out) over every slot that ever started a
    host thread, `used` or not, and a `psp_sched_stopping` flag that
    `psp_sched_stop_all` raises. The interpreter polls that flag once per
    instruction, after the budget check, and returns a new `I_STOPPED` --
    or `I_EXIT` if the guest itself asked -- so a stop reaches a thread in
    pure computation within the instruction rather than at its next
    firmware call; the nested-run bookkeeping counts it as a finish, not a
    failure. main.c stops and joins after printing its summary, so the
    "still alive" list on stdout still names who was alive, and before
    the free. The boot host is untouched: recompiled code has no poll, its
    threads make a firmware call every few microseconds anyway, and the
    window there was never wide enough to hit. `07-autotests.sh` now
    passes `--drain 600` (DRAIN_S): at the real budget a test is bounded
    by its instructions, not the clock. The sweep keeps the 10 s default
    and its `timeout 25`, and a test that hits the deadline there now
    prints its summary instead of a core.

    cpu/vfpu runs in thirteen seconds, all eight tests to their last
    line. `vector.prx` differs on 16 lines of 5,329, every one of them
    `vasin` in the sixth decimal (0.621185 for hardware's 0.621184 -- an
    approximation the hardware makes that libm does not), and all 64
    `vrot` and `vidt` lines match: item 33's fix is now hardware-verified,
    not just game-verified. `prefixes.prx` is one line, the sign of a NaN.
    `vregs.prx` keeps its 34: `inf` where hardware has 201.001 and the like
    in its "Upgrade" and "Combine" rows, a real bug in something those
    register combinations exercise, not looked at. `dialog.prx` is back
    to its 139-line DIFFER, and that is the sweep's only movement: 137
    MATCH, 3 NOOUTPUT, every other row byte-identical. The hangar replay
    is unchanged to the pixel (1,296 lists, 0 bad).

36. **Fog, and the immediate-mode vertices that let the oracle measure it**
    (3 Sep). The GE decoded FOG1/FOG2/FOG_COLOR since item 33 and counted
    fogged vertices without applying them; the mission's sky and far field
    were dark grey against the reference's light blue. `gpu/commands/fog` is
    the oracle: nine "Common" rows draw with sceGuFog(near, far) at depth
    0, and 256 "Rounding" rows draw a box under every coefficient byte
    with the vertex at 0x881100 and the fog at 0xFF33FF, 768 channel
    values in all. A brute-force search over blend arithmetics finds
    exactly one that reproduces every value: `(c*f + fog*(255-f) + 255)
    >> 8` -- a divide by 256 with a +255 bias, not by 255 in any rounding.
    The distinguishing rows are blue at f=1 (hardware 254, exact 254.53)
    and green at f=6 (hardware 51, exact 50.2): no single rounding of the
    exact value gives both. The Common rows fix the coefficient: `f = (end
    - depth) * range`, depth the eye-space distance (w of the clip
    position under a standard projection), clamped to 0..1 and quantised
    to the byte with 255 unfogged; near == far makes sceGuFog's range 1/0
    and the hardware reads the infinite product as fully fogged whichever
    its sign ("Basic" and "Both neg" both read the fog colour). Applied
    after the texture function and before blending, alpha untouched;
    interpolated across the triangle like a colour channel; through-mode
    and clear-mode geometry carry 255.

    The 256 rounding rows draw through immediate mode -- registers
    0xF0..0xF9, one per vertex component, 0xF7 committing a vertex with
    its alpha in the low byte, the primitive type in bits 8..10 (7 meaning
    the one in progress) and bit 22 saying the fog byte in 0xF8 applies --
    which nothing had decoded, so 256 of the test's 272 values were
    unmeasurable before. Now decoded: screen-space 12.4 positions on the
    4096 grid with OFFSET_X/Y subtracted, 16-bit depth, lists and strips
    and fans, untextured (0xF3..0xF5 are accepted and not applied; the
    game never uses the path). The draw-time state push is factored out
    of draw_prim so both paths share it, and the backend gains set_fog
    for the colour and the enable.

    fog.prx matches hardware on all 272 rows. The mission's end frame goes
    from mean 83 to 92 against the reference's 89 at the same moment, and
    the sky and far field are its light blue (fog colour 2DA8FF, end
    18000, range 1/10000: fog starts 8000 units out). The hangar is
    byte-identical, as it must be -- its fog starts at 2167 units and the
    room is thirty across. Sweep 138 MATCH, fog.prx the only row moved;
    title-idle 5,406, skip-intro 7,506, New Game 18,006 with its stop,
    all 0 bad; ctest 13/13.

37. **The blend arithmetic, and the stencil that is the alpha byte** (3 Sep).
    Item 33 left three findings against `gpu/commands/blend`: doubling
    factors saturating where hardware doubles, products rounding where
    hardware truncates, the stencil byte unmodelled. All three are one
    reading of the test's 64 rows. Every plain factor -- source and
    destination colour, their inverses, the alphas, the fixed colours --
    fits a single term, `((c + 1) * f) >> 8`, and nothing over 255 does:
    "Zero + Inverse src alpha" reads 28 from 64 x 111 (exact 27.86) and
    "Inverse src alpha + Zero" reads 55 from 128 x 111 (exact 55.72), which
    no one rounding of c*f/255 gives. The doubling factors are twice that
    term, clamped: 0xFF707070 under double source alpha reads 0xE0, an
    exact 2x, so the factor was never saturated at 255 as this renderer
    had it. The inverse-doubling ones take 255 - 2a, clamped at zero, as an
    ordinary factor: 0x40808080 reads 0x3F and 0x7FFFFFFF reads 0x01. A
    script over the test's own operand list confirms 0 of 192 channel
    values off under that rule set, then the rasterizer does the same.

    The alpha byte of every blended row reads 0xAA, and 0xAA is the ref of
    the stencil test the harness turns on around each draw: ALWAYS, ref
    0xAA, REPLACE on every outcome. So the stencil is implemented where it
    lives -- the framebuffer's alpha byte, the value this renderer already
    knew an ordinary draw leaves alone -- with the test before the depth
    test, the three operations (KEEP, ZERO, REPLACE, INVERT, INCR, DECR)
    on fail, depth-fail and pass, and only the pass writing colour. The
    GE decodes 0x24, 0xDC and 0xDD. A 5650 target has neither alpha nor
    stencil, and the blend reads its destination alpha as zero: that is
    the 565 variant's last 14 rows, "Double dest alpha" black and
    "Inverse double dest alpha" the source colour whole.

    Both oracles match on every value; the runner still counts them as
    differing on the `[r]`/`[x]` prefix alone, which is a scheduling fact
    (hardware runs the checkpoint helper's equal-priority thread across a
    sceGuSync; this scheduler does not), not a renderer one. The game's
    compositing passes use plain alpha blends and a doubling pass, so the
    corrected doubling reaches the screen. With it the option-menu hangar
    measures the same as the PPSSPP frame at t=12 s: mean 25 against 24
    over the whole frame, 32 against 31 in a patch of the floor -- the
    "nearly black" hangar of items 31 and 33 is closed. The mission's end
    frame is 91 against 89. Sweep 138 MATCH with no row moved; title-idle
    5,406, skip-intro 7,506, mission-1 5,376, New Game 18,006 with its
    stop, all 0 bad; ctest 13/13.

38. **M2's gate: three frames against the PPSSPP capture** (3 Sep). The gate
    asks for frame dumps compared against PPSSPP reference shots at
    matching moments, the way the logo was judged. The reference is Sif's
    capture, `~/Videos/Screencasts/ppsspp-into-mission.webm`, and the three
    moments are the main menu with the AC behind it (t=38 s), the sortie
    launch where the camera holds on the AC in the hangar (t=50 s), and the
    mission at its first chatter box (t=77 s). Ours are headless replays
    with `PSPRECOMP_FRAME`: `main-menu.pad` recut to stop at 748 -- it
    stopped at 810 before, past the cross into MISSION select, and dumped
    the world map over the hangar's edges with no mech to judge --
    `garage.pad`, new, cut from mission-1.pad at 925 in the three seconds
    after the "Commence mission? Ok", and `mission-1.pad` at its stop.
    Side by side, ours left, in `reports/m2-gate/*-ours-vs-ppsspp.png`
    (gitignored with the rest of reports/; the commands above rebuild
    them).

    | frame | ours | PPSSPP |
    |---|---|---|
    | main menu, GARAGE highlighted | mean 36 | mean 36 |
    | garage AC, sortie launch | mean 39 | mean 38 |
    | mission, first chatter box | mean 91 | mean 91 |

    Layout agrees in the garage and the mission: the mech's pose and the
    camera, the crates and floor plates behind it, the camp, smoke and
    sky. In the main menu the hangar, the menu and its caption and the
    AC agree, and the AC's angle does not: the menu's camera orbits the
    mech -- the capture has it front-on at t=35 s and turned away by
    t=41 s -- so the angle is a matter of which second is sampled, and
    the two sessions are not in phase. Brightness agrees to within one
    level of 255 in all three. What differs is texture: the capture has three times our distinct colours on every
    frame (13,700 against 3,600 on the garage), which is the reference's
    video compression on one side and, on ours, affine rather than
    perspective-correct interpolation and no dithering. Neither is what the
    gate measures. Also in the mission frame, the translucent AC across
    the foreground is in both -- the game's own effect, as item 34 noted.

    **The gate passes.** M2's "a mission renders, in software" is met by
    the software rasterizer on 3 Sep. What the comparison leaves for M5 is
    the sampling texture above, and speed: the mission replay rasterizes
    at 58 ns a pixel, sixteen times slower than real time.

39. **The music plays: ATRAC3+ through libavcodec, and the rules the oracle
    gave for free** (3 Sep). M3's first half. FFmpeg's libavcodec is found
    by CMake and linked dynamically when present -- `find_library`, the
    openh264 shape, `PSPRECOMP_HAVE_FFMPEG` -- and `scripts/common.sh` reads
    the CMake cache so the hand-linked hosts agree with it. Its ATRAC3+
    decoder wants a block_align and a channel count and nothing else
    (read from atrac3plusdec.c; plain ATRAC3 wants the fourteen bytes after
    the fmt chunk's cbSize as extradata, and gets them). Planar float out,
    2048 samples a frame, converted to the interleaved stereo the hardware
    always produces.

    The game's own use is the simple one: `GetAtracID(AT3+)`, `SetData` with
    the whole track in one buffer (0x81000 bytes for the title track,
    0xB1800 for the main menu's, and a 14 KB jingle on a second ID at the
    same time), `SetLoopNum(-1)`, then `DecodeData` per frame with
    `GetLoopStatus` after each -- logged with the new
    `PSPRECOMP_ATRAC_LOG=1`. No streaming, no seeking. The first decoded
    audio, dumped headless with `PSPRECOMP_AUDIO_DUMP=<prefix>` (raw guest
    PCM per channel, whatever the game hands sceAudio), is the title
    track's opening fading in: RMS 568, 791, 1210 over its first 1.4 s.

    What audio/atrac's `.expected` files fixed on the way, each a rule this
    file's stand-in had wrong or absent:
    - Setting the data decodes the first frame at once. The stream's first
      frame is encoder warm-up and output begins 368 samples into the
      second ("firstValidSample: 0970" = 2416), so the first DecodeData
      answers 1680 and the total is the fact chunk's first word exactly:
      247,501 for sample.at3 as 1680 + 120 x 2048 + 61, the 61 being the
      last decode (replay.expected). decode.expected's "Drained in 121
      calls" follows once ResetPlayPosition(0) lands the same way.
    - `GetSoundSample`'s end is that total less one; the smpl chunk's loop
      points read back 2048 lower (a loop written at 0 reads -2048).
    - IDs come in pairs by codec: 0 and 1 for ATRAC3+, 2 and 3 for ATRAC3
      (ids.expected). Handing out the lowest free ID regardless made
      setdata's "Unallocated (1)" allocated.
    - AddStreamData on a buffer that holds the whole file answers
      80630009, even for zero bytes; a streamed file that has all been read
      keeps answering OK to zero-byte adds (replay.expected).
    - Streaming is a ring with frames contiguous in it -- the lap ends at
      the last whole-frame boundary, a partial frame's head moves to the
      ring's start, and the free run is reported from the write position
      to the lap's end or to the decoder's next frame. Read off
      stream.expected's numbers (0x34 / 0x1E8 after a 0x4000 load, 0x350
      after the first wrap); the game never streams, so this is for the
      oracle and for a track that does not fit.

    The suite also wanted its data files: every test does
    `fopen("sample.at3")`, which iofilemgr resolves under the working
    directory's `disc/`, and the runners never put anything there -- so
    every audio/atrac test had been failing on the open, and the sweep's
    numbers for the whole directory measured that. Both runners now link
    the test's directory into `disc/`. With that and the rules above:
    decode, setdata, addstreamdata and atractest match on every value;
    getremainframe and getsoundsample differ only on sceAtracReinit's
    states and a streaming seek; the seek tests (resetting, reset2,
    resetpos) want GetBufferInfoForResetting, not modelled; stream.prx
    stops at once on `_sceAtracGetContextAddress`, which it uses to print
    the hardware's own context structure -- the layout is in the suite's
    atrac.h, and a synthetic one is the next oracle to turn on. In the
    sweep's own terms (raw lines, prefix and all, at its reduced budget)
    the directory reads addstreamdata, atractest and replay MATCH, decode
    8, setdata 6, getremainframe 30, getsoundsample 10 -- measured by
    sweeping the audio subtree alone; a full sweep run while another
    session was rebuilding the tree still showed the old numbers for these
    rows and is not trusted here. The map file keeps the full sweep's rows
    until the next quiet full run.

    Two things on the host side. `present.c` was queueing every channel
    into one SDL stream in turn, so music and effects together would have
    played as alternating blocks; it is a mixer now, a ring per channel
    summed in the device callback, volumes applied on the way in, each
    channel paced by its own depth (half a second). And
    `sceAudioSetChannelDataLen` -- called by this game before nearly every
    output -- was accepted and ignored, so a channel whose count the game
    changed was read at its reserve length; it and ChangeChannelConfig are
    real now.

    A caution on this session's regression numbers. Every replay made
    after the decoder went in reports one GE list per frame where the
    rows say three -- the hangar at 432 or 433 against 1,296, title-idle
    at 1,803 against 5,406 -- with commands within a tenth of a percent,
    finishes within one, and the hangar frame byte-identical. That is not
    the audio: the fork's ge.c carried another session's uncommitted
    change throughout (lists counted once at enqueue rather than once per
    stall-resumed run), and the builds here picked it up. Nothing in the
    audio path touches the GE. The rows keep their counts until that
    change lands with its own measurements; what this session vouches for
    is 0 bad accesses and every stop reached on title-idle, skip-intro,
    main-menu, garage, mission-1 and New Game, and the hangar frame.

40. **The movie has a voice: sceMpeg's audio through the same decoder**
    (3 Sep). Sif: "cutscene audio doesn't appear to play, but as soon as
    I make it to the title menu and in the hanger it does". The intro is
    a PSMF, and its sound is ATRAC3+ inside the program stream's private
    stream 1 -- a path sceMpegGetAtracAu and sceMpegAtracDecode had
    answered with a running clock and silence since there was no decoder
    to hand the frames to. With libavcodec linked for item 39 there is.

    The layout, read off the game's own intro with a temporary dump of the
    first audio PES payloads: each begins with a 4-byte private header --
    the substream number, two bytes, and the offset of the first frame
    header in the payload -- and then ATRAC3+ frames back to back, each
    behind 8 bytes: `0F D0`, two bytes of parameters, four zero bytes. The
    parameters are the OMA/AA3 container's: three bits of sample-rate
    index, three of channel configuration, ten of payload size in 8-byte
    units less one. This intro's `0F D0 28 5C` is 44.1kHz, stereo, 744
    bytes, and the next PES's fourth byte, 0xF3, is exactly where the
    fourth frame lands when frames are 752 bytes with their headers --
    the check that settled whether the size counts the header (it does
    not) and that frames span PES boundaries (they do). The demuxer now
    keeps the audio substream the way it keeps the video, one buffer of
    payload with the private headers taken off; GetAtracAu walks it by the
    `0F D0` headers, copies the whole frame into the caller's ES buffer
    and advances the audio clock one frame; AtracDecode reads the frame
    back from that buffer, opens the decoder from the frame's own
    parameters, and writes 2048 stereo samples. A frame that has not
    arrived is NO_DATA -- the "go round again" the header comment on this
    file documents, which now means exactly that, since the frame will
    come with the next ring-buffer put -- and once the ring has
    short-delivered and the frames are gone, the stream is over.

    The first three seconds of the intro come out of a headless
    skip-intro replay: RMS rising 0, 2022, 2557, 12071 and a zero-crossing
    rate of 0.09-0.22, which is music, not noise. The WAV is what Sif
    hears. title-idle, which lets the intro run, decodes 41.5 s of it
    without a gap, and both title-idle and skip-intro land on their
    baseline command counts to the digit (874,060 and 2,280,256) with 0
    bad accesses. The decoder wrapper moved out of atrac.c's statics into
    four shared calls (`psp_at3_open/decode/flush/close`) so the two
    users have one copy.

    Sif then heard popping through the intro. The dump acquits the
    decode: across the 893 frame seams of the full intro the sample jump
    is the same as inside the frames (median 111 against 109, 99th
    percentile 1974 against 2045), two blocks repeat and six are silent,
    all near the start. So the gaps are the host's. The movie's sound
    thread is paced by the movie's own clock, not by the mixer's backlog,
    so its ring hovers near empty and a push a few milliseconds late is a
    gap filled with silence. The mixer now holds each channel until it
    has a 4096-frame pre-roll, 93 ms, and waits for one again after
    running dry, and it prints per-channel pushed, dropped and underrun
    counts when the window closes -- the number to read after the next
    windowed run.

    Sif read it: 67 underruns in 29 s of movie, and still popping. So the
    gaps were bigger than a pre-roll, and headless with
    `PSPRECOMP_REALTIME=1` -- the guest clock on the wall clock, no window
    -- reproduced them through a new line in the boot summary that times
    each channel's outputs on the host clock: the movie channel's longest
    wait between two outputs was 1.8 s and 102 of them arrived later than
    the buffer before them would have played, while the effects channel
    never waited more than 52 ms. The token was not held that long by
    anyone; the movie's sound thread was waiting on its own. The census
    said for what: 85 million sceMpegGetAtracAu calls and 232 million
    sceMpegRingbufferAvailableSize calls in 75 seconds. The sound thread
    was spinning on NO_DATA and the reader thread on a ring that read as
    full, because free packets were computed from the *video* decoder's
    progress alone -- and the audio is interleaved ahead of the video in
    the file, so whenever the video fell behind real time the reader
    stopped putting and the audio ran out. Every refill was a pop.

    A packet is now freed once its faster consumer is past it: each put
    records where the two elementary streams stood, the decoders' read
    positions map back through that to ring bytes, and the held count is
    what the further-along decoder has not taken; one packet stays held
    until the stream is over, since the game reads an entirely free ring
    as the movie's end. The same run then reads: longest wait 62 ms, none
    late, and the GetAtracAu spin gone from the census. The reader
    thread's polling of AvailableSize remains (284 million calls) -- that
    is the game's own loop, and it is hot on hardware too; a
    lower-priority spinner costs this scheduler nothing it would otherwise
    use. Headless and unpaced the whole intro still decodes, 41.9 s in 903
    blocks with the six silent ones at its start; title-idle lands on its
    baseline command count and skip-intro within 261 commands of its own
    (2,280,517 against 2,280,256 -- the reader now puts on a different
    cadence), both at 0 bad accesses; and the video test rows of the sweep
    do not move.

41. **The intro's sound and picture: three clocks, three faults** (3 Sep).
    Sif, after the pops were gone: "it appears that it's out of sync with
    the video". Headless with `PSPRECOMP_REALTIME=1` and two new lines in
    the boot summary -- the movie's audio clock against its picture clock
    at each decoded frame, and each stream's stamps against the wall clock
    between its first and last fetch -- the intro read: picture 49.0 s of
    stamps over 67.8 s of wall clock, sound 64.4 s over the same, and the
    audio clock 6 ms ahead of the picture at the first frame and 6 s
    ahead by the end. Three things, each its own fault.

    The picture clock was invented. The demuxer discarded the PES
    timestamps and stamped every picture at 25 fps from its frame count --
    a comment in the file said so -- and the intro is 29.97 fps, so its
    picture clock ran 12% slow against a sound clock that was right, and
    the player, pacing on the stamps, showed each frame later than its
    sound. The PES timestamps are kept now: a 33-bit field in the PES
    header when its flag says so, and only 125 of the intro's 12,000 video
    PES carry one, so each is anchored to where in the elementary stream
    its PES began, a picture whose data holds an anchor takes it, and the
    pictures between anchors are spaced by the frame duration measured
    from the first anchor to the latest (33.40 ms; two adjacent anchors a
    dozen frames apart gave 35.6). The audio's first stamp seeds the
    audio clock, which then advances 4180 ticks a frame exactly, since
    every ATRAC3+ frame is 2048 samples. The stream begins at 90000 and
    85069, one second in, which is what a PSMF does.

    The sound was throttled to the wrong thing. Under real-time pacing the
    picture fetched at 72% of real time, and the reason was in the census:
    284 million sceMpegRingbufferAvailableSize calls, the game's reader
    polling a full ring in a hot loop -- one firmware call each, and each
    a scheduling point, and the host's time went to the loop. A poll that
    finds the ring seven-eighths full now sleeps 8 ms of guest time: the
    same answer later, and the picture fetches at 98.5%. A millisecond
    was tried and cost 1.5% in thread handoffs; half a frame was tried and
    starved the ring. And the mixer let a channel run half a second ahead
    of the speaker, which was half a second of latency between a picture
    and its sound, since the player takes the sound thread as its clock;
    hardware's blocking output lets two buffers queue, and so does the
    mixer now, with the pre-roll as the floor. The headless model of the
    speaker had the same defect the other way -- a flat wait of a buffer
    per call ran the sound at 91% -- and is a virtual speaker now, drained
    at 44.1kHz, the call waiting only for the excess over two buffers.

    That left the picture at 98.5% of real time against sound at 100%,
    and the lead climbing -8 ms, 550, 737, 908, 1007, 1085, 1196 across
    the intro -- which Sif watched and saw. (An earlier reading of that
    first step as the player's own audio pre-buffer was wrong: with the
    drift closed below, the lead stays near zero from the first picture,
    so there is no pre-buffer. The 550 was the same drift, steeper while
    the decoder and the first draws were cold.)

    **A late picture is dropped rather than shown.** The sound plays at
    its own rate whatever the host does, so the only way to hold them
    together on a machine that is 1.5% short is to give the picture less
    to do. sceMpegGetAvcAu now decodes and discards a picture that is
    already late, and the game draws the next one instead -- a trade that
    is lopsided in our favour, since decoding a 480x272 frame costs about
    2 ms and drawing it about 6. Measured against the lead at the first
    picture rather than against zero, with two frames of slack so
    ordinary jitter drops nothing, and at most two in a row so a much
    slower machine would show a slow picture rather than a frozen one.
    Only under real-time pacing: an unpaced replay has no real time to be
    late against, and dropping there would make a replay depend on how
    fast the host is.

    Two runs, with PPSSPP and a browser running beside them: 45 and 44 of
    about 1,635 pictures dropped, 2.7%, and the lead now reads -8, 23,
    -6, -21, -15, -30, -12 ms across the same seven samples -- flat
    within a frame instead of climbing to 1.3 s. Both streams fetch 54.6 s
    of stamps over 54.2 s of wall clock. Unpaced, title-idle, skip-intro
    and the hangar keep their exact baseline command counts, so the
    replays that matter are untouched.

42. **The end of a movie: the ring has to read empty, or the player never
    finishes** (3 Sep). Sif watched the intro to its end and the game hung
    there and never came back. The first suspicion was the frame dropping
    of item 41, since that walks the elementary stream ahead of the
    player -- and it was wrong. Three controls, each hanging identically
    at the same poll: dropping switched off with the new
    `PSPRECOMP_MPEG_NODROP=1`; an unpaced replay, where dropping never
    runs at all; and the fork's own mpeg.c from before dropping existed,
    swapped in and built. The hang was there all along and nothing had
    ever reached a movie's end to find it.

    What it looks like: eight guest threads alive, the module's main
    thread blocked on `sceKernelWaitThreadEnd`, everything else in a
    timed wait, and one thread spinning -- 850 million
    `sceMpegRingbufferAvailableSize` calls in a run that never left the
    movie, with pad polls stopped for three minutes.

    Reading the spinning thread in the emitted C settles the mechanism.
    Its loop exits only when its next step returns non-zero, and that
    step first asks a small helper for a free frame buffer -- a
    produced-minus-consumed count over a pool -- and returns zero
    without doing anything when the pool is empty. So it never reaches
    the `sceMpegAvcDecodeStop` that would report no frames left and let
    it exit. The pool is empty because the player's display side has
    finished and will not hand a buffer back; the player has finished
    because the movie is over. Its main thread waits for that spinning
    thread to end, and so the whole game stops.

    What the player wants first is an empty ring. Item 40 gave the ring
    an occupancy derived from what the decoders had consumed, with one
    packet always held so the game could not read an entirely free ring
    as nothing buffered -- correct during playback, and a deadlock at the
    end, because the player stops fetching about three seconds before the
    data runs out (the stream's tail is padding it does not want), so
    those last packets are never accounted consumed. The rule now: hold
    by consumption while the file is still arriving, and once it has been
    fully delivered the ring is empty. That is also true -- the ring is a
    transport, and everything it ever carried is on our side by then.

    With it the run goes on past the movie: 8,480 pad polls where it
    stopped at 4,770, GE work from 2.3 to 4.8 million commands, the spin
    gone from the census (255 thousand semaphore signals against 294
    million). The intro plays, ends, and the game carries on. Unpaced,
    title-idle, skip-intro, the hangar and the garage all keep their
    exact baseline command counts at 0 bad accesses; ctest 13/13.

43. **M4 reviewed, and the gate met by the game rather than the suite**
    (3 Sep). The savedata work came from the parallel session; this is what
    re-measuring it found.

    The suite half stands up. Of `utility/savedata`'s fourteen tests, eight
    are byte-exact with the timing prefix stripped -- autosave, deletebroken,
    deletedata, deleteemptyfilename, loadbroken, loademptyfilename, makedata,
    saveemptyfilename -- which is exactly the eight its commit message
    claims. The six that differ break down as stated there: `getsize` (6
    lines) and `sizes` (16) report the host's disk where hardware reports a
    16GB card, so those lines track whichever machine runs them; `idlist` (2)
    and `filelist` (1) write `idList.resultCount` and `bind` at a different
    point in the sequence; `secureversion` (684) measures PGD crypto that was
    deliberately not attempted. No regressions anywhere: title-idle, the
    hangar and the garage keep their exact baseline command counts at 0 bad
    accesses, ctest 13/13.

    One difference is not environment. `loaddata` (10 lines): when
    `param.saveName` is empty, hardware falls back to the first
    `saveNameList` entry and writes `TEST99901ABC`, while this writes to the
    bare `TEST99901`. It bites only a caller that leaves the name to the
    dialog, which may be no one here, but it is a behavioural gap rather
    than a measurement artifact.

    **The gate.** Both halves turned out to be the game's own. Sif saved from
    the garage in a windowed run, and the card holds
    `ms0:/PSP/SAVEDATA/NPUH10024ACLRSAVELIST00/` -- `SAVEDATA.BIN` 28,316
    bytes, a `PARAM.SFO` with the correct `\0PSF` magic naming the title,
    the pilot and the AC, and the game's own 22K icon and 198K background --
    and a later launch loaded it and came up in the hangar. Nothing
    automated covers this: across the garage, a complete new game and a
    whole mission, the only savedata call a replay makes is the boot
    free-space query, because `garage.pad` starts a new game every time. The
    write path had never run under any test until Sif played it.

    Two things found while looking. `ms/` was not gitignored, so the first
    real save dropped a quarter-megabyte of the player's data into the
    working tree -- now ignored, on the same grounds as `game/`. And the
    roadmap's "no dialog UI" line was read as settling slot selection, which
    it does not: see M4 there for what one slot actually costs and the
    deferred fix.

44. **SAS: the two calls the game makes, and the 32 samples before a voice
    starts** (3 Sep). M3's last piece. The import census names the gap
    exactly: of 27 `sceSasCore` functions the module imports, 25 were
    implemented and two were not -- `__sceSasSetVoicePCM` and
    `__sceSasGetAllEnvelopeHeights` -- so both returned zero from the
    unimplemented path and the game's PCM voices played nothing at all.

    `__sceSasSetVoicePCM` is raw signed 16-bit samples rather than ADPCM,
    and its refusals are all signed comparisons (pcm.expected): a size at or
    below zero or above 0x10000 samples is 0x8042001A, a loop position at or
    past the size is 0x80420015 -- which lets -1 and even 0x80000001 through
    while turning 0x40000001 away -- and a null address is accepted and
    plays silence. `__sceSasGetAllEnvelopeHeights` writes exactly 32 entries;
    the test reads the 33rd back as the 0xCCCCCCCC it seeded.
    `__sceSasSetSL` sets the sustain level rather than being accepted and
    ignored. And keying on a voice that is already on is refused with
    0x80420016, where key-off alone does not clear the state: keyon.expected
    refuses it after a key-off and a pause, then accepts it after one core,
    so what clears it is the release actually reaching zero.

    **A voice does not start when it is keyed on. It starts 32 samples
    later.** Three measurements, two of them independent of the third.
    keyon.expected reads the envelope as 0 before a core and 0x60000 after
    one, which at rate 0x1000 is 96 steps of a 128-sample grain -- 32 short.
    getheight.expected reads 0x1e0000 after four such cores, which is
    96 + 128 + 128 + 128: the same 32 missing once, not once per core. And
    pcm.expected's rendered output settles it from the other side, with the
    sample the voice starts from landing at output index 32 and the loop
    point arriving 32 samples late to match. So both the sound and the
    envelope are held for 32 samples after key-on, together.

    With those: `getheight` is byte-exact, `keyon` 8 lines to 2, `keyoff` 8
    to 6, `pcm` 44 to 10, `vag` 330 to 308, `setadsr` 390 to 378. The game
    keeps its exact baseline command counts on title-idle, the hangar, the
    garage and New Game -- the last of which is the run that once hung on a
    menu sound -- at 0 bad accesses, with the effects channel carrying
    signal throughout; ctest 13/13.

    `adsrcurve` rose from 1,916 lines to 2,032, and that is the curves, not
    a regression: with PCM voices actually playing, sections of it that used
    to produce nothing now produce numbers from the wrong curve. What is
    left there is the four non-linear modes, and a start on them: the two
    linear modes are confirmed as plus or minus the rate per sample;
    EXPONENT_REV is refused outright for attack with 0x80420013; and
    LINEAR_BENT does **not** fit the obvious model of full rate to
    three-quarter height and then a quarter of it. Its twelfth core, from
    0x2E000000 with rate 0x100000 over 64 samples, moves 0x028C0000, and no
    integer split of 64 samples between those two rates produces that
    number -- the closest, 32 and 32, gives 0x02800000. The rule is
    something else, and the sections at rate 1 are where it will show.

45. **The ADSR curve modes: the rule that decides them, and how far the
    shapes are derived** (3 Sep). `__sceSasSetADSRmode` names one of six
    curves for each of attack, decay, sustain and release, and had been
    accepted and ignored. What it accepts is now measured and enforced, and
    the shapes themselves are derived here as far as the corpus takes them.

    **Which curve a phase will accept comes down to the curve's parity.**
    setadsr.expected sweeps every mode against every phase, and the matrix
    is exact: the even curves -- linear increase 0, bent 2, exponent 4 --
    are the rising shapes, and only attack and sustain take them; the odd
    ones -- linear decrease 1, exponent-rev 3, direct 5 -- are the falling
    shapes, and only decay, sustain and release take them. Sustain takes
    either, since it may go up or down. Anything above 5 is refused, and so
    is any bit outside the low three and the sign: 0x80000001 is accepted as
    mode 1 while 0x40000001 is not, which is what says the mask is
    `~0x80000007` and not simply a range. Everything is checked before
    anything is stored. That is 0x80420013, and it takes setadsr from 378
    differing lines to 304 and adsrcurve from 2,032 to 1,988, with the game
    unchanged on every replay.

    **The shapes, from adsrcurve.expected** (grain 64, so a core is 64
    samples and the first after a key-on is 32 -- item 44). All six are
    stepped now, and 48 of the file's 53 sweeps come out byte-exact.

    - Linear increase and decrease are plus or minus the rate a sample.
    - Bent is the rate below three-quarter height and a quarter of it above.
      Exact on every core of its sweeps but the one where it crosses:
      hardware moves 0x028C0000 there and this moves 0x02800000, and no split
      of 64 samples between those two rates gives hardware's number, so the
      crossing sample does something one measurement cannot name. The 0x43
      it leaves behind is carried to the end of that sweep.
    - The rising exponent, mode 4, is **0x4000 a sample plus the room left
      scaled by the rate**: `0x4000 + ((MAX - h) * rate >> 32)`. The fixed
      part is why a small rate climbs in a straight line -- rate 0 and rate 1
      both step exactly 0x4000, with no curvature anywhere in their sweeps --
      and the scaled part is what bends the large ones. Exact on all twelve
      attack sweeps, from rate 0 to 0x7FFFFFFF.
    - The falling exponent, mode 3, is the height scaled by the rate and
      **rounded up**: `ceil(h * rate / 2^32)`. From the top at rate 9
      hardware steps 3 a sample where truncation gives 2; a product that is
      exact keeps its value; and rounding up is what stops a small height
      from never falling. Exact on twelve of the fourteen decay sweeps.
    - Direct jumps to where the phase ends. Attack refuses it, and the
      falling exponent, by the parity rule.

    **The naming trap that cost the most time.** The test calls its decay
    sweeps "Decay exponent", and the mode they pass is not EXPONENT. It
    cannot be: the parity rule refuses mode 4 for a decay, and the sweeps
    have data rather than refusals. Mode 4 is the *rising* exponential and
    mode 3, named EXPONENT_REV, is the falling one -- which is what the
    parity rule was saying all along. Reading the section titles as the mode
    names left the decays stepping linearly and looking like an unexplained
    curve mismatch.

    **Two invented defaults, removed.** Keying a voice on used to default its
    attack rate when it was zero, and keying it off used to default its
    release rate. Neither is hardware's: adsrcurve's rate-0 attack sweep
    climbs at exactly 0x4000 a sample under the exponent curve and not at all
    under linear increase, which is what a voice with no rate should do. The
    unit test that asserted audio after a bare key-on was asserting that
    default rather than the mixer, and now sets an envelope first.

    **Where it lands.** adsrcurve goes from 1,988 differing lines to 24, and
    from 12 of its 53 sweeps matching exactly to 48; setadsr from 378 to 304.
    getheight stays byte-exact and keyon, keyoff, pcm, vag and pause are
    unmoved. The game is unchanged on title-idle, the hangar, the garage and
    New Game -- exact baseline command counts, 0 bad accesses, and the same
    audio out of every channel -- and ctest is 13/13.

    What is left in this file is the bent crossing above, and four
    high-rate decay sweeps that differ only where the height meets the
    sustain level: hardware's step carries it to 0 and this one stops at the
    level the test set, which is 1. Both are single-sample boundary rules
    that one more measurement each would settle.

    A note on the metric, since it misled once: the line count is a poor
    measure here. An exponential prints a change line every core where a
    linear one prints none, so a near-miss can score far worse than a gross
    miss -- an earlier pass of this work read as a threefold regression while
    being strictly closer to hardware. Count the sweeps that match exactly.
