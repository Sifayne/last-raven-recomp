#!/usr/bin/env python3
"""Read a PSPRECOMP_VIEW_LOG and say what the camera did.

The log is the only direct measurement we have of the guest's own control
response: twelve floats of matrix per change, stamped with the pad-poll count
a scenario is keyed on. This turns that into the numbers a control decision
actually needs -- does a given stick deflection produce yaw, how much per
poll, and where the deadzone and the ceiling are.

    scripts/view-analyze.py reports/look-probe.view
    scripts/view-analyze.py reports/sweep.view --window 1800-2000=left \\
                                               --window 2060-2260=right
    scripts/view-analyze.py reports/all.view --deltas

Each line carries a tag: V for a GE view-matrix upload, W for a world-matrix
upload. This game keeps its view matrix constant and carries the camera in the
world matrices, so W is the one to read; the default picks W when any W lines
exist and V otherwise.

--deltas is for a log written with PSPRECOMP_VIEW_LOG_WORLD=all, where every
upload of a frame is present and an `F <poll>` line closes each frame. The
k-th upload of a frame is not one object -- draw order shifts with what is on
screen -- but every *static* object's matrix rotates by exactly the camera's
rotation between two frames. So: match uploads by position across consecutive
frames, take each one's yaw delta, and cluster. The largest cluster is the
static scenery, and its delta is the camera's; a second cluster near zero is
HUD geometry that never moves; the rest are things with a life of their own.

Yaw is unwrapped before any rate is taken. Without that a turn through the
+/-180 seam reads as a 359-degree jump in one poll, which would put a spurious
maximum in the middle of every sweep.
"""

import argparse
import sys
from collections import Counter


def read_log(path):
    """-> [(tag, poll, yaw, pitch)]; F lines carry None for the angles."""
    rows = []
    with open(path) as f:
        for line in f:
            if line.startswith("#") or not line.strip():
                continue
            parts = line.split()
            if parts[0] == "F" and len(parts) == 2:
                rows.append(("F", int(parts[1]), None, None))
            elif len(parts) == 16:
                rows.append((parts[0], int(parts[1]),
                             float(parts[14]), float(parts[15])))
    return rows


def wrap(d):
    """A difference of two angles, brought into (-180, 180]."""
    while d > 180.0:
        d -= 360.0
    while d <= -180.0:
        d += 360.0
    return d


def unwrap(values):
    """Remove the +/-180 discontinuity so differences are real rotation."""
    if not values:
        return []
    out = [values[0]]
    offset = 0.0
    for prev, cur in zip(values, values[1:]):
        d = cur - prev
        if d > 180.0:
            offset -= 360.0
        elif d < -180.0:
            offset += 360.0
        out.append(cur + offset)
    return out


def summarise(rows, lo, hi, label):
    span = [(p, y) for _, p, y, _ in rows if lo <= p <= hi]
    if len(span) < 2:
        print(f"  {label:<10} {len(span)} sample(s) in {lo}-{hi} "
              f"-- not enough to measure")
        return
    polls = [p for p, _ in span]
    yaws = unwrap([y for _, y in span])
    total = yaws[-1] - yaws[0]
    npolls = polls[-1] - polls[0]
    rate = total / npolls if npolls else 0.0
    print(f"  {label:<10} polls {polls[0]}-{polls[-1]} ({len(span)} samples)  "
          f"yaw {yaws[0]:8.2f} -> {yaws[-1]:8.2f}  "
          f"total {total:+8.2f}deg  rate {rate:+7.4f} deg/poll")


def deltas(rows, bucket):
    """Per frame: the clusters of per-object yaw delta against the previous frame."""
    frames = []           # (poll the frame closed at, [yaw per upload])
    cur = []
    for tag, poll, yaw, _ in rows:
        if tag == "F":
            if cur:
                frames.append((poll, cur))
            cur = []
        elif tag == "W":
            cur.append(yaw)
    if len(frames) < 2:
        print("  fewer than two complete frames -- was the log written with "
              "PSPRECOMP_VIEW_LOG_WORLD=all?")
        return

    print(f"  {len(frames)} frames; per frame, the top clusters of "
          f"per-object yaw delta (bucket {bucket} deg):")
    for (p0, a), (p1, b) in zip(frames, frames[1:]):
        n = min(len(a), len(b))
        if n == 0:
            continue
        # The first frame of a poll window follows the last frame of the
        # previous window, tens of polls earlier; its delta is a turn's worth
        # of rotation folded into +/-180 and means nothing. Say so instead.
        if p1 - p0 > 1:
            print(f"    @{p1:<6} -- window opens ({p1 - p0} polls after the "
                  f"previous frame; first frame is partial)")
            continue
        ds = [wrap(b[i] - a[i]) for i in range(n)]
        clusters = Counter(round(d / bucket) * bucket for d in ds)
        top = clusters.most_common(3)
        # The bucket centre is coarse; the mean of the members is the number.
        # The log carries three decimals, so this is good to about a
        # thousandth -- enough to tell 2.1094 (0x180 of a 16-bit turn) from 2.1.
        def mean_near(v):
            near = [d for d in ds if abs(d - v) <= bucket]
            return sum(near) / len(near) if near else v
        desc = "  ".join(f"{mean_near(v):+.4f}deg x{c}" for v, c in top)
        print(f"    @{p1:<6} {len(a):>3}->{len(b):<3} objects  {desc}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log")
    ap.add_argument("--tag", choices=["V", "W", "auto"], default="auto",
                    help="which matrix to read (default: W if present, else V)")
    ap.add_argument("--window", action="append", default=[], metavar="LO-HI=LABEL",
                    help="measure the yaw rate over a poll range; repeatable")
    ap.add_argument("--every", type=int, default=0, metavar="N",
                    help="also dump every Nth sample")
    ap.add_argument("--deltas", action="store_true",
                    help="cluster per-object yaw deltas frame to frame (needs "
                         "a WORLD=all log)")
    ap.add_argument("--bucket", type=float, default=0.05, metavar="DEG",
                    help="cluster width for --deltas (default 0.05)")
    args = ap.parse_args()

    try:
        rows = read_log(args.log)
    except OSError as e:
        print(f"cannot read {args.log}: {e}", file=sys.stderr)
        return 1

    if not rows:
        print(f"{args.log}: no samples. No matrix ever changed, or the run "
              f"never reached a scene that sets one.")
        return 1

    counts = Counter(t for t, *_ in rows)
    print(f"{args.log}: lines by tag {dict(counts)}")

    if args.deltas:
        deltas(rows, args.bucket)
        return 0

    tag = args.tag
    if tag == "auto":
        tag = "W" if counts.get("W") else "V"
    rows = [r for r in rows if r[0] == tag]
    print(f"  reading {tag}")
    if not rows:
        print(f"  no {tag} lines")
        return 1

    polls = [p for _, p, _, _ in rows]
    yaws = unwrap([y for _, _, y, _ in rows])
    pitches = [p for _, _, _, p in rows]

    print(f"  {len(rows)} changes, polls {polls[0]}-{polls[-1]}")
    print(f"  yaw    {min(yaws):8.2f} .. {max(yaws):8.2f}  "
          f"(span {max(yaws) - min(yaws):.2f} deg, unwrapped)")
    print(f"  pitch  {min(pitches):8.2f} .. {max(pitches):8.2f}  "
          f"(span {max(pitches) - min(pitches):.2f} deg)")

    if args.window:
        print("\nwindows:")
        for spec in args.window:
            try:
                rng, label = spec.split("=", 1)
                lo, hi = (int(x) for x in rng.split("-", 1))
            except ValueError:
                print(f"  bad --window {spec!r}, want LO-HI=LABEL", file=sys.stderr)
                continue
            summarise(rows, lo, hi, label)

    if args.every:
        print("\nsamples:")
        for i in range(0, len(rows), args.every):
            print(f"  @{rows[i][1]:<6} yaw {yaws[i]:8.2f}  pitch {pitches[i]:7.2f}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
