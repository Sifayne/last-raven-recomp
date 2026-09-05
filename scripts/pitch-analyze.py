#!/usr/bin/env python3
"""Read the pitch lines of a PSPRECOMP_INPUT_LOG and print the law they show.

host/replacements.c writes, once per frame the look integrator ran for the
player's AC:

    <poll> pitch=<rad> rate=<rad/frame> ry=<stick> mdy=<counts> <orig|orig-button|dual>

This folds consecutive frames with the same per-frame change into one row, so
a ramp reads as a few rows of growing steps, a hold as one row at the cap,
and a clamp as a row of zero change at the limit -- and shows the degrees,
because the constants are radians and nobody thinks in those.

    scripts/pitch-analyze.py reports/pitch-sweep.log [--from 2100] [--to 2600]
"""

import argparse
import math
import re

LINE = re.compile(r"^(\d+) pitch=(\S+) rate=(\S+) ry=(-?\d+) mdy=(-?\d+) (\S+)")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    ap.add_argument("--eps", type=float, default=1e-5,
                    help="two per-frame changes within this are the same row")
    args = ap.parse_args()

    rows = []
    for line in open(args.log):
        m = LINE.match(line)
        if not m:
            continue
        poll = int(m.group(1))
        if poll < args.lo or poll > args.hi:
            continue
        rows.append((poll, float(m.group(2)), float(m.group(3)),
                     int(m.group(4)), int(m.group(5)), m.group(6)))
    if not rows:
        print("no pitch lines in that range")
        return

    # Fold runs of equal change.
    segs = []
    prev = None
    for r in rows:
        poll, angle, rate, ry, mdy, how = r
        d = None if prev is None else angle - prev[1]
        if segs and d is not None and abs(d - segs[-1]["d"]) <= args.eps \
                and how == segs[-1]["how"] and ry == segs[-1]["ry"] \
                and mdy == segs[-1]["mdy"] and poll == segs[-1]["end"] + 1:
            segs[-1]["end"] = poll
            segs[-1]["angle_end"] = angle
            segs[-1]["n"] += 1
        else:
            segs.append({"start": poll, "end": poll, "angle_start": angle,
                         "angle_end": angle, "d": 0.0 if d is None else d,
                         "n": 1, "ry": ry, "mdy": mdy, "how": how, "rate": rate})
        prev = r

    print(f"{'polls':>13}  {'n':>3}  {'angle start -> end (deg)':>28}  "
          f"{'per frame':>12}  {'ry':>4} {'mdy':>4}  how")
    for s in segs:
        if s["n"] == 1 and abs(s["d"]) < args.eps and s["how"] == "orig" \
                and s["ry"] == 0 and s["mdy"] == 0:
            continue                      # idle frame in the game's own law
        deg = math.degrees
        print(f"{s['start']:>6}-{s['end']:<6} {s['n']:>3}  "
              f"{deg(s['angle_start']):>+12.3f} -> {deg(s['angle_end']):>+12.3f}  "
              f"{s['d']:>+12.6f}  {s['ry']:>4} {s['mdy']:>4}  {s['how']}")

    lo = min(r[1] for r in rows)
    hi = max(r[1] for r in rows)
    biggest = max((abs(rows[i][1] - rows[i - 1][1]), rows[i][0])
                  for i in range(1, len(rows)))
    print(f"\nangle range {math.degrees(lo):+.3f} .. {math.degrees(hi):+.3f} deg; "
          f"largest step {biggest[0]:.6f} rad/frame ({math.degrees(biggest[0]):.3f} deg) at poll {biggest[1]}")


if __name__ == "__main__":
    main()
