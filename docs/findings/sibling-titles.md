# Sibling titles: Armored Core 3 Portable and Silent Line Portable

Measured 7 Sep 2026, on the pipeline as of that day (fork at the submodule's
recorded commit, Last Raven unchanged by the work below). Both discs are Sif's
US PSN dumps in `~/Downloads/Armored Core PSP/`, hard-linked into
`games/ac3p/` and `games/acsl/`.

## Verdict

Both titles go through every stage unmodified and boot: a blind probe
(`scenarios/<slug>/probe.pad`, start/circle/cross round-robin every hundred
polls) carries each from the logo through its title screen and NEW GAME to the
game's OPTION screen in 3,000 polls, headless, with the software rasterizer,
**0 bad memory accesses, no VFPU traps, and the same three advisory firmware
calls missing that Last Raven lacks**. Frames: `reports/<slug>/frame.png`
(the OPTION screen, red for AC3 and purple for Silent Line) and
`reports/<slug>/frame-best.png` (the fullest presented frame: Silent Line's
title screen with PRESS START; AC3's title logo mid-fade). Since then (8 Sep)
a blind route reaches AC3P's first sortie headless, and Last Raven's four
control laws — the stick converter, the yaw integrator, the look controller
and the walk's push — are ported to both titles in one shared header and
measured on AC3P (the sections below), then confirmed on Silent Line
against Sif's recording of its whole first mission (9 Sep). Sound, movies
and saves are unmeasured.

## The discs

| | Last Raven | AC3 Portable | Silent Line Portable |
|---|---|---|---|
| DISC_ID / version | NPUH10024 1.00 | NPUH10023 1.01 | NPUH10025 1.00 |
| SKU / firmware / devkit | US PSN / 5.50 / 0x05050010 | same | same |
| ~PSP module name | `ACLR_App` | `Module_name` (SDK placeholder) | `Module_name` |
| decrypt mode / tag | 9 / 0xD91612F0 | 9 / 0xD9160BF0 | 9 / 0xD9160BF0 |
| elf size | 4,192,292 | 3,212,228 | 3,464,380 |
| text (`.text` bytes) | 3,032,208 | 2,356,400 | 2,510,248 |
| bss | 2.79 MB | 3.81 MB | 3.67 MB |
| entry | 0x00253210 | 0x001C8768 | 0x001E65EC |
| extra PRX modules | none | none | none |
| disc | 160 files: AC.BIN 226 MB, 129 AT3, 18 PMF | 82: ac3data.bin 171 MB, 20 AT3, 27 PMF, 21 Shift-JIS `.ini` + `parts.txt` | 61: AC3DATA.BIN 189 MB, 26 AT3, 21 PMF |

`tools/pspdecrypt` already carries the key for tag 0xD9160BF0
(`PrxDecrypter.cpp:477`); stage 01 decrypts both and the size check against
the header's elf size passes.

## Stage by stage

Last Raven's column is its phase-0 row (`phase0.md`) where the metric is a
phase-0 one, and today's `reports/` otherwise.

| Metric | Last Raven | AC3 Portable | Silent Line Portable |
|---|---|---|---|
| 02 decode coverage | 758,052 words, 0 unknown | 589,100 words, 0 unknown | 627,562 words, 0 unknown |
| 02 discovery: instructions / vfpu / invalid | 741,848 / 331 (0.04%) / 213 | 594,754 / 206 (0.03%) / 165 | 635,949 / 217 (0.03%) / 164 |
| 02 computed-jump sites unresolved / tables | 0 / 55 → 615 targets | 0 / 30 → 294 | 0 / 13 → 658 |
| 02 functions touching the VFPU | 127 (0.5%) | 56 (0.5%) | 54 (0.4%) |
| 03 imports: need / have / miss | 218 / 207 / 11, 25 libraries | 210 / 196 / 14, 26 libraries | 210 / 196 / 14, 26 libraries |
| 04 emit: functions / interior entries / imports | 15,850 / 43,324 / 196 | 11,804 / 34,264 / 190 | 13,392 / 35,476 / 190 |
| 04 emit: VFPU traps | 272 of 761,712 (0.04%) | 206 of 594,754 (0.03%) | 217 of 635,949 (0.03%) |
| 04 lines of C / -O0 build + link, wall | 2.1M / ~45 s | 1,602,069 / 33 s | 1,717,379 / 36 s |
| 04 probe executable | 25 MB | 20 MB | 21 MB |
| 06 constructors (`.cplinit`) | 151 | 117, 0 undiscovered | 138, 0 undiscovered |
| 06 no input, no decoder, 120 s | movie gate | 339 polls, 342 lists, 0 bad | 333 polls, 336 lists, 0 bad |
| 09 probe, decoder on, 3,000 polls | — | 3,003 lists, 14,258,104 cmds, 0 bad, 60/60 events | 3,003 lists, 14,247,587 cmds, 0 bad, 60/60 events |
| 09 probe: unimplemented calls hit | — | 8 (3 NIDs, below) | 8 (same) |
| 09 probe: disc bytes read | — | 1,466,368 | 1,685,504 |
| 09 probe: raster | — | 2.90 G pixels, 61.8 ns/px, 179 s | 2.26 G pixels |
| 09 probe: lighting | — | 421,508 lit vertices, 3 lights | same shape |
| where the probe ends | — | OPTION screen | OPTION screen |

Both new modules import exactly the same 210 functions from the same 26
libraries, and emit the same 190 import thunks: they are the same engine
build, one year older than Last Raven's.

## The import gap

The 14 missing NIDs are Last Raven's 11 plus three. Names by SHA-1 of the
candidate, verified:

| Library | NID | Function | Hit in the probe |
|---|---|---|---|
| sceUtility | 0x2A2B3DE0 | sceUtilityLoadModule | 5× (returns 0 — advisory) |
| scePower | 0xEBD177D6 | scePowerSetClockFrequency | 2× |
| sceImpose | 0x36AA6E91 | sceImposeSetLanguageMode | 1× |
| sceUtility | 0xE49BFE92 / 0x2AD8E239 / 0x9A1C91D7 / 0x95FC253B / 0x67AF3428 / 0x34B78343 | UnloadModule, MsgDialog Init/GetStatus/Update/Shutdown, GetSystemParamString | no |
| ModuleMgrForUser | 0x8F2DF740 | sceKernelStopUnloadSelfModule (the host's exit path, `boot.c`) | no |
| sceOpenPSID | 0xC69BEBCE | sceOpenPSIDGetOpenPSID | no |
| **InterruptManager** | 0xCA04A2B9 / 0xD61E6961 / 0xFB8E22EC | sceKernelRegisterSubIntrHandler / ReleaseSubIntrHandler / EnableSubIntr — **new**, not imported by Last Raven | no |

The three InterruptManager functions are the one thing these titles ask for
that Last Raven never did. Nothing called them in 3,000 polls of menus; a
later state (a mission, a movie) that registers a VBLANK sub-interrupt and
waits on it would stall on a zero return. Worth knowing before chasing a
hang. The HLE table has room (340 of 512 registrations used).

## What the pipeline needed

Nothing in the toolkit; the per-title state was all in this repo and is now
keyed on `GAME` (README, "Another title"): a five-line profile in
`scripts/games/<slug>.sh`, a per-title `games/<slug>/`, `build/<slug>/host`
and `reports/<slug>/`, and an empty `host/replace-<slug>.txt` +
`host/replacements-<slug>.c`. With `GAME` unset every path is what it was:
after the change, `BOOT_NO_RUN=1 scripts/06-boot.sh` relinked Last Raven and
all 16 objects and both binaries hashed identical to before, and
`scripts/09-replay.sh --decode scenarios/garage.pad` reached its stop at
925 polls, 928 lists, 0 bad accesses.

The window title follows the profile (`-DGAME_TITLE` into `host/present.c`).
The launcher (`host/launcher.c`, `scripts/15-settings.sh`) takes one `--game
SLUG|TITLE|BOOT|MODULE[|ISO]` per built title and shows them as tabs; the
chosen slug is saved as `game=` in the presets file (`host/settings.c`, an
optional header key, older files load unchanged) and `--select SLUG` or
`GAME=<slug>` opens on a given one. Presets are shared across titles on
purpose: window mode, display, renderer and the keyboard layout apply to
every game, and the control-scheme options are inert where no replacement
exists. The preferences directory stays `Last Raven` so nobody's saved
presets move.

Controller: the modern layout (Controller preset, or "Controller layout:
Modern") sends A/B/X/Y, the bumpers, triggers and stick clicks as carrier
bits (`host/controls.h`) that only Last Raven's replacement of its button
converter reads, so on the new titles only the d-pad and Start arrived.
`host/present.c` now asks for `lr_modern_controls_available`, a weak symbol
that `host/replacements.c` defines as 1 and the per-title stubs as 0, and
refuses the modern layout with a note on stderr when it is absent or zero:
the pad speaks classic PSP buttons whatever the preset says. Verified with a
dummy display: AC3P prints the fallback and opens the pad with PSP buttons,
Last Raven keeps the modern map.

Known and left: `scripts/14-aspect-tests.sh` is a Last Raven
fixture and now refuses other slugs; `host/render_gl.c`'s body-font heuristic
is Last Raven's and simply does not match another font.

## Porting Last Raven's replacements: what is shared

Measured with `scripts/fn-twins.py` (7 Sep): each function Last Raven
replaces, matched against every function of the other title by instruction
sequence with numbers masked.

| Last Raven function | AC3P | Silent Line |
|---|---|---|
| 00279A10 PSP buttons → game buttons (16 insns) | 001ECE44 identical | 0020CE44 identical |
| 00279A50 stick → virtual bits (26) | 001ECE84 identical | 0020CE84 identical |
| 00279648 pad adaptor (68) | 001ECA80 identical | 0020CA80 identical |
| 00279910 circular deadzone (helper) | 001ECD44 identical | 0020CD44 identical |
| 00255DE4 sceCtrlPeek caller (32) | 001C9B38 0.96 | 001E7F14 0.96 |
| 0007467C / 000746D4 camera lag blends (22/24) | 0.52 | 00025668 identical / 0.91 |
| 00065824 player walk handler (170) | 0.78 | 0.78 |
| 001DFC80 animation updater (458) | 0.74 | 0.64 |
| 0005F348 purge modifier (43) | 0.66 | 0.66 |
| 0005EFA0 / 0005EFD4 action held/pressed (13) | 0.54 | 0.54 |
| 0004F06C the push (38) | 0.58 | 0.58 |
| 000889B4 projection/aspect (164) | 0.41 | 0.41 |
| 0004F248 yaw integrator (228) | 0.32 | 0.33 |
| 00053234 pitch integrator (345) | 0.30 | 0.29 |

The pad layer is one shared library: identical code, and at run time the
same stick threshold T = 100 and deadzone radius R = 30 (AC3P at
0x00463A54/58, Silent Line at 0x005223CC/D0, bss, peeked after a short run).
The two converter replacements port by address alone; their in-play gate
(`in_play()`, Last Raven's AC object) needs a new observable per title.
Everything game-side -- the turn and look laws, the push, the camera, the
key-assign lookups behind the modern pad's semantic actions -- is different
code and needs the same hunt Last Raven had (RAMSNAP, WATCHMEM, the static
call-site scan, `fn-source.sh`), once: 74% of AC3P's functions of 12+
instructions have an identical twin in Silent Line (6,110 of 8,276), against
24% of Last Raven's in AC3P, so a function found in one of the two new titles
is likely findable in the other by fingerprint.

## The converter, ported (7 Sep, late)

Both titles now carry Last Raven's stick converter as a native replacement:
`host/ac3_converter.h`, included by `host/replacements-ac3p.c` (twin
001ECE84) and `host/replacements-acsl.c` (twin 0020CE84), each listed in its
`host/replace-<slug>.txt`. The stick arithmetic it shares with Last Raven
moved verbatim into `host/stick.h` (Last Raven's `replacements.o` and `boot`
hashed identical before and after the move). What the port does, per
`PSPRECOMP_INPUT` / the Control scheme setting:

- classic: the game's own converter, untouched.
- modern: the left stick's X asks to turn from 4% of travel (the game's own
  threshold is 79%) and its Y to walk from 4%.
- dual: the left stick moves as eight sectors (a diagonal walks and strafes
  together through the L/R virtual bits), the right stick's X or recent
  mouse X turns, and the right stick's Y or recent mouse Y looks through the
  triangle/circle virtual bits. Turning and looking stay the game's two-state
  laws: there is no yaw or pitch integrator here yet.

Verified headless on Silent Line with `scenarios/acsl/option-stick.pad`
(`PSPRECOMP_INPUT_LOG` prints each change of the asked-for bits): up 1000,
right 0200 (a strafe, as in Last Raven's dual), a 30° and a 60° diagonal both
1200, right stick right 8000, right stick up 0040; 0 bad accesses in classic
and dual, and identical GE rows (3,723 lists, 17,965,667 commands) because
the option screen ignores the stick bits.

### The in-play gate

Last Raven's converter only re-sources the stick in a mission (its AC
object's movement-state pointer plus a heartbeat), because in a menu an
off-axis push would otherwise ask for L/R and change tabs. Each new title
needs its own observable.

**AC3 Portable: the unit-list head at 0x00490B90.** A blind route reaches its first
sortie (`scenarios/ac3p/probe-mission.pad`: start/circle/cross round-robin to
the option screen, circle -- the key guide's "End setup" -- six downs and
cross, then cross every 80 polls with a circle every 500 and no Start, which
opens the key guide): the corporate menu at poll 5,500, the sortie's opening
cutscene at 6,000-6,500, HUD and control from 7,000, still in play at 9,000
(0 bad accesses). `PSPRECOMP_RAMSNAP` images at 3,000, 5,500 and 6,500
against 7,500, 8,200, 8,600 and 8,900 through `scripts/ram-gate.py` (words
null on one side and one 4-aligned pointer on the other) leave 155
candidates. The first one taken was 0x002C4180, the player's slot in a
per-unit record array (below): null in every menu and through the cutscene,
then one pointer to the last of the four 0x180-byte unit objects the mission
allocates (09B04A60/BE0/D60/EE0 -- the player's). **It opens late.** Sif
played a sortie and found the first seconds on the game's own converter,
switching to dual after moving a little; a run with snapshots on the edge
(`scenarios/ac3p/sortie-edges.pad`) shows the HUD up and the AC under
control at polls 6,950, 7,050 and 7,150 with that slot still null -- the
builder skips a unit until a flag at +142 is raised -- and set only by
9,000 (the first run saw `play=1` at 7,252). The gate is now the head of the
unit-object list the builder itself walks, 0x00490B90: null in every menu,
set from the opening cutscene (poll 6,500; 6,800 in the edge run) onward,
constant through play. Re-sourcing the stick during the cutscene, where
input is ignored, costs nothing; a gate that opens late costs the feel of
every sortie's start. Not measured: whether the head is nulled again after
the sortie ends (the edge run's AC was still standing at 14,000 polls).

Verified with `scenarios/ac3p/mission-stick.pad` in dual (the route, then
holds: left stick up, right stick right, a 45-degree diagonal, right stick
up; `PSPRECOMP_INPUT_LOG` and GE captures before and after each): `play=1`
at poll 7,252, between the cutscene and the HUD; bits 1000 / 8000 / 1200 /
0040 as asked; and the frames show the AC walked down the street, turned to
face the buildings, walked-and-strafed to the wall, and looked up at the
sky. 0 bad accesses, 9,101 lists. Start in a sortie opens the QUIT MISSION
dialog (Abort Mission? OK / CANCEL) and the word stays set there
(`scenarios/ac3p/pause-peek.pad`, `PSPRECOMP_PEEK` at poll 7,800 while the
dialog is up), so the re-sourced stick stays on in that dialog; its choice is
left/right, which the stick asks for in every mode. Not measured: the word
after the sortie ends.

What the AC3P word is: a memory watch on the traced build
(`PSPRECOMP_WATCHMEM=0x002C4180`, `_FROM=6000`, `scenarios/ac3p/gate-watch.pad`)
shows it rewritten every frame in play with the same pointer by the store
helper 000FC98C called from 000FDFA8, a loop over the mission's unit objects
(stride 0x180) that fills a per-unit record array (stride 0x80) whose base
its callers form as 0x002C4000. The gate word is slot 3, the player's -- the
last unit. Null in menus because the array is cleared outside a sortie.

**Silent Line: 0x0047A7D4, by twin.** The record builder's twin, 000E2464
(0.94 by instruction sequence, the same prologue), loads its unit-list head
from 0x0047A7D4 (`lui 0x48; lw -22572`) where AC3P's loads from 0x00490B90
(`lui 0x49; lw 2960`). Null in all thirteen Silent Line menu snapshots
(title, option screen, name entry, prologue, assembly). The earlier slot
derivation stands as a cross-check: Its flow differs -- circle on the
option screen asks to return to the title, the prologue follows, and the
first blind taps end in the garage's ASSEMBLY screen. The functions that
form AC3P's array base (000FD29C, 000FD3CC, 000FD5B0, 000FE27C) have Silent
Line twins (000E16F8, 000E1A28, 000E1C40 identical, 000E271C at 0.91) that
form 0x0043AF00, so slot 3 is 0x0043B080. It and the other three slots are
null in every Silent Line menu snapshot (option screen, prologue, assembly).
Its behaviour in a sortie is inferred from AC3P: three blind routes did not
reach one (the second was diverted into the pilot name entry, where cross
types characters, then a long prologue; the third stayed in the garage's
ASSEMBLY screen, where circle closes the notice rather than leaving, so the
new-game flow seems to want the AC built first). A recorded route settles
it: `GAME=acsl scripts/09-replay.sh --window --decode --record
scenarios/acsl/mission.pad scenarios/empty.pad`, then `PSPRECOMP_RAMSNAP` at a
poll in control and `PSPRECOMP_PEEK=0x0043B080`.

## The yaw integrator, found and replaced (8 Sep)

The hunt was Last Raven's: `scenarios/ac3p/turn-probe.pad` holds the left
stick hard right in the sortie (the game's own two-state turn), RAM snapshots
a poll apart at 7,700..7,703 go through `scripts/ram-diff.py` and a float
scan for the cap-sized step, and a memory watch on the traced build names
the writer.

- **The player's AC object is 0x0046EED0**, the first of the four the record
  builder walks (stride 0x2EC0; the enemy's is 0x00471D90). Its yaw is the
  float at +100 (radians, right positive, wrapped elsewhere), its turn rate
  the float at +168; +180 is the pad word, +1860/+1862 the key-assign masks,
  +7108 a hold flag. A left hold flips the sign of AC0's rate and not the
  enemy's. The stepping word first found, 0x002C4044, is a bearing in the
  radar-style record array, derived from the yaw, not the yaw.
- **The integrator is `psp_func_00106B3C(ac, f12 = accel, f13 = max)`**,
  called every frame by the turn state's handler `psp_func_00102B9C(ac)`,
  which the movement state machine (`psp_func_00105A60`, `*(ac+3448)`) enters
  only while a turn bit is set. The handler zeroes the rate on its first
  frame, picks the turn animation, and passes accel and twice a half-cap
  from the parts block at `*(0x004F0394)` (+2940, +2944). The law, from the
  listing: a turn bit held → rate ± 4·accel toward that side; neither → decay
  by 4·accel and snap to zero; clamp to ±max; unless the hold at +7108 is set,
  store the rate and add it to the yaw; return the turn's sign. Measured: 0,
  0.0073, 0.0146, 0.0220, 0.0293, 0.03658 rad/frame held — 0.42°/frame² for
  five frames to 2.10°/frame, Last Raven's numbers to the third digit.
- **Silent Line's is `psp_func_00009D80`**, the one its turn handler's twin
  (000006F4) calls with the same arguments; it has Last Raven's structure
  instead: `pressed(pad, 3)` / `pressed(pad, 2)` through `*(*(ac+8892)+4)`, a
  state byte at `(*(ac+8888))+340` that must be −1, the hold flag from the
  getter `psp_func_0000AF40`, the same +168 and +100. Its player AC is taken
  to be the first of its array, 0x00459B20 (stride 8896) — confirmed on
  9 Sep by Sif's recording of the first mission (the look-and-walk section
  below).

The replacement (`host/ac3_controls.h`, the renamed converter header) is
Last Raven's: for the player's AC in modern or dual, the rate is the stick —
max × the left stick's X through the radial deadzone in modern, max × the
look stick's X through the look curve plus the banked mouse X at the mouse
sensitivity in dual — stored with the yaw unless the hold is set. The
game's state machine still decides when it runs, so classic and the enemy
ACs are the original.

Measured with `scenarios/ac3p/yaw-sweep.pad` in dual (right-stick holds of
100 polls, then a 30-count-a-poll mouse drag), from the integrator's log:

| hold | calls | rate, rad/frame |
|---|---|---|
| right 127/127 | 97 | +0.03658 (the cap) |
| right 64/127 | 97 | +0.00919 |
| right 32/127 | 97 | +0.00295 |
| left 127/127 | 97 | −0.03658 |
| mouse, 30 counts a poll | 37 | +0.0300, the first frame +0.1200 (four banked polls) |

The integrator ran once a frame throughout play (565 idle calls at rate 0
from the cutscene on, so AC3P's idle state calls it too); input reaches it
three polls after the scenario delivers it. 0 bad accesses in dual and in
classic (8,801 lists; classic 77,822,869 commands, dual 76,299,681). Silent
Line's menus under dual: unchanged row, no integrator calls.

## The look and the walk, found and replaced (8 Sep, late)

The same hunt twice more on AC3P — a held triangle (`scenarios/ac3p/look-probe.pad`)
and a held forward stick (`walk-probe.pad`), snapshots a poll apart, a
second-difference scan for the words whose steps grow by a constant, and
memory watches on the traced build for the writers. The watch names the last
function *entered* before a store, which for both was a NaN check the writer
calls first; the writers are its callers.

- **The look's state is a struct at ac+32**: a lockout counter at +4, the
  pitch angle at +16 (radians, up positive), the pitch rate at +32. **The
  controller is `psp_func_00104FC4(ac)`**, once a frame from the player's
  update `00100450`. Its six constants at 0x0025DEE0 are Last Raven's to the
  digit — 27.0 (the lockout after a recentre), 0.00136 accel, 0.009 decel,
  0.027 max rate, ±1.1781 (67.5°) — and it scales the middle three by 60/30
  as it reads them: measured, the rate climbs 0.00272 a frame to 0.054 and the
  angle follows under the clamp. It reads its pad inline: the held word at
  +180 and the pressed word at +184 against the key-assign masks at +1876 (up)
  and +1878 (down); both keys recentre and start the lockout; neither decays
  the rate to zero. Silent Line's `psp_func_000081A4` has Last Raven's shape
  instead — the movement-state byte gate, actions 10/11 through the pad
  helpers `000130B8` (held) and `000130D8` (pressed) — on the same state
  layout, with the same constants at 0x00275B60.
- **The velocity is at ac+144/152, the position at ac+80/88.** Every moving
  state ends in `psp_func_00106EF8(ac, vec, cap)` (Silent Line `00009500`):
  velocity += 4·vec, then the speed is scaled back to `cap` when it passes
  it, or decays toward it by four times a parts constant when the AC was
  already faster. Boosts and jumps reach it through the direction-table push
  `00106D94`, Last Raven's `0004F06C` to the instruction. **The walk does
  not**: its two handlers (one per leg family, identical code) call an
  assembler, `psp_func_0010D5A4` or `0010DF98(ac, direction)`, which takes
  the vector from the *animation* — `000DC628(*(ac+3164), out)` is the root's
  travel between this frame of the playing walk cycle and the next — rotates
  it by the yaw at +100 (`0021A790` builds the matrix from 0x00268800, a VFPU
  `vtfm4` applies it), scales it by the start-up ramp at ac+3060 while that
  is between 0 and 1, and passes its own length as the cap. Measured: the
  velocity's z steps −0.75 then back to −0.1875 the first frame (four times
  the vector, capped to its length; the ramp starts at the legs' 0.15 of the
  cycle's 1.2) and reaches −1.2 within the second. So AC3P walks at the
  animation's speed in the direction the animation was chosen for: eight
  directions, one speed. Silent Line's assemblers are `000193D4`/`00019E24`
  with the animation pointer at +3104, the parts at +1860, the ramp at +3024,
  everything they call identical by fingerprint.

The replacements (`host/ac3_controls.h`, shared by both titles; the addresses,
offsets and two small hooks per title in `host/replacements-<slug>.c`) are
Last Raven's laws:

- **Pitch, under dual**: the right stick's Y is a rate in the game's cap
  through the look curve, eased in at the game's own 0.00272 a frame and
  eased off, reversed or released at once; mouse Y is a displacement at the
  mouse sensitivity. Same three words, same clamp. A held look key or a
  running lockout defers to the original, so the buttons still work and the
  two laws compose. Modern and classic are the original.
- **The walk, under dual**: the animation's vector keeps its length and takes
  the left stick's direction (the game's body frame: x left, z back), and the
  cap is that length times how far the stick is pushed past the deadzone —
  a creep at a nudge, the cycle's speed at the edge, a walk and a strafe one
  push at an angle. Under modern the direction stays the animation's and the
  cap scales by Y alone. The travel, the rotation, the ramp and the push are
  the game's own functions called on the game's own stack frame; only the
  vector's direction and the cap change. A centred stick defers.

Measured with `scenarios/ac3p/look-stick.pad` and `walk-stick.pad` (the probe
route, then the stick in three positions of 100..150 polls each), from the
replacements' logs; 0 bad accesses in every run:

| run | stick | result |
|---|---|---|
| look, dual | right Y −64 (half up) | rate +0.00272 a frame for five frames to +0.01337 (a quarter of the cap through the look curve); angle 0 → 1.1781, the clamp, by ~90 frames |
| look, dual | right Y −128 at the clamp | rate 0, angle held at 1.1781 |
| look, dual | right Y +64 (half down) | −0.01356; angle 1.1754 → −0.1509 in 100 frames |
| look, dual | centred | rate 0 the same frame |
| walk, dual | left Y −60 (half) | m 0.419: cap 0.0786 rising with the ramp to 0.5028, the speed equal to it every frame |
| walk, dual | left Y −120 (full) | m 0.952: cap and speed 1.142; z −1.14 a frame |
| walk, dual | left (−85, −85) | dir (+0.707, −0.707); the state machine switched to the forward-left cycle two frames later (its vector (+0.848, −0.848), the same frame and sign); x +0.81, z −0.81 a frame |
| walk, modern | left (−85, −85) | direction the animation's, m = |Y| 0.674, cap 0.809; the turn bit curves the path |
| walk, classic | `walk-probe.pad` | the module image at poll 7700 byte-identical to the snapshot taken before these replacements; GE 8,001 lists, 66,461,963 commands, as before |

The controller and the pushes ran once a frame throughout (150/100/100/100
log lines a phase); three `orig-centred` pushes follow a release before the
walk state ends.

**Silent Line, confirmed on a recording (9 Sep).** Sif played the whole first
mission in a window and recorded it: `scenarios/acsl/mission.pad`, 5,305
polls from the title screen through NEW GAME, the menus, control from about
poll 690 and the mission's end; `scenarios/acsl/sortie.pad` is its first
1,400 polls with a `stop`, the regular headless route into the sortie. Under
classic the recording replays with 0 bad accesses (14,848 polls before the
10-minute drain, the game left running after the last event), and the
replacements' `orig` lines — 4,488 pitch, 1,355 push — are all for
0x00459B20: the player's AC is the first of the array, as assumed. The
unit-list head at 0x0047A7D4 is null at poll 300 and 0x099DFE70 at 1,500,
3,000 and 4,400. Frame: `reports/acsl/mission-3000-classic.png`. The clipped
sortie under dual: 0 bad accesses, and the four laws fire for the player —
242 yaw calls at max 0.03658 (the right stick centred, rate 0), 751 pitch
lines `dual`, 262 pushes `dual` in the stick's direction and 53
`orig-centred` — so Silent Line's addresses, offsets and hooks are measured
now, not inferred.

## The modern pad, ported (9 Sep, late)

Last Raven's mechanism, on these titles' own pad path (`host/ac3_controls.h`,
the last section). present.c carries the ten modern-pad controls — A, B, X,
Y, the bumpers, the triggers, the stick clicks — in sceCtrl button bits the
game's twelve-entry PSP converter ignores (`host/controls.h`), so recordings
keep them. That converter, `psp_func_001ECE44(state, raw)` on AC3P and
`0020CE44` on Silent Line, is Last Raven's `00279A10` to the instruction and
gets the same replacement: strip the carrier bits, remember their held and
edge state per poll, and outside play alias A/B/X/Y to cross, circle, square
and triangle and either shoulder pair to L/R, so menus navigate as before
(`scenarios/ac3p/modern-menu.pad`: the probe route with every cross tap sent
as A and every circle as B reaches the sortie and walks; 0 bad accesses).

In play the actions are answered by the game's own numbering. The key-assign
row is sixteen 16-bit masks on the AC's *remapped* pad word — at ac+1856+2n
on AC3P (the AC's held word is at +180, its pressed word at +184; measured
by holding each PSP button in the sortie, `scenarios/ac3p/padmap.pad`: cross
lands there as 0x0400, square 0x0800, triangle 0x0100, circle 0x0200, L
0x0040, R 0x0080, the stick's right 0x2000 and left 0x8000 — the row's masks
for boost, arm R, the look, the strafes and the turns exactly — while the
d-pad, select and start leave nothing in it even a poll into the hold, so
the d-pad's own path into the weapon-change, arm-L, OB and extension
actions is elsewhere and unmeasured; the injected masks reach them all the
same), and at cfg+1792+2n through the pad object on Silent Line, Last
Raven's layout. The two titles differ in how the game asks, so
there are two ways in:

- **Silent Line** asks through two helpers, held `psp_func_000130B8(pad,
  action)` and pressed `000130D8`, Last Raven's shape: they are replaced to
  answer 1 for the player's pad (`*(*(ac+8892)+4)`) when the action's carrier
  is down or was pressed this poll.
- **AC3 Portable** reads the masks inline, in two dozen handlers, so there is
  nothing to replace per query. The per-frame copy that lands the AC's words,
  `psp_func_00100894(ac, word)` — a ring of eight past words at +192 indexed
  by +190 and the delay at +188, then held at +180 and pressed = held &
  ~previous at +184 — is replaced to OR the action's own mask from the AC's
  table into those words afterwards. Whatever button the row binds an action
  to, that mask is what is set: independent of the key assignment by
  construction.

The numbering is Last Raven's up to the look, and measured here by injecting
each action in the sortie and capturing the frame after
(`scenarios/ac3p/modern-buttons.pad`; `reports/ac3p/modern-buttons-grid.png`,
nine frames): 5 lifts the AC on its thrusters (boost), 6 drops the rifle's
count 0200 → 0199 (arm R), 4 makes the back unit the selected weapon (change
weapon), 7 swings the laser blade (arm L/event), 8/9 are the strafes and
10/11 the look. Above that the AC3 generation is its own: there is no inside
button (AC3 cycles insides as weapons) and no view-reset action — the reset
is triangle and circle together, the look controller's own recentre path; 13
is the extension (default d-pad right), 14 and 15 share one mask and are
OB/EO (default d-pad left: the OB indicator lights on either), and 12 is the
"dump" of the purge chord (L, R, triangle and circle held). The game's own
default layout, from its promotional key-assign screens as reported on
GameFAQs, agrees: d-pad up change weapon, down fire L unit/event, left OB/EO,
right extension, square fire R, cross boost, L/R strafe, triangle/circle look.

The layout, then: **RT arm R, LT boost, RB change weapon, LB arm L/event, B
view reset (both look actions at once, the game's own recentre and lockout),
L3 extension, R3 OB/EO; A, X and Y have no in-play meaning** (A and B are
cross and circle in menus). Last Raven's action 16 (the disarmament modifier)
and its A (inside) have no counterpart; the purge chords are not bound.
Measured on the final build (`modern-buttons.pad` under dual, 0 bad
accesses): every carrier logs its action for exactly its polls; B at poll
8,000 takes the pitch from the 1.1781 clamp to 0.0000 on that poll and
starts the lockout (5 frames left at 8,010, `orig-button` while it runs;
`reports/ac3p/modern-B-reset-8020.png`); R3 lights the OB indicator
(`reports/ac3p/modern-R3-ob-8170.png`). The classic walk's module image at
poll 7,700 is still byte-identical to the snapshot taken before any of these
replacements, and Silent Line's clipped sortie under dual is unchanged by the
gate (play from poll 651, the laws as before).

**The in-play gate, revised.** The unit-list head alone is not enough for the
modern pad: on Silent Line it stays set through the mission's results and
the menus after them (Sif's recording: set from the sortie to its end at
5,305 polls) while the player's per-frame update stops at poll 5,137 — and
Sif's cross to leave the results is at 5,225. `in_play()` is now the list
*and* Last Raven's heartbeat, the poll at which the player's AC last ran its
update, which the look controller records because that update calls it once
a frame; within two polls counts. So the face buttons go back to cross and
circle the moment the AC stops being updated.

## The adaptive aspect, ported (10 Sep)

Sif hit this as a bug before it was a feature: after the first AC3P mission the
garage and assembly previews rendered without their AC, a sliver of the mech
showing past the right edge of its panel. The cause is that the settings file
is shared by every title (`SDL_GetPrefPath("", "Last Raven")`, one folder),
so a preset saved with **Aspect = Match window** on Last Raven reaches AC3P —
where nothing widened the camera. The GL backend's wide mapping spreads guest
x by `wide_w / 480` because Last Raven's camera replacement compressed a wider
view into those 480 columns; with no such replacement the scene simply
stretches, and the assembly's inset cameras, which are placed by the centered
menu mapping only when their scissor matches their viewport
(`host/render_gl.c`, `menu_preview`), leave their panels.

Two changes.

**The gate.** `present_adaptive_aspect()` now asks a weak
`lr_adaptive_aspect_available`, as `gamepad_modern()` asks about the modern
pad: a title with no camera replacement keeps the original PSP view and says
so once on stderr. A preset saved on one title can no longer distort another.

**The replacement.** All three titles build their projection through one
19-instruction routine — Last Raven `psp_func_0025879C`, AC3P `001CC4DC`,
Silent Line `001EA920`, identical instruction for instruction — which converts
the camera's field of view at +192 from degrees to radians and hands that with
the aspect at +204, the near plane at +196 and the far plane at +200 to the
matrix builder (`002B0420` / `0021AB34` / `0023DAD0`, also identical), which
writes the projection into the camera at +128. The camera is three 4x4
matrices then those four floats.

It was found by scanning the emitted code for functions holding both pi
(0x40490FDB) and 180.0f, and confirmed by a memory watch on the module
camera's projection. The camera objects themselves were found by scanning a
RAM image for 4x4 blocks with the projection's zero pattern and −1 at [1][1]:
AC3P's module camera is 0x0045BEC0 and its sortie camera was in the heap with
a 30 degree field of view, near 6, far 4000 and the aspect 480/272; Silent
Line's module camera is 0x00474700.

The replacement lends the camera a **wider aspect** for exactly that one call
and puts its own value back before returning, so the vertical field of view is
untouched and a wider drawable shows more to the left and right instead of
stretching. Only cameras at the native 480/272 are widened; the assembly's
part and AC previews project at their panel's aspect and keep it. At the
native ratio the widened value is exactly the original, so enabling the option
without resizing stays on the original path bit for bit. Last Raven does the
same thing one level up, in its own camera rebuild, because its display
cameras cache width/480 and height/272 beside the aspect; the AC3 generation
has no such fields, so the projection call itself is the place.

Verified headless, where no drawable means no widening and every path is the
original: AC3P's classic walk leaves the module image at poll 7,700
byte-identical to the snapshot from before any of this work; Silent Line's
clipped sortie under dual is unchanged (242 yaw calls, 751 pitch lines, 262
pushes); Last Raven's garage replay still reads 928 lists and 11,012,854
commands. 0 bad accesses in all three. The widened view itself needs a window
and is Sif's to try.

## The chase camera's lag, ported (10 Sep)

Both siblings keep Last Raven's camera struct field for field, in a table in
module memory — AC3P at 0x00497800, Silent Line at 0x00474100, stride 256.
The eye is at +16, the pitch at +32, the yaw at +36 (the atan2/asin of target
minus eye), the look-at target at +48, and the two blend ratios the lag is
made of at +104 for the eye and +108 for the target. Both titles ship 0.900
and 0.821 against Last Raven's 0.830 and 0.831.

Finding it took the long way round and one real mistake. A RAM snapshot's
offset 0 is `PSP_RAM_BASE` = **0x08000000**, not 0x08800000; addresses derived
with the wrong base are 8 MB high, so a memory watch on them reports nothing
and reads exactly like "the heap moved". Corrected, the camera's eye was in
the heap, and the chain up from it — the view-matrix builder, its caller
reading `*(0x0026BA98)`, and the matrix compose above that — ended at the
per-frame update, which is where the table's address is formed.

That update (AC3P `psp_func_0011B440`, Silent Line `00025268`, the same
function to 0.82 by instruction sequence) takes a camera index and an output
matrix, dispatches on the camera's mode byte at +0 through a handler table,
and hands the eye and target the handler produced to the installer that
derives the yaw, the pitch and the view matrix. Mode 0 is the chase camera
(AC3P `0011C320` — which is also what the fingerprint tool guessed for Last
Raven's `000757E8`, at a weak 0.49 — and Silent Line `000263C4`). It does not
go where the camera should be: it blends `current + (ideal - current) * r`,
with r read from those two fields. Last Raven's is the same law in two small
helpers, so it could replace the leaves; the AC3 generation inlines the
arithmetic into the handler and leaves nothing small to replace.

So the replacement takes the update instead and lends the camera the wanted
ratio in its own two fields for the duration of the call, restoring the
game's values afterwards — the same borrow-and-restore as the adaptive aspect.
`PSPRECOMP_CAMERA_LAG=<r>` sets r under `modern` or `dual`: 0.90 is the game's,
0 fixes the camera to the AC. It is deliberately narrow: camera 0 only, in
its chase mode only, and only while the player is being played, so the garage
and the cutscene cameras keep the game's own feel.

Measured on AC3P with `scenarios/ac3p/look-stick.pad` under dual, 0 bad
accesses in every run. The gate lands where it should: the override is off
through the sortie's opening cutscene (polls 5,616 to 6,815, when the camera
is in another mode) and on from 6,816, covering the whole stick window. With
the setting left at Game every one of 2,435 camera lines reads the game's
0.900; at 0.3 the camera's pitch crosses zero in five frames against the
game's eight, and after twelve frames it has travelled 0.127 radians against
0.062.

The classic path is untouched: AC3P's classic walk still leaves the module
image at poll 7,700 byte-identical to the snapshot from before any of this
work.

## The walk cycle's cadence, ported (10 Sep)

The walk animation plays at one speed, and the analog push above walks at any
speed, so a creep slides its feet: the cycle was authored for the cap. Last
Raven fixes that by holding a per-slot playback *step* at zero on chosen
frames. The AC3 generation has no such step. It keeps **one time counter for
the whole skeleton** — the u16 at +204 of the pose array `*(anim + 160)`,
where `anim` is the AC's animation object at ac+3164 (Silent Line ac+3104).
It advances by exactly one a frame, and the pose is sampled at twice it:
`psp_func_000DC628`, the same routine the push already reads its root travel
from, looks up 2t and 2t+1 and takes the difference.

Found by diffing three consecutive walk frames of the object for a word that
steps by one, then watching that word to name the animation updater (AC3P
`psp_func_000DD29C`, Silent Line `00099E38`, the same function to 0.90 by
instruction sequence), which advances the counter and poses all 36 bones from
it.

The replacement reads the counter before that call and writes it back
afterwards on the frames an accumulator fed with the stick's magnitude says to
hold. The cycle then advances m frames per frame on average — one stride per
stride's worth of ground. Only a value the game itself wrote is ever restored,
so there is no wrap to handle. Which pushes are walks needs no guessing here:
the two walk assemblers are the only callers of the push replacement, so the
note is taken there, where Last Raven has to recognise its walk by the return
address into a shared push helper.

Measured on AC3P with `scenarios/ac3p/walk-stick.pad` under dual, 0 bad
accesses, the counter's advance rate against the stick's magnitude:

| stick | magnitude | counter advance, per frame |
|---|---|---|
| half forward | 0.419 | 0.42 |
| full forward | 0.952 | 0.96 |
| full diagonal | 0.954 | 0.96 |

At full deflection the accumulator carries almost every frame and the cycle is
the game's own; at half it holds 83 frames of 140. The classic path is
untouched: the module image at poll 7,700 is still byte-identical to the
snapshot from before any of this work.

## Commands

```bash
ln "/path/to/Armored Core 3 Portable.iso" games/ac3p/
export GAME=ac3p            # or acsl
scripts/00-identify.sh && scripts/01-extract-decrypt.sh && scripts/02-analyze.sh
python3 scripts/03-imports.py | tee reports/$GAME/03-imports.txt
scripts/04-emit-build.sh
scripts/09-replay.sh --drain 120 scenarios/empty.pad
scripts/09-replay.sh --decode --env PSPRECOMP_FRAME=reports/$GAME/frame.ppm scenarios/$GAME/probe.pad
scripts/06-boot.sh --window       # the windowed look
```

Two replays of each game ran concurrently with the other game's, so the
drain-bound no-input rows are host-timing dependent; the probe rows end at the
scenario's `stop` and are trajectory counts.

## Trying it

The launcher's Controller and Mouse & Keyboard presets use the dual control
scheme, which on these titles now means: the left stick walks where it points
as fast as it is pushed, the right stick or mouse turns and looks at rates
in the game's caps, and the pad's buttons are the modern layout (RT fire, LT
boost, RB change weapon, LB left arm, B view reset, L3 extension, R3 OB/EO;
triangle and circle still look the game's own way). Classic keeps every game law;
modern lowers the left stick's thresholds and makes its X a turn rate and its
Y the walk's speed. Headless:
`GAME=ac3p scripts/09-replay.sh --decode --env PSPRECOMP_INPUT=dual ...`.

A windowed replay or recording (`scripts/09-replay.sh --window --record`)
runs with the settings' defaults, and RENDER=auto resolves to the software
rasterizer at the window's size in real time: Sif's Silent Line recording
averaged 24.5 polls a second over 216 s (`reports/acsl/09-empty-20260909-190211.txt`,
`effective: renderer=software`), the slowdown felt during play. The launcher's
presets take the OpenGL path; for a replay, `--env PSPRECOMP_RENDER=gl`.

Aspect = Match window now works on both siblings (the section above). Anything
saved before 10 Sep that turned it on for a title without the replacement was
distorting the 3D; the option refuses itself on such a title now.

## Not measured

Sound (SAS ran: `__sceSasCore` 6,608 calls; no sink headless), whether the
opening PMF decoded (the probe's start taps skip it), saves, what clears
the unit-list head after a sortie (the heartbeat covers the gate meanwhile),
the purge chords and the AC3P results screens under the modern pad, and
windowed play under the new laws.
