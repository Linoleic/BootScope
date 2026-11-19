"""Summarize, compare, and export BootScope probe captures without inferring causation."""
import argparse
from collections import defaultdict
import math
import re
import sys
from pathlib import Path
from bootscope import build_snapshot, parse_properties

if hasattr(sys.stdout, 'reconfigure'):
    sys.stdout.reconfigure(encoding='utf-8', errors='replace')

HEADER = re.compile(r'^--- sample=(\d+) uptime=([0-9.]+) ---$')
PROC_HEADER = re.compile(
    r'^# sample=(\d+) uptime=([0-9.]+) tot=(\d+) idle=(\d+) iow=(\d+) '
    r'mem_avail_mb=(\d+) mem_tot_mb=(\d+)$'
)
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


def read_qcom_kpi(path):
    """Read Qualcomm boot_stats KPI metrics from bootloader-kpi.tsv into derived ms/raw dict."""
    if not path.is_file():
        return {}
    raw = {}
    for line in path.read_text(encoding='utf-8').splitlines():
        parts = line.split('\t')
        if len(parts) != 2:
            continue
        k, v = parts[0].strip(), parts[1].strip()
        if not k or not v.isdecimal():
            continue
        raw[k] = int(v)
    if not raw:
        return {}
    out = {}
    freq = raw.get('mpm_freq', 0)
    bl_start = raw.get('bl_start')
    bl_end = raw.get('bl_end')
    bl_load = raw.get('bl_load_kernel')
    if freq > 0 and bl_start is not None and bl_end is not None and bl_end >= bl_start:
        out['QCOM_BL_TOTAL_MS'] = round((bl_end - bl_start) * 1000.0 / freq)
        if bl_load is not None and bl_start <= bl_load <= bl_end:
            out['QCOM_BL_PRE_MS'] = round((bl_load - bl_start) * 1000.0 / freq)
            out['QCOM_BL_KL_MS'] = round((bl_end - bl_load) * 1000.0 / freq)
        out['QCOM_MPM_FREQ_HZ'] = freq
    else:
        for k, v in raw.items():
            out[f'qcom_{k}'] = v
    return out


def read_psi_samples(path):
    """Read psi.tsv entries emitted by probe.sh."""
    if not path.is_file():
        return []
    rows = []
    for line in path.read_text(encoding='utf-8').splitlines():
        parts = line.split('\t')
        if len(parts) != 11:
            continue
        try:
            idx = int(parts[0])
            uptime = float(parts[1])
            cpu_some_us, cpu_full_us, cpu_avg10 = int(parts[2]), int(parts[3]), float(parts[4])
            io_some_us, io_full_us, io_avg10 = int(parts[5]), int(parts[6]), float(parts[7])
            mem_some_us, mem_full_us, mem_avg10 = int(parts[8]), int(parts[9]), float(parts[10])
        except ValueError:
            continue
        if not (math.isfinite(uptime) and uptime >= 0):
            continue
        rows.append({
            'sample': idx,
            'uptime': uptime,
            'cpu_some_us': cpu_some_us,
            'cpu_full_us': cpu_full_us,
            'cpu_avg10': cpu_avg10,
            'io_some_us': io_some_us,
            'io_full_us': io_full_us,
            'io_avg10': io_avg10,
            'mem_some_us': mem_some_us,
            'mem_full_us': mem_full_us,
            'mem_avg10': mem_avg10,
        })
    return rows


def _clamp_pct(value):
    return max(0.0, min(100.0, value))


def compute_psi_intervals(rows):
    """Convert cumulative PSI microsecond counters into per-sample stall percentages."""
    intervals = []
    prev = None
    for row in rows:
        if prev is not None and row['uptime'] > prev['uptime']:
            dt_us = (row['uptime'] - prev['uptime']) * 1e6
            cpu_some = _clamp_pct((row['cpu_some_us'] - prev['cpu_some_us']) / dt_us * 100.0)
            io_some = _clamp_pct((row['io_some_us'] - prev['io_some_us']) / dt_us * 100.0)
            io_full = _clamp_pct((row['io_full_us'] - prev['io_full_us']) / dt_us * 100.0)
            mem_some = _clamp_pct((row['mem_some_us'] - prev['mem_some_us']) / dt_us * 100.0)
            mem_full = _clamp_pct((row['mem_full_us'] - prev['mem_full_us']) / dt_us * 100.0)
            mode = 'delta'
        else:
            cpu_some = _clamp_pct(row['cpu_avg10'])
            io_some = _clamp_pct(row['io_avg10'])
            io_full = 0.0
            mem_some = _clamp_pct(row['mem_avg10'])
            mem_full = 0.0
            mode = 'avg10'
        intervals.append({
            'sample': row['sample'],
            'uptime': row['uptime'],
            'cpu_some_pct': cpu_some,
            'io_some_pct': io_some,
            'io_full_pct': io_full,
            'mem_some_pct': mem_some,
            'mem_full_pct': mem_full,
            'mode': mode,
        })
        prev = row
    return intervals


def read_proc_samples(path):
    """Read proc.tsv system + per-process CPU/state snapshots."""
    if not path.is_file():
        return []
    samples = []
    current = None
    for line in path.read_text(encoding='utf-8').splitlines():
        match = PROC_HEADER.match(line)
        if match:
            if current is not None:
                samples.append(current)
            uptime = float(match.group(2))
            if not math.isfinite(uptime) or uptime < 0:
                current = None
                continue
            current = {
                'sample': int(match.group(1)),
                'uptime': uptime,
                'tot': int(match.group(3)),
                'idle': int(match.group(4)),
                'iow': int(match.group(5)),
                'mem_avail_mb': int(match.group(6)),
                'mem_tot_mb': int(match.group(7)),
                'procs': {},
            }
            continue
        if current is None:
            continue
        parts = line.split('\t')
        if len(parts) != 5:
            continue
        try:
            pid = int(parts[0])
            ppid = int(parts[1])
            comm = parts[2]
            state = parts[3]
            ticks = int(parts[4])
        except ValueError:
            continue
        if not comm or not state:
            continue
        current['procs'][pid] = {
            'pid': pid,
            'ppid': ppid,
            'comm': comm,
            'state': state,
            'ticks': ticks,
        }
    if current is not None:
        samples.append(current)
    return samples


def compute_proc_hotspots(proc_samples):
    """Compute per-interval CPU/mem metrics, Top CPU processes, and D-state observations."""
    intervals = []
    comm_totals = defaultdict(int)
    comm_peak_pct = defaultdict(float)
    d_state_events = []
    prev = None

    for cur in proc_samples:
        d_procs = sorted({p['comm'] for p in cur['procs'].values() if p['state'] == 'D'})
        for comm in d_procs:
            d_state_events.append((cur['uptime'], comm))

        if prev is not None and cur['tot'] > prev['tot']:
            dtot = cur['tot'] - prev['tot']
            didle = max(0, cur['idle'] - prev['idle'])
            diow = max(0, cur['iow'] - prev['iow'])
            cpu_busy_pct = _clamp_pct((1.0 - didle / dtot) * 100.0)
            iowait_pct = _clamp_pct((diow / dtot) * 100.0)

            per_comm_ticks = defaultdict(int)
            for pid, proc in cur['procs'].items():
                prev_proc = prev['procs'].get(pid)
                if prev_proc is None:
                    dticks = max(0, proc['ticks'])
                elif prev_proc['comm'] == proc['comm'] and proc['ticks'] >= prev_proc['ticks']:
                    dticks = proc['ticks'] - prev_proc['ticks']
                else:
                    dticks = max(0, proc['ticks'])
                if dticks > 0:
                    per_comm_ticks[proc['comm']] += dticks

            top = []
            for comm, dticks in sorted(per_comm_ticks.items(), key=lambda kv: (-kv[1], kv[0])):
                pct = _clamp_pct((dticks / dtot) * 100.0)
                comm_totals[comm] += dticks
                if pct > comm_peak_pct[comm]:
                    comm_peak_pct[comm] = pct
                top.append({'comm': comm, 'ticks': dticks, 'cpu_pct': pct})

            intervals.append({
                'sample': cur['sample'],
                'uptime': cur['uptime'],
                'cpu_busy_pct': cpu_busy_pct,
                'iowait_pct': iowait_pct,
                'mem_avail_mb': cur['mem_avail_mb'],
                'mem_tot_mb': cur['mem_tot_mb'],
                'top': top[:5],
                'd_procs': d_procs,
                'mode': 'delta',
            })
        else:
            intervals.append({
                'sample': cur['sample'],
                'uptime': cur['uptime'],
                'cpu_busy_pct': None,
                'iowait_pct': None,
                'mem_avail_mb': cur['mem_avail_mb'],
                'mem_tot_mb': cur['mem_tot_mb'],
                'top': [],
                'd_procs': d_procs,
                'mode': 'baseline',
            })
        prev = cur

    top_overall = [
        {'comm': comm, 'total_ticks': ticks, 'peak_cpu_pct': comm_peak_pct[comm]}
        for comm, ticks in sorted(comm_totals.items(), key=lambda kv: (-kv[1], kv[0]))[:10]
    ]
    return intervals, top_overall, d_state_events


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


def _print_bootloader_boottime(bl):
    if not bl:
        return
    print('\nBootloader 阶段指标（ro.boot.boottime* / 高通 boot_stats KPI）')
    items = [f'{k}={v}' for k, v in bl.items()]
    for idx in range(0, len(items), 6):
        print('  ' + ', '.join(items[idx:idx + 6]))


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


def _print_psi_summary(intervals):
    if not intervals:
        return
    deltas = [row for row in intervals if row['mode'] == 'delta'] or intervals
    peak_cpu = max(deltas, key=lambda r: r['cpu_some_pct'])
    peak_io = max(deltas, key=lambda r: r['io_some_pct'])
    peak_mem = max(deltas, key=lambda r: r['mem_some_pct'])
    print('\n内核 PSI 资源压力（/proc/pressure，采样间隔内阻塞占比）')
    print(
        f"  峰值: CPU some={peak_cpu['cpu_some_pct']:.1f}% (@{peak_cpu['uptime']:.2f}s), "
        f"IO some={peak_io['io_some_pct']:.1f}% (full={peak_io['io_full_pct']:.1f}% @{peak_io['uptime']:.2f}s), "
        f"MEM some={peak_mem['mem_some_pct']:.1f}% (@{peak_mem['uptime']:.2f}s)"
    )
    for row in intervals:
        tag = 'avg10' if row['mode'] == 'avg10' else 'delta'
        print(
            f"  +{row['uptime']:6.2f}s  [{tag}]  "
            f"CPU={row['cpu_some_pct']:5.1f}%  "
            f"IO={row['io_some_pct']:5.1f}% (full={row['io_full_pct']:4.1f}%)  "
            f"MEM={row['mem_some_pct']:5.1f}%"
        )


def _print_proc_summary(intervals, top_overall, d_state_events):
    if not intervals:
        return
    deltas = [row for row in intervals if row['mode'] == 'delta']
    print('\n进程 CPU / 内存 / IO 热点（/proc/stat + /proc/[pid]/stat）')
    if deltas:
        peak_cpu = max(deltas, key=lambda r: r['cpu_busy_pct'])
        min_mem = min(intervals, key=lambda r: r['mem_avail_mb'])
        print(
            f"  总 CPU 峰值={peak_cpu['cpu_busy_pct']:.1f}% (iowait={peak_cpu['iowait_pct']:.1f}% @{peak_cpu['uptime']:.2f}s), "
            f"最低可用内存={min_mem['mem_avail_mb']}/{min_mem['mem_tot_mb']} MB (@{min_mem['uptime']:.2f}s)"
        )
    for row in intervals:
        if row['mode'] == 'baseline':
            d_note = f"  [D: {', '.join(row['d_procs'][:3])}]" if row['d_procs'] else ''
            print(f"  +{row['uptime']:6.2f}s  [base ]  Mem={row['mem_avail_mb']}/{row['mem_tot_mb']} MB{d_note}")
            continue
        top_str = ', '.join(f"{t['comm']}({t['cpu_pct']:.1f}%)" for t in row['top'][:3]) or '-'
        d_note = f"  [D: {', '.join(row['d_procs'][:3])}]" if row['d_procs'] else ''
        print(
            f"  +{row['uptime']:6.2f}s  [delta]  CPU={row['cpu_busy_pct']:5.1f}% (iow={row['iowait_pct']:4.1f}%)  "
            f"Mem={row['mem_avail_mb']:4d}/{row['mem_tot_mb']} MB  Top: {top_str}{d_note}"
        )
    if top_overall:
        print('  观测窗口累计 CPU 消耗前 10 进程（按 comm 聚合，100%=全核满载）:')
        for item in top_overall:
            print(f"    {item['comm']:<18} 累计={item['total_ticks']:>6} ticks  区间峰值={item['peak_cpu_pct']:5.1f}%")
    if d_state_events:
        comms = sorted({comm for _, comm in d_state_events})
        print(f"  观察到处于 D 态（IO 阻塞）的进程 ({len(d_state_events)} 次): {', '.join(comms[:12])}")


def print_single_report(samples_path):
    samples = read_samples(samples_path)
    if not samples:
        raise ValueError('no valid samples')
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

    bl = samples[0][1].get('bootloader_boottime', {})
    if not bl:
        bl = read_qcom_kpi(samples_path.with_name('bootloader-kpi.tsv'))
    _print_bootloader_boottime(bl)
    _print_init_stage_properties(samples[-1][1].get('init_stages', ()))
    _print_init_events_summary(read_init_events(samples_path.with_name('init-events.tsv')))
    _print_psi_summary(compute_psi_intervals(read_psi_samples(samples_path.with_name('psi.tsv'))))
    proc_intervals, proc_top, proc_d = compute_proc_hotspots(read_proc_samples(samples_path.with_name('proc.tsv')))
    _print_proc_summary(proc_intervals, proc_top, proc_d)

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
    events = read_boot_events(samples_path.with_name('boot-events.tsv'))
    if events:
        print('\n框架启动事件（原始毫秒字段，相邻差值为原始字段差）')
        prev_value = None
        for tag, value in events:
            delta = '' if prev_value is None else f'  (+{value - prev_value} ms)'
            print(f'  {tag:<36} {value:>7} ms{delta}')
            prev_value = value


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('samples', type=Path, help='Primary samples.txt path')
    parser.add_argument('--compare', type=Path, help='Second samples.txt path for A/B comparison (B vs A)')
    parser.add_argument('--html', type=Path, help='Write self-contained interactive HTML timeline report')
    args = parser.parse_args()

    try:
        if args.compare or args.html:
            from report_utils import format_comparison_text, load_boot_run, render_html_report

            run_a = load_boot_run(args.samples)
            run_b = load_boot_run(args.compare) if args.compare else None
            if args.compare:
                print(format_comparison_text(run_a, run_b))
            else:
                print_single_report(args.samples)
            if args.html:
                args.html.parent.mkdir(parents=True, exist_ok=True)
                args.html.write_text(render_html_report(run_a, run_b), encoding='utf-8')
                print(f'\nHTML 报告已写入: {args.html}')
        else:
            print_single_report(args.samples)
    except ValueError as exc:
        parser.error(str(exc))


if __name__ == '__main__':
    main()
