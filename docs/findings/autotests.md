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
4. **`matrix` matches hardware exactly.** All 50 lines, byte-identical to real
   PSP output: every `vtfm`, `vhtfm`, `vmmul`, `vmidt`, `vmscl` and transpose
   case. That is the first end-to-end confirmation of anything in the VFPU
   against hardware rather than against ourselves.

   | test | stopped | ours / expected |
   |---|---|---|
   | `matrix` | ExitGame | **MATCHES hardware** |
   | `prefixes` | ExitGame | 26 / 26 lines, 38 differ |
   | `vregs` | ExitGame | 30 / 48 lines, 42 differ |
   | `gum` | `mtv $a1, v52` | 2 / 45 lines |
   | `colors` | `.word 0xD480000C` | 1 / 5 lines |
   | `vavg` | `.word 0xD0470480` | no output |
   | `convert` | budget | 1 / 108 lines, 99.3M bad accesses |
   | `vector` | budget | 1045 / 5329 lines |

   Getting there took three bugs, each invisible to the differential oracle
   because the oracle runs the same decoder and the same `psp_vfpu_*` helpers on
   both sides:

   - **`lv`/`sv` decoded the wrong register field.** The arithmetic ops carry
     `vt` as a contiguous 7 bits at 22..16; load/store cannot, because bits
     22..21 are the top of the *base register*, so it puts `vt`'s low five bits
     at 20..16 and its top two at 1..0. Reading it as the contiguous field mixes
     the base register into the register number -- `lv.q R000, 0($a1)` becomes
     0x60 instead of 0x20, the same matrix and column at row 2 instead of row 0,
     four lanes rotated by two. Which registers were affected depended on which
     base register the compiler happened to pick, which is why it looked erratic.
   - **A matrix register names a sub-matrix.** `M022` is the 2x2 at column 2,
     row 2. `matrix_cols` built a synthetic register from the matrix and
     transpose bits only, forcing both offsets to zero, so every matrix op wrote
     the top-left corner whatever register it was given.
   - **`vmmul` and `vtfm` indexed their matrix operand transposed.** `vmmul`
     reads its first operand transposed *twice* -- the assembler sets the
     transpose bit in the encoding, and the hardware then indexes by
     [output row][summation] -- and the two cancel. Honouring only the encoded
     bit gives the product of the transpose: 7621 where hardware gives 1585.
     `vhtfm` was not implemented at all; it has no opcode of its own, being
     `vtfm` with the vector one element narrower than the instruction's order.

   **This was also the logo bug.** With the load/store field fixed the game's
   `FROM SOFTWARE` logo moved 68 pixels right and is now centred (x 69..417,
   midpoint 243 on a 480-wide screen). The earlier note about a 68 in `m[9]`
   instead of `m[12]` was a symptom of lanes rotated by two, seen from the far
   end of the chain -- which is why chasing the write that produced it never
   reached a cause.

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
