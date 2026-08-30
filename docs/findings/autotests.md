# A behavioural oracle: pspautotests through the interpreter

Written as scaffolding, not as a working harness — the difference matters and
is stated up front.

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
`allegrexrecomp interp`, which loads any ELF/PRX, applies relocations, binds
import thunks to HLE, and runs from the module entry under an instruction
budget. Reports land in `reports/07-<name>.txt`.

You build the tests yourself (`pspdev/pspautotests`, your own toolchain) and
point the script at the directory. Nothing prebuilt is fetched, nothing built
from them is committed — the same policy as game data, for the same reason.

## What it cannot do yet, and the milestones

Stated plainly, because a harness that silently runs less than it looks like
it runs is the whole class of bug this project keeps writing down:

1. **A test's main thread never runs.** The standard crt's `module_start`
   creates and starts the main thread through HLE, and the interpreter does
   not yet service HLE re-entry into guest code — it counts it (`re-entry:` in
   the report). Today a test exercises its startup path and nothing else.
   *Milestone: run the thread entry the way the boot host's scheduler does.*
   The scheduler exists in the runtime (`sched.c`) and the boot host proves it
   works; the interpreter does not use it yet.
2. **No output capture.** A test prints through `sceKernelStdout`/`printf`,
   which reaches stderr via the fd-1 path in `hle_Write` (and via the async
   path since patch 0020). The script's report captures interpreter stdout,
   and the guest's own prints are interleaved in it. Comparing against
   recorded real-hardware output comes after milestone 1.
3. **No expected-output database.** Some tests need hardware recordings;
   those come from whoever has the hardware, test by test.

The honest first measurement this scaffold supports: point it at any built
test ELF and read the `re-entry:` count — that number is how much of the test
never ran, and it is the size of milestone 1.

## Not this tool's job

- Building the PSP toolchain or the tests. `pspautotests` upstream documents
  that; it is out of scope here.
- Recompiling the tests through the emit pipeline. That is the eventual shape
  (the differential oracle ran the same module both ways), but milestone 1
  comes first: if a test cannot run under the interpreter, recompiling it
  proves nothing about the environment.
