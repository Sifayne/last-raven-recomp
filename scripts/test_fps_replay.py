#!/usr/bin/env python3
"""Compare stock simulation with higher-FPS replays using locally owned assets."""
import argparse
import os
from pathlib import Path
import re
import subprocess


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    for key in ('boot', 'elf', 'iso', 'scenario'):
        ap.add_argument('--' + key, type=Path, required=True)
    ap.add_argument('--caps', nargs='+', default=['30', '60', '144'])
    ap.add_argument('--stop-at', type=int, help='truncate a recording and stop at this poll')
    ap.add_argument('--output', type=Path, required=True)
    ap.add_argument('--timeout', type=int, default=300)
    ap.add_argument('--gap-after', type=int, help='inject a 100 ms clock gap after this enhanced frame')
    args = ap.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    output = args.output.resolve()
    scenario = args.scenario.resolve()
    if args.stop_at:
        lines = []
        for line in scenario.read_text().splitlines():
            event = re.match(r'\s*@(\d+)\s+(\w+)', line)
            if event and (int(event[1]) >= args.stop_at or event[2] == 'stop'):
                continue
            lines.append(line)
        scenario = output / 'scenario.pad'
        scenario.write_text('\n'.join(lines) + f'\n@{args.stop_at} stop\n')
    reference = None
    for cap in ['off', *args.caps]:
        if cap not in ('off', 'unlimited') and not (cap.isdigit() and 30 <= int(cap) <= 1000):
            ap.error('caps must be whole numbers from 30 to 1000, or unlimited')
        env = os.environ.copy()
        env.update(PSPRECOMP_HIGH_FPS=str(int(cap != 'off')),
                   PSPRECOMP_FPS_CAP='60' if cap == 'off' else cap,
                   PSPRECOMP_RENDER='null', PSPRECOMP_WINDOW='0', PSPRECOMP_REALTIME='0',
                   PSPRECOMP_MPEG_DECODE='1', PSPRECOMP_INPUT='dual', PSPRECOMP_MOUSE='1',
                   PSPRECOMP_REPLAY=str(scenario), PSPRECOMP_REPLAY_LIVE='0',
                   PSPRECOMP_BAD_STOP='1', PSPRECOMP_DRAIN=str(args.timeout - 5),
                   PSPRECOMP_INPUT_LOG=str(output / f'{cap}.input'),
                   PSPRECOMP_FPS_LOG=str(output / f'{cap}.fps'))
        env.pop('PSPRECOMP_FPS_INTERPOLATE', None)
        env.pop('PSPRECOMP_FPS_TEST_GAP', None)
        if args.gap_after:
            env['PSPRECOMP_FPS_TEST_GAP'] = f'{args.gap_after}:100'
        (output / f'{cap}.fps').unlink(missing_ok=True)
        with (output / f'{cap}.log').open('w') as stdout, (output / f'{cap}.err').open('w') as stderr:
            subprocess.run([args.boot.resolve(), args.elf.resolve(), args.iso.resolve()],
                           env=env, stdout=stdout, stderr=stderr, check=True, timeout=args.timeout)
        log = (output / f'{cap}.log').read_text()
        assert 'bad mem:  0 accesses' in log, f'{cap}: guest memory fault; see {output}'
        assert 'replay finished' in log, f'{cap}: replay did not finish; see {output}'
        events = re.search(r'replay:\s+(\d+)/(\d+) event', log)
        assert events and events[1] == events[2], f'{cap}: incomplete input delivery'
        controls = (output / f'{cap}.input').read_text().splitlines()
        if reference is None:
            reference = controls
        else:
            assert controls == reference, f'{cap}: gameplay differs from stock; compare {output}/off.input and {cap}.input'
        print(f'{cap}: {len(controls)} control records match; no bad guest accesses', flush=True)


if __name__ == '__main__':
    main()
