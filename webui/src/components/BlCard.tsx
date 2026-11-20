import { fmtMs } from '../format';
import type { BootloaderDetail } from '../types';

const SEG_COLORS = ['#6ee7c8', '#68b0ff', '#f7c568', '#c792ea', '#ff8c7b', '#8bd3dd'];

export function BlCard({ bl }: { bl: BootloaderDetail }) {
  const active = bl.stages.filter((s) => !s.wait_only && s.dur_ms > 0);
  const sum = Math.max(1, active.reduce((acc, s) => acc + s.dur_ms, 0));

  return (
    <section class="card">
      <div class="card-head">
        <h2>Bootloader 分段详情</h2>
        <span class="pill mono">{bl.source || 'NONE'}</span>
      </div>
      <div class="sub mono bl-meta">
        {bl.meta || `硬件总耗时 ${fmtMs(bl.total_ms)}`}
        {bl.wait_ms > 0 ? ` · 解锁警告等待 ${fmtMs(bl.wait_ms)}（不计入硬件耗时）` : ''}
      </div>

      {active.length > 0 ? (
        <>
          <div class="bl-bar">
            {active.map((s, idx) => (
              <div
                key={s.id}
                class="bl-seg"
                style={`width:${((s.dur_ms / sum) * 100).toFixed(2)}%;background:${SEG_COLORS[idx % SEG_COLORS.length]}`}
                title={`${s.id}: ${s.dur_ms}ms`}
              />
            ))}
          </div>
          <div class="bl-legend mono">
            {active.map((s, idx) => (
              <span key={s.id} class="bl-legend-item">
                <span class="dot" style={`background:${SEG_COLORS[idx % SEG_COLORS.length]}`} />
                {s.id} {s.dur_ms}ms
              </span>
            ))}
          </div>
          <div class="table-wrap">
            <table>
              <thead>
                <tr>
                  <th>阶段</th>
                  <th>POR 起点</th>
                  <th>耗时</th>
                  <th>说明</th>
                </tr>
              </thead>
              <tbody>
                {bl.stages.map((s) => (
                  <tr key={`${s.id}-${s.start_ms}`}>
                    <td class="mono">{s.id}{s.wait_only ? ' (等待)' : ''}</td>
                    <td class="mono">{s.start_ms}ms</td>
                    <td class="mono">{s.dur_ms}ms</td>
                    <td class="mono">{s.detail}</td>
                  </tr>
                ))}
              </tbody>
            </table>
          </div>
        </>
      ) : (
        <div class="sub">当前记录无细分 Bootloader 阶段数据</div>
      )}
    </section>
  );
}
