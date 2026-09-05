#!/usr/bin/env python3
"""Find the words in guest RAM that change by a constant from snapshot to snapshot.

Feed it PSPRECOMP_RAMSNAP files taken a poll apart while the stick is held:

    scripts/ram-diff.py reports/snap-2130.ram reports/snap-2131.ram \\
                        reports/snap-2132.ram reports/snap-2133.ram

A word that moves by the same non-zero amount between every consecutive pair is
an integrator's output -- a heading, a timer, a position component. The step
says what the unit is: 0x180 per frame in a 16-bit word is a binary angle
stepping 384/65536 of a turn, which is 2.109 degrees, which is what the camera
was measured doing. Three snapshots give two differences and one equality;
four give three, and the false positives fall away fast.

Every width is tried -- 16-bit, 32-bit, and 32-bit float -- because the
representation is the unknown. --delta narrows to one step once it is known;
--min-abs hides the frame counters ticking by 1.

FINDPTR cannot do this: it needs a value to look for, and the value is exactly
what is not known until this has run.
"""

import argparse
import struct
import sys

RAM_BASE = 0x08000000


def load(path):
    with open(path, "rb") as f:
        data = f.read()
    return data


def signed(v, bits):
    return v - (1 << bits) if v >= (1 << (bits - 1)) else v


def scan_int(snaps, width, args):
    """Yield (addr, delta, values) for words with a constant non-zero delta."""
    fmt = {16: "H", 32: "I"}[width]
    n = len(snaps[0]) // (width // 8)
    views = [memoryview(s).cast(fmt) for s in snaps]
    mask = (1 << width) - 1
    lo, hi = args.range
    first = (lo - RAM_BASE) // (width // 8)
    last = min(n, (hi - RAM_BASE) // (width // 8))
    for i in range(first, last):
        a, b = views[0][i], views[1][i]
        d = (b - a) & mask
        if d == 0:
            continue
        ok = True
        for k in range(1, len(views) - 1):
            if ((views[k + 1][i] - views[k][i]) & mask) != d:
                ok = False
                break
        if not ok:
            continue
        sd = signed(d, width)
        if args.delta is not None and sd != args.delta:
            continue
        if abs(sd) < args.min_abs:
            continue
        yield RAM_BASE + i * (width // 8), sd, [v[i] for v in views]


def scan_float(snaps, args):
    n = len(snaps[0]) // 4
    views = [memoryview(s).cast("f") for s in snaps]
    lo, hi = args.range
    first = (lo - RAM_BASE) // 4
    last = min(n, (hi - RAM_BASE) // 4)
    for i in range(first, last):
        vals = [v[i] for v in views]
        if any(x != x or abs(x) > 1e30 for x in vals):   # NaN, inf, garbage
            continue
        d = vals[1] - vals[0]
        if abs(d) < 1e-6:
            continue
        tol = abs(d) * 1e-3 + 1e-7
        if any(abs((vals[k + 1] - vals[k]) - d) > tol for k in range(1, len(vals) - 1)):
            continue
        yield RAM_BASE + i * 4, d, vals


def parse_range(s):
    lo, hi = s.split("-", 1)
    return int(lo, 0), int(hi, 0)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("snapshots", nargs="+", help="PSPRECOMP_RAMSNAP files, in poll order")
    ap.add_argument("--delta", type=lambda s: int(s, 0), default=None,
                    help="only integer words stepping by exactly this (e.g. 0x180, -0x180)")
    ap.add_argument("--min-abs", type=int, default=2,
                    help="hide integer steps smaller than this (default 2: skips "
                         "the frame counters)")
    ap.add_argument("--base", type=lambda s: int(s, 0), default=None,
                    help="guest address of the file's first byte (default: "
                         "0x08000000 for .ram files, 0 for .mod files)")
    ap.add_argument("--range", type=parse_range, default=None,
                    metavar="LO-HI", help="guest address range to scan")
    ap.add_argument("--no-float", action="store_true")
    ap.add_argument("--top", type=int, default=60, help="lines per width (default 60)")
    args = ap.parse_args()

    if len(args.snapshots) < 3:
        print("need at least three snapshots: two differences to compare", file=sys.stderr)
        return 2
    snaps = [load(p) for p in args.snapshots]
    if len({len(s) for s in snaps}) != 1:
        print("snapshots differ in size", file=sys.stderr)
        return 2
    global RAM_BASE
    if args.base is not None:
        RAM_BASE = args.base
    elif args.snapshots[0].endswith(".mod"):
        RAM_BASE = 0                      # the module image starts at guest 0
    if args.range is None:
        args.range = (RAM_BASE, RAM_BASE + len(snaps[0]))
    print(f"scanning guest {args.range[0]:08X}-{args.range[1]:08X}")
    print(f"{len(snaps)} snapshots of {len(snaps[0]) >> 20} MB, "
          f"{len(snaps) - 1} differences each must agree")

    for width in (16, 32):
        hits = list(scan_int(snaps, width, args))
        print(f"\n{width}-bit words with a constant step: {len(hits)}")
        for addr, d, vals in hits[:args.top]:
            shown = " ".join(f"{v:0{width // 4}X}" for v in vals)
            print(f"  {addr:08X}  step {d:+d} (0x{d & ((1 << width) - 1):X})  {shown}")
        if len(hits) > args.top:
            print(f"  ... {len(hits) - args.top} more; narrow with --delta or --range")

    if not args.no_float:
        hits = list(scan_float(snaps, args))
        print(f"\nfloat words with a constant step: {len(hits)}")
        for addr, d, vals in hits[:args.top]:
            shown = " ".join(f"{v:.6g}" for v in vals)
            print(f"  {addr:08X}  step {d:+.6g}  {shown}")
        if len(hits) > args.top:
            print(f"  ... {len(hits) - args.top} more")
    return 0


if __name__ == "__main__":
    sys.exit(main())
