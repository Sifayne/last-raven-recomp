# PSP save dialogs: implementation plan

Investigated 16 September 2026 against parent `f06468f` and runtime
`tools/psprecomp` at `0c864de`. The shared runtime and host dialog are now
implemented. Validation evidence and remaining player checks are recorded
below. This builds on [M4's save/load foundation](ROADMAP.md#m4--saves--3-sep).

## Finding

The filesystem supports multiple saves already. The missing feature is the
PSP utility dialog that lets the player choose one. Last Raven, Armored Core 3,
and Silent Line all contain list-save and list-load calls and construct a list
of permitted save names. Their list setup selects the latest save as the
initial focus. The previous runtime skipped the UI and ignored that focus.

Changing the default to the newest or first empty slot would not provide the
ability to choose an older playthrough or deliberately overwrite a particular
save. Implement the interactive savedata utility once for all three titles.

## Delivery and code ownership

The first delivery is a shared in-game save/load/delete dialog for Last Raven,
AC3 Portable and Silent Line. It uses the slots each game supplies, reads
existing saves in place, and supports keyboard and controller operation in
both software and GL presentation. Automatic operations remain automatic.

The runtime owns the PSP contract and save operations. The host owns drawing
and physical input. Use the runtime checkout inside this repository,
`tools/psprecomp`, for runtime changes; a separate standalone clone of
[the fork](https://github.com/Sifayne/psprecomp) is not the checkout built by
the current host.

| File or area | Responsibility |
| --- | --- |
| `tools/psprecomp/src/hle/utility.c` | Dialog session, candidate enumeration, initial focus, confirmation state, selection writeback, results and file operations |
| `tools/psprecomp/include/psprecomp/savedata.h` (included by `hle.h`) | Small host-facing snapshot/response interface; no SDL or GL types |
| `tools/psprecomp/src/host/save_dialog.c`, `include/psprecomp/host/save_dialog.h` | Host dialog layout, text/icons, scrolling, input navigation and overlay image; the runtime's optional SDL2 host layer, shared with The 3rd Birthday |
| `host/present.c` / `.h` | Register the host bridge, route SDL input, software composition and dialog teardown |
| `host/render_gl.c` | Upload/composite the overlay on the GL owner thread; preserve game render state |
| `host/boot.c` | Unchanged: a headless host registers no dialog, and the runtime's own diagnostic names each cancelled request |
| `tools/psprecomp/tests/test_savedata.c`, registered in `tests/CMakeLists.txt` | Synthetic-card tests through the actual HLE interface |
| `host/save_dialog_tests.c` and `scripts/test_savedata.sh` | Input/layout/presentation checks and a repeatable focused test entry point |
| Build scripts, package recipe and source allowlist | Include the new host code, assets and dependencies in every affected target |
| `scenarios/` and replay support | Explicit dialog responses and save/load regression scenarios |

Keep the first delivery focused on savedata. The PSP shell, general message
dialogs, on-screen keyboard, animated save icons, sound previews, encryption
compatibility and a launcher save manager are separate features. Use the
existing save directories and encoding; do not introduce a new save format or
arbitrary user-defined slot names.

## How the PSP interface works

The game supplies `SceUtilitySavedataParam`: the operation, game and save names,
allowed name list, data buffer, save title/details, icon data, and initial
focus. It starts the utility, calls Update while it is visible, polls status,
then shuts it down. These fields and calls are defined in the
[PSPSDK savedata header](https://github.com/pspdev/pspsdk/blob/master/src/utility/psputility_savedata.h).

| Request | Player interaction |
| --- | --- |
| AUTOLOAD / AUTOSAVE | Automatic file operation |
| LOAD / SAVE | Confirmation for a specified save |
| LISTLOAD / LISTSAVE | Select a save; saving can offer unused allowed slots |
| LISTDELETE / LISTALLDELETE | Select a save to delete, then confirm |
| LIST / FILES / SIZES and low-level read/write modes | Data or metadata operation without a slot picker |

`LIST` is an enumeration API, separate from the interactive `LISTLOAD` family.
For list-save, selecting existing data leads to overwrite confirmation;
cancelling leaves without saving. `LISTALLDELETE` is a browser of saves on the
memory stick, not an instruction to erase all saves. The contract comes from
the PSPSDK header and from real-hardware captures of the utility (see
"Presentation direction"); no emulator's source, constants, strings or assets
are used as a reference anywhere in this project.

An implementation can provide its own readable in-game overlay using this API.
Recreating the entire PSP shell or requiring a firmware installation is not
necessary for this feature.

## Baseline runtime behavior (before this implementation)

These observations refer to `utility.c` at runtime commit `0c864de`, before
the implementation below. The obsolete behavior and comments have been replaced.

- `sd_dir` uses `ms0:/PSP/SAVEDATA/<gameName><saveName>/`. Independent names
  produce independent directories. The existing save format can stay in place.
- `sd_do_mode`, starting at line 696, groups SAVE, AUTOSAVE and LISTSAVE with
  save creation. LISTSAVE uses a nonempty `saveName` directly; with an empty
  name, it picks the first `saveNameList` entry. It does not return a chosen
  name to the game. `SD_FOCUS` and `SD_OVERWRITE` are defined but unused.
- LOAD and LISTLOAD read `saveName` directly. LISTLOAD with no matching
  directory converts the no-data result to success, even though no data was
  loaded. The candidate list is not consulted.
- LISTDELETE, line 797, deletes every existing listed directory, then returns
  `0x80110347` (DELETE_NO_DATA). An earlier comment says it deletes nothing;
  that comment does not describe the code.
- LISTALLDELETE, line 814, deletes every directory matching the game prefix,
  or every directory if the prefix is empty, and returns the count.
- GetStatus, line 947, advances VISIBLE to QUIT simply by being polled.
  Update, line 966, performs the operation on its first call. ShutdownStart,
  line 974, also performs pending work. None waits for a player decision.
- `sd_write_file` and the SFO writer silently ignore open/write failures.
  The save path can report success without verifying that data reached disk.
  A usable confirmation screen must receive real I/O errors.

Secure-mode storage is currently plaintext. Encryption compatibility is a
separate existing limitation; slot selection itself does not require changing
the save encoding.

## Evidence from the three games

Static inspection of the locally generated C shows corresponding routines:

| Game / inspected build | LISTSAVE wrapper | LISTLOAD wrapper | Build save-name list |
| --- | --- | --- | --- |
| Last Raven / NPUH10024 | `00277A98` | `00277B1C` | `002785F8` |
| AC3 Portable / NPUH10023 | `001EB0A4` | `001EB11C` | `001EBACC` |
| Silent Line / NPUH10025 | `0020AE94` | `0020AF18` | `0020B9F4` |

The save and load wrappers pass modes 5 and 4 respectively to their shared
utility starter. Each list builder creates 20-byte name entries with an empty
terminator, writes the list pointer at parameter offset 96, and writes focus 3
(latest) at offset 1480. In Last Raven these writes are at `002786F0` and
`002786F4`; AC3 uses `001EBBC4` / `001EBBC8`, Silent Line uses
`0020BAEC` / `0020BAF0`.

These are static code paths, not a claim that each menu was exercised live in
this investigation. Existing logged replays only establish Last Raven's boot
mode-8 space query. They do not cover choosing, saving and reloading slots.

## Runtime probe before this implementation (historical)

A standalone C probe called the registered utility HLE functions with two
synthetic slots on a fresh temporary card, against the runtime at `0c864de`.
No player save directory was used or modified. Its scratch directory is gone;
the assertions below describe the behaviour that this implementation replaced
(items 4 and 5 in particular no longer hold):

1. LISTSAVE with an empty name and LASTEMPTY focus still creates SLOT00.
2. Repeating it writes SLOT00 again and does not create SLOT01.
3. Explicit SAVE to SLOT01 creates a second independent save; explicit LOAD
   reads its contents.
4. LISTLOAD with an empty name reports success but leaves the destination
   buffer unchanged, despite both list entries existing.
5. LISTDELETE removes both listed saves and returns DELETE_NO_DATA.
6. All operations finish the status sequence without input.

This validates the backend diagnosis. It does not validate a new UI or prove
PSP hardware behavior.

## Implementation sequence

Each phase has a completion gate. Complete the runtime contract before wiring
the live UI; enable interactive play after the host can present and answer it.
The public bridge is in `savedata.h`; private transaction helpers are in
`src/hle/savedata_io.h`. Neither adds a UI dependency to the runtime.

### 1. Runtime dialog session and host interface

- [x] Separate interactive modes (LOAD, SAVE, LISTLOAD, LISTSAVE, LISTDELETE,
  LISTALLDELETE and DELETE) from automatic and low-level operations.
- [x] Replace the polling ratchet with explicit session state. Keep PSP status
  VISIBLE during selection, confirmation and outcome acknowledgement. Move to
  QUIT only after completion/cancellation; FINISHED follows shutdown.
- [x] Make repeated status polls side-effect free for pending operations.
  Update consumes responses and executes an approved operation once.
  Shutdown/reset must never perform a pending unconfirmed save or deletion.
- [x] Define a copied snapshot containing a session ID/revision, operation,
  stage, candidates, selected item, confirm-button convention and result text.
  Define typed responses for selection, confirm, cancel and acknowledgement.
  Reject stale-session responses and choices outside the permitted list.
- [x] Keep guest memory reads and result writes on the guest side. Publish
  bounded owned data through a short synchronized handoff; the SDL thread
  must not retain guest pointers. Never wait for player input while holding
  the guest scheduler token.
- [x] Return the chosen `saveName` and mode-appropriate success, cancellation
  or error result. Check short/versioned parameter blocks before accessing
  optional fields and reject overlapping sessions consistently.

**Gate:** synthetic HLE tests can leave a dialog pending across many updates,
select either of two candidates, cancel or shut down without filesystem
mutation, and complete exactly once. No window or game data is required.

### 2. Candidates, metadata and reliable file operations

- [x] Preserve game-provided candidate order. LISTSAVE includes unused allowed
  names; LISTLOAD/LISTDELETE include existing candidates. Distinguish a
  nonempty initial `saveName` from a forced selection in list modes.
- [x] Support initial focus by name, list position, newest/oldest timestamp,
  and first/last existing or empty entry, with deterministic ties/fallbacks.
  Handle missing lists and wildcard names according to the requested mode.
- [x] Keep separate progress/replay lists separate. LISTALLDELETE enumerates
  card saves but deletes only the entry the player selects and confirms.
- [x] Read PARAM.SFO titles/details and filesystem dates. Accept saves from
  the earlier minimal SFO writer, whose `DETAIL` key predates the conventional
  `SAVEDATA_DETAIL` the current writer emits. Invalid/missing icons get a
  placeholder; broken data stays distinguishable from an empty slot.
- [x] Implement single-save confirmation, overwrite confirmation, no-data
  and error outcomes. Declining overwrite returns to the list where the mode
  permits it. No-data loading must not masquerade as a successful load.
- [x] Propagate open/write/flush/close failures. Stage replacements on the same
  filesystem, preserve other files in the slot, and retain the previous save
  until replacement succeeds. Define rollback/recovery for failures between
  replacement steps; a multi-file save is not made atomic by renaming only
  its data file. Validate names before using them as path components.

**Gate:** temporary-card tests verify both slots' bytes, selected-name
writeback, declined overwrite, cancellation, deletion of exactly one slot,
broken/missing data, and injected write/replacement failures. The original
save remains recoverable after a failed overwrite.

### 3. Shared host overlay and controller/keyboard input

- [x] Implement `host/save_dialog.c` / `.h` with one layout and navigation
  model: save title, date, details, existing/empty state, icon or placeholder,
  selected row, scrolling, confirmation prompts and readable errors.
- [x] Rasterize the host overlay to a copied RGBA image. Software presentation
  composites it over the game frame; GL uploads/composites it after the game
  blit and before swap. Keep dialog pixels out of guest render targets and
  guest framebuffer readback; include them in final-window captures.
- [x] Establish a redraw path during utility updates even when the game
  produces no new GE frames. Audit GL ownership before connecting this path:
  only the GL owner may upload/draw/swap, and SDL event handling must remain
  responsive. Prove this with a deliberately frozen game background before
  polishing the dialog.
- [x] Restore renderer state after drawing. Scale text/layout for native,
  wide, high-DPI, resized and borderless windows without stretching icons.
- [x] Route D-pad/stick and keyboard navigation before game-specific modern
  input conversion. Honor PSP confirm-button swapping, use press edges plus
  controlled navigation repeat, and require release of the opening confirm
  button. Suspend mouse look while the dialog is active; suppress consumed
  buttons until released when returning to the game.
- [x] Handle focus loss, controller disconnection, cancellation and window
  close without a stuck modal or an unconfirmed write. Keep close/quit
  shortcuts available.
- [x] Reuse the bundled font and SDL_ttf for text, with an explicit asset path
  available to the game child. Select and bundle a static PNG decoder for
  icons if needed. Headless/runtime-only builds retain no UI dependency.

The launcher runs in a separate process. Reuse suitable resources/helpers,
but do not put the game dialog in `host/launcher.c` or its SDL renderer.

**Gate:** a synthetic dialog fixture navigates and dismisses in both renderer
paths with no fresh game frames. Verify keyboard and real-controller input,
resize, readable screenshots and no leaked confirm/gameplay input. An absent
font or failed UI initialization produces an explicit failure/cancellation,
never an implicit save or overwrite.

### 4. Deterministic dialog responses and regression coverage

- [x] Add an explicit scripted response path using session identity and
  dialog update sequence. Identify selected slots by their actual save names
  and validate the expected operation/game so a changed request fails clearly.
- [x] Keep this independent of the existing guest pad-poll clock:
  `misc.c::ctrl_fill` advances pad replays, but a waiting game may stop reading
  the pad. Do not manufacture extra guest polls to advance dialog input.
- [x] In ordinary headless operation without a response provider, cancel
  interactive requests with a diagnostic. Automated fixtures may explicitly
  supply choices and confirmations. Automatic/low-level modes still execute
  directly; there is no implicit bulk-delete fallback.
- [x] Run the focused runtime and host checks through `scripts/test_savedata.sh`.
  Every mutating test uses a fresh temporary memory-stick root. Record dialog
  requests/responses/results sufficiently to diagnose a failed replay.
- [x] Re-run the savedata autotest suite and relevant runtime/presentation
  checks. Review interactive fixtures rather than preserving accidental bulk
  deletion or false success to match old output. Keep low-level compatibility
  differences explicit and compare unchanged gameplay replay baselines.

**Gate:** repeated scripted runs choose the same slots, return the same guest
results and leave identical save contents without live input or a window.
Missing/mismatched scripted responses terminate diagnostically instead of
hanging or silently choosing a slot.

### 5. Build, packaging and player validation

- [x] Wire the new host sources/dependencies into `scripts/06-boot.sh` and
  `packaging/linux/CMakeLists.txt`. Audit scripts that compile `present.c` or
  `render_gl.c` directly: render, resolution, preview and presentation tests.
- [x] Update `scripts/package-linux.py`'s source allowlist, package tests and
  `packaging/linux/build.py` for new sources/assets. Carry the bundled font
  path through the launcher-to-game path and include any decoder dependency
  and notices. Players should not need to install extra UI libraries/fonts.
- [x] Verify per-game host/runtime build fingerprints and importer behavior.
  Existing prepared executables must be marked for preparation when required
  to link the new runtime/host code; launcher-only validation is insufficient.
- [ ] Complete the player acceptance matrix below on copies of save data.
  Add progress/replay list scenarios where each title exposes them, and
  confirm the existing slot-00 save remains discoverable/loadable.

**Gate:** both local and packaged game executables pass the full matrix.
Record actual commands, artifacts and hardware used, with any untested cases
left visibly open. A successful compile or synthetic controller fixture does
not close the live-play gate.

## Player acceptance matrix

Run the core flow for ACLR, AC3P and ACSL through both software and GL
presentation. Use keyboard and a real controller; include packaged
launcher-to-game controller handoff and a Steam Deck check when available.

| Scenario | Required result |
| --- | --- |
| Open save with an existing slot-00 save | Existing metadata is readable and that save remains selectable |
| Save distinguishable progress into two allowed slots | Two independent saves; no unintended overwrite |
| Quit, relaunch and choose each save | Each restores its own progress; the game receives the selected name |
| Open list / confirm overwrite / cancel or decline | All previous save bytes unchanged |
| Confirm overwrite of one slot | Only the selected slot changes; success shown only after verified I/O |
| Select and confirm deletion | Only the selected save is removed; cancelling removes nothing |
| Load with no data, broken data, or an I/O error | Clear outcome and correct guest result; no false success |
| Wait, resize, lose focus or disconnect the controller | Dialog remains usable; no automatic confirmation |
| Close dialog while confirm or movement is held | No accidental next menu action, firing or mouse-look jump |
| Automatic save/load and space queries | No interactive slot picker; documented mode behavior preserved |

## Presentation direction

The overlay reproduces the PSP savedata utility's own layout, measured from two
real-hardware captures supplied by the user (the Load screens of Trails in the
Sky and Final Fantasy Type-0). The game supplies every image and string; the
host draws only the chrome. Coordinates are PSP pixels; the canvas is 2x.

- Backdrop: the selected save's `PIC1.PNG` fills the screen at full
  brightness (no dimming; both captures show none). An unused slot shows the
  PIC1 the request itself carries, the artwork the game would write there.
  With no artwork at all, a plain dark gradient.
- Top-left: a small memory-stick glyph and the operation name ("Save",
  "Load", "Delete") in white, no bar, no rules, no counter.
- Icon column: the selected save's `ICON0.PNG` at its native 144x80 at
  (27, 97) inside a 2 px white frame; the neighbours' icons at 81x45 with a
  fixed left edge (x = 48) and a 45 px stride above and below, so two are
  visible each way. Moving the selection slides the whole column: the drawn
  position eases toward the new selection (about 60 ms per step, so held
  repeats chain into one continuous slide); the white frame rides with the
  newly selected icon as it grows into the big slot, and the text and
  backdrop switch at once. Missing icon: a plain dark plate;
  corrupted data: the plate with a muted cross.
- Text column at x = 178: the game title (16 px, shrunk to fit one line),
  the date line `M/D/YYYY  h:MM AM  N KB` (12-hour clock, no leading zeros),
  the save title (14 px, "Corrupted Data" when broken), then the detail text
  wrapped to four lines with its spacing preserved. White with a one-pixel
  dark shadow, as the utility draws it over artwork.
- Bottom, centred: the Cross glyph then the Circle glyph, each followed by
  its label; the labels swap with the request's button convention, the
  glyph order does not (`x Enter  o Back` on a US title, `x Back  o Enter`
  on a Japanese one).
- Confirmation and outcome: a translucent rounded box over the text column
  with the message centred; confirmations add `Yes` / `No` (Left/Right, A/D,
  D-pad or stick select; Enter or the confirm button acts on the highlighted
  one, the cancel button always cancels); errors add the PSP result code.

Typefaces: the bundled DejaVu Sans, sized 32/28/26/24 on the 2x canvas.
`PSPRECOMP_UI_FONT` selects another face; a rounded OFL face (M PLUS Rounded
1c) was rendered beside it for comparison and bundling it is a separate
decision. The host distributes no Sony UI assets and takes nothing from any
emulator.

## Using and testing the implementation

Normal windowed play opens the dialog when a game requests an interactive
save/load/delete. Up/Down or the left stick selects entries. Enter/Escape and
physical A/B (Cross/Circle, respecting the request's button convention) confirm
or cancel. Single-save and overwrite/delete requests require confirmation.
Acknowledging the outcome returns control to the game after held inputs settle.

Headless interactive requests cancel unless an explicit response script is
provided. Set `PSPRECOMP_SAVEDATA_SCRIPT=/absolute/path/dialog.txt`. Each line is
`dialogOrdinal mode gameName saveName action`, with `select`, `accept`, or
`cancel`; `-` means an empty name. Only interactive requests increment the
ordinal, starting at one. One action is consumed per UtilityUpdate, independent
of guest pad polls. Mode, game, and selected name must match; stale responses,
EOF and mismatches cannot silently choose another save.

```text
# Choose a second allowed slot, save, acknowledge completion.
1 5 NPUH10024 ACLRSAVELIST01 select
1 5 NPUH10024 ACLRSAVELIST01 accept
1 5 NPUH10024 ACLRSAVELIST01 accept
```

`PSPRECOMP_SAVEDATA_LOG=1` logs requests and chosen names/results.
`PSPRECOMP_UI_FONT` explicitly selects a host font. AppRun exports the bundled
DejaVu font for the launcher and its game child (`scripts/import_game.py`
does the same for imports); a game binary started by hand from an extracted
AppDir without AppRun falls back to the system font paths. The PNG decoder is
pinned in the runtime's `third_party/stb`; its MIT license is shipped with the package.
In a script, `accept`/`cancel` lines must name the slot that is focused
(`-` for an empty name); any other name is a mismatch and aborts with 2.
With `PSPRECOMP_GL_SHOT`, frames composed by the dialog's own redraws are
written as `<prefix>-dialog-NNNN.ppm`, numbered apart from the game's presents.

Run `scripts/test_savedata.sh` for runtime, software, controller, missing-font
and GL checks; the GL mode runs offscreen so a desktop focus change cannot
disarm it. Use `--software` where there is no GL provider. Cards are disposable
`/tmp` directories; captures go to `build/savedata-checks`.

## Review and restyle (17 Sep)

Two read-only reviews of the 16 Sep work (runtime, and host/build/packaging)
were applied before anything landed. Fixed:

- Runtime: a GL redraw refused from a non-owner thread no longer cancels the
  dialog (the backend skips that frame instead); card enumeration skips
  foreign or over-long directory names and a full 1024-entry listing instead
  of refusing every save/load; a directory-sync failure after the commit
  rename is a warning, not a failed save; an empty allowed list on LISTSAVE
  returns the save-family code; the redraw hook is read and written under the
  bridge lock; a guest reset keeps the host's registrations; `<>` is rejected
  for AUTOLOAD instead of being accepted and never expanded; a read-only load
  no longer creates `PSP/SAVEDATA`; dead list-delete cases, an empty branch
  and a duplicated comment are gone; result codes are named.
- Host: game input resumes after the dialog once its own controls are
  released or after one second regardless (a stick at rest off-centre could
  leave the game deaf for the session); the dialog redraw path marks GL state
  disturbed and restores the viewport even when the overlay did not draw;
  the overlay is fitted into the game's picture box in both renderers and
  blacks that box out first; redraws are paced to the display and their
  captures are numbered apart (`-dialog`); keyboard navigation repeats on the
  dialog's clock, not the desktop's; mouse look is restored on close; icons
  are cached per slot and source, not per revision; the software path
  uploads only changed frames; a missing font is reported at startup and the
  runtime cancels requests with its own diagnostic; a missing SDL2_ttf now
  fails the windowed build with an explicit message instead of silently
  producing a headless host.
- Tests: the runtime test covers focus 3 (the value the games write), the
  new-data icon and title, the request's PIC1, icon/PIC1 path resolution,
  ShutdownStart from the confirmation stage, a foreign directory on the card,
  the empty-list code and a scripted mismatch; the host fixtures add a
  missing-font mode (`nofont`) and a window close while a list is open, and
  the stick-repeat check polls instead of sleeping a fixed margin.

Recorded as open, not fixed: PARAM.SFO over 16 KB reads as broken; the view
is copied whole per cursor move and carries no size/version word; a
scripted `cancel` must still name the focused slot; the `mkdtemp` roots the
tests create are left behind; `gl_dialog_redraw` does not flush pending GE
work before composing; the vendored `stb_image.h` is compiled under
`-Werror`; `scripts/common.sh` is missing from the package source allowlist;
no test covers a real controller, a resize or a controller disconnect while
the dialog is open, or a held input staying suppressed at close.

The overlay was then redrawn to the "Presentation direction" above, and the
runtime hands the host each save's `PIC1.PNG` path plus the request's own
PIC1 for unused slots. Evidence, 17 Sep:

- Runtime CTest 25/25 with the extended savedata test; `scripts/test_savedata.sh`
  software, controller, nofont and offscreen GL all pass; `scripts/test_present.sh`
  passes; `scripts/12-render-tests.sh` 430/430 in software and GL.
- The `scenarios/savedata/` route on the rebuilt optimized host: modes 5, 5, 4
  choose `ACLRSAVELIST01`, `02`, `01`, results `00000000`, 1600 polls, 1603
  lists, zero bad guest accesses, two 28,316-byte `SAVEDATA.BIN` files with
  the game's ICON0 and PIC1 sidecars; identical to the run before the changes
  except the GE command count (20,852,757 vs 20,866,761, frame pacing while
  the dialog is open). The dialog capture from that run shows Last Raven's
  PIC1 behind its NEW DATA icons and title.
- Fixture renders were produced with the bundled DejaVu Sans and with
  M PLUS Rounded 1c through `PSPRECOMP_UI_FONT` for the user's comparison
  against real hardware.

## Completion record

- [x] Current runtime inspected and temporary-card diagnosis recorded.
- [x] Save/load entry points and list-focus setup traced in all three titles.
- [x] Phases 1–2: synthetic runtime/session/file-operation checks passed.
- [ ] Phase 3: both renderers and live input gate passed (presentation
  redrawn to the hardware layout 17 Sep; live input still open). The first
  live test, 17 Sep evening with a Steam Controller, stalled on the Load
  screen: the dialog armed only once SDL reported nothing held, and a thumb
  resting on the controller's trackpad reads as a mouse button, so it never
  armed and ignored every press. It now also arms 300 ms after opening;
  accept and cancel act on press edges, which a button held since before the
  dialog cannot produce. Two sessions then loaded, saved and played through.
- [x] Phase 4: deterministic response, mismatch, EOF, and headless-cancel checks passed.
- [ ] Phase 5: package and trilogy player acceptance gates passed.

Record evidence here as implementation proceeds. M4's original save/load
gate stays historical; it does not establish completion of this UI plan.

Validation recorded during implementation:

- Runtime CTest: 25/25 passed. The one savedata test covers the lifecycle,
  two independent slots, name writeback, metadata, overwrite refusal, selected
  deletion, missing/broken data, failure preservation, interrupted-rename
  recovery, mutable requests, older parameter sizes, script EOF, and zero bad
  memory (script identity mismatch and the games' focus mode were added by the
  17 Sep review below).
- Host fixtures: software and offscreen GL, keyboard, swapped confirm/back,
  virtual-controller opening-button suppression, stick repeat and focus loss.
  GL scene pixels after dismissal match their expected color. A desktop GL
  fixture also passed on the initial visual design; a later desktop run lost
  focus during automation. Offscreen runs avoid that external focus dependency.
- Renderer regression: software 430/430 and GL 430/430 checks passed.
- All three local optimized game hosts rebuilt; initial replay smoke runs
  report zero bad guest accesses. These are not the complete player matrix.
- ACLR's actual System menu saved slots 01 and 02, then loaded slot 01 in
  an offscreen GL run on a disposable card: three successful operations,
  all 1,600 replay polls, zero bad guest accesses. This caught and fixed a
  completed-shutdown state that blocked the next InitStart when the game did
  not poll FINISHED. The regression is also covered by the runtime test.
  The repeatable route and utility responses live in `scenarios/savedata/`.
  This confirms round-trip requests; it does not prove distinct campaign
  progress, relaunch restoration, or physical input acceptance.
- The isolated Ubuntu package build runs the new runtime and three host UI
  fixtures using bundled SDL2/SDL_ttf/font. Package (9), importer (13) and
  fingerprint (10) checks passed. The development artifact is
  `build/releases/savedata-ui-final`, with source archive and build evidence.
- All 14 legacy savedata autotests run to completion on disposable cards.
  Autosave and makedata still match normalized hardware output. Tests that
  assumed unattended interactive save/delete now differ because headless
  requests cancel. Existing disk-space, query writeback-order and unsupported
  encryption differences remain; these are not claimed as hardware parity.

Physical controller play, Steam Deck acceptance, and the full per-title matrix
remain open until exercised. Neither virtual-controller input nor an offscreen
rendering test closes those hardware checks.
