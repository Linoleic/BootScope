"""Load, compare (A/B), and render self-contained HTML reports for BootScope runs."""
import html
import json
from pathlib import Path

from analyze_probe import (
    KEY_PHASES,
    KEY_SERVICES,
    collect_transitions,
    compute_proc_hotspots,
    compute_psi_intervals,
    read_boot_events,
    read_init_events,
    read_proc_samples,
    read_psi_samples,
    read_samples,
)


def _read_kv_file(path: Path) -> dict[str, str]:
    data = {}
    if not path.is_file():
        return data
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        key, sep, val = line.partition('=')
        if sep:
            data[key.strip()] = val.strip()
    return data


def load_boot_run(samples_path: Path) -> dict:
    """Load all BootScope artifacts from the directory containing samples.txt."""
    samples = read_samples(samples_path)
    if not samples:
        raise ValueError(f'no valid samples in {samples_path}')
    run_dir = samples_path.parent
    metadata = _read_kv_file(run_dir / 'metadata.txt')
    init_events = read_init_events(run_dir / 'init-events.tsv')
    boot_events = read_boot_events(run_dir / 'boot-events.tsv')
    psi_intervals = compute_psi_intervals(read_psi_samples(run_dir / 'psi.tsv'))
    proc_intervals, proc_top, proc_d = compute_proc_hotspots(read_proc_samples(run_dir / 'proc.tsv'))

    renderer_path = run_dir / 'display-renderer.txt'
    if not renderer_path.is_file():
        renderer_path = run_dir / 'renderer.txt'
    display_renderer = _read_kv_file(renderer_path)
    display_status = _read_kv_file(run_dir / 'display-status.txt')

    label = metadata.get('boot_id') or run_dir.name or samples_path.name
    return {
        'label': label,
        'path': str(samples_path),
        'metadata': metadata,
        'samples': samples,
        'transitions': collect_transitions(samples),
        'bootloader': samples[0][1].get('bootloader_boottime', {}),
        'init_stages': samples[-1][1].get('init_stages', ()),
        'init_events': init_events,
        'boot_events': boot_events,
        'psi_intervals': psi_intervals,
        'proc_intervals': proc_intervals,
        'proc_top': proc_top,
        'proc_d_events': proc_d,
        'display_renderer': display_renderer,
        'display_status': display_status,
    }


def _first_observed_uptime(run: dict, predicate) -> float | None:
    return next((hdr[1] for hdr, data in run['samples'] if predicate(data)), None)


def _extract_milestone_map(run: dict) -> list[tuple[str, float | None, str]]:
    """Return ordered list of (milestone_name, value_ms, unit_note) for comparison."""
    items: list[tuple[str, float | None, str]] = []

    for name, ms, _, _ in run['init_stages']:
        if name in ('init', 'init.first_stage', 'init.selinux', 'init.modules',
                    'init.mount_all.early', 'init.mount_all.late', 'init.fsck.data', 'init.mount.data'):
            items.append((f'ro.boottime.{name}', ms, 'ms'))

    phase_first = {}
    svc_first = {}
    for ts, kind, name, _ in run['init_events']:
        if kind == 'phase' and name not in phase_first:
            phase_first[name] = ts * 1000.0
        elif kind == 'start' and name not in svc_first:
            svc_first[name] = ts * 1000.0

    for phase in ('early-init', 'init', 'late-init', 'post-fs-data', 'zygote-start', 'boot'):
        if phase in phase_first:
            items.append((f'dmesg.phase:{phase}', phase_first[phase], 'ms'))
    for svc in ('ueventd', 'surfaceflinger', 'bootanim', 'apexd', 'zygote'):
        if svc in svc_first:
            items.append((f'dmesg.start:{svc}', svc_first[svc], 'ms'))

    for tag, val in run['boot_events']:
        items.append((f'events:{tag}', float(val), 'ms'))

    anim_stopped = _first_observed_uptime(
        run, lambda d: d['services'].get('bootanim', {}).get('state') == 'stopped'
    )
    if anim_stopped is not None:
        items.append(('observed:bootanim=stopped', anim_stopped * 1000.0, 'ms'))
    boot_completed = _first_observed_uptime(run, lambda d: d['boot_completed_raw'] == '1')
    if boot_completed is not None:
        items.append(('observed:sys.boot_completed=1', boot_completed * 1000.0, 'ms'))

    dr = run['display_renderer']
    for k in ('started_ms', 'first_frame_submitted_ms', 'released_ms'):
        if k in dr and dr[k].isdecimal():
            items.append((f'display:{k}', float(int(dr[k])), 'ms'))

    return items


def compare_runs(run_a: dict, run_b: dict) -> dict:
    """Compute structured A/B differences (B relative to A)."""
    map_a = {name: val for name, val, _ in _extract_milestone_map(run_a)}
    map_b = {name: val for name, val, _ in _extract_milestone_map(run_b)}
    ordered_names = [n for n, _, _ in _extract_milestone_map(run_a)]
    for n, _, _ in _extract_milestone_map(run_b):
        if n not in map_a:
            ordered_names.append(n)

    milestones = []
    for name in ordered_names:
        va = map_a.get(name)
        vb = map_b.get(name)
        delta = (vb - va) if (va is not None and vb is not None) else None
        milestones.append({'name': name, 'a_ms': va, 'b_ms': vb, 'delta_ms': delta})

    bl_keys = list(run_a['bootloader'].keys())
    for k in run_b['bootloader']:
        if k not in run_a['bootloader']:
            bl_keys.append(k)
    bootloader = [
        {
            'key': k,
            'a': run_a['bootloader'].get(k),
            'b': run_b['bootloader'].get(k),
            'delta': (run_b['bootloader'][k] - run_a['bootloader'][k])
            if (k in run_a['bootloader'] and k in run_b['bootloader']) else None,
        }
        for k in bl_keys
    ]

    svcs_a = run_a['samples'][-1][1]['services']
    svcs_b = run_b['samples'][-1][1]['services']
    added = sorted(set(svcs_b) - set(svcs_a))
    removed = sorted(set(svcs_a) - set(svcs_b))
    shifted = []
    for name in sorted(set(svcs_a) & set(svcs_b)):
        ta = svcs_a[name]['first_start_boottime_ns']
        tb = svcs_b[name]['first_start_boottime_ns']
        if ta is not None and tb is not None:
            diff_ms = (tb - ta) / 1e6
            if abs(diff_ms) >= 200.0:
                shifted.append({'service': name, 'a_ms': ta / 1e6, 'b_ms': tb / 1e6, 'delta_ms': diff_ms})
    shifted.sort(key=lambda r: -abs(r['delta_ms']))

    def _resource_summary(run: dict) -> dict:
        psi_d = [r for r in run['psi_intervals'] if r['mode'] == 'delta'] or run['psi_intervals']
        proc_d = [r for r in run['proc_intervals'] if r['mode'] == 'delta']
        return {
            'peak_psi_cpu': max((r['cpu_some_pct'] for r in psi_d), default=None),
            'peak_psi_io': max((r['io_some_pct'] for r in psi_d), default=None),
            'peak_cpu_busy': max((r['cpu_busy_pct'] for r in proc_d), default=None),
            'min_mem_avail_mb': min((r['mem_avail_mb'] for r in run['proc_intervals']), default=None),
        }

    return {
        'a_label': run_a['label'],
        'b_label': run_b['label'],
        'milestones': milestones,
        'bootloader': bootloader,
        'services_added': added,
        'services_removed': removed,
        'services_shifted': shifted,
        'resources_a': _resource_summary(run_a),
        'resources_b': _resource_summary(run_b),
    }


def format_comparison_text(run_a: dict, run_b: dict) -> str:
    """Format a human-readable A/B comparison report."""
    comp = compare_runs(run_a, run_b)
    lines = [
        f"BootScope A/B 对比: A={comp['a_label']}  vs  B={comp['b_label']} (Δ = B - A)",
        f"  A: {run_a['path']} (样本={len(run_a['samples'])})",
        f"  B: {run_b['path']} (样本={len(run_b['samples'])})",
    ]
    if comp['bootloader']:
        lines.append('\n[Bootloader 指标差值 (ro.boot.boottime*)]')
        for row in comp['bootloader']:
            va = '-' if row['a'] is None else str(row['a'])
            vb = '-' if row['b'] is None else str(row['b'])
            d = '-' if row['delta'] is None else f"{row['delta']:+d}"
            lines.append(f"  {row['key']:<16} A={va:<8} B={vb:<8} Δ={d}")

    lines.append('\n[阶段与里程碑耗时/时刻对比]')
    for row in comp['milestones']:
        va = '-' if row['a_ms'] is None else f"{row['a_ms']:.1f}"
        vb = '-' if row['b_ms'] is None else f"{row['b_ms']:.1f}"
        d = '-' if row['delta_ms'] is None else f"{row['delta_ms']:+.1f} ms"
        lines.append(f"  {row['name']:<38} A={va:>9}  B={vb:>9}  Δ={d}")

    ra, rb = comp['resources_a'], comp['resources_b']
    if any(v is not None for v in (*ra.values(), *rb.values())):
        lines.append('\n[资源峰值对比]')
        for key, label, unit in (
            ('peak_cpu_busy', '总 CPU 峰值', '%'),
            ('peak_psi_cpu', 'PSI CPU 峰值', '%'),
            ('peak_psi_io', 'PSI IO 峰值', '%'),
            ('min_mem_avail_mb', '最低可用内存', 'MB'),
        ):
            va = '-' if ra[key] is None else f"{ra[key]:.1f}{unit}"
            vb = '-' if rb[key] is None else f"{rb[key]:.1f}{unit}"
            delta = (rb[key] - ra[key]) if (ra[key] is not None and rb[key] is not None) else None
            ds = '-' if delta is None else f"{delta:+.1f}{unit}"
            lines.append(f"  {label:<18} A={va:>10}  B={vb:>10}  Δ={ds}")

    lines.append(
        f"\n[服务集合差异] B 新增={len(comp['services_added'])}, "
        f"B 减少={len(comp['services_removed'])}, 首次启动偏移≥200ms={len(comp['services_shifted'])}"
    )
    if comp['services_added']:
        lines.append('  新增服务: ' + ', '.join(comp['services_added'][:20]))
    if comp['services_removed']:
        lines.append('  减少服务: ' + ', '.join(comp['services_removed'][:20]))
    if comp['services_shifted']:
        lines.append('  首次启动时间偏移前 10 项:')
        for row in comp['services_shifted'][:10]:
            lines.append(
                f"    {row['service']:<36} A={row['a_ms']:>7.1f}ms  B={row['b_ms']:>7.1f}ms  Δ={row['delta_ms']:+.1f}ms"
            )
    return '\n'.join(lines)


def _serialize_run_for_html(run: dict) -> dict:
    """Build compact JSON-friendly structure for the interactive HTML report."""
    timeline = []
    init_phases = set(KEY_PHASES)
    key_svcs = set(KEY_SERVICES)

    for ts, kind, name, detail in run['init_events']:
        if kind == 'phase' and name in init_phases:
            timeline.append({'t': round(ts, 3), 'track': 'Init 阶段', 'name': name, 'detail': detail})
        elif kind == 'start' and name in key_svcs:
            timeline.append({'t': round(ts, 3), 'track': '核心服务', 'name': f'{name} start', 'detail': ''})
        elif kind == 'exit':
            timeline.append({'t': round(ts, 3), 'track': '服务退出', 'name': f'{name} exit', 'detail': detail})

    for tag, val_ms in run['boot_events']:
        timeline.append({'t': round(val_ms / 1000.0, 3), 'track': '框架事件', 'name': tag, 'detail': f'{val_ms} ms'})

    dr = run['display_renderer']
    if dr.get('first_frame_submitted_ms', '').isdecimal():
        t_s = int(dr['first_frame_submitted_ms']) / 1000.0
        timeline.append({'t': round(t_s, 3), 'track': '显示面板', 'name': 'first_frame', 'detail': dr.get('layout', '')})
    if dr.get('released_ms', '').isdecimal():
        t_s = int(dr['released_ms']) / 1000.0
        timeline.append({'t': round(t_s, 3), 'track': '显示面板', 'name': 'released', 'detail': dr.get('exit', '')})

    timeline.sort(key=lambda e: e['t'])

    final_services = [
        {
            'name': name,
            'state': info['state'],
            'start_s': round(info['first_start_boottime_ns'] / 1e9, 3)
            if info['first_start_boottime_ns'] is not None else None,
        }
        for name, info in sorted(run['samples'][-1][1]['services'].items())
    ]

    psi_by_sample = {r['sample']: r for r in run['psi_intervals']}
    series = []
    if run['proc_intervals']:
        for row in run['proc_intervals']:
            p = psi_by_sample.get(row['sample'], {})
            series.append({
                't': round(row['uptime'], 2),
                'cpu_busy': round(row['cpu_busy_pct'], 1) if row['cpu_busy_pct'] is not None else None,
                'iowait': round(row['iowait_pct'], 1) if row['iowait_pct'] is not None else None,
                'mem_avail': row['mem_avail_mb'],
                'mem_tot': row['mem_tot_mb'],
                'psi_cpu': round(p['cpu_some_pct'], 1) if 'cpu_some_pct' in p else None,
                'psi_io': round(p['io_some_pct'], 1) if 'io_some_pct' in p else None,
                'top': row['top'][:3],
                'd_procs': row['d_procs'],
            })
    elif run['psi_intervals']:
        for p in run['psi_intervals']:
            series.append({
                't': round(p['uptime'], 2),
                'cpu_busy': None,
                'iowait': None,
                'mem_avail': None,
                'mem_tot': None,
                'psi_cpu': round(p['cpu_some_pct'], 1),
                'psi_io': round(p['io_some_pct'], 1),
                'top': [],
                'd_procs': [],
            })

    boot_completed = _first_observed_uptime(run, lambda d: d['boot_completed_raw'] == '1')
    return {
        'label': run['label'],
        'metadata': run['metadata'],
        'bootloader': run['bootloader'],
        'init_stages': [{'name': n, 'ms': round(ms, 2), 'unit': u, 'raw': r} for n, ms, u, r in run['init_stages']],
        'timeline': timeline,
        'series': series,
        'proc_top': run['proc_top'],
        'services': final_services,
        'transitions': [
            {'t': round(t, 2), 'name': n, 'before': b, 'after': a}
            for t, n, b, a, _ in run['transitions']
        ],
        'boot_completed_s': boot_completed,
        'display_renderer': dr,
    }


def render_html_report(run_a: dict, run_b: dict | None = None) -> str:
    """Render a zero-dependency interactive HTML timeline (and optional A/B diff) report."""
    payload = {
        'runA': _serialize_run_for_html(run_a),
        'runB': _serialize_run_for_html(run_b) if run_b else None,
        'comparison': compare_runs(run_a, run_b) if run_b else None,
    }
    json_str = json.dumps(payload, ensure_ascii=False).replace('</', '<\\/')
    title = (
        f"BootScope Report · {html.escape(run_a['label'])}"
        + (f" vs {html.escape(run_b['label'])}" if run_b else '')
    )

    return f"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>{title}</title>
<style>
  :root {{
    --bg: #0c141b; --card: #121f29; --card2: #162633; --border: #233849;
    --accent: #7be0ca; --accent2: #62a8ea; --warn: #f5b971; --bad: #f07b7b;
    --text: #edf4f7; --muted: #8ea8b8;
  }}
  * {{ box-sizing: border-box; }}
  body {{
    margin: 0; padding: 24px; background: var(--bg); color: var(--text);
    font: 14px/1.5 -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, sans-serif;
  }}
  h1, h2, h3 {{ margin: 0 0 12px; font-weight: 600; }}
  h1 {{ font-size: 22px; color: var(--accent); }}
  h2 {{ font-size: 16px; color: var(--text); border-left: 4px solid var(--accent); padding-left: 10px; }}
  .meta {{ color: var(--muted); font-size: 12px; margin-bottom: 20px; }}
  .grid {{ display: grid; grid-template-columns: repeat(auto-fit, minmax(180px, 1fr)); gap: 12px; margin-bottom: 20px; }}
  .kpi {{ background: var(--card); border: 1px solid var(--border); border-radius: 8px; padding: 12px 14px; }}
  .kpi .lbl {{ color: var(--muted); font-size: 12px; }}
  .kpi .val {{ font-size: 20px; font-weight: 700; color: var(--accent); margin-top: 4px; }}
  .panel {{ background: var(--card); border: 1px solid var(--border); border-radius: 8px; padding: 16px; margin-bottom: 20px; }}
  .controls {{ display: flex; flex-wrap: wrap; gap: 10px; align-items: center; margin-bottom: 12px; }}
  .pill {{
    background: var(--card2); border: 1px solid var(--border); color: var(--text);
    padding: 4px 10px; border-radius: 999px; cursor: pointer; font-size: 12px;
  }}
  .pill.active {{ border-color: var(--accent); color: var(--accent); }}
  input[type="search"] {{
    background: var(--card2); border: 1px solid var(--border); color: var(--text);
    padding: 6px 10px; border-radius: 6px; outline: none; min-width: 220px;
  }}
  svg {{ width: 100%; height: auto; display: block; background: #0f1a22; border-radius: 6px; }}
  table {{ width: 100%; border-collapse: collapse; font-size: 13px; }}
  th, td {{ text-align: left; padding: 7px 10px; border-bottom: 1px solid var(--border); }}
  th {{ color: var(--muted); font-weight: 500; position: sticky; top: 0; background: var(--card); }}
  .scroll {{ max-height: 360px; overflow: auto; }}
  .good {{ color: var(--accent); }}
  .bad {{ color: var(--bad); }}
  .mono {{ font-family: ui-monospace, SFMono-Regular, Consolas, monospace; }}
  .tooltip {{
    position: fixed; pointer-events: none; background: #091016; border: 1px solid var(--accent);
    border-radius: 6px; padding: 8px 10px; font-size: 12px; display: none; z-index: 99; max-width: 320px;
  }}
  .note {{ color: var(--muted); font-size: 12px; margin-top: 8px; }}
</style>
</head>
<body>
<h1>BootScope 启动分析报告</h1>
<div id="subtitle" class="meta"></div>
<div id="kpis" class="grid"></div>

<div id="abSection" class="panel" style="display:none">
  <h2>A / B 启动对比 (Δ = B - A)</h2>
  <div id="abContent" class="scroll"></div>
</div>

<div class="panel">
  <h2>启动全链路时间轴</h2>
  <div id="trackFilters" class="controls"></div>
  <svg id="ganttSvg" viewBox="0 0 1100 280"></svg>
  <div class="note">注：dmesg 为内核单调时钟，Bootloader 与框架事件使用各自写入方定义时钟，轮询为观测时刻。</div>
</div>

<div id="resourcePanel" class="panel" style="display:none">
  <h2>资源压力与 CPU / 内存曲线</h2>
  <svg id="resSvg" viewBox="0 0 1100 220"></svg>
</div>

<div id="procPanel" class="panel" style="display:none">
  <h2>观测窗口累计 CPU 前 10 进程</h2>
  <div id="procTable"></div>
</div>

<div class="panel">
  <h2>init 服务与采样变化明细</h2>
  <div class="controls">
    <input id="svcSearch" type="search" placeholder="过滤服务名（如 zygote, surfaceflinger, artd）...">
    <span id="svcCount" class="meta" style="margin:0"></span>
  </div>
  <div id="svcTable" class="scroll"></div>
</div>

<div id="tip" class="tooltip"></div>

<script>
const DATA = {json_str};
const tip = document.getElementById('tip');
const runA = DATA.runA;
const runB = DATA.runB;

function esc(s) {{
  return String(s ?? '').replace(/[&<>"]/g, c => ({{'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;'}}[c]));
}}
function showTip(ev, htmlStr) {{
  tip.innerHTML = htmlStr;
  tip.style.display = 'block';
  tip.style.left = Math.min(window.innerWidth - 330, ev.clientX + 12) + 'px';
  tip.style.top = Math.min(window.innerHeight - 120, ev.clientY + 12) + 'px';
}}
function hideTip() {{ tip.style.display = 'none'; }}

document.getElementById('subtitle').textContent =
  `Run A: ${{runA.label}} (Android ${{runA.metadata.android_version || '?'}} / SDK ${{runA.metadata.sdk || '?'}})` +
  (runB ? `  vs  Run B: ${{runB.label}}` : '');

// KPIs
(function renderKpis() {{
  const cards = [];
  const evMap = Object.fromEntries(runA.timeline.map(e => [e.name, e.t]));
  cards.push(['可见 init 服务', runA.services.length]);
  if (runA.boot_completed_s != null) cards.push(['boot_completed=1 (观测)', runA.boot_completed_s.toFixed(2) + ' s']);
  if (evMap['boot_progress_enable_screen'] != null) cards.push(['enable_screen', evMap['boot_progress_enable_screen'].toFixed(2) + ' s']);
  if (runA.display_renderer.first_frame_submitted_ms) {{
    cards.push(['首帧上屏', (Number(runA.display_renderer.first_frame_submitted_ms) / 1000).toFixed(2) + ' s']);
  }}
  const cpuPeak = Math.max(0, ...runA.series.map(s => s.cpu_busy ?? 0));
  if (cpuPeak > 0) cards.push(['总 CPU 峰值', cpuPeak.toFixed(1) + '%']);
  const ioPeak = Math.max(0, ...runA.series.map(s => s.psi_io ?? 0));
  if (runA.series.some(s => s.psi_io != null)) cards.push(['PSI IO 峰值', ioPeak.toFixed(1) + '%']);
  document.getElementById('kpis').innerHTML = cards.map(([l, v]) =>
    `<div class="kpi"><div class="lbl">${{esc(l)}}</div><div class="val">${{esc(v)}}</div></div>`
  ).join('');
}})();

// A/B Comparison
if (DATA.comparison) {{
  document.getElementById('abSection').style.display = 'block';
  const rows = DATA.comparison.milestones.map(m => {{
    const va = m.a_ms == null ? '-' : m.a_ms.toFixed(1) + ' ms';
    const vb = m.b_ms == null ? '-' : m.b_ms.toFixed(1) + ' ms';
    let d = '-', cls = '';
    if (m.delta_ms != null) {{
      d = (m.delta_ms >= 0 ? '+' : '') + m.delta_ms.toFixed(1) + ' ms';
      cls = m.delta_ms < -5 ? 'good' : (m.delta_ms > 5 ? 'bad' : '');
    }}
    return `<tr><td class="mono">${{esc(m.name)}}</td><td class="mono">${{va}}</td><td class="mono">${{vb}}</td><td class="mono ${{cls}}">${{d}}</td></tr>`;
  }}).join('');
  document.getElementById('abContent').innerHTML =
    `<table><thead><tr><th>里程碑 / 指标</th><th>A (${{esc(runA.label)}})</th><th>B (${{esc(runB.label)}})</th><th>Δ (B - A)</th></tr></thead><tbody>${{rows}}</tbody></table>`;
}}

// Timeline Gantt
const TRACKS = ['Init 阶段', '核心服务', '服务退出', '框架事件', '显示面板'];
const TRACK_COLORS = {{'Init 阶段':'#7be0ca','核心服务':'#62a8ea','服务退出':'#f5b971','框架事件':'#c792ea','显示面板':'#ffd166'}};
const activeTracks = new Set(TRACKS);

function drawTimeline() {{
  const svg = document.getElementById('ganttSvg');
  const events = runA.timeline.filter(e => activeTracks.has(e.track));
  const maxT = Math.max(25, ...events.map(e => e.t), runA.boot_completed_s || 0);
  const padL = 110, padR = 24, padT = 24, rowH = 44;
  const W = 1100, H = padT + TRACKS.length * rowH + 34;
  svg.setAttribute('viewBox', `0 0 ${{W}} ${{H}}`);
  const xOf = t => padL + (t / maxT) * (W - padL - padR);
  let out = '';
  for (let s = 0; s <= Math.ceil(maxT); s += 2) {{
    const x = xOf(s);
    out += `<line x1="${{x}}" y1="${{padT}}" x2="${{x}}" y2="${{H - 26}}" stroke="#1d2f3e" stroke-width="1"/>`;
    out += `<text x="${{x}}" y="${{H - 8}}" fill="#8ea8b8" font-size="11" text-anchor="middle">${{s}}s</text>`;
  }}
  TRACKS.forEach((tr, i) => {{
    const y = padT + i * rowH;
    out += `<text x="12" y="${{y + 24}}" fill="#edf4f7" font-size="12">${{esc(tr)}}</text>`;
    out += `<line x1="${{padL}}" y1="${{y + 20}}" x2="${{W - padR}}" y2="${{y + 20}}" stroke="#233849" stroke-width="1"/>`;
  }});
  events.forEach((ev, idx) => {{
    const ti = TRACKS.indexOf(ev.track);
    if (ti < 0) return;
    const x = xOf(ev.t), y = padT + ti * rowH + 20;
    const color = TRACK_COLORS[ev.track] || '#7be0ca';
    out += `<circle class="ev-dot" data-idx="${{idx}}" cx="${{x}}" cy="${{y}}" r="5" fill="${{color}}" stroke="#0c141b" stroke-width="1.5" style="cursor:pointer"/>`;
  }});
  svg.innerHTML = out;
  svg.querySelectorAll('.ev-dot').forEach(dot => {{
    const ev = events[Number(dot.dataset.idx)];
    dot.addEventListener('mousemove', e => showTip(e,
      `<div class="mono"><b>${{esc(ev.name)}}</b></div><div>${{esc(ev.track)}} · +${{ev.t.toFixed(3)}}s</div>${{ev.detail ? `<div class="muted">${{esc(ev.detail)}}</div>` : ''}}`
    ));
    dot.addEventListener('mouseleave', hideTip);
  }});
}}

document.getElementById('trackFilters').innerHTML = TRACKS.map(t =>
  `<button class="pill active" data-track="${{esc(t)}}">${{esc(t)}}</button>`
).join('');
document.querySelectorAll('#trackFilters .pill').forEach(btn => {{
  btn.addEventListener('click', () => {{
    const t = btn.dataset.track;
    if (activeTracks.has(t)) {{ activeTracks.delete(t); btn.classList.remove('active'); }}
    else {{ activeTracks.add(t); btn.classList.add('active'); }}
    drawTimeline();
  }});
}});
drawTimeline();

// Resource chart
if (runA.series.length > 0) {{
  document.getElementById('resourcePanel').style.display = 'block';
  const svg = document.getElementById('resSvg');
  const W = 1100, H = 220, padL = 54, padR = 24, padT = 20, padB = 30;
  const xs = runA.series.map(s => s.t);
  const minT = Math.min(...xs), maxT = Math.max(minT + 1, ...xs);
  const xOf = t => padL + ((t - minT) / (maxT - minT)) * (W - padL - padR);
  const yOf = pct => padT + (1 - Math.max(0, Math.min(100, pct)) / 100) * (H - padT - padB);
  let out = '';
  [0, 25, 50, 75, 100].forEach(p => {{
    const y = yOf(p);
    out += `<line x1="${{padL}}" y1="${{y}}" x2="${{W - padR}}" y2="${{y}}" stroke="#1d2f3e"/>`;
    out += `<text x="8" y="${{y + 4}}" fill="#8ea8b8" font-size="11">${{p}}%</text>`;
  }});
  function poly(field, color) {{
    const pts = runA.series.filter(s => s[field] != null).map(s => `${{xOf(s.t)}},${{yOf(s[field])}}`).join(' ');
    return pts ? `<polyline fill="none" stroke="${{color}}" stroke-width="2.2" points="${{pts}}"/>` : '';
  }}
  out += poly('cpu_busy', '#7be0ca') + poly('psi_cpu', '#62a8ea') + poly('psi_io', '#f5b971');
  runA.series.forEach((s, i) => {{
    const x = xOf(s.t);
    out += `<circle class="res-pt" data-idx="${{i}}" cx="${{x}}" cy="${{yOf(s.cpu_busy ?? s.psi_cpu ?? 0)}}" r="5" fill="#7be0ca" opacity="0.85" style="cursor:pointer"/>`;
    out += `<text x="${{x}}" y="${{H - 8}}" fill="#8ea8b8" font-size="10" text-anchor="middle">${{s.t.toFixed(1)}}s</text>`;
  }});
  out += `<text x="${{padL + 10}}" y="15" fill="#7be0ca" font-size="11">● CPU Busy%</text>`;
  out += `<text x="${{padL + 110}}" y="15" fill="#62a8ea" font-size="11">● PSI CPU%</text>`;
  out += `<text x="${{padL + 205}}" y="15" fill="#f5b971" font-size="11">● PSI IO%</text>`;
  svg.innerHTML = out;
  svg.querySelectorAll('.res-pt').forEach(pt => {{
    const s = runA.series[Number(pt.dataset.idx)];
    const topStr = (s.top || []).map(t => `${{esc(t.comm)}} (${{t.cpu_pct.toFixed(1)}}%)`).join(', ');
    pt.addEventListener('mousemove', e => showTip(e,
      `<div class="mono"><b>+${{s.t.toFixed(2)}}s</b></div>` +
      (s.cpu_busy != null ? `<div>CPU=${{s.cpu_busy}}% (iow=${{s.iowait}}%) · Mem=${{s.mem_avail}}/${{s.mem_tot}} MB</div>` : '') +
      (s.psi_cpu != null ? `<div>PSI CPU=${{s.psi_cpu}}% · IO=${{s.psi_io}}%</div>` : '') +
      (topStr ? `<div>Top: ${{topStr}}</div>` : '') +
      (s.d_procs && s.d_procs.length ? `<div class="bad">D态: ${{esc(s.d_procs.join(', '))}}</div>` : '')
    ));
    pt.addEventListener('mouseleave', hideTip);
  }});
}}

if (runA.proc_top && runA.proc_top.length > 0) {{
  document.getElementById('procPanel').style.display = 'block';
  const rows = runA.proc_top.map(p =>
    `<tr><td class="mono">${{esc(p.comm)}}</td><td class="mono">${{p.total_ticks}}</td><td class="mono">${{p.peak_cpu_pct.toFixed(1)}}%</td></tr>`
  ).join('');
  document.getElementById('procTable').innerHTML =
    `<table><thead><tr><th>进程名 (comm)</th><th>累计 CPU ticks</th><th>区间峰值</th></tr></thead><tbody>${{rows}}</tbody></table>`;
}}

function renderServices(filter) {{
  const q = filter.trim().toLowerCase();
  const list = runA.services.filter(s => !q || s.name.toLowerCase().includes(q));
  document.getElementById('svcCount').textContent = `显示 ${{list.length}} / ${{runA.services.length}} 项`;
  const rows = list.map(s =>
    `<tr><td class="mono">${{esc(s.name)}}</td><td class="mono">${{esc(s.state)}}</td><td class="mono">${{s.start_s == null ? '-' : '+' + s.start_s.toFixed(3) + ' s'}}</td></tr>`
  ).join('');
  document.getElementById('svcTable').innerHTML =
    `<table><thead><tr><th>服务名</th><th>末次采样状态</th><th>首次启动 (ro.boottime)</th></tr></thead><tbody>${{rows}}</tbody></table>`;
}}
document.getElementById('svcSearch').addEventListener('input', e => renderServices(e.target.value));
renderServices('');
</script>
</body>
</html>
"""
