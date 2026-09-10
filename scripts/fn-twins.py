#!/usr/bin/env python3
"""Find a Last Raven function's twin in another title's emitted C.

    scripts/fn-twins.py ac3p 00279A50 0004F248 ...
    scripts/fn-twins.py acsl --replaced        # every address in host/replace.txt
    scripts/fn-twins.py acsl --from ac3p 00106B3C   # twins of another title's functions

Every statement of the emitted C carries its disassembly, so a function is a
sequence of instructions. Two functions are twins when the sequence matches
exactly with every number masked (addresses and offsets differ per build,
registers and opcodes do not). Otherwise the closest function by opcode
sequence is reported with its similarity; below about 0.8 it is not the same
function, just the least different one, and a small function can score high
by accident. This is how the 7 Sep survey found that the pad layer (button
converter, stick converter, pad adaptor, deadzone helper) is shared by all
three titles and the gameplay code is not -- docs/findings/sibling-titles.md.

The parse of a 60-80 MB funcs.c is cached beside the file as <prefix>_twins.pickle."""
import difflib, os, pickle, re, sys
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
HDR = re.compile(r'^ \* psp_func_([0-9A-F]{8})  --  (\d+) instructions')
INS = re.compile(r'^\s*/\* ([0-9A-F]{8})  (\S+)\s*(.*?)\s*\*/\s*$')
NUM = re.compile(r'-?\b(0x[0-9A-Fa-f]+|\d+)\b')

def profile(slug):
    text = open(os.path.join(ROOT, 'scripts', 'games', slug + '.sh')).read()
    prefix = re.search(r'^PREFIX="([^"]+)"', text, re.M).group(1)
    gdir = 'game' if slug == 'aclr' else os.path.join('games', slug)
    return os.path.join(ROOT, gdir, 'generated', prefix + '_funcs.c')

def load(slug):
    path = profile(slug); cache = path.replace('_funcs.c', '_twins.pickle')
    if os.path.exists(cache) and os.path.getmtime(cache) > os.path.getmtime(path):
        return pickle.load(open(cache, 'rb'))
    funcs, cur = {}, None
    with open(path, errors='replace') as f:
        for line in f:
            m = HDR.match(line)
            if m: cur = funcs.setdefault(m.group(1), []); continue
            if cur is None: continue
            m = INS.match(line)
            if m: cur.append((m.group(2), NUM.sub('#', m.group(3))))
    pickle.dump(funcs, open(cache, 'wb')); return funcs

def main():
    args = sys.argv[1:]
    if not args: sys.exit(__doc__)
    other = args.pop(0)
    source = 'aclr'
    if args[:1] == ['--from']:            # twins of another title's functions
        source = args[1]; args = args[2:]
    if args == ['--replaced']:
        args = [l.split()[0] for l in open(os.path.join(ROOT, 'host', 'replace.txt')) if l.strip() and not l.startswith('#')]
    lr, db = load(source), load(other)
    full = lambda f: tuple(m + ' ' + o for m, o in f)
    index = {}
    for k, v in db.items(): index.setdefault(full(v), []).append(k)
    print(f"{source:9} {'len':>4}  {other + ' twin':>12} {'len':>4} {'sim':>5}")
    for addr in args:
        addr = addr.upper().replace('0X', '')
        f = lr.get(addr)
        if f is None: print(f"{addr:9} not a function in {source}'s emit"); continue
        exact = index.get(full(f))
        if exact: print(f"{addr:9} {len(f):4d}  {exact[0]:>12} {len(f):4d}  1.00  identical" + (f" (+{len(exact)-1} more)" if len(exact) > 1 else "")); continue
        mn = [m for m, o in f]; best = (0.0, '-', 0)
        for k, v in db.items():
            if not 0.7 * len(f) <= len(v) <= 1.3 * len(f): continue
            sm = difflib.SequenceMatcher(None, mn, [m for m, o in v], autojunk=False)
            if sm.real_quick_ratio() <= best[0] or sm.quick_ratio() <= best[0]: continue
            r = sm.ratio()
            if r > best[0]: best = (r, k, len(v))
        print(f"{addr:9} {len(f):4d}  {best[1]:>12} {best[2]:4d} {best[0]:5.2f}  {'probably the same' if best[0] >= 0.8 else 'different code'}")

if __name__ == '__main__':
    main()
