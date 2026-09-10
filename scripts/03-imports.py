#!/usr/bin/env python3
"""Stage 03 — measure the firmware surface this module needs.

This is the number that sizes the OS work. A PSP game reaches the firmware
through a module import table: a list of libraries, each with an array of NIDs
(the first 4 bytes of the SHA-1 of the function name). That table is a complete,
enumerable statement of everything the game asks the OS for — which is the
structural reason a PSP recompilation is tractable at all.

So: parse the table out of the ELF, diff it against the NIDs psprecomp actually
registers, and report the gap per library.

`allegrexrecomp funcs` reports the same totals. This exists to break them down
into implemented-vs-missing, which is what decides the work order.
"""

import glob
import os
import re
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load_elf(path):
    with open(path, "rb") as f:
        d = f.read()
    if d[:4] != b"\x7fELF":
        sys.exit(f"not an ELF: {path} (decrypt it first — scripts/01-extract-decrypt.sh)")
    return d


def segments(d):
    """PT_LOAD segments as (vaddr, file_offset, filesz), for vaddr->offset."""
    (phoff,) = struct.unpack("<I", d[28:32])
    phentsz, phnum = struct.unpack("<HH", d[42:46])
    out = []
    for i in range(phnum):
        o = phoff + i * phentsz
        p_type, p_off, p_vaddr, _, p_filesz = struct.unpack("<5I", d[o:o + 20])
        if p_type == 1:
            out.append((p_vaddr, p_off, p_filesz))
    return out


def find_module_info(d, segs):
    """Locate .rodata.sceModuleInfo, whose fixed layout ends with the import
    table bounds. Reading the section header is exact; scanning for the magic
    is not, and a wrong guess here silently mis-sizes the whole report."""
    shoff, = struct.unpack("<I", d[32:36])
    shentsz, shnum, shstrndx = struct.unpack("<HHH", d[46:52])
    names_off = struct.unpack("<I", d[shoff + shstrndx * shentsz + 16:
                                      shoff + shstrndx * shentsz + 20])[0]
    for i in range(shnum):
        o = shoff + i * shentsz
        name_idx, _, _, sh_addr, sh_off, sh_size = struct.unpack("<6I", d[o:o + 24])
        name = d[names_off + name_idx:d.index(b"\0", names_off + name_idx)].decode()
        if name == ".rodata.sceModuleInfo":
            return sh_off, sh_addr, sh_size
    sys.exit("no .rodata.sceModuleInfo section — is this a PRX?")


def imports_from_elf(path):
    d = load_elf(path)
    segs = segments(d)

    def v2o(va):
        for vaddr, off, fsz in segs:
            if vaddr <= va < vaddr + fsz:
                return off + (va - vaddr)
        return None

    def cstr(va):
        o = v2o(va)
        return d[o:d.index(b"\0", o)].decode("latin-1") if o is not None else f"<{va:#x}>"

    mi_off, _, _ = find_module_info(d, segs)
    # sceModuleInfo: attr, ver[2], name[28], gp, ent_top, ent_end, stub_top, stub_end
    stub_top, stub_end = struct.unpack("<II", d[mi_off + 44:mi_off + 52])

    need = {}
    o = v2o(stub_top)
    for i in range((stub_end - stub_top) // 20):
        e = o + i * 20
        name_va, _flags, _entsz, _varcnt, funccnt, nid_va, _stub_va = \
            struct.unpack("<IIBBHII", d[e:e + 20])
        no = v2o(nid_va)
        need[cstr(name_va)] = set(struct.unpack(f"<{funccnt}I", d[no:no + funccnt * 4]))
    return need


def implemented():
    """NIDs psprecomp registers, by exact call site — not a loose hex grep,
    which would count unrelated constants as coverage."""
    pat = re.compile(r'psp_hle_register\(\s*0x([0-9A-Fa-f]{8})\s*,\s*"([^"]+)"\s*,\s*"([^"]+)"')
    # A NID whose name is not known is registered without one; it is still
    # implemented, and leaving it out here once understated coverage by two.
    pat_unnamed = re.compile(r'psp_hle_register_unnamed\(\s*0x([0-9A-Fa-f]{8})\s*,\s*"([^"]+)"')
    have = {}
    for p in glob.glob(os.path.join(ROOT, "tools/psprecomp/src/hle/*.c")):
        with open(p) as f:
            src = f.read()
        for nid, lib, name in pat.findall(src):
            have[int(nid, 16)] = (lib, name)
        for nid, lib in pat_unnamed.findall(src):
            have[int(nid, 16)] = (lib, "<unnamed>")
    return have


def default_elf():
    """The decrypted module for the title GAME selects (see scripts/common.sh):
    scripts/games/<slug>.sh names the module, and every slug but Last Raven's
    lives under games/<slug>/. Read from the profile directly rather than
    through common.sh, whose checks and side effects are not wanted here."""
    game = os.environ.get("GAME", "aclr")
    module = "ACLR_App"
    profile = os.path.join(ROOT, "scripts", "games", game + ".sh")
    if os.path.exists(profile):
        with open(profile) as f:
            m = re.search(r'^MODULE="([^"]+)"', f.read(), re.M)
        if m:
            module = m.group(1)
    game_dir = "game" if game == "aclr" else os.path.join("games", game)
    return os.path.join(ROOT, game_dir, "extracted", module + ".elf")

def main():
    elf = sys.argv[1] if len(sys.argv) > 1 else default_elf()
    need = imports_from_elf(elf)
    have = implemented()

    total = sum(len(v) for v in need.values())
    done = sum(len(v & have.keys()) for v in need.values())

    print(f"module:   {os.path.basename(elf)}")
    print(f"imports:  {total} functions across {len(need)} libraries")
    print(f"in psprecomp: {done}/{total} ({100 * done / total:.1f}%)\n")

    print(f"{'library':<22} {'need':>5} {'have':>5} {'miss':>5}")
    print("-" * 42)
    for lib, nids in sorted(need.items(), key=lambda kv: (-len(kv[1] - have.keys()), kv[0])):
        h = len(nids & have.keys())
        print(f"{lib:<22} {len(nids):>5} {h:>5} {len(nids) - h:>5}")
    print("-" * 42)
    print(f"{'TOTAL':<22} {total:>5} {done:>5} {total - done:>5}\n")

    print("missing NIDs by library (names unknown — needs a NID table):")
    for lib, nids in sorted(need.items()):
        miss = sorted(nids - have.keys())
        if miss:
            print(f"  {lib} ({len(miss)})")
            for n in miss:
                print(f"      0x{n:08X}")


if __name__ == "__main__":
    main()
