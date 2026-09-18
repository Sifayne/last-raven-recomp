#!/usr/bin/env python3
"""Generate the title's native mission loop from the user's emitted module.

The original instructions remain in the user's generated directory. Hooks are
anchored to instruction addresses and fail closed when an emitter changes the
expected shape. No generated game code is shipped in the source package.
"""
import argparse
from pathlib import Path
import re


SPECS = {
    # loop, frame start, input, draw, interpolate open, tick tail, present
    'aclr': ('00102018','0010209C','00102148','001024C8','0010261C','0010265C','001026A8'),
    'ac3p': ('000E0F10','000E1118','000E1240','000E1A14','000E1B98','000E1DCC','000E1E30'),
    'acsl': ('000914B0','0009154C','0009167C','00091950','00091AD8','00091B04','00091B64'),
}


def generate(source, slug):
    address, frame, input_at, draw, opening, tail, present = SPECS[slug]
    closing = '000E1DD0' if slug == 'ac3p' else tail
    start = source.index(f'static void psp_body_{address}(')
    end = source.index(f'\nvoid psp_func_{address}', start)
    body = source[start:end].replace(f'psp_body_{address}', 'fps_native_loop')

    def insert(at, addition):
        nonlocal body
        anchor = f'    /* {at}  '
        if body.count(anchor) != 1:
            raise ValueError(f'{slug}: expected exactly one FPS anchor: {anchor}')
        body = body.replace(anchor, addition + anchor)

    setup = {'aclr': 'r_s0=0x00380000u; r_s1=psp_read32(r_sp+20);',
             'acsl': 'r_s0=0x002D0000u; r_s1=0x00520000u;',
             'ac3p': 'r_s1=4;'}[slug]
    adapter = 'aclr' if slug == 'aclr' else 'ac3'
    insert(frame, f'    fps_{adapter}_begin_frame();\n')
    # Keep display-list setup on every render. Only ticks enter input and
    # simulation. Restore the live locals that the original frame setup set.
    insert(input_at, f'    if (fps.inserted) goto L_{draw};\nfps_tick_top:\n    {setup}\n')
    before_render = {'aclr': opening, 'ac3p': '000E1B8C', 'acsl': '00091ACC'}[slug]
    catchup_updates = ' fps_aclr_catchup_updates();' if slug == 'aclr' else ''
    insert(before_render, f'    if (fps.ticks_left > 1) {{ fps.catchup=1;{catchup_updates} goto L_{tail}; }}\n')
    insert(opening, f'    fps_{adapter}_render_open();\n')
    insert(closing, f'    fps_{adapter}_render_close();\n')
    # The game's own tick tail runs unchanged, including pause, countdown and
    # local state. Catch-up takes that path before returning to input.
    prelude = {'aclr': ('002230D8', '00107A3C'),
               'ac3p': ('0017BB2C',), 'acsl': ('000D4AC8',)}[slug]
    repeat_prelude = ' '.join(f'psp_func_{f}();' for f in prelude)
    insert(present, f'    if (fps.catchup) {{\n'
                    f'        fps.catchup=0; fps.ticks_left--; fps.ticks++;\n'
                    f'        {{ psp_cpu_state saved=psp_cpu; {repeat_prelude} psp_cpu=saved; }}\n'
                    f'        goto fps_tick_top;\n'
                    f'    }}\n    fps_{adapter}_frame_log();\n')
    if slug == 'aclr':
        # Tick-only helpers: stop-state writers, the HUD lifecycle and the layer
        # dispatcher. 0008D8BC is deliberately NOT among them: it is the render
        # system's frame begin (0008B890 viewport/projection, 0008CECC/0008CF58/
        # 0008D100 view setup, and through them the display-list kick). With it
        # suppressed an inserted frame executed 1,737 GE commands instead of
        # 22,000 -- no world, only the HUD and the full-screen passes over the
        # previous frame's colour, which dimmed a little more on each inserted
        # frame (reports/aspect-reticle, 17 Sep). It advances no simulation
        # state that the stock-versus-enhanced replay comparison can see.
        for target in ('002230D8', '00107A3C', '000EFEB4', '00222974', '00229D6C', '0008F43C', '0022DCAC'):
            pattern = rf'(    )(\{{ uint32_t _spc = r_sp; psp_func_{target}\(\); PSP_SP_CALL\(0x{target}u, _spc, r_sp\); \}})'
            body, count = re.subn(pattern, r'\1if (!fps.inserted) \2', body)
            if count != 1:
                raise ValueError(f'missing tick-only helper {target}')
        call = '{{ uint32_t _spc = r_sp; psp_func_002231F8(); PSP_SP_CALL(0x002231F8u, _spc, r_sp); }}'.replace('{{','{').replace('}}','}')
        anchor = 'r_ra = 0x00102664u;\n    ' + call
        if body.count(anchor) != 1:
            raise ValueError('missing countdown query')
        body = body.replace(anchor, 'r_ra = 0x00102664u;\n    if (fps.inserted) r_v0=1; else ' + call)
    else:
        layer = '000E047C' if slug == 'acsl' else '0017CA9C'
        for target in (*prelude, layer):
            pattern = rf'(    )(\{{ uint32_t _spc = r_sp; psp_func_{target}\(\); PSP_SP_CALL\(0x{target}u, _spc, r_sp\); \}})'
            body, count = re.subn(pattern, r'\1if (!fps.inserted) \2', body)
            if count != 1:
                raise ValueError(f'missing tick-only update {target}')
        skip = ('r_a0=psp_read8(0x002D01C4u);' if slug == 'acsl'
                else 'r_a0=psp_read8(0x002BDB94u);')
        insert(closing, f'    if (fps.inserted) {{ {skip} goto L_{present}; }}\n')
    # Some starts are fall-through instructions, not emitter branch targets.
    for target in (tail, present):
        label = f'L_{target}: PSP_MARK(0x{target}u);'
        if label not in body:
            anchor = f'    /* {target}  '
            # Insert the label before the added hooks as well as the instruction.
            if target == tail:
                hook = (f'    fps_{adapter}_render_close();' if closing == tail else anchor)
            else:
                hook = '    if (fps.catchup) {'
            body = body.replace(hook, label + '\n' + hook, 1)
    return '/* Generated by scripts/fps-loop.py from this title\'s module. */\n' + body


def generate_rebuild(source, slug):
    # The AC rebuild ends in status/effect updates (including stagger timers).
    # Extra renders must stop before that tail, keeping only pose/mesh work.
    address, stop, epilogue = {
        'aclr': ('00052D38', '00052FF4', '00053024'),
        'ac3p': ('00108940', '00108C1C', '00108C2C'),
        'acsl': ('0000CB54', '0000CED0', '0000CEE0'),
    }[slug]
    start = source.index(f'static void psp_body_{address}(')
    end = source.index(f'\nvoid psp_func_{address}', start)
    body = source[start:end].replace(f'psp_body_{address}', 'fps_native_rebuild')
    for at in (stop, epilogue):
        if body.count(f'    /* {at}  ') != 1:
            raise ValueError(f'{slug}: missing rebuild boundary {at}')
    body = body.replace(f'    /* {stop}  ', f'    goto fps_rebuild_done;\n    /* {stop}  ')
    body = body.replace(f'    /* {epilogue}  ', f'fps_rebuild_done:\n    /* {epilogue}  ')
    return body


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('slug', choices=['aclr', 'ac3p', 'acsl'])
    ap.add_argument('generated', type=Path)
    args = ap.parse_args()
    source = (args.generated / f'{args.slug}_funcs.c').read_text()
    generated = generate(source, args.slug)
    generated += generate_rebuild(source, args.slug)
    if args.slug == 'aclr':
        generated += generate_hud(source)
    (args.generated / 'fps_loop.inc').write_text(generated)


def generate_hud(source):
    address = '0022E800'
    start = source.index(f'static void psp_body_{address}(')
    end = source.index(f'\nvoid psp_func_{address}', start)
    body = source[start:end].replace(f'psp_body_{address}', 'fps_native_hud')
    anchor = '    /* 0022E840  '
    if body.count(anchor) != 1 or 'L_0022E9D4:' not in body:
        raise ValueError('missing ACLR HUD update/draw boundary')
    # Use the existing update/draw split, retaining the original pause
    # argument on the stack so the complete HUD still draws between ticks.
    body = body.replace(anchor, '    if (fps.inserted) goto L_0022E9D4;\n' + anchor)
    anchor = '    /* 0022E9D4  '
    if body.count(anchor) != 1 or 'L_0022EA04:' not in body:
        raise ValueError('missing ACLR HUD catch-up boundary')
    return body.replace(anchor, '    if (fps.catchup) goto L_0022EA04;\n' + anchor)


if __name__ == '__main__':
    main()
