#!/usr/bin/env python3
"""Read the push lines of a PSPRECOMP_INPUT_LOG and print the walk they show.

host/replacements.c writes, once per frame the push primitive ran for the
player's AC:

    <poll> push m=<0..1> dir=<x>,<z> cap=<units/frame> speed=<units/frame> <orig|orig-centred|dual|modern>

This folds consecutive frames with the same stick (m, dir) into one row and
reports the speed the AC reached by the end of it and how many frames it
took to get within a percent of the cap -- the walk's ramp -- so the law
reads as a table: deflection in, speed out.

    scripts/walk-analyze.py reports/walk-sweep.log [--from 2100] [--to 2700]
"""

import argparse
import math
import re

LINE = re.compile(r"^(\d+) push m=(\S+) dir=(\S+),(\S+) cap=(\S+) speed=(\S+)"
                  r"(?: pos=(\S+),(\S+))? (\S+)$")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--from", dest="lo", type=int, default=0)
    ap.add_argument("--to", dest="hi", type=int, default=1 << 30)
    args = ap.parse_args()

    rows = []
    for line in open(args.log):
        m = LINE.match(line)
        if not m:
            continue
        poll = int(m.group(1))
        if poll < args.lo or poll > args.hi:
            continue
        pos = (float(m.group(7)), float(m.group(8))) if m.group(7) else None
        rows.append((poll, float(m.group(2)), float(m.group(3)), float(m.group(4)),
                     float(m.group(5)), float(m.group(6)), m.group(9), pos))
    if not rows:
        print("no push lines in that range")
        return

    segs = []
    for r in rows:
        poll, mag, dx, dz, cap, speed, how, pos = r
        key = (round(mag, 3), round(dx, 3), round(dz, 3), how)
        if segs and segs[-1]["key"] == key and poll == segs[-1]["end"] + 1:
            s = segs[-1]
            s["end"] = poll
            s["speeds"].append(speed)
            s["pos_end"] = pos
        else:
            segs.append({"key": key, "start": poll, "end": poll, "cap": cap,
                         "speeds": [speed], "pos_start": pos, "pos_end": pos})

    print(f"{'polls':>13}  {'n':>3}  {'m':>5}  {'dir (deg from fwd)':>18}  "
          f"{'cap':>7}  {'speed end':>9}  {'to 99%':>6}  {'moved (deg, /frame)':>20}  how")
    for s in segs:
        mag, dx, dz, how = s["key"]
        n = len(s["speeds"])
        # The game's frame has x to the LEFT and z back; print angles from
        # forward with positive to the right, the way a player reads them.
        ang = math.degrees(math.atan2(-dx, -dz)) if (dx or dz) else float("nan")
        reached = next((i + 1 for i, v in enumerate(s["speeds"]) if v >= 0.99 * s["cap"]), None)
        # A speed that fell back under the cap while the stick held still is a wall.
        blocked = reached is not None and any(v < 0.95 * s["cap"] for v in s["speeds"][reached:])
        moved = "--"
        if s["pos_start"] and s["pos_end"] and n > 1:
            mx = s["pos_end"][0] - s["pos_start"][0]
            mz = s["pos_end"][1] - s["pos_start"][1]
            if mx or mz:
                moved = f"{math.degrees(math.atan2(-mx, -mz)):+7.1f} {math.hypot(mx, mz) / (n - 1):.4f}"
        print(f"{s['start']:>6}-{s['end']:<6} {n:>3}  {mag:>5.3f}  "
              f"{(f'{ang:+7.1f}' if ang == ang else '   --  '):>18}  "
              f"{s['cap']:>7.4f}  {s['speeds'][-1]:>9.4f}  "
              f"{(str(reached) if reached else '--'):>6}  {moved:>20}  {how}"
              f"{'  BLOCKED' if blocked else ''}")


if __name__ == "__main__":
    main()
