import { useEffect, useState } from 'preact/hooks';
import { fetchCompare } from '../bridge';
import { fmtDelta, fmtMs, shortRunLabel } from '../format';
import type { CompareResponse, RunSummary } from '../types';

export function CompareView({ runs }: { runs: RunSummary[] }) {
  const [idA, setIdA] = useState<string>(runs[1]?.boot_id || runs[0]?.boot_id || '');
  const [idB, setIdB] = useState<string>(runs[0]?.boot_id || '');
  const [data, setData] = useState<CompareResponse | null>(null);
  const [err, setErr] = useState<string>('');
  const [loading, setLoading] = useState<boolean>(false);

  useEffect(() => {
    if (!idA || !idB) return;
    setLoading(true);
    setErr('');
    fetchCompare(idA, idB)
      .then((res) => setData(res))
      .catch((e: Error) => setErr(e.message))
      .finally(() => setLoading(false));
  }, [idA, idB]);

  if (runs.length < 2) {
    return (
      <section class="card">
        <div class="sub">至少需要 2 条启动记录才可进行 A / B 耗时对比。</div>
      </section>
    );
  }

  return (
    <div style="display:flex;flex-direction:column;gap:12px">
      <section class="card">
        <div class="card-head">
          <h2>选择对比基准 (A → B)</h2>
          {loading && <span class="pill mono">计算中…</span>}
        </div>
        <div class="two-col">
          <div>
            <div class="stat-k" style="margin-bottom:4px">基准记录 A</div>
            <select class="select mono" value={idA} onChange={(e) => setIdA((e.target as HTMLSelectElement).value)}>
              {runs.map((r, i) => (
                <option key={r.boot_id} value={r.boot_id}>
                  {shortRunLabel(r.boot_id, i, [fmtMs(r.boot_done_ms)])}
                </option>
              ))}
            </select>
          </div>
          <div>
            <div class="stat-k" style="margin-bottom:4px">目标记录 B</div>
            <select class="select mono" value={idB} onChange={(e) => setIdB((e.target as HTMLSelectElement).value)}>
              {runs.map((r, i) => (
                <option key={r.boot_id} value={r.boot_id}>
                  {shortRunLabel(r.boot_id, i, [fmtMs(r.boot_done_ms)])}
                </option>
              ))}
            </select>
          </div>
        </div>
        {err && <p class="status-line err mono">{err}</p>}
      </section>

      {data && (
        <>
          <section class="summary-grid">
            {[
              { label: 'BL 硬件耗时 Δ', a: data.run_a.bl_total_ms, b: data.run_b.bl_total_ms },
              { label: '早期首帧时刻 Δ', a: data.run_a.first_frame_ms, b: data.run_b.first_frame_ms },
              { label: '系统完成启动 Δ', a: data.run_a.boot_done_ms, b: data.run_b.boot_done_ms },
            ].map((item) => {
              const valid = item.a >= 0 && item.b >= 0;
              const d = fmtDelta(item.b - item.a, valid);
              return (
                <article key={item.label} class="stat-card">
                  <div class="stat-k">{item.label}</div>
                  <div class={`stat-v mono ${d.cls}`}>{d.text}</div>
                  <div class="stat-s mono">A: {fmtMs(item.a)} · B: {fmtMs(item.b)}</div>
                </article>
              );
            })}
          </section>

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
        </>
      )}
    </div>
  );
}
