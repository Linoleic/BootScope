"""Summarize a BootScope probe samples.txt file without inferring causation."""
import argparse
import sys
import re
from pathlib import Path
from bootscope import build_snapshot, parse_properties

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HEADER = re.compile(r'^--- sample=(\d+) uptime=([0-9.]+) ---$')


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
    return snapshot


def read_boot_events(path):
    if not path.is_file():
        return []
    events = []
    for line in path.read_text(encoding='utf-8').splitlines():
        tag, separator, value = line.partition('\t')
        if separator and value.isascii() and value.isdecimal():
            events.append((tag, int(value)))
    return events


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
    transitions = collect_transitions(samples)
    print(f'相邻快照的服务记录变化数: {len(transitions)}（包括新增和时间属性出现）')
    if transitions:
        print('\n观察到的服务变化（时间为采样观察时刻）')
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
