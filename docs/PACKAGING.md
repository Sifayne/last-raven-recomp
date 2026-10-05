# Linux application package

The native x86-64 AppImage includes the launcher, ISO importer, compiler,
Python runtime and media libraries. Users supply their own supported PSP ISO;
its executable is extracted, verified and compiled locally. The distributed
image and matching source archive contain no generated game code or assets.

## Use on Steam Deck

Open the AppImage in Desktop Mode after marking it executable. Choose **Add
Game**, browse to the ISO, then **Open**. **Drives** opens removable media
locations. Drag-and-drop and pasting a path with Ctrl+V are also supported.
Keep the app open during preparation, then choose **Save & Play**. No separate
FFmpeg, Python or compiler installation is required. Preparation uses two
compiler workers and needs at least 2 GB of available space in the library.

Tabs appear in release order: AC3P, Silent Line, Last Raven. Settings remain
editable before launch, with saved presets and a remembered game selection.
After Desktop Mode setup, add the AppImage as a non-Steam game for Gaming Mode.
It is a native Linux application; leave Proton compatibility overrides off.

The `deck-test-6` package includes Higher FPS for all three games. After updating,
choose **Prepare Game** once per title, then enable **Graphics → Higher FPS**
and set **FPS cap** to 60 to start. Presets can also save a custom cap or
Unlimited. Existing saves, ISO locations and presets are retained. See
[Higher FPS](FPS.md) for the mission scope and simulation behavior.

To quit during play, hold **View + Menu (Select + Start) for two seconds**.
Releasing either button, losing window focus or disconnecting the controller
resets the hold. Keyboard users can press **Ctrl+Shift+Q**. Both shortcuts use
the normal window-close shutdown path. Save progress in-game before quitting.
Escape continues to release mouse capture.

This build accepts these exact US PSN executable versions:

| Game | Disc ID | Version |
| --- | --- | --- |
| Armored Core 3 Portable | NPUH10023 | 1.01 |
| Armored Core: Silent Line Portable | NPUH10025 | 1.00 |
| Armored Core: Last Raven Portable | NPUH10024 | 1.00 |

`pack.json`'s titles pin the decrypted executable SHA-256 for each
profile. Other regions, revisions and modified executables are rejected even
if the disc ID matches. Convert compressed images to ISO before importing.
The ISO must stay accessible because the game reads its assets from that file.

## Build

Since 5 Oct the pipeline is psprecomp's player (`tools/psprecomp/player/`, see
its README and `docs/PLAYER-LAYER.md`), and this repository is a title pack:
`pack.json` names the app, the three titles, the sources compiled on the
player's machine, and `packaging/linux/pack.cmake`. That file builds the
player's launcher with this pack's settings and `host/launcher_info.c` (the
app's name, About text and title order), and what has not moved into
psprecomp yet (`boot.c`, the GL backend).

On an x86-64 Linux host, install Python 3.9+, curl, tar and Bubblewrap (`bwrap`).
Unprivileged user namespaces must be available. Initialize the psprecomp
submodule, then run:

```sh
python3 tools/psprecomp/player/package-linux.py build --pack . --output build/releases/deck-test-2
```

The output must be a new directory. SHA-256-pinned archives are downloaded,
then an unprivileged Ubuntu 22.04 environment is bootstrapped under
`build/package/ubuntu-22.04/`. An explicit allowlist snapshots original
app/runtime source. No game dump or developer build products enter the build.
After bootstrap, compilation and tests run without networking. No Docker
daemon or root access is needed. Logs are retained in
`build/package/last-build.log`, including after failed builds.

`fetch` only verifies/populates downloads; `bootstrap` also prepares the builder.
Repeated builds reuse downloads and the builder but compile fresh source.
A process lock prevents two package builds sharing the same cache.

The baseline is glibc 2.35 and the GCC 11 C++ ABI. Dependency pins live in
`tools/psprecomp/player/linux/dependencies.json`; FFmpeg uses
`tools/psprecomp/third_party/ffmpeg/source.json`.
Ubuntu build-tool packages track Jammy updates; installed versions are recorded.
This is an auditable recipe, not a claim of byte-for-byte reproducibility.

Outputs include:

- `Armored-Core-Portable-x86_64.AppImage`
- `Last-Raven.AppDir/`, usable through `AppRun` without FUSE
- `Armored-Core-Portable-sources.tar.gz`, matching source and build recipes
- `BUILD.json`, source hashes, dependency pins and ELF dependency/ABI audit
- `build.log`, `build-packages.txt`, UI fixture captures and `SHA256SUMS`
- `README.txt`, setup and troubleshooting instructions

The source archive includes original application source and pinned dependency
inputs, including the Zig distribution and matching bootstrap/compiler source,
CPython, pspdecrypt, OpenSSL and zlib. Extract it and run the same build command;
Ubuntu bootstrap still needs its package repositories. pspdecrypt is a separate
GPLv3 executable. The launcher/runtime do not link its implementation. Dependency
licenses and original notices are included under `licenses/` in the application.

## Preparation, storage and updates

The importer inspects the disc, extracts EBOOT.BIN, decrypts it if needed, then
verifies its exact SHA-256 before generating any game code. Zig compiles that
code with a glibc 2.35 target and links original host/runtime objects. A completed
installation contains the local executable and decrypted module; intermediate
generated source and objects are removed. No shell command strings are used.
An import lock prevents simultaneous preparation in one library.

Each title has a game build fingerprint, separate from the application's source
fingerprint. Launcher, importer UI, icon, notices and tab-label changes keep
prepared games ready. A title's replacements or replace list invalidate only
that title; shared controls invalidate the titles that include them. Shared
runtime, code generator, compiler or compilation-recipe changes invalidate all
affected games. **Prepare Game** rebuilds each using the saved ISO location.

`game-builds.json` records the ingredients and fingerprint for each title:
the actual original host/runtime archives, runtime headers, generator binary,
Zig compiler/support files, compilation/splitting recipes, supported executable
hash, and the title's replacement source with its transitive local headers.
Shared libraries contribute their SONAMEs, so compatible library updates do not
force relinking. Actual build outputs capture compiler/flag changes without
making every packaging-script edit invalidate games. Fingerprints use relative
paths, independent of the AppImage's location or mount directory.

The compiler recipe lives in `tools/psprecomp/player/compile_game.py`;
library/UI bookkeeping is in `tools/psprecomp/player/import_game.py`. Keep commands and flags affecting generated
executables in the compiler recipe so they remain covered by the fingerprint.
The whole-app build ID remains recorded for provenance, not compatibility.
Old preview install records without a game fingerprint require one preparation
per game when moving to this format; their files and saves are preserved.

An existing installation is replaced in the library record only after the new
build succeeds. Cancellation/failure leaves previous installations and saves
available. Moving an ISO requires selecting it again with Add Game; the cached
executable is reused when its build and verified module match. Old build folders
are retained, so repeated updates can use additional disk space.

Default persistent paths:

| Content | Location |
| --- | --- |
| Settings | `~/.config/last-raven/settings.ini` |
| Install records and prepared games | `~/.local/share/last-raven/` |
| Game save data | `~/.local/share/last-raven/saves/<slug>/ms/PSP/SAVEDATA/` |
| Import, launcher and game logs | `~/.local/state/last-raven/logs/` |

Absolute XDG base-directory overrides are supported. A source build started
through `scripts/15-settings.sh` uses the same save folder; to play from more
than one computer, see [the save sync guide](SAVE-SYNC.md). First packaged launch
copies legacy SDL settings from `~/.local/share/Last Raven/settings.ini` only
when the new settings file is absent. Moving or replacing the AppImage leaves
user files intact. Developer scripts and explicit `--config` / `--preset`
arguments remain available, with existing default and environment precedence.

ELF helpers and libraries use relative RUNPATHs. System graphics, audio and
input drivers remain supplied by the OS. Import/game subprocesses add the
bundled library directory after any user `LD_LIBRARY_PATH`, allowing compatible
replacement libraries. FFmpeg remains a separate LGPL shared library, with its
exact corresponding source and rebuild instructions alongside the download.

AppRun enables SDL2's `SDL_GAMECONTROLLER_ALLOW_STEAM_VIRTUAL_GAMEPAD` default
for the launcher and its game subprocesses. SDL2 otherwise filters Steam
Input's virtual controller, even when the operating system exposes it as an
Xbox pad. An explicit environment override is preserved. This startup-only
change does not invalidate prepared games.

## Validation

Every package build runs the settings and actual launcher event-handler fixtures,
including title ordering, ISO browsing, import subprocess arguments, cancellation,
preset saving and game launch handoff. Backend fixtures exercise exact-version
rejection, record corruption, import locking, interrupted preparation, moved ISO
cache reuse and update invalidation. They contain synthetic data, not game bytes.
Presentation fixtures check the exact two-second quit threshold, release and
disconnect cancellation, and keyboard modifiers. Virtual controller and keyboard
events also exercise the real SDL window loop under Xvfb.
FPS clock and pose fixtures also run inside the builder, covering simulation
rate, bounded catch-up, interpolation and exact restoration of guest poses.

Package checks cover relocation into paths containing spaces, read-only app
contents, persistent preferences, XDG paths, migration, missing dependencies,
empty library handling and absence of game payload. ELF audits reject unresolved
dependencies, absolute RUNPATHs and newer glibc/C++ ABI requirements. X11 startup
runs at 1280x800 under Xvfb, followed by an AppImage extraction-and-run check.

Real ISO preparation and gameplay are separate private validation steps; ISOs,
prepared executables, saves and game output must stay outside release/source
staging. Test a mission, audio, save/reload, controller input and exit on the Deck.
The earlier launcher-only preview was confirmed to launch and display correctly
on the user's Deck; that does not establish gameplay support for this build.

The `deck-test-2` build passed all three real ISO imports with system compiler,
Python and FFmpeg commands guarded against use. Imports took 57–75 seconds on
the development PC; these are not Deck timing estimates. A fresh AC3P import
through the mounted, read-only AppImage also passed. Last Raven completed the
garage replay in software and on a live OpenGL window; AC3P's menu probe and
Silent Line's sortie replay completed with the null renderer. All four runs
delivered their full recorded inputs and reported zero bad memory accesses.
The mounted launcher detected one controller. This does not verify live
controller gameplay, perceived audio quality or an in-game save/reload cycle
on the Deck. Private results are recorded in
`reports/package-validation/game-import.json`.

The selective-update checks upgraded all three real installations, preserved
their previous files, and confirmed that launcher-only edits reuse every game
without compiling. A Silent Line replacement change invalidated only Silent
Line. The `deck-test-4` build adds the two-second quit shortcut and launcher
reminders; both shortcuts closed a real Last Raven OpenGL run through normal
host shutdown with zero bad memory accesses. These were injected SDL events
on the development PC, not a physical Steam Deck controller test. Results are
recorded in `reports/package-validation/selective-updates.json`.

The `deck-test-5` startup fix enables Steam Input's virtual gamepad without
changing any game fingerprint. The mounted AppImage opened the connected local
Steam virtual controller in both launcher and game; injected SDL button/stick
events reached the guest input recorder, and keyboard shutdown completed
normally. The user separately confirmed controller input in the launcher and
game on the Deck with `deck-test-4`. Local detection and handoff evidence is in
`reports/package-validation/steam-controller.json`.

The icon is the unchanged generic Adwaita application icon by the GNOME Project,
under CC BY-SA 3.0 US. No generated artwork is included.

The `deck-test-6` FPS package passed all three real ISO upgrades through a
mounted, read-only AppImage using only bundled preparation tools. Preparation
took 58–78 seconds on the development PC. Previous installations, preferences
and the isolated save-preservation fixture remained unchanged. Packaged builds
at 60 and 90 FPS matched stock control logs exactly: 2,713 records for ACLR,
11,991 for AC3P and 41,665 for ACSL, with zero bad guest accesses. A live
1280×800 OpenGL ACLR replay also matched stock, using the saved 60 FPS preset
through the packaged launch path. It measured 60.00 rendered FPS and about
30 Hz simulation over 30.15 seconds. The package, checksum, dependency and
source-payload audits passed. Results are recorded in
`reports/package-validation/fps-deck.json`; this is development-PC evidence,
not a physical Steam Deck performance or save/reload test.

References: [AppDir packaging](https://docs.appimage.org/packaging-guide/manual.html),
[AppImage compatibility](https://docs.appimage.org/reference/best-practices.html),
[AppImage runtime](https://github.com/AppImage/type2-runtime/tree/20251108),
[FFmpeg legal information](https://ffmpeg.org/legal.html),
[Zig downloads and source](https://ziglang.org/download/),
[pspdecrypt source](https://github.com/John-K/pspdecrypt).
