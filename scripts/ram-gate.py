#!/usr/bin/env python3
"""Find the words that tell a mission from a menu: null in every menu snapshot,
the same non-zero pointer in every mission snapshot (or the reverse).

    scripts/ram-gate.py --menu reports/snap-3000.mod --play reports/snap-7000.mod reports/snap-7400.mod

Feed it the <prefix>-<poll>.mod files PSPRECOMP_RAMSNAP writes (the module
image: data and BSS, where a game keeps its globals), one or more taken in
menus and one or more in play. A native replacement that must only act while
the player is being played -- Last Raven's converter gates on its AC object's
movement-state pointer, docs/findings/state.md -- needs exactly one such word
in another title, and this lists the candidates so a WATCHMEM or a PEEK can
sort them. Pointer-like means into RAM (0x08800000..0x0A000000) or into the
module (below its image size). --any drops the pointer test and lists every
word that is zero on one side and one constant on the other.

.ram files work too (--base 0x08000000 is assumed for them)."""
import argparse, struct, sys

def load(path):
    with open(path, 'rb') as f: return f.read()

def words(data):
    n = len(data) // 4
    return struct.unpack(f'<{n}I', data[:n * 4])

def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--menu', nargs='+', required=True, help='snapshots taken in menus')
    ap.add_argument('--play', nargs='+', required=True, help='snapshots taken in play')
    ap.add_argument('--any', action='store_true', help='no pointer-shape test')
    ap.add_argument('--top', type=int, default=80)
    args = ap.parse_args()
    base = 0x08000000 if args.menu[0].endswith('.ram') else 0
    menu = [words(load(p)) for p in args.menu]
    play = [words(load(p)) for p in args.play]
    n = min(len(w) for w in menu + play)
    size = n * 4
    def pointer_like(v):
        if args.any: return True
        if v & 3: return False
        return (0x08800000 <= v < 0x0A000000) or (0x1000 <= v < size and base == 0)
    up, down = [], []
    for i in range(n):
        m = menu[0][i]
        if any(w[i] != m for w in menu): continue
        p = play[0][i]
        if any(w[i] != p for w in play): continue
        if m == 0 and p != 0 and pointer_like(p): up.append((base + i * 4, p))
        elif p == 0 and m != 0 and pointer_like(m): down.append((base + i * 4, m))
    for title, hits in (('null in menus, one pointer in play', up), ('one pointer in menus, null in play', down)):
        print(f'{title}: {len(hits)}')
        for a, v in hits[:args.top]: print(f'  0x{a:08X} = 0x{v:08X}')
        if len(hits) > args.top: print(f'  ... {len(hits) - args.top} more')

if __name__ == '__main__':
    main()
