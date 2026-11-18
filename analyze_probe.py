"""Summarize a BootScope probe samples.txt file without inferring causation."""
import argparse
import math
import re
import sys
from pathlib import Path
from bootscope import build_snapshot, parse_properties

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HEADER = re.compile(r'^--- sample=(\d+) uptime=([0-9.]+) ---$')
INIT_STAGE_NS_KEYS = ('init', 'init.first_stage', 'init.selinux')
INIT_STAGE_MS_PREFIXES = (
    'init.cold_boot_wait',
    'init.modules',
    'init.mount_all.',
    'init.fsck.',
    'init.mount.',
)
KEY_PHASES = (
    'kernel_modules',
    'switch_root',
    'second_stage',
    'early-init',
    'init',
    'late-init',
    'early-fs',
    'fs',
    'post-fs',
    'late-fs',
    'post-fs-data',
    'zygote-start',
    'firmware_mounts_complete',
    'early-boot',
    'boot',
)
KEY_SERVICES = (
    'ueventd',
    'logd',
    'servicemanager',
    'vold',
    'surfaceflinger',
    'bootanim',
    'apexd',
    'zygote',
)


def read_samples(path):
    samples, lines, header = [], [], None
    for line in path.read_text(encoding='utf-8').splitlines():
        match = HEADER.match(line)
        if match:
            if header is not None:
                samples.append((header, sample_snapshot(lines)))
            header, lines = (int(match.group(1)), float(match.group(2))), []
        else:
            lines.append(line)
    if header is not None:
        samples.append((header, sample_snapshot(lines)))
    return samples


def sample_snapshot(lines):
    props = parse_properties('\n'.join(lines))
    snapshot = build_snapshot(props)
    snapshot['bootanim_exit_raw'] = props.get('service.bootanim.exit')
    snapshot['init_stages'] = extract_init_boottime(props)
    return snapshot


def extract_init_boottime(props):
    """Extract AOSP init stage timing properties from parsed getprop mapping."""
    stages = []
    for key in sorted(props):
        if not key.startswith('ro.boottime.'):
            continue
        suffix = key[len('ro.boottime.'):]
        raw = props[key]
        if not raw.isascii() or not raw.isdecimal():
            continue
        value = int(raw)
        if suffix in INIT_STAGE_NS_KEYS:
            stages.append((suffix, value / 1e6, 'ns', value))
        elif suffix.startswith(INIT_STAGE_MS_PREFIXES):
            stages.append((suffix, float(value), 'ms', value))
    return stages


def read_boot_events(path):
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding='utf-8').splitlines():
        tag, separator, value = line.partition('\t')
        if separator and value.isascii() and value.isdecimal():
            events.append((tag, int(value)))
    return events


def read_init_events(path):
    """Read sanitized dmesg init-events.tsv entries: (ts_s, kind, name, detail)."""
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding='utf-8').splitlines():
        parts = line.split('\t')
        if len(parts) < 3:
            continue
        ts_raw, kind, name = parts[0], parts[1], parts[2]
        detail = parts[3] if len(parts) > 3 else ''
        if kind not in {'phase', 'start', 'exit'} or not name:
            continue
        try:
            ts = float(ts_raw)
        except ValueError:
            continue
        if not math.isfinite(ts) or ts < 0:
            continue
        events.append((ts, kind, name, detail))
    return events


def parse_exit_detail(detail):
    """Extract (status, took_s) from exit detail string 'status=0;took=0.192s'."""
    status, took = None, None
    for part in detail.split(';'):
        if part.startswith('status='):
            status = part[len('status='):]
        elif part.startswith('took=') and part.endswith('s'):
            try:
                took = float(part[len('took='):-1])
            except ValueError:
                took = None
    return status, took


def collect_transitions(samples):
    """Return observed service changes across consecutive samples."""
    transitions = []
    for (_, previous), (header, current) in zip(samples, samples[1:]):
        uptime = header[1]
        for name in sorted(set(previous['services']) | set(current['services'])):
            before = previous['services'].get(name)
            after = current['services'].get(name)
            if before == after:
                continue
            before_state = before['state'] if before else 'absent'
            after_state = after['state'] if after else 'absent'
            first_ns = after['first_start_boottime_ns'] if after else None
            transitions.append((uptime, name, before_state, after_state, first_ns))
    return transitions


def _print_init_stage_properties(stages):
    if not stages:
        return
    print('\ninit 阶段属性（ro.boottime.init*，init/first_stage/selinux 原值为纳秒，其余为毫秒）')
    for name, ms, unit, raw in stages:
        note = '时间点' if name == 'init' else '耗时'
        print(f'  {name:<26} {ms:>7.1f} ms  ({note}, raw={raw} {unit})')


def _print_init_events_summary(init_events):
    if not init_events:
        return
    phases = [e for e in init_events if e[1] == 'phase']
    starts = [e for e in init_events if e[1] == 'start']
    exits = [e for e in init_events if e[1] == 'exit']
    print(
        f'\n早期 dmesg init 事件（覆盖 {init_events[0][0]:.2f}–{init_events[-1][0]:.2f} 秒，'
        f'阶段={len(phases)}，启动={len(starts)}，退出={len(exits)}）'
    )
    key_set = set(KEY_PHASES)
    shown_phases = [e for e in phases if e[2] in key_set]
    if shown_phases:
        print('  关键 init 阶段（dmesg 单调时钟）:')
        for ts, _, name, detail in shown_phases:
            suffix = f'  [{detail}]' if detail else ''
            print(f'    +{ts:6.3f}s  {name}{suffix}')

    start_map = {}
    for ts, _, name, _ in starts:
        start_map.setdefault(name, ts)
    exit_map = {}
    for ts, _, name, detail in exits:
        exit_map.setdefault(name, (ts, detail))

    core_rows = [n for n in KEY_SERVICES if n in start_map]
    if core_rows:
        print('  核心服务 dmesg 生命周期:')
        for name in core_rows:
            ex = exit_map.get(name)
            if ex:
                st, took = parse_exit_detail(ex[1])
                took_str = f'{took:.3f}s' if took is not None else ex[1]
                ex_info = f' -> 退出 +{ex[0]:.3f}s (status={st}, took={took_str})'
            else:
                ex_info = ''
            print(f'    +{start_map[name]:6.3f}s  {name:<16} started{ex_info}')

    durations = []
    for ts, _, name, detail in exits:
        st, took = parse_exit_detail(detail)
        if took is not None:
            durations.append((took, name, st, ts))
    if durations:
        print('  耗时最长的已退出服务（前 5 项，含后台长驻后退出的服务）:')
        for took, name, st, ts in sorted(durations, reverse=True)[:5]:
            print(f'    {took:7.3f}s  {name:<34} status={st} (exit +{ts:.3f}s)')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path)
    args = parser.parse_args()
    samples = read_samples(args.samples)
    if not samples:
        parser.error('no valid samples')
    print(f'采样数: {len(samples)}，观察窗口: {samples[0][0][1]:.2f}–{samples[-1][0][1]:.2f} 秒')
    print(f"首次采样的 init 服务数: {len(samples[0][1]['services'])}")
    complete = next((header[1] for header, data in samples if data['boot_completed_raw'] == '1'), None)
    print('首次观察到 boot_completed=1: ' + (f'{complete:.2f} 秒' if complete is not None else '未观察到'))
    for name, match in (
        ('bootanim=running', lambda d: d['services'].get('bootanim', {}).get('state') == 'running'),
        ('service.bootanim.exit=1', lambda d: d['bootanim_exit_raw'] == '1'),
        ('bootanim=stopped', lambda d: d['services'].get('bootanim', {}).get('state') == 'stopped'),
    ):
        first = next((header[1] for header, data in samples if match(data)), None)
        print(f'首次观察到 {name}: ' + (f'{first:.2f} 秒' if first is not None else '未观察到'))
    for name in ('surfaceflinger', 'zygote'):
        svc = samples[0][1]['services'].get(name)
        if svc:
            ns = svc['first_start_boottime_ns']
            stamp = f'{ns / 1e9:.3f} 秒' if ns is not None else '未知'
            print(f"{name}: 首次采样状态={svc['state']}，系统记录的首次启动={stamp}")

    _print_init_stage_properties(samples[-1][1].get('init_stages', ()))
    _print_init_events_summary(read_init_events(args.samples.with_name('init-events.tsv')))

    transitions = collect_transitions(samples)
    print(f'\n相邻快照的服务记录变化数: {len(transitions)}（包括新增和时间属性出现）')
    if transitions:
        print('观察到的服务变化（时间为采样观察时刻）')
        for uptime, name, before_state, after_state, first_ns in transitions:
            detail = (
                f'{before_state} -> {after_state}'
                if before_state != after_state
                else f'{after_state}（记录到首次启动时间）'
            )
            stamp = f' [boottime {first_ns / 1e9:.3f}s]' if first_ns is not None else ''
            print(f'  +{uptime:6.2f}s  {name:<36} {detail}{stamp}')
    print('\n所有采样时间为观察时间；首次启动时间不是初始化耗时。')
    events = read_boot_events(args.samples.with_name('boot-events.tsv'))
    if events:
        print('\n框架启动事件（原始毫秒字段，相邻差值为原始字段差）')
        prev_value = None
        for tag, value in events:
            delta = '' if prev_value is None else f'  (+{value - prev_value} ms)'
            print(f'  {tag:<36} {value:>7} ms{delta}')
            prev_value = value


if __name__ == '__main__':
    main()
