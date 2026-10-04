#!/usr/bin/env python3
"""Internal offline build, invoked in the private container by package-linux.py."""
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import preparation

ROOT = Path(__file__).resolve().parents[2]
BUILD = ROOT / "build"
DEPS = BUILD / "package-prefix"
SOURCES = BUILD / "package-source"
RELEASE = BUILD / "release"
APP = RELEASE / "Last-Raven.AppDir"
RECIPE = ROOT / "packaging/linux"
DOWNLOADS = Path("/downloads")
FFMPEG = BUILD / "deps/ffmpeg"
JOBS = "8"
LOG = BUILD / "package-build.log"


def run(args, **kw):
    with LOG.open("a") as log:
        log.write("\n$ " + " ".join(map(str, args)) + "\n")
        log.flush()
        subprocess.run(list(map(str, args)), check=True, stdout=log, stderr=subprocess.STDOUT, **kw)


def capture(args):
    return subprocess.check_output(list(map(str, args)), text=True)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def unpack(archive, name):
    dest = SOURCES / name
    dest.mkdir(parents=True)
    run(["tar", "-xf", DOWNLOADS / archive, "--strip-components=1", "-C", dest])
    return dest


def cmake(source, name, flags):
    obj = BUILD / name
    run(["cmake", "-S", source, "-B", obj, "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release",
         f"-DCMAKE_INSTALL_PREFIX={DEPS}", "-DCMAKE_INSTALL_LIBDIR=lib",
         "-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic",
         "-DCMAKE_CXX_FLAGS=-march=x86-64 -mtune=generic", *flags])
    run(["cmake", "--build", obj, "--parallel", JOBS])
    run(["cmake", "--install", obj])
    return obj


def copy(src, dest):
    dest.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(src, dest, follow_symlinks=False)


def audit():
    # libc and the C++ ABI stay with the OS, as do graphics/audio drivers.
    system = {"libc.so.6", "libm.so.6", "libpthread.so.0", "libdl.so.2", "librt.so.1",
              "libstdc++.so.6", "libgcc_s.so.1", "ld-linux-x86-64.so.2"}
    bundled = {p.name for p in (APP / "usr/lib").iterdir()}
    for link in APP.rglob("*"):
        if link.is_symlink() and (os.path.isabs(os.readlink(link)) or
                                  not link.resolve().is_relative_to(APP) or not link.exists()):
            raise ValueError(f"nonrelocatable or broken symlink: {link}")
    report = {}
    for path in sorted((APP / "usr").rglob("*")):
        if not path.is_file() or path.is_symlink() or path.read_bytes()[:4] != b"\x7fELF":
            continue
        dynamic = capture(["readelf", "-d", path])
        needed = re.findall(r"\(NEEDED\).*\[(.*?)\]", dynamic)
        unknown = set(needed) - system - bundled
        if unknown:
            raise ValueError(f"unbundled dependencies in {path.name}: {sorted(unknown)}")
        if "(NEEDED)" not in dynamic:
            report[str(path.relative_to(APP))] = {"static": True}
            continue
        relative = os.path.relpath(APP / "usr/lib", path.parent)
        expected = "$ORIGIN" + ("/" + relative if relative != "." else "")
        runpath = re.findall(r"\(RUNPATH\).*\[(.*?)\]", dynamic)
        if runpath != [expected]:
            raise ValueError(f"nonrelocatable RUNPATH in {path}: {runpath}")
        versions = capture(["readelf", "--version-info", path])
        glibc = sorted({tuple(map(int, v.split('.'))) for v in re.findall(r"GLIBC_([0-9.]+)", versions)})
        cxx = sorted({tuple(map(int, v.split('.'))) for v in re.findall(r"GLIBCXX_([0-9.]+)", versions)})
        if glibc and glibc[-1] > (2, 35):
            raise ValueError(f"{path.name} requires glibc newer than 2.35")
        if cxx and cxx[-1] > (3, 4, 29):
            raise ValueError(f"{path.name} requires a newer C++ ABI than GCC 11")
        linked = capture(["ldd", path])
        if "not found" in linked:
            raise ValueError(f"unresolved dependencies: {linked}")
        for line in linked.splitlines():
            match = re.search(r"(\S+) => (\S+)", line)
            if match and match[1] in bundled and not Path(match[2]).resolve().is_relative_to(APP):
                raise ValueError(f"library escaped package: {line}")
        report[str(path.relative_to(APP))] = {"needed": needed, "runpath": runpath,
            "glibc": '.'.join(map(str, glibc[-1])) if glibc else None,
            "glibcxx": '.'.join(map(str, cxx[-1])) if cxx else None}
    return report


def main():
    BUILD.mkdir(exist_ok=True)
    RELEASE.mkdir()
    os.environ["PKG_CONFIG_PATH"] = str(DEPS / "lib/pkgconfig")
    # The unprivileged namespace maps only one uid; archive owners are not
    # meaningful here (including archives extracted by the FFmpeg recipe).
    os.environ["TAR_OPTIONS"] = "--no-same-owner"
    print("Building pinned SDL, font renderer, OpenH264 and FFmpeg...", flush=True)
    sdl = unpack("SDL2-2.32.10.tar.gz", "sdl")
    cmake(sdl, "sdl-build", ["-DSDL_SHARED=ON", "-DSDL_STATIC=OFF", "-DSDL_TEST=OFF",
        "-DSDL_TESTS=OFF", "-DSDL_X11=ON", "-DSDL_X11_SHARED=ON", "-DSDL_WAYLAND=ON",
        "-DSDL_WAYLAND_SHARED=ON", "-DSDL_KMSDRM=OFF", "-DSDL_PIPEWIRE=OFF",
        "-DSDL_PULSEAUDIO=ON", "-DSDL_PULSEAUDIO_SHARED=ON", "-DSDL_ALSA_SHARED=ON"])
    ttf = unpack("SDL2_ttf-2.24.0.tar.gz", "ttf")
    cmake(ttf, "ttf-build", [f"-DCMAKE_PREFIX_PATH={DEPS}", "-DBUILD_SHARED_LIBS=ON",
        "-DSDL2TTF_VENDORED=ON", "-DSDL2TTF_HARFBUZZ=OFF", "-DSDL2TTF_SAMPLES=OFF"])
    h264 = unpack("openh264-2.6.0.tar.gz", "openh264")
    run(["make", "-j" + JOBS, "OS=linux", "ARCH=x86_64", "USE_ASM=No", "BUILDTYPE=Release",
         "libraries"], cwd=h264)
    run(["make", "OS=linux", "ARCH=x86_64", "USE_ASM=No", "BUILDTYPE=Release",
         f"PREFIX={DEPS}", "install-shared"], cwd=h264)
    ff = json.loads((ROOT / "third_party/ffmpeg/source.json").read_text())
    cached = BUILD / "deps/downloads"
    cached.mkdir(parents=True)
    copy(DOWNLOADS / f"ffmpeg-{ff['version']}.tar.xz", cached / f"ffmpeg-{ff['version']}.tar.xz")
    run(["python3", ROOT / "scripts/ffmpeg.py", "build"])
    print("Building the bundled game preparation tools...", flush=True)
    tools = preparation.build_tools(sys.modules[__name__])
    font = unpack("dejavu-fonts-ttf-2.37.tar.bz2", "font")
    print("Building the launcher and running settings/UI checks...", flush=True)
    obj = BUILD / "launcher-build"
    run(["cmake", "-S", RECIPE, "-B", obj, "-G", "Ninja", f"-DDEPS={DEPS}", f"-DFFMPEG={FFMPEG}",
         "-DCMAKE_C_FLAGS=-march=x86-64 -mtune=generic"])
    run(["cmake", "--build", obj, "--parallel", JOBS])
    run([obj / "settings-tests"])
    run([obj / "fps-tests"])
    run([obj / "fps-pose-tests"])
    run([obj / "present-tests"])
    for mode in ("keyboard", "controller"):
        # The game's software presentation requests an accelerated SDL
        # renderer, which the dummy driver does not provide in this build.
        run(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", obj / "present-tests", mode],
            env={**os.environ, "SDL_AUDIODRIVER": "dummy"})
    ui = BUILD / "ui-checks"
    ui.mkdir()
    run([obj / "savedata-tests"])
    for mode in ("software", "controller", "gl"):
        run(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24",
             obj / "save-dialog-tests", mode, ui],
            env={**os.environ, "SDL_AUDIODRIVER": "dummy",
                 "PSPRECOMP_UI_FONT": str(font / "ttf/DejaVuSans.ttf")})
    run([obj / "launcher-tests", ui, font / "ttf/DejaVuSans.ttf"],
        env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
    print("Staging only app binaries, dependencies, font and notices...", flush=True)
    (APP / "usr/lib").mkdir(parents=True)
    for name in ("launcher", "settings-tool"):
        copy(obj / name, APP / "usr/bin" / name)
        run(["strip", APP / "usr/bin" / name])
        run(["patchelf", "--set-rpath", "$ORIGIN/../lib", APP / "usr/bin" / name])
    for prefix, patterns in [(DEPS, ("libSDL2*.so*", "libopenh264.so*")),
                             (FFMPEG, ("libavcodec.so*", "libavutil.so*"))]:
        for pattern in patterns:
            for lib in (prefix / "lib").glob(pattern):
                copy(lib, APP / "usr/lib" / lib.name)
    for lib in (APP / "usr/lib").iterdir():
        if not lib.is_symlink():
            run(["patchelf", "--set-rpath", "$ORIGIN", lib])
    copy(font / "ttf/DejaVuSans.ttf", APP / "usr/share/last-raven/DejaVuSans.ttf")
    icon = unpack("adwaita-icon-theme-48.0.tar.xz", "adwaita")
    copy(icon / "Adwaita/scalable/mimetypes/application-x-executable.svg", APP / "last-raven.svg")
    for name in ("AppRun", "last-raven.desktop"):
        copy(RECIPE / name, APP / name)
    (APP / "AppRun").chmod(0o755)
    (APP / ".DirIcon").symlink_to("last-raven.svg")
    runtime = unpack("runtime-source.tar.gz", "appimage-runtime")
    fuse = unpack("fuse-3.15.0.tar.xz", "fuse")
    squash = unpack("squashfuse-0.5.2.tar.gz", "squashfuse")
    notices = [(ROOT / "LICENSE", "last-raven/LICENSE"),
        (ROOT / "tools/psprecomp/LICENSE", "psprecomp/LICENSE"),
        (ROOT / "tools/psprecomp/third_party/stb/LICENSE", "stb/LICENSE"),
        (sdl / "LICENSE.txt", "SDL/LICENSE.txt"), (ttf / "LICENSE.txt", "SDL_ttf/LICENSE.txt"),
        (ttf / "external/freetype/docs/FTL.TXT", "FreeType/FTL.TXT"),
        (h264 / "LICENSE", "OpenH264/LICENSE"), (font / "LICENSE", "DejaVu/LICENSE"),
        (icon / "COPYING", "Adwaita/COPYING"), (runtime / "LICENSE", "AppImage/LICENSE"),
        (fuse / "LGPL2.txt", "AppImage/libfuse-LGPL2.txt"),
        (squash / "LICENSE", "AppImage/squashfuse-LICENSE"),
        *[(DOWNLOADS / name, "AppImage/" + name) for name in
          ("musl-COPYRIGHT", "zstd-LICENSE", "zlib-LICENSE")]]
    for src, dest in notices:
        copy(src, APP / "licenses" / dest)
    shutil.copytree(FFMPEG / "share/licenses/ffmpeg", APP / "licenses/ffmpeg")
    preparation.stage_tools(sys.modules[__name__], tools, obj)
    copy(RECIPE / "README.txt", APP / "README.txt")
    copy(ROOT / "docs/FPS.md", APP / "usr/share/last-raven/FPS.md")
    # The original font rendering credit is required by the FreeType license.
    (APP / "licenses/THIRD-PARTY.txt").write_text(
        "This software uses FFmpeg under the LGPL v2.1 or later. https://ffmpeg.org/\n"
        "Exact source and rebuild instructions accompany the AppImage in the sources archive.\n"
        "Portions of this software are copyright (C) 1996-2024 The FreeType Project\n"
        "(www.freetype.org). All rights reserved. FreeType is used under the FreeType License.\n"
        "Icon: GNOME Project (https://gnome.org), Adwaita 48.0, unchanged, CC BY-SA 3.0 US.\n"
        "https://creativecommons.org/licenses/by-sa/3.0/us/\n"
        "See each dependency's directory for its original notices.\n")
    manifest = {"kind": "game-import-preview", "architecture": "x86_64", "glibc_maximum": "2.35",
                "contains_game_code": False, "game_import_available": True,
                "game_builds": json.loads((APP / "usr/share/last-raven/game-builds.json").read_text()),
                "elf": audit(), "inputs": json.loads((ROOT / "INPUTS.json").read_text()),
                "dependencies": json.loads((RECIPE / "dependencies.json").read_text())}
    copy(Path("/build-packages.txt"), RELEASE / "build-packages.txt")
    (RELEASE / "BUILD.json").write_text(json.dumps(manifest, indent=2) + "\n")
    run(["python3", ROOT / "scripts/test_package.py", APP])
    run([APP / "usr/python/bin/python3.12", "-I", "-B", ROOT / "scripts/test_import_game.py", APP])
    run(["python3", ROOT / "scripts/test_game_fingerprints.py"])
    # Xvfb exercises the actual SDL X11 backend at Deck screen dimensions.
    run(["xvfb-run", "-a", "-s", "-screen 0 1280x800x24", APP / "AppRun", "--check-startup"])
    print("Wrapping the validated AppDir into an AppImage...", flush=True)
    tool = BUILD / "appimagetool.AppImage"
    copy(DOWNLOADS / "appimagetool-x86_64.AppImage", tool)
    tool.chmod(0o755)
    tool_dir = BUILD / "appimage-tool"
    tool_dir.mkdir()
    run([tool, "--appimage-extract"], cwd=tool_dir)
    artifact = RELEASE / "Armored-Core-Portable-x86_64.AppImage"
    run([tool_dir / "squashfs-root/AppRun", "--no-appstream", "--runtime-file",
         DOWNLOADS / "runtime-x86_64", APP, artifact], env={**os.environ, "ARCH": "x86_64"})
    run([artifact, "--appimage-extract-and-run", "--check-startup"],
        env={**os.environ, "SDL_VIDEODRIVER": "dummy"})
    print("Writing the matching source archive and build evidence...", flush=True)
    source = BUILD / "matching-source"
    source.mkdir()
    for path in ROOT.iterdir():
        if path.name == "build":
            continue
        if path.is_dir():
            shutil.copytree(path, source / path.name, ignore=shutil.ignore_patterns("__pycache__"))
        else:
            copy(path, source / path.name)
    shutil.copytree(DOWNLOADS, source / "build/package/downloads",
                    ignore=shutil.ignore_patterns("*.part", "initial-downloads.json"))
    run(["python3", ROOT / "scripts/ffmpeg.py", "bundle", "--output", source / "ffmpeg"])
    copy(RELEASE / "BUILD.json", source / "BUILD.json")
    copy(RELEASE / "build-packages.txt", source / "build-packages.txt")
    with tarfile.open(RELEASE / "Armored-Core-Portable-sources.tar.gz", "w:gz") as archive:
        archive.add(source, arcname="last-raven-launcher-sources")
    copy(RECIPE / "README.txt", RELEASE / "README.txt")
    copy(LOG, RELEASE / "build.log")
    shutil.copytree(ui, RELEASE / "ui-checks")
    files = sorted(p for p in RELEASE.rglob("*") if p.is_file() and not p.is_symlink())
    (RELEASE / "SHA256SUMS").write_text(''.join(f"{sha(p)}  {p.relative_to(RELEASE)}\n" for p in files))
    print(f"All checks passed: {artifact.name}", flush=True)


if __name__ == "__main__":
    try:
        main()
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Package build failed: {error}", file=sys.stderr)
        if LOG.exists():
            print('\n'.join(LOG.read_text(errors="replace").splitlines()[-60:]), file=sys.stderr)
        sys.exit(1)
