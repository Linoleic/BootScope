import { useEffect, useState } from 'preact/hooks';
import { fetchCompare } from '../bridge';
import { fmtDelta, fmtMs } from '../format';
import { RunSelect } from './RunSelect';
import type { CompareResponse, RunSummary } from '../types';

export function CompareView({ runs, refreshSeq = 0 }: { runs: RunSummary[]; refreshSeq?: number }) {
  const [idA, setIdA] = useState<string>(runs[1]?.boot_id || runs[0]?.boot_id || '');
  const [idB, setIdB] = useState<string>(runs[0]?.boot_id || '');
  const [data, setData] = useState<CompareResponse | null>(null);
  const [err, setErr] = useState<string>('');
  const [loading, setLoading] = useState<boolean>(false);

  useEffect(() => {
    if (runs.length === 0) return;
    if (!runs.some((r) => r.boot_id === idB)) {
      setIdB(runs[0]?.boot_id || '');
    }
    if (!runs.some((r) => r.boot_id === idA)) {
      setIdA(runs[1]?.boot_id || runs[0]?.boot_id || '');
    }
  }, [runs]);

  useEffect(() => {
    if (!idA || !idB) return;
    setLoading(true);
    setErr('');
    fetchCompare(idA, idB)
      .then((res) => setData(res))
      .catch((e: Error) => setErr(e.message))
      .finally(() => setLoading(false));
  }, [idA, idB, refreshSeq]);

  if (runs.length < 2) {
    return (
      <section class="card">
        <div class="sub">至少需要 2 条启动记录才可进行 A / B 耗时对比。</div>
      </section>
    );
  }

  return (
    <div class="tab-pane">
      <section class="card">
        <div class="card-head">
          <h2>选择对比基准 (A → B)</h2>
          <span class="pill mono">{loading ? '计算中…' : 'B - A'}</span>
        </div>
        <div class="two-col">
          <div>
            <div class="stat-k" style="margin-bottom:4px">基准记录 A</div>
            <RunSelect runs={runs} value={idA} onChange={setIdA} ariaLabel="选择基准记录 A" />
          </div>
          <div>
            <div class="stat-k" style="margin-bottom:4px">目标记录 B</div>
            <RunSelect runs={runs} value={idB} onChange={setIdB} ariaLabel="选择目标记录 B" />
          </div>
        </div>
        {err && <p class="status-line err mono">{err}</p>}
      </section>

      {!data && loading && (
        <section class="card compare-placeholder">
          <div class="sub mono">正在计算 A / B 对比数据…</div>
        </section>
      )}

      {data && (
        <div class={`tab-pane compare-results ${loading ? 'is-loading' : ''}`}>
          <section class="summary-grid">
            {[
              { label: 'BL 耗时 Δ', a: data.run_a.bl_total_ms, b: data.run_b.bl_total_ms },
              { label: '浮层首帧 Δ', a: data.run_a.first_frame_ms, b: data.run_b.first_frame_ms },
              { label: '启动完成 Δ', a: data.run_a.boot_done_ms, b: data.run_b.boot_done_ms },
            ].map((item) => {
              const valid = item.a >= 0 && item.b >= 0;
              const d = fmtDelta(item.b - item.a, valid);
              return (
                <article key={item.label} class="stat-card">
                  <div class="stat-k">{item.label}</div>
                  <div class={`stat-v mono ${d.cls}`}>{d.text}</div>
                  <div class="stat-s mono">{fmtMs(item.a)} → {fmtMs(item.b)}</div>
                </article>
              );
            })}
          </section>

          {data.bl_deltas.length > 0 && (
            <section class="card">
              <div class="card-head">
                <h2>Bootloader 分段耗时差异 (B - A)</h2>
                <span class="pill mono">{data.run_a.bl_source} → {data.run_b.bl_source}</span>
              </div>
              <div class="table-wrap">
                <table>
                  <thead>
                    <tr><th>阶段</th><th>A</th><th>B</th><th>Δ</th></tr>
                  </thead>
                  <tbody>
                    {data.bl_deltas.map((r) => {
                      const d = fmtDelta(r.delta_ms, r.has_delta);
                      return (
                        <tr key={`bl-${r.id}`}>
                          <td class="mono">{r.id}</td>
                          <td class="mono">{fmtMs(r.a_ms)}</td>
                          <td class="mono">{fmtMs(r.b_ms)}</td>
                          <td class={`mono ${d.cls}`}>{d.text}</td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </section>
          )}

          <section class="card">
            <div class="card-head">
              <h2>关键路径与里程碑差异 (B - A)</h2>
            </div>
            <div class="table-wrap">
              <table>
                <thead>
                  <tr><th>阶段 / 里程碑</th><th>A</th><th>B</th><th>Δ</th></tr>
                </thead>
                <tbody>
                  {data.phase_deltas.map((r) => {
                    const d = fmtDelta(r.delta_ms, r.has_delta);
                    return (
                      <tr key={`ph-${r.id}`}>
                        <td class="mono">阶段:{r.id}</td>
                        <td class="mono">{fmtMs(r.a_ms)}</td>
                        <td class="mono">{fmtMs(r.b_ms)}</td>
                        <td class={`mono ${d.cls}`}>{d.text}</td>
                      </tr>
                    );
                  })}
                  {data.milestone_deltas.map((r) => {
                    const d = fmtDelta(r.delta_ms, r.has_delta);
                    return (
                      <tr key={`ms-${r.name}`}>
                        <td class="mono">{r.name}</td>
                        <td class="mono">{fmtMs(r.a_ms)}</td>
                        <td class="mono">{fmtMs(r.b_ms)}</td>
                        <td class={`mono ${d.cls}`}>{d.text}</td>
                      </tr>
                    );
                  })}
                </tbody>
              </table>
            </div>
          </section>

          {data.service_deltas.length > 0 && (
            <section class="card">
              <div class="card-head">
                <h2>Init 一次性服务耗时变化 (Top 15)</h2>
              </div>
              <div class="table-wrap">
                <table>
                  <thead>
                    <tr><th>服务名</th><th>A</th><th>B</th><th>Δ</th></tr>
                  </thead>
                  <tbody>
                    {data.service_deltas.map((r) => {
                      const d = fmtDelta(r.delta_ms, r.has_delta);
                      return (
                        <tr key={r.name}>
                          <td class="mono">{r.name}</td>
                          <td class="mono">{fmtMs(r.a_ms)}</td>
                          <td class="mono">{fmtMs(r.b_ms)}</td>
                          <td class={`mono ${d.cls}`}>{d.text}</td>
                        </tr>
                      );
                    })}
                  </tbody>
                </table>
              </div>
            </section>
          )}
        </div>
      )}
    </div>
  );
}
