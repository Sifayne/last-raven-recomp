#!/usr/bin/env python3
"""Stage 06 — cluster oracle divergences into distinct patterns.

A corpus run reports one block of lines per disagreeing function. Read as a
flat list that is unusable: hundreds of entries, no way to tell whether they
are hundreds of bugs or one bug hit hundreds of times.

They are rarely independent. Every divergence chased so far turned out to be
systematic — a constant $sp delta, a register the two sides disagree about in
the same way each time. So the useful move is to reduce each function to a
*signature* (which things differed, and by how much where that is meaningful)
and group by it. A hundred functions with one signature is one investigation.

Output is ordered by cluster size, because the biggest cluster is both the most
informative and the most likely to be a harness artifact rather than a codegen
bug — the base rate for that in this project is high.
"""

import collections
import re
import sys

REG   = re.compile(r"^\s+([0-9A-F]{8})\s+\$(\w+)\s+interp=([0-9A-F]{8})\s+recomp=([0-9A-F]{8})")
HILO  = re.compile(r"^\s+([0-9A-F]{8})\s+(hi|lo)\s+interp=([0-9A-F]{8})\s+recomp=([0-9A-F]{8})")
MEM   = re.compile(r"^\s+([0-9A-F]{8})\s+stack\[([0-9A-F]{8})\]")
MOD   = re.compile(r"^\s+([0-9A-F]{8})\s+module(?:\[([0-9A-F]{8})\]| image differs)")


def parse(path):
    """-> {func_addr: [(kind, detail, interp, recomp), ...]}"""
    out = collections.defaultdict(list)
    with open(path) as f:
        for line in f:
            m = REG.match(line) or HILO.match(line)
            if m:
                fn, what, iv, rv = m.groups()
                out[fn].append(("reg", what, int(iv, 16), int(rv, 16)))
                continue
            m = MEM.match(line)
            if m:
                out[m.group(1)].append(("stack", m.group(2), None, None))
                continue
            m = MOD.match(line)
            if m:
                out[m.group(1)].append(("module", m.group(2) or "", None, None))
    return out


def signature(items):
    """A stable description of *how* a function diverged, not by how much —
    except where the magnitude is itself the pattern, as with a constant $sp
    delta, which is the tell for a frame-size disagreement."""
    parts = []
    for kind, what, iv, rv in sorted(items):
        if kind == "reg":
            if what == "sp":
                # Signed delta matters here and is usually constant across a
                # whole cluster; the absolute addresses are not comparable.
                d = (rv - iv) & 0xFFFFFFFF
                d = d - 0x100000000 if d > 0x7FFFFFFF else d
                parts.append(f"$sp{d:+#x}")
            else:
                zero = "interp=0" if iv == 0 else ("recomp=0" if rv == 0 else "")
                parts.append(f"${what}" + (f"({zero})" if zero else ""))
        elif kind == "stack":
            parts.append("stack")
        else:
            parts.append("module")
    return " ".join(parts)


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "reports/05-oracle-full.txt"
    byfn = parse(path)
    if not byfn:
        print(f"no divergences parsed from {path}")
        return

    clusters = collections.defaultdict(list)
    for fn, items in byfn.items():
        clusters[signature(items)].append(fn)

    print(f"{len(byfn)} diverging functions in {len(clusters)} distinct patterns\n")
    print(f"{'n':>5}  {'pattern':<52} examples")
    print("-" * 100)
    for sig, fns in sorted(clusters.items(), key=lambda kv: -len(kv[1])):
        ex = " ".join("0x" + a for a in sorted(fns)[:3])
        print(f"{len(fns):>5}  {sig[:52]:<52} {ex}")

    print("\nInvestigate one exemplar per pattern. Reproduce with:")
    print("  build/host/oracle_diff game/extracted/ACLR_App.elf --from 0x<addr> --verbose")
    print("  build/psprecomp/tools/allegrexrecomp/allegrexrecomp interp \\")
    print("      game/extracted/ACLR_App.elf --from 0x<addr> --regs --budget 400")


if __name__ == "__main__":
    main()
