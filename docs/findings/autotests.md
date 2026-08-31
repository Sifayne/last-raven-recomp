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

   | test | stopped | ours / expected |
   |---|---|---|
   | `matrix` | ExitGame | **MATCHES hardware** |
   | `gum` | ExitGame | **MATCHES hardware** |
   | `prefixes` | ExitGame | 26 / 26 lines, **1** differs |
   | `vregs` | ExitGame | 30 / 48 lines — stale test data, see 9 |
   | `colors` | `.word 0xD480000C` | 1 / 5 lines |
   | `vavg` | `.word 0xD0470480` | no output |
   | `convert` | budget | 1 / 108 lines |
   | `vector` | budget | 1045 / 5329 lines |

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

   - The four lines containing `inf` come from `funNumbers[10]`, which is
     `1e+07` in `vregs.prx` and `100.0f` in `vregs.cpp`. The test converts its
     fill values to half-precision at run time, and `float_to_half_fast3` quite
     correctly clamps 1e7 to half-infinity -- we execute that conversion right,
     instruction for instruction. Patch that one word in a copy of the binary
     and all 30 lines we emit match hardware byte for byte.
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

   Worth keeping as method: the same two moves settled this and the gum bugs.
   Replay the sequence in isolation and see whether it agrees with hardware,
   which separates "this function is wrong" from "this function is being told
   the wrong thing"; and where the disagreement is in the data, patch the datum
   in a copy of the binary and re-run, which turns a hypothesis into a
   measurement.

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
