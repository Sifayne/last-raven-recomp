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
2. **The tests run and print nothing.** This is now the blocker, and it is
   specific rather than general. All eight `cpu/vfpu` tests load, execute and
   return cleanly — `matrix.prx` runs 574,221 instructions — but not one emits
   a line. From an HLE log of `matrix.prx`, in order: `sceKernelStdin/Stdout/
   Stderr` answer 0/1/2, an **unimplemented `IoFileMgrForUser` NID
   `0x54F5FB11`** is called twice, the display is set up, three bad accesses
   happen (`read32 at 0xFFFF800C`, `read16`/`write16 at 0xAFB40070` — the
   latter is the encoding of `sw $s4, 0x70($sp)`, so a code word is being used
   as an address), the test opens its output file (`sceIoOpen` flags 0x602 →
   fd 3), and returns without a single `sceIoWrite`.

   The uncached mirror is *not* the cause: `psp_mem_ptr` already collapses
   mirrors with `addr & PSP_ADDR_MASK`, so `sceDisplaySetFrameBuf(0x44000000)`
   resolves correctly.
3. **Output comparison exists now.** The script diffs the guest's prints
   against the test's `.expected` and reports `MATCHES hardware`, `differs: N
   line(s)`, or `NO OUTPUT (test ran but printed nothing)`. Today every VFPU
   test reports the last of those, which is an honest reading of a harness that
   is complete except for the tests being able to speak.
4. **The expected-output database is no longer missing.** It ships with the
   tests. `cpu/vfpu/matrix.expected` covers exactly the open question — it has
   explicit `non transpose:` and `transpose:` sections and four `vmmul.q`
   cases — so the moment output flows, the VFPU matrix orientation that
   `vfpu.c` flags as unverified is answerable against hardware.

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
