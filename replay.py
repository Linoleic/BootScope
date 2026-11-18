"""Replay recorded Android startup snapshots as a terminal panel."""
import argparse
from collections import deque
import math
import os
from pathlib import Path
import sys
import time

from analyze_probe import read_boot_events, read_samples

GROUPS = (
    ('Core services', ('ueventd', 'logd', 'servicemanager', 'vold')),
    ('Display', ('surfaceflinger', 'bootanim')),
    ('Runtime', ('zygote', 'zygote_secondary')),
)
STATE_ICONS = {'running': '●', 'stopped': '○', 'restarting': '↻',
               'stopping': '◐', 'unknown': '?'}


def render(header, snapshot, recent, total):
    index, uptime = header
    services = snapshot['services']
    yield f'BootScope  /  recorded boot{uptime:>8.2f} s'
    yield f'Sample {index + 1}/{total}  ·  {len(services)} visible init services'
    yield ''
    for title, names in GROUPS:
        yield title
        for number, name in enumerate(names):
            prefix = '└─' if number == len(names) - 1 else '├─'
            service = services.get(name, {})
            state = service.get('state', 'unknown')
            first = service.get('first_start_boottime_ns')
            since = f'  first start +{first / 1e9:.3f} s' if first is not None else ''
            yield f'{prefix} {STATE_ICONS[state]} {name:<17} {state:<10}{since}'
    yield ''
    completed = snapshot['boot_completed_raw'] or '?'
    anim_exit = snapshot['bootanim_exit_raw'] or '?'
    yield f'sys.boot_completed={completed}  ·  service.bootanim.exit={anim_exit}'
    yield ''
    yield 'Recent observations'
    yield from recent or ('  Waiting for a second sample',)


def frames(samples):
    recent = deque(maxlen=5)
    previous = None
    for header, snapshot in samples:
        if previous is not None:
            for _, names in GROUPS:
                for name in names:
                    before = previous['services'].get(name, {}).get('state', 'unknown')
                    after = snapshot['services'].get(name, {}).get('state', 'unknown')
                    if before != after:
                        recent.append(f'  +{header[1]:.2f} s  {name}: {before} → {after}')
            for key, label in (('bootanim_exit_raw', 'service.bootanim.exit'),
                               ('boot_completed_raw', 'sys.boot_completed')):
                if previous[key] != snapshot[key]:
                    recent.append(f'  +{header[1]:.2f} s  {label}={snapshot[key] or "?"}')
        yield header[1], '\n'.join(render(header, snapshot, recent, len(samples)))
        previous = snapshot


def terminal_supports_redraw():
    if not sys.stdout.isatty():
        return False
    if os.name != 'nt':
        return os.environ.get('TERM') != 'dumb'
    import ctypes
    import msvcrt
    from ctypes import wintypes

    console = ctypes.windll.kernel32
    handle = wintypes.HANDLE(msvcrt.get_osfhandle(sys.stdout.fileno()))
    mode = wintypes.DWORD()
    return bool(console.GetConsoleMode(handle, ctypes.byref(mode)) and
                console.SetConsoleMode(handle, mode.value | 0x0004))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    parser.add_argument('--speed', type=float, default=1, help='Replay speed multiplier')
    parser.add_argument('--frame', type=int, help='Print one frame by its 1-based number')
    args = parser.parse_args()
    if not math.isfinite(args.speed) or args.speed <= 0:
        parser.error('--speed must be a finite positive number')
    try:
        playback = list(frames(read_samples(args.samples)))
        events = read_boot_events(args.samples.with_name('boot-events.tsv'))
    except (OSError, ValueError) as error:
        parser.error(str(error))
    if not playback:
        parser.error('no valid samples')
    if args.frame is not None:
        if not 1 <= args.frame <= len(playback):
            parser.error(f'--frame must be between 1 and {len(playback)}')
        print(playback[args.frame - 1][1])
        return
    if terminal_supports_redraw():
        start = time.monotonic()
        for uptime, frame in playback:
            due = start + (uptime - playback[0][0]) / args.speed
            time.sleep(max(0, due - time.monotonic()))
            print('\x1b[H\x1b[2J' + frame, flush=True)
    else:
        print(playback[-1][1])
    if events:
        print('\nFramework boot events (raw milliseconds)')
        for tag, value in events:
            print(f'  {tag:<34} {value:>7} ms')


if __name__ == '__main__':
    try:
        main()
    except KeyboardInterrupt:
        sys.exit(130)
