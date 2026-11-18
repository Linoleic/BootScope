"""Replay recorded Android startup snapshots and early init events as a terminal panel."""
import argparse
from collections import deque
import math
import os
from pathlib import Path
import sys
import time

from analyze_probe import read_boot_events, read_init_events, read_samples

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

GROUPS = (
    ('Core services', ('ueventd', 'logd', 'servicemanager', 'vold')),
    ('Display', ('surfaceflinger', 'bootanim')),
    ('Runtime', ('zygote', 'zygote_secondary')),
)
CORE_NAMES = {name for _, names in GROUPS for name in names}
STATE_ICONS = {'running': '●', 'stopped': '○', 'restarting': '↻',
               'stopping': '◐', 'unknown': '?'}
MAX_OTHER_CHANGED = 6
MAJOR_PHASES = {
    'kernel_modules', 'switch_root', 'second_stage', 'early-init',
    'init', 'late-init', 'early-fs', 'fs', 'post-fs', 'late-fs',
    'post-fs-data', 'zygote-start', 'early-boot', 'boot',
}


def _format_service_line(prefix, name, service):
    state = service.get('state', 'unknown') if service else 'unknown'
    first = service.get('first_start_boottime_ns') if service else None
    since = f'  first start +{first / 1e9:.3f} s' if first is not None else ''
    return f'{prefix} {STATE_ICONS[state]} {name:<26} {state:<10}{since}'


def render(subtitle, uptime, snapshot, recent, changed_other=(), init_phase=None):
    services = snapshot['services']
    yield f'BootScope  /  recorded boot{uptime:>8.2f} s'
    phase_note = f'  ·  phase: {init_phase}' if init_phase else ''
    yield f'{subtitle}  ·  {len(services)} visible init services{phase_note}'
    yield ''
    for title, names in GROUPS:
        yield title
        for number, name in enumerate(names):
            prefix = '└─' if number == len(names) - 1 else '├─'
            yield _format_service_line(prefix, name, services.get(name))
    if changed_other:
        shown = list(changed_other)[-MAX_OTHER_CHANGED:]
        extra = len(changed_other) - len(shown)
        heading = 'Changed services (other)' if extra <= 0 else f'Changed services (other, +{extra} earlier)'
        yield heading
        for number, name in enumerate(shown):
            prefix = '└─' if number == len(shown) - 1 else '├─'
            yield _format_service_line(prefix, name, services.get(name))
    yield ''
    completed = snapshot['boot_completed_raw'] or '?'
    anim_exit = snapshot['bootanim_exit_raw'] or '?'
    yield f'sys.boot_completed={completed}  ·  service.bootanim.exit={anim_exit}'
    yield ''
    yield 'Recent observations'
    yield from recent or ('  Waiting for a second sample',)


def _pre_snapshot_frames(init_events, first_sample_uptime, recent):
    """Synthesize early-boot timeline frames from dmesg init-events.tsv."""
    early = [ev for ev in init_events if ev[0] < first_sample_uptime]
    if not early:
        return [], None
    services = {}
    phase = None
    last_emit_ts = None
    emitted = []

    for idx, (ts, kind, name, detail) in enumerate(early):
        milestone = False
        if kind == 'phase':
            if name in MAJOR_PHASES or phase is None:
                phase = name
                milestone = True
            note = f' ({detail})' if detail else ''
            if name in MAJOR_PHASES:
                recent.append(f'  +{ts:.2f} s  phase: {name}{note}')
        elif kind == 'start':
            services.setdefault(name, {'state': 'running', 'first_start_boottime_ns': int(ts * 1e9)})
            services[name]['state'] = 'running'
            if name in CORE_NAMES:
                milestone = True
                recent.append(f'  +{ts:.2f} s  {name}: started (dmesg)')
        elif kind == 'exit':
            entry = services.setdefault(name, {'state': 'stopped', 'first_start_boottime_ns': int(ts * 1e9)})
            entry['state'] = 'stopped'
            if name in CORE_NAMES:
                milestone = True
                recent.append(f'  +{ts:.2f} s  {name}: exited ({detail})')

        is_last = idx == len(early) - 1
        next_ts = early[idx + 1][0] if not is_last else ts
        batch_end = is_last or (next_ts - ts) >= 0.05
        time_due = last_emit_ts is None or (ts - last_emit_ts) >= 0.75
        if (milestone and batch_end) or (time_due and batch_end) or is_last:
            snapshot = {
                'boot_completed_raw': None,
                'bootanim_exit_raw': None,
                'services': {k: dict(v) for k, v in services.items()},
            }
            subtitle = f'dmesg init {len(emitted) + 1}'
            emitted.append((ts, '\n'.join(render(subtitle, ts, snapshot, recent, init_phase=phase))))
            last_emit_ts = ts
    return emitted, phase


def frames(samples, init_events=None):
    recent = deque(maxlen=6)
    init_phase = None
    if init_events and samples:
        pre_frames, init_phase = _pre_snapshot_frames(init_events, samples[0][0][1], recent)
        yield from pre_frames

    changed_other = []
    seen_other = set()
    previous = None
    for header, snapshot in samples:
        if previous is not None:
            all_names = sorted(set(previous['services']) | set(snapshot['services']))
            for name in all_names:
                before_svc = previous['services'].get(name)
                after_svc = snapshot['services'].get(name)
                if before_svc == after_svc:
                    continue
                before = before_svc['state'] if before_svc else 'absent'
                after = after_svc['state'] if after_svc else 'absent'
                if before != after:
                    recent.append(f'  +{header[1]:.2f} s  {name}: {before} → {after}')
                else:
                    recent.append(f'  +{header[1]:.2f} s  {name}: boottime recorded')
                if name not in CORE_NAMES and name not in seen_other:
                    seen_other.add(name)
                    changed_other.append(name)
            for key, label in (('bootanim_exit_raw', 'service.bootanim.exit'),
                               ('boot_completed_raw', 'sys.boot_completed')):
                if previous[key] != snapshot[key]:
                    recent.append(f'  +{header[1]:.2f} s  {label}={snapshot[key] or "?"}')
        subtitle = f'Sample {header[0] + 1}/{len(samples)}'
        yield header[1], '\n'.join(
            render(subtitle, header[1], snapshot, recent, changed_other, init_phase=init_phase)
        )
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
        samples = read_samples(args.samples)
        init_events = read_init_events(args.samples.with_name('init-events.tsv'))
        playback = list(frames(samples, init_events=init_events))
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
