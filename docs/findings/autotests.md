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
