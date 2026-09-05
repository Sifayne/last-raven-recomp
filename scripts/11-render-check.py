#!/usr/bin/env python3
"""Capture a scene suite once, then compare both backends on identical GE work.

All captures and pictures are local, ignored game-derived artifacts. A saved
results.json can gate later runs on those exact capture hashes; recapturing a
scenario is deliberately a new specimen, not silently the same baseline.
Uses the Python standard library; ImageMagick adds optional review images.
"""
import argparse
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def clean_env(**settings):
    # A caller's old frame/draw probes must not alter this experiment or write
    # into its previous output paths. Preserve desktop/driver environment.
    env = {k: v for k, v in os.environ.items() if not k.startswith("PSPRECOMP_")}
    env.update(settings)
    return env


def run(command, log, *, env=None, cwd=ROOT, timeout=120):
    with log.open("w") as f:
        result = subprocess.run([str(p) for p in command], cwd=cwd, env=env,
                                stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
    if result.returncode:
        raise ValueError(f"command exited {result.returncode}; see {log}")
    return log.read_text(errors="replace")


def build(out):
    run([ROOT / "scripts/06-boot.sh"], out / "build.log",
        env={**clean_env(), "BOOT_NO_RUN": "1"}, timeout=180)


def load_suite(path):
    suite = json.loads(path.read_text())
    cases = suite["cases"]
    if suite["version"] != 1 or not cases or len(cases) > 64:
        raise ValueError("expected a version-1 suite with 1..64 cases")
    names = [case["name"] for case in cases]
    polls = [case["poll"] for case in cases]
    if len(set(names)) != len(names) or any(not re.fullmatch(r"[a-z0-9-]+", n) for n in names):
        raise ValueError("case names must be unique, lowercase filename components")
    if any(type(p) is not int or not 0 <= p < suite["stop_poll"] for p in polls):
        raise ValueError("capture polls must precede stop_poll")
    if polls != sorted(set(polls)):
        raise ValueError("capture polls must increase strictly")
    return suite


def capture_header(path):
    with path.open("rb") as f:
        data = f.read(40)
    if len(data) != 40:
        raise ValueError(f"short capture: {path}")
    magic, version, state, lists, _, ram, _, vram, _, mod = struct.unpack("<10I", data)
    if magic != 0x50414347 or version != 2 or not state or not lists:
        raise ValueError(f"invalid capture header: {path}")
    if path.stat().st_size != 40 + state + lists * 12 + ram + vram + mod:
        raise ValueError(f"incomplete capture: {path}")
    # The current recorder snapshots memory at the first list completion. A
    # multi-list frame needs a per-list memory/event stream before this suite
    # can claim the same inputs survived; fail explicitly instead of grading it.
    if lists != 1:
        raise ValueError(f"{path}: multi-list capture needs per-list memory snapshots")
    return {"version": version, "state_bytes": state, "lists": lists}


def capture(args):
    suite = load_suite(args.suite)
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    build(out)
    scenario = (ROOT / suite["scenario"]).resolve()
    iso = args.iso.resolve() if args.iso else next(iter(sorted((ROOT / "game").glob("*.iso"))), None)
    elf = args.elf.resolve()
    if not iso or not iso.is_file() or not elf.is_file():
        raise ValueError("supply your own ELF and ISO (or use the existing game/ defaults)")
    # Keep any guest-created saves isolated from the player's ms/ directory.
    run_root = out / "hostfs"
    run_root.mkdir()
    prefix = out / "frame"
    print(f"Capturing {len(suite['cases'])} scenes in one recorded run…", flush=True)
    text = run([ROOT / "build/host/boot", elf, iso], out / "capture.log", cwd=run_root,
               timeout=suite["timeout_seconds"], env=clean_env(
                   PSPRECOMP_REPLAY=str(scenario), PSPRECOMP_MPEG_DECODE="1",
                   PSPRECOMP_RENDER="gl", PSPRECOMP_FRAME=str(out / "end.ppm"),
                   PSPRECOMP_GE_CAPTURE=str(prefix),
                   PSPRECOMP_GE_CAPTURE_POLLS=",".join(str(c["poll"]) for c in suite["cases"]),
                   PSPRECOMP_GE_CAPTURE_MINCMDS=str(suite["min_commands"]),
                   PSPRECOMP_GE_CAPTURE_MINMEAN=str(suite["min_mean"])))
    if "[TAINTED]" in text or not re.search(r"bad mem:\s+0 accesses", text):
        raise ValueError(f"capture run had live input or bad memory accesses; see {out / 'capture.log'}")
    if f"stop at poll {suite['stop_poll']}" not in text:
        raise ValueError("scenario did not reach its stop poll; refusing partial coverage")
    records = []
    for case in suite["cases"]:
        path = out / f"frame-{case['poll']}.gcap"
        header = capture_header(path)
        match = re.search(r"ge: captured (\d+) list\(s\), (\d+) command\(s\).*?"
                          r"polls (\d+)\.\.(\d+) -> " + re.escape(str(path)), text)
        if not match:
            raise ValueError(f"no capture provenance for {path}")
        start, end = int(match[3]), int(match[4])
        # Brightness/size filters must not silently drift to another scene.
        if start > case["poll"] + 8:
            raise ValueError(f"{case['name']} drifted from poll {case['poll']} to {start}")
        records.append({**case, "capture": path.name, "sha256": digest(path),
                        "header": header, "commands": int(match[2]),
                        "actual_polls": [start, end]})
    index = {"version": 1, "suite": suite, "scenario_sha256": digest(scenario),
             "boot_sha256": digest(ROOT / "build/host/boot"), "cases": records}
    (out / "captures.json").write_text(json.dumps(index, indent=2) + "\n")
    print(f"Saved {out / 'captures.json'}", flush=True)


def read_ppm(path):
    """Strict 8-bit P6 reader; do not strip whitespace-valued first pixels."""
    data = path.read_bytes()
    pos, tokens = 0, []
    while len(tokens) < 4:
        while pos < len(data) and data[pos] in b" \t\r\n":
            pos += 1
        if pos < len(data) and data[pos] == ord("#"):
            newline = data.find(b"\n", pos)
            if newline < 0:
                raise ValueError(f"unterminated PPM comment: {path}")
            pos = newline + 1
            continue
        start = pos
        while pos < len(data) and data[pos] not in b" \t\r\n":
            pos += 1
        if pos == start:
            raise ValueError(f"short PPM header: {path}")
        tokens.append(data[start:pos])
    if tokens[0] != b"P6" or tokens[3] != b"255":
        raise ValueError(f"expected 8-bit P6 image: {path}")
    w, h = int(tokens[1]), int(tokens[2])
    if w <= 0 or h <= 0 or pos >= len(data):
        raise ValueError(f"invalid PPM dimensions/header: {path}")
    # One whitespace separator terminates the header (CRLF is one newline).
    pos += 2 if data[pos:pos + 2] == b"\r\n" else 1
    rgb = data[pos:]
    if len(rgb) != w * h * 3:
        raise ValueError(f"PPM payload size does not match {w}x{h}: {path}")
    return w, h, rgb


def metrics(a, b, region=None):
    if a[:2] != b[:2]:
        raise ValueError("comparison images have different dimensions")
    w, h, src = a
    dst = b[2]
    x0, y0, rw, rh = region if region is not None else (0, 0, w, h)
    if min(x0, y0) < 0 or min(rw, rh) <= 0 or x0 + rw > w or y0 + rh > h:
        raise ValueError("comparison region falls outside the frame")
    squared = absolute = exact = over2 = peak = total = 0
    bbox = [w, h, -1, -1]
    for y in range(y0, y0 + rh):
        for x in range(x0, x0 + rw):
            i = (y * w + x) * 3
            delta = [abs(src[i + c] - dst[i + c]) for c in range(3)]
            worst = max(delta)
            peak = max(peak, worst)
            squared += sum(d * d for d in delta)
            absolute += sum(delta)
            total += sum(src[i:i + 3])
            exact += worst == 0
            over2 += worst > 2
            if worst > 2:
                bbox = [min(bbox[0], x), min(bbox[1], y), max(bbox[2], x), max(bbox[3], y)]
    n = rw * rh
    return {"pixels": n, "exact_pixels": exact, "pixels_over_2": over2,
            "max_channel_error": peak, "rmse": math.sqrt(squared / (n * 3)) / 255,
            "mean_absolute_error": absolute / (n * 3), "software_mean": total / (n * 3),
            "error_bbox": bbox if over2 else None}


def coverage(text):
    patterns = {"point_line_draws_skipped": r"(\d+) point/line draw\(s\) skipped",
                "stencil_draws_unsupported": r"(\d+) draw\(s\) need alpha-backed stencil",
                "blend_factors_unsupported": r"blend not represented: (\d+) factor",
                "blend_equations_unsupported": r"blend not represented: \d+ factor, (\d+) equation"}
    return {name: int(m[1]) if (m := re.search(pat, text)) else 0 for name, pat in patterns.items()}


def regressions(current, baseline):
    old = {c["name"]: c for c in baseline["cases"]}
    if set(old) != {c["name"] for c in current["cases"]}:
        raise ValueError("baseline contains a different set of cases")
    failures = []
    for case in current["cases"]:
        ref = old[case["name"]]
        if case["capture_sha256"] != ref["capture_sha256"]:
            raise ValueError(f"{case['name']}: baseline uses a different capture")
        if case["software_sha256"] != ref["software_sha256"]:
            raise ValueError(f"{case['name']}: software oracle changed; review a new baseline")
        if case["region_rects"] != ref["region_rects"]:
            raise ValueError("baseline compares different regions")
        for region, values in case["regions"].items():
            for key in ("rmse", "pixels_over_2", "max_channel_error"):
                if values[key] > ref["regions"][region][key] + 1e-12:
                    failures.append(f"{case['name']}/{region}: {key} increased")
        for key, value in case["coverage"].items():
            if value > ref["coverage"][key]:
                failures.append(f"{case['name']}: {key} increased")
    return failures


def review_images(out, name):
    if not shutil.which("magick"):
        return
    src, dst = (out / f"{name}-{be}.ppm" for be in ("software", "gl"))
    run(["magick", src, dst, "+append", "-filter", "point", "-resize", "200%",
         out / f"{name}-pair.png"], out / f"{name}-images.log")
    run(["magick", src, dst, "-compose", "difference", "-composite", "-evaluate",
         "multiply", "16", out / f"{name}-diff-x16.png"], out / f"{name}-diff.log")


def compare(args):
    index = json.loads(args.captures.read_text())
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=False)
    build(out)
    results = {"version": 1, "gereplay_sha256": digest(ROOT / "build/host/gereplay"), "cases": []}
    for case in index["cases"]:
        name = case["name"]
        path = args.captures.resolve().parent / case["capture"]
        capture_header(path)
        if digest(path) != case["sha256"]:
            raise ValueError(f"capture changed: {path}")
        texts, images = {}, {}
        for backend in ("software", "gl"):
            for repeat in range(2):
                suffix = "" if repeat == 0 else "-repeat"
                ppm = out / f"{name}-{backend}{suffix}.ppm"
                command = [ROOT / "build/host/gereplay", path, backend, ppm]
                if "target" in case:
                    command.append(case["target"])
                text = run(command, ppm.with_suffix(".log"), env=clean_env(), timeout=120)
                m = re.search(r"GE: (\d+) lists, (\d+) commands, (\d+) finishes", text)
                if not m or int(m[1]) != 1 or int(m[2]) != case["commands"] or int(m[3]) != 1:
                    raise ValueError(f"{name}/{backend}: replay did not execute the captured work")
                if backend == "gl" and "gl:       ran" not in text:
                    raise ValueError(f"{name}: GL did not initialise")
                if repeat == 0:
                    images[backend] = read_ppm(ppm)
                    texts[backend] = text
                elif read_ppm(ppm) != images[backend]:
                    raise ValueError(f"{name}/{backend}: repeat is not byte-identical")
        regions = {"full": metrics(images["software"], images["gl"])}
        for region, rect in case.get("regions", {}).items():
            regions[region] = metrics(images["software"], images["gl"], rect)
        if regions["full"]["software_mean"] < index["suite"]["min_mean"]:
            raise ValueError(f"{name}: replay is dark; review target selection before grading")
        record = {"name": name, "capture_sha256": case["sha256"],
                  "software_sha256": digest(out / f"{name}-software.ppm"),
                  "region_rects": case.get("regions", {}),
                  "regions": regions, "coverage": coverage(texts["gl"])}
        results["cases"].append(record)
        if not args.no_images:
            review_images(out, name)
        full = regions["full"]
        print(f"{name}: RMSE {full['rmse']:.6f}, {full['exact_pixels']}/{full['pixels']} exact, "
              f"{full['pixels_over_2']} pixels >2; {record['coverage']}", flush=True)
    failures = regressions(results, json.loads(args.baseline.read_text())) if args.baseline else []
    results["baseline_failures"] = failures
    results["status"] = "regression" if failures else "no-regression" if args.baseline else "measured"
    (out / "results.json").write_text(json.dumps(results, indent=2) + "\n")
    print(f"{results['status']}: {out / 'results.json'}", flush=True)
    if failures:
        raise ValueError("; ".join(failures))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    cap = sub.add_parser("capture", help="record all suite scenes in one GL run")
    cap.add_argument("--suite", type=Path, default=ROOT / "scenarios/render-checks.json")
    cap.add_argument("--elf", type=Path, default=ROOT / "game/extracted/ACLR_App.elf")
    cap.add_argument("--iso", type=Path)
    cap.add_argument("--output", required=True, type=Path, help="new output directory")
    cmp = sub.add_parser("compare", help="replay each capture twice per backend and measure")
    cmp.add_argument("captures", type=Path, help="captures.json from capture")
    cmp.add_argument("--output", required=True, type=Path, help="new output directory")
    cmp.add_argument("--baseline", type=Path, help="gate on prior results from these exact captures")
    cmp.add_argument("--no-images", action="store_true")
    args = parser.parse_args()
    try:
        (capture if args.action == "capture" else compare)(args)
    except (ValueError, OSError, KeyError, subprocess.TimeoutExpired) as error:
        parser.exit(1, f"render-check: {error}\n")


if __name__ == "__main__":
    main()
