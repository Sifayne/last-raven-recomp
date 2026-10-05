# Bundled FFmpeg audio

The Linux player build uses private shared `libavcodec` and `libavutil`
libraries. Players do not need to install FFmpeg. The dependency contains
only the ATRAC3 and ATRAC3+ decoders used by `sceAtrac3plus` and movie audio;
OpenH264 remains the separate video decoder.

## Source and configuration

`tools/psprecomp/third_party/ffmpeg/source.json` pins FFmpeg **9.0.1**, its official release
URL and SHA-256. The initial archive was verified against its detached PGP
signature with the official FFmpeg release key:

```
FCF986EA15E6E293A5644F10B4322F04D67658D8
```

Every download and cached source archive must match the pinned SHA-256.
The build uses the unmodified release source. To update the pin, first
verify the new release's signature using [FFmpeg's verification
instructions](https://ffmpeg.org/download.html#releases), update the version,
checksum and library ABI majors together, then rerun the checks below.

The recipe explicitly disables GPL, nonfree and version-3-only components,
external-library autodetection, static libraries, programs, network support
and unrelated codecs/libraries. It enables shared libraries and the `atrac3`
and `atrac3p` decoders. Standalone x86 assembly is disabled so NASM is not an
additional build prerequisite; compiler-generated SIMD remains available.

`patchelf` sets each library's `DT_RUNPATH` to `$ORIGIN` after installation.
The host searches `$ORIGIN/lib` first, then the checkout's relative
`$ORIGIN/../deps/ffmpeg/lib` development location. No `LD_LIBRARY_PATH`
wrapper or absolute checkout path is needed to run a relocated host with
its adjacent `lib/` directory. Compatible user replacements remain usable;
the license/configuration checks run during builds and staging, not in the
game to prohibit replacement libraries.

## Build and inspect

From the repository root:

```bash
scripts/build-tools.sh
```

This builds the pinned dependency, explicitly configures the runtime with
its headers and libraries, builds the tools, and runs runtime CTest. It also
migrates existing CMake caches away from system FFmpeg. The ordinary boot,
settings, replay, oracle and renderer scripts link the same selected libraries.

For dependency work alone:

```bash
python3 tools/psprecomp/player/ffmpeg.py build --deps build/deps
python3 tools/psprecomp/player/ffmpeg.py verify --deps build/deps
FFMPEG_DEPS=build/deps python3 tools/psprecomp/player/tests/test_ffmpeg.py
```

The recipe is psprecomp's since 5 Oct (`player/ffmpeg.py`); `--deps` keeps
its downloads and output in this checkout. Moving it changed the recipe's own
hash, so the first `scripts/build-tools.sh` afterwards rebuilds the library.

Build prerequisites are Linux, Python 3.9+, a C compiler, make, tar/xz,
readelf, patchelf, and curl for the first download. `JOBS` controls build
parallelism (default: up to eight CPUs); `CC` selects the compiler. External
CFLAGS/CPPFLAGS/CXXFLAGS/LDFLAGS are cleared for the dependency build, keeping
the recipe explicit. The compiler/version, exact configure options and
post-install operation are recorded in `.last-raven-ffmpeg.json`.

Downloads live in `build/deps/downloads/`, installed files in
`build/deps/ffmpeg/`, and the build log in `build/deps/ffmpeg-build.log`.
The cache is reused only when the source pin, recipe, notices and compiler
match and the libraries pass verification. `build --rebuild` forces a rebuild.
An invalid cached source archive is rejected, never silently used.

This is a pinned, repeatable build recipe; identical binary hashes across
different compilers or operating-system toolchains are not promised.

## Stage a distribution bundle

```bash
python3 tools/psprecomp/player/ffmpeg.py bundle --deps build/deps --output build/ffmpeg-bundle
```

The output must not already exist. The command verifies actual loaded library
paths, LGPL license reports, the release version, ABI majors, enabled codecs,
relative RUNPATHs and the dependency list before staging:

```
ffmpeg-bundle/
  lib/                       shared libraries and their SONAME symlinks
  licenses/ffmpeg/            full LGPL text and attribution
  source/                    exact source tarball and BUILD.json
    rebuild/                 self-contained build recipe and source pin
  manifest.json              source identity and library inspection
  SHA256SUMS                 hashes of regular files in the bundle
  README.txt
```

The source README explains how to rebuild offline from the included archive.
This is an **FFmpeg dependency bundle**, not a complete game installer.
Copy its `lib/` beside the host executable, include `licenses/` with the app,
and offer the exact `source/` package alongside the binary download. Do not
include game data or generated game code in this dependency bundle.

The launcher includes FFmpeg attribution under **About**. A future download
page must also identify FFmpeg, link its LGPL license, and provide a working
link to the matching source package. Follow [FFmpeg's distribution
checklist](https://ffmpeg.org/legal.html); these materials address this
dependency, not the other components or original game's distribution rights.

## Validation

`tools/psprecomp/player/tests/test_ffmpeg.py` checks relocated execution from a path containing
spaces, source/license staging, the bundle checksums, rejection of corrupt
source and missing/externally resolved libraries, and preservation of an
existing output directory.

For an audio change, compare the game's `PSPRECOMP_AUDIO_DUMP` output from a
fixed replay before and after, run the ATRAC conformance cases, and exercise
movie audio as well as music. Build-only decoder discovery does not establish
that real audio works. Runtime and launcher checks remain:

```bash
ctest --test-dir build/psprecomp --output-on-failure
scripts/16-settings-tests.sh --ui
```

Only the Linux build and ELF packaging path are implemented here. Windows
DLL packaging remains part of the Windows port. Other app dependencies and
the first-run game import are separate release work.

Measured on 7 Sep 2026 (artifacts in ignored `reports/ffmpeg-validation/`):

- Runtime CTest: 24/24; distribution checks: 6/6; launcher UI checks pass.
- All 17 ATRAC conformance rows are identical with the system and bundled
  libraries (same interpreter, inputs and limits). This preserves existing
  differences from PSP hardware; it does not claim all 17 match hardware.
- Garage replay: 78/78 events, 925 polls, zero bad accesses. Music channel 3
  is byte-identical over 1,388,544 signed 16-bit samples. Channels 1/2 have
  19/48 differing samples respectively, with a maximum difference of one.
- A host relocated into a directory containing spaces runs a 12-second GL
  windowed intro, with non-silent decoded movie audio and zero bad accesses.
  This checks SDL playback startup and generated audio, not a human listening
  assessment or movie-sync tuning.
- Software renderer: 430/430; camera aspect checks: 35/35.
- The staged source recipe rebuilds offline. Shared libraries occupy about
  1.5 MiB; the corresponding source package about 12 MiB.
