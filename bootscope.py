#!/usr/bin/env python3
"""Read-only Android init service snapshot and change monitor over ADB."""
import argparse
import json
import re
import subprocess
import sys
import time
from datetime import datetime, timezone

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

PROPERTY = re.compile(r'^\[([^]]+)\]: \[(.*)\]$')
STATES = {'running', 'stopped', 'stopping', 'restarting'}


def parse_properties(text):
    props = {}
    for line in text.splitlines():
        match = PROPERTY.match(line)
        if match:
            props[match.group(1)] = match.group(2)
    return props


def build_snapshot(props):
    services = {}
    for key, value in props.items():
        if not key.startswith('init.svc.') or key.startswith('init.svc_debug_pid.'):
            continue
        name = key[len('init.svc.'):]
        raw_time = props.get('ro.boottime.' + name, '')
        start_ns = int(raw_time) if raw_time.isdecimal() else None
        services[name] = {'state': value if value in STATES else 'unknown',
                          'first_start_boottime_ns': start_ns}
    raw_completed = props.get('sys.boot_completed')
    return {'captured_at_utc': datetime.now(timezone.utc).isoformat(),
            'boot_completed': None if raw_completed is None else raw_completed == '1',
            'boot_completed_raw': props.get('sys.boot_completed'),
            'services': services}


def read_snapshot(adb, serial):
    cmd = [adb] + (['-s', serial] if serial else []) + ['shell', 'getprop']
    result = subprocess.run(cmd, capture_output=True, text=True, encoding='utf-8',
                            errors='replace', timeout=15)
    if result.returncode:
        raise RuntimeError(result.stderr.strip() or 'adb getprop failed')
    props = parse_properties(result.stdout)
    if not props:
        raise RuntimeError('No Android properties returned')
    return build_snapshot(props)


def print_snapshot(snapshot):
    print('BootScope | init 服务快照 | ' + snapshot['captured_at_utc'])
    raw = snapshot['boot_completed_raw']
    print('sys.boot_completed: ' + (raw if raw is not None else '未知'))
    print('首次启动时间是开机后的时间点，并非初始化耗时。')
    print('状态表示 init 记录的进程状态，并不代表服务已就绪。')
    for name, service in sorted(snapshot['services'].items()):
        ns = service['first_start_boottime_ns']
        stamp = f'{ns / 1e9:.3f}s' if ns is not None else '未知'
        print(f"  {name}: {service['state']} | 首次启动 {stamp}")
    print(f"共 {len(snapshot['services'])} 项；仅包括可读取的 init 服务。")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('-s', '--serial', help='ADB device serial')
    parser.add_argument('--adb', default='adb', help='adb executable path')
    parser.add_argument('--watch', action='store_true', help='Print subsequent state changes')
    parser.add_argument('--interval', type=float, default=1.0, help='Polling interval in seconds')
    parser.add_argument('--json', action='store_true', help='Print snapshot as JSON')
    args = parser.parse_args()
    if args.interval < 0.2:
        parser.error('--interval must be at least 0.2 seconds')
    try:
        previous = read_snapshot(args.adb, args.serial)
        if args.json:
            print(json.dumps(previous, ensure_ascii=False, indent=2))
        else:
            print_snapshot(previous)
        while args.watch:
            time.sleep(args.interval)
            current = read_snapshot(args.adb, args.serial)
            for name in sorted(set(previous['services']) | set(current['services'])):
                before = previous['services'].get(name)
                after = current['services'].get(name)
                if before != after:
                    event = {'observed_at_utc': current['captured_at_utc'],
                             'service': name, 'before': before, 'after': after}
                    print(json.dumps(event, ensure_ascii=False), flush=True)
            if current['boot_completed_raw'] != previous['boot_completed_raw']:
                print(json.dumps({'observed_at_utc': current['captured_at_utc'],
                                  'property': 'sys.boot_completed',
                                  'before': previous['boot_completed_raw'],
                                  'after': current['boot_completed_raw']}, ensure_ascii=False), flush=True)
            previous = current
    except KeyboardInterrupt:
        return 0
    except (OSError, subprocess.TimeoutExpired, RuntimeError) as exc:
        print(f'BootScope: {exc}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
