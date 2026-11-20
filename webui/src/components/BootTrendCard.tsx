import { useEffect, useRef, useState } from 'preact/hooks';
import { fmtEpochShort, fmtMs, nearestChartIndex } from '../format';
import type { RunSummary } from '../types';

function resolveDoneMs(r: RunSummary): number {
  if (r.boot_done_ms >= 0) return r.boot_done_ms;
  if (r.finished_uptime_s > 0) return Math.round(r.finished_uptime_s * 1000);
  return -1;
}

export function BootTrendCard({
  runs,
  selectedId,
  onSelect,
}: {
  runs: RunSummary[];
  selectedId: string;
  onSelect: (bootId: string) => void;
}) {
  const [inspectedId, setInspectedId] = useState<string>(selectedId);
  const draggingRef = useRef<boolean>(false);

  useEffect(() => {
    setInspectedId(selectedId);
  }, [selectedId]);

  const ordered = runs.slice(0, 12).reverse();
  if (ordered.length < 2) return null;

  const doneVals = ordered.map(resolveDoneMs).filter((v) => v > 0);
  const blVals = ordered.map((r) => r.bl_total_ms).filter((v) => v > 0);
  const meanDone = doneVals.length > 0
    ? Math.round(doneVals.reduce((acc, v) => acc + v, 0) / doneVals.length)
    : -1;

  const w = 640;
  const h = 120;
  const padL = 44;
  const padR = 14;
  const padT = 12;
  const padB = 22;
  const innerW = w - padL - padR;
  const innerH = h - padT - padB;

  const maxY = Math.max(5000, ...doneVals, ...blVals);
  const toX = (idx: number) =>
    ordered.length <= 1 ? padL + innerW / 2 : padL + (idx / (ordered.length - 1)) * innerW;
  const toY = (ms: number) =>
    padT + innerH - (Math.max(0, Math.min(maxY, ms)) / maxY) * innerH;

  const xs = ordered.map((_, i) => toX(i));

  const donePts = ordered
    .map((r, i) => {
      const v = resolveDoneMs(r);
      return v > 0 ? `${xs[i].toFixed(1)},${toY(v).toFixed(1)}` : '';
    })
    .filter(Boolean)
    .join(' ');

  const blPts = ordered
    .map((r, i) => (r.bl_total_ms > 0 ? `${xs[i].toFixed(1)},${toY(r.bl_total_ms).toFixed(1)}` : ''))
    .filter(Boolean)
    .join(' ');

  const inspectedIdx = (() => {
    const byInsp = ordered.findIndex((r) => r.boot_id === inspectedId);
    if (byInsp >= 0) return byInsp;
    const bySel = ordered.findIndex((r) => r.boot_id === selectedId);
    return bySel >= 0 ? bySel : ordered.length - 1;
  })();
  const inspectedRun = ordered[inspectedIdx];
  const inspectedDone = inspectedRun ? resolveDoneMs(inspectedRun) : -1;
  const inspectedX = xs[inspectedIdx];

  const scrubAt = (svg: SVGSVGElement, clientX: number) => {
    const idx = nearestChartIndex(svg, clientX, w, xs);
    if (idx >= 0 && ordered[idx]) {
      setInspectedId(ordered[idx].boot_id);
    }
  };

  return (
    <section class="card">
      <div class="card-head">
        <h2>多次开机耗时趋势</h2>
        <span class="pill mono">近 {ordered.length} 次均值 {fmtMs(meanDone)}</span>
      </div>

      <svg
        class="chart-svg mono"
        viewBox={`0 0 ${w} ${h}`}
        aria-label="多次开机耗时趋势"
        onPointerDown={(e) => {
          draggingRef.current = true;
          try {
            (e.currentTarget as SVGSVGElement).setPointerCapture(e.pointerId);
          } catch {
            // Ignore if pointer capture is unavailable.
          }
          scrubAt(e.currentTarget as SVGSVGElement, e.clientX);
        }}
        onPointerMove={(e) => {
          if (!draggingRef.current) return;
          scrubAt(e.currentTarget as SVGSVGElement, e.clientX);
        }}
        onPointerUp={() => {
          draggingRef.current = false;
        }}
        onPointerCancel={() => {
          draggingRef.current = false;
        }}
      >
        {[0, 0.5, 1].map((frac) => {
          const valMs = maxY * frac;
          const y = toY(valMs);
          return (
            <g key={frac}>
              <line x1={padL} y1={y} x2={w - padR} y2={y} stroke="#1d2f42" stroke-width="1" />
              <text x={padL - 6} y={y + 3} fill="#93acc0" font-size="9" text-anchor="end">
                {(valMs / 1000).toFixed(0)}s
              </text>
            </g>
          );
        })}

        <text x={padL} y={h - 5} fill="#93acc0" font-size="9" text-anchor="start">
          较早 ({ordered[0].boot_id.slice(0, 6)})
        </text>
        <text x={w - padR} y={h - 5} fill="#93acc0" font-size="9" text-anchor="end">
          最新 ({ordered[ordered.length - 1].boot_id.slice(0, 6)})
        </text>

        {blVals.length >= 2 && (
          <polyline
            fill="none"
            stroke="#68b0ff"
            stroke-width="1.6"
            stroke-dasharray="4 3"
            points={blPts}
            opacity="0.88"
          />
        )}
        {doneVals.length >= 2 && (
          <polyline fill="none" stroke="#6ee7c8" stroke-width="2.2" points={donePts} />
        )}

        {inspectedRun && inspectedX != null && (
          <g class="chart-ruler-group">
            <line
              class="chart-ruler"
              x1={inspectedX}
              y1={padT}
              x2={inspectedX}
              y2={padT + innerH}
            />
            {inspectedRun.bl_total_ms > 0 && (
              <circle
                cx={inspectedX}
                cy={toY(inspectedRun.bl_total_ms)}
                r={3.8}
                fill="#68b0ff"
                stroke="#0b131c"
                stroke-width="1.2"
              />
            )}
          </g>
        )}

        {ordered.map((r, i) => {
          const v = resolveDoneMs(r);
          if (v <= 0) return null;
          const cx = xs[i];
          const cy = toY(v);
          const isCur = r.boot_id === selectedId;
          const isInsp = r.boot_id === inspectedRun?.boot_id;
          return (
            <circle
              key={r.boot_id}
              cx={cx}
              cy={cy}
              r={isInsp ? 4.8 : (isCur ? 4.4 : 3)}
              fill={isCur ? '#6ee7c8' : (isInsp ? '#eef4f8' : '#121e2b')}
              stroke={isInsp ? '#eef4f8' : '#6ee7c8'}
              stroke-width="1.8"
            />
          );
        })}
      </svg>

      <div class="chart-legend mono">
        <span class="bl-legend-item"><span class="dot" style="background:#6ee7c8" />系统完成</span>
        {blVals.length >= 2 && (
          <span class="bl-legend-item"><span class="dot" style="background:#68b0ff" />BL 耗时</span>
        )}
      </div>

      {inspectedRun && (
        <div class="trend-tip-row">
          <div class="gantt-tip mono trend-tip">
            {fmtEpochShort(inspectedRun.mtime_epoch) ? `${fmtEpochShort(inspectedRun.mtime_epoch)} · ` : ''}
            {inspectedRun.boot_id.slice(0, 8)} · 完成 {fmtMs(inspectedDone)}
            {inspectedRun.bl_total_ms > 0 ? ` · BL ${fmtMs(inspectedRun.bl_total_ms)}` : ''}
          </div>
          {inspectedRun.boot_id !== selectedId && (
            <button
              type="button"
              class="btn ghost trend-jump-btn"
              onClick={() => onSelect(inspectedRun.boot_id)}
            >
              查看
            </button>
          )}
        </div>
      )}
    </section>
  );
}
