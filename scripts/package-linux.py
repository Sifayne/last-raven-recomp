#!/usr/bin/env python3
"""Build a launcher preview AppImage in an unprivileged Ubuntu 22.04 container.

Only Bubblewrap, Python 3, curl and tar are required on the build host. No
daemon, root, game dump, generated C, or developer build products are used.
"""
import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parent.parent
WORK = ROOT / "build/package"
DOWNLOADS = WORK / "downloads"
ROOTFS = WORK / "ubuntu-22.04"
RECIPE = ROOT / "packaging/linux"
LOCK = json.loads((RECIPE / "dependencies.json").read_text())


def run(args, **kw):
    return subprocess.run([str(a) for a in args], check=True, **kw)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for chunk in iter(lambda: f.read(1024 * 1024), b""):
            h.update(chunk)
    return h.hexdigest()


def fetch():
    DOWNLOADS.mkdir(parents=True, exist_ok=True)
    pins = dict(LOCK)
    ff = json.loads((ROOT / "third_party/ffmpeg/source.json").read_text())
    pins[f"ffmpeg-{ff['version']}.tar.xz"] = ff
    for name, pin in pins.items():
        dest = DOWNLOADS / name
        if not dest.exists():
            cached = ROOT / "build/deps/downloads" / name
            if cached.exists() and sha(cached) == pin["sha256"]:
                shutil.copy2(cached, dest)
            else:
                part = dest.with_name(name + ".part")
                try:
                    run(["curl", "-fL", "--retry", "2", "--connect-timeout", "20",
                         "--max-time", "600", pin["url"], "-o", part])
                    if sha(part) != pin["sha256"]:
                        raise ValueError(f"checksum mismatch: {name}")
                    part.replace(dest)
                finally:
                    part.unlink(missing_ok=True)
        if sha(dest) != pin["sha256"]:
            raise ValueError(f"checksum mismatch: {dest}; remove it and fetch again")
    print("All pinned downloads verified.", flush=True)


def container(args, *, network=False, mounts=(), writable_root=False):
    cmd = ["bwrap", "--die-with-parent", "--unshare-all", "--uid", "0", "--gid", "0"]
    if network:
        cmd += ["--share-net"]
    cmd += ["--bind" if writable_root else "--ro-bind", ROOTFS, "/",
            "--proc", "/proc", "--dev", "/dev", "--tmpfs", "/tmp",
            "--clearenv", "--setenv", "PATH", "/usr/bin:/bin",
            "--setenv", "HOME", "/tmp", "--setenv", "LC_ALL", "C.UTF-8"]
    if network:
        cmd += ["--ro-bind", Path("/etc/resolv.conf").resolve(), "/etc/resolv.conf"]
    for source, target, writable in mounts:
        (ROOTFS / target.lstrip("/")).mkdir(parents=True, exist_ok=True)
        cmd += ["--bind" if writable else "--ro-bind", source, target]
    return run(cmd + list(args))


def bootstrap():
    marker = ROOTFS / ".last-raven-builder"
    expected = sha(RECIPE / "bootstrap.sh") + LOCK["ubuntu-base-22.04.5-base-amd64.tar.gz"]["sha256"]
    if marker.exists() and marker.read_text() == expected:
        return
    if not ROOTFS.exists():
        with tempfile.TemporaryDirectory(prefix="rootfs-", dir=WORK) as tmp:
            root = Path(tmp) / "root"
            root.mkdir()
            # Official, checksum-verified Ubuntu base archive; skip devices.
            run(["tar", "--no-same-owner", "--exclude=dev/*", "-xf",
                 DOWNLOADS / "ubuntu-base-22.04.5-base-amd64.tar.gz", "-C", root])
            (root / "etc/resolv.conf").unlink(missing_ok=True)
            (root / "etc/resolv.conf").touch()
            (root / "etc/apt/apt.conf.d/99last-raven").write_text(
                'APT::Sandbox::User "root";\nDPkg::Options { "--force-not-root"; "--force-bad-path"; };\n')
            # Never attempt to start services in the build namespace.
            policy = root / "usr/sbin/policy-rc.d"
            policy.write_text("#!/bin/sh\nexit 101\n")
            policy.chmod(0o755)
            root.rename(ROOTFS)
    container(["/bin/sh", "/recipe/bootstrap.sh"], network=True, writable_root=True,
              mounts=[(RECIPE, "/recipe", False)])
    marker.write_text(expected)


def copy_inputs(dest):
    # An allowlist prevents dumps, keys, generated game code and build products
    # from entering either the container or the matching source archive.
    files = ["LICENSE", "host/launcher.c", "host/launcher_library.h", "host/settings.c", "host/settings.h",
             "host/settings_tool.c", "host/settings_tests.c", "host/launcher_tests.c",
             "host/boot.c", "host/render_gl.c", "host/render_gl.h",
             "tools/psprecomp/tests/test_present.c", "scripts/test_present.sh",
             "host/save_dialog_tests.c", "scripts/test_savedata.sh", "docs/SAVEDATA-UI.md", "docs/SAVE-SYNC.md",
             "tools/psprecomp/tests/test_savedata.c",
             "host/stick.h", "host/ac3_controls.h",
             "host/fps.h", "host/fps_clock.h", "host/fps_aclr.h", "scripts/fps-loop.py",
             "host/fps_joints.h", "host/fps_ac3.h",
             "host/fps_tests.c", "host/fps_pose_tests.c", "docs/FPS.md",
             "host/replacements.c", "host/replacements-ac3p.c", "host/replacements-acsl.c",
             "host/replace.txt", "host/replace-ac3p.txt", "host/replace-acsl.txt",
             "scripts/import_game.py", "scripts/compile_game.py", "scripts/emit-split.py", "scripts/test_import_game.py",
             "scripts/test_game_fingerprints.py",
             "scripts/ffmpeg.py", "scripts/package-linux.py", "scripts/test_package.py"]
    for name in files:
        target = dest / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(ROOT / name, target)
    shutil.copytree(RECIPE, dest / "packaging/linux", ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copytree(ROOT / "third_party/ffmpeg", dest / "third_party/ffmpeg")
    runtime = ROOT / "tools/psprecomp"
    if not (runtime / "src/cpu.c").is_file():
        raise ValueError("tools/psprecomp runtime source is missing; initialize the submodule first")
    for folder in ("src", "include"):
        shutil.copytree(runtime / folder, dest / "tools/psprecomp" / folder,
                        ignore=shutil.ignore_patterns("__pycache__"))
    shutil.copy2(runtime / "LICENSE", dest / "tools/psprecomp/LICENSE")
    shutil.copytree(runtime / "third_party/stb", dest / "tools/psprecomp/third_party/stb")
    recomp = runtime / "tools/allegrexrecomp"
    for path in recomp.rglob("*"):
        if path.is_file() and path.suffix in (".c", ".h"):
            target = dest / "tools/psprecomp/tools/allegrexrecomp" / path.relative_to(recomp)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(path, target)
    manifest = {str(p.relative_to(dest)): sha(p) for p in sorted(dest.rglob("*")) if p.is_file()}
    (dest / "INPUTS.json").write_text(json.dumps(manifest, indent=2) + "\n")


def build(output):
    if output.exists():
        raise ValueError(f"output exists: {output}; select a new --output directory")
    bootstrap()
    output.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="build-", dir=WORK) as tmp:
        stage = Path(tmp)
        source = stage / "work"
        source.mkdir()
        copy_inputs(source)
        try:
            container(["/usr/bin/python3", "/work/packaging/linux/build.py"], mounts=[
                (source, "/work", True), (DOWNLOADS, "/downloads", False)])
        finally:
            log = source / "build/package-build.log"
            if log.exists():
                shutil.copy2(log, WORK / "last-build.log")
        # Move on the destination filesystem only after all checks pass.
        with tempfile.TemporaryDirectory(prefix="package-", dir=output.parent) as publish:
            ready = Path(publish) / "ready"
            shutil.copytree(source / "build/release", ready, symlinks=True)
            ready.rename(output)
    print(f"Validated launcher preview: {output}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("command", choices=("fetch", "bootstrap", "build"), nargs="?", default="build")
    p.add_argument("--output", type=Path, default=ROOT / "build/releases/launcher-preview")
    args = p.parse_args()
    try:
        if platform.system() != "Linux" or platform.machine() != "x86_64":
            raise ValueError("this pipeline currently requires an x86-64 Linux build host")
        WORK.mkdir(parents=True, exist_ok=True)
        with (WORK / ".lock").open("w") as guard:
            fcntl.flock(guard, fcntl.LOCK_EX | fcntl.LOCK_NB)
            fetch()
            if args.command != "fetch":
                if not shutil.which("bwrap"):
                    raise ValueError("install Bubblewrap (bwrap) on the build host")
                if args.command == "bootstrap":
                    bootstrap()
                else:
                    build(args.output.resolve())
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"Packaging: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
