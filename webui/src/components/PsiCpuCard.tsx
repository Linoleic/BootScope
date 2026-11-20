import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { buildMonotoneCubic, svgXFromPointer } from '../format';
import type { PsiPoint, TopCpuProc } from '../types';

export function PsiCpuCard({ psi, topCpu }: { psi: PsiPoint[]; topCpu: TopCpuProc[] }) {
  const [scrubX, setScrubX] = useState<number | null>(null);
  const draggingRef = useRef<boolean>(false);

  const w = 640;
  const h = 120;
  const padL = 34;
  const padR = 12;
  const padT = 10;
  const padB = 22;
  const innerW = w - padL - padR;
  const innerH = h - padT - padB;

  const toY = (pct: number) =>
    padT + innerH - (Math.max(0, Math.min(100, pct)) / 100) * innerH;

  useEffect(() => {
    setScrubX(null);
  }, [psi]);

  const chart = useMemo(() => {
    const pts: PsiPoint[] = [];
    for (const p of psi) {
      if (pts.length === 0 || p.ts_ms > pts[pts.length - 1].ts_ms) {
        pts.push(p);
      }
    }
    const peakCpu = pts.length > 0 ? Math.max(...pts.map((p) => p.cpu)) : 0;
    const peakIo = pts.length > 0 ? Math.max(...pts.map((p) => p.io)) : 0;
    const minTs = pts.length > 0 ? pts[0].ts_ms : 0;
    const maxTs = pts.length > 1 ? Math.max(minTs + 1000, pts[pts.length - 1].ts_ms) : minTs + 1000;
    const toX = (tsMs: number) =>
      pts.length <= 1 ? padL + innerW / 2 : padL + ((tsMs - minTs) / (maxTs - minTs)) * innerW;
    const xs = pts.map((p) => toX(p.ts_ms));

    return {
      pts,
      peakCpu,
      peakIo,
      minTs,
      maxTs,
      xs,
      cpuCurve: buildMonotoneCubic(xs, pts.map((p) => p.cpu), toY),
      ioCurve: buildMonotoneCubic(xs, pts.map((p) => p.io), toY),
      memCurve: buildMonotoneCubic(xs, pts.map((p) => p.mem), toY),
    };
  }, [psi]);

  if (chart.pts.length === 0 && topCpu.length === 0) return null;

  const { pts, peakCpu, peakIo, minTs, maxTs, xs, cpuCurve, ioCurve, memCurve } = chart;
  const minX = xs.length > 0 ? xs[0] : padL;
  const maxX = xs.length > 0 ? xs[xs.length - 1] : padL + innerW;

  const scrubAt = (svg: SVGSVGElement, clientX: number) => {
    if (xs.length === 0) return;
    setScrubX(svgXFromPointer(svg, clientX, w, minX, maxX));
  };

  const activeX = scrubX != null && pts.length > 0 ? Math.min(maxX, Math.max(minX, scrubX)) : null;
  const activeTsMs =
    activeX == null
      ? 0
      : pts.length <= 1
        ? pts[0].ts_ms
        : minTs + ((activeX - padL) / innerW) * (maxTs - minTs);
  const cpuVal = activeX != null ? cpuCurve.evalAt(activeX) : 0;
  const ioVal = activeX != null ? ioCurve.evalAt(activeX) : 0;
  const memVal = activeX != null ? memCurve.evalAt(activeX) : 0;
  const maxProcPct = topCpu.length > 0 ? Math.max(10, ...topCpu.map((c) => c.pct)) : 100;

  return (
    <section class="card">
      <div class="card-head">
        <h2>资源压力与高负载进程</h2>
        <span class="pill mono">CPU峰 {peakCpu}% · IO峰 {peakIo}%</span>
      </div>

      {pts.length > 0 && (
        <>
          <svg
            class="chart-svg mono"
            viewBox={`0 0 ${w} ${h}`}
            aria-label="开机 PSI 压力曲线"
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
            <defs>
              <linearGradient id="psi-cpu-fill" x1="0" y1="0" x2="0" y2="1">
                <stop offset="0%" stop-color="#6ee7c8" stop-opacity="0.22" />
                <stop offset="100%" stop-color="#6ee7c8" stop-opacity="0" />
              </linearGradient>
            </defs>

            {[0, 50, 100].map((tick) => {
              const y = toY(tick);
              return (
                <g key={tick}>
                  <line x1={padL} y1={y} x2={w - padR} y2={y} stroke="#1d2f42" stroke-width="1" />
                  <text x={padL - 5} y={y + 3} fill="#93acc0" font-size="9" text-anchor="end">
                    {tick}%
                  </text>
                </g>
              );
            })}

            <text x={padL} y={h - 5} fill="#93acc0" font-size="9" text-anchor="start">
              {(minTs / 1000).toFixed(1)}s
            </text>
            {pts.length > 1 && (
              <text x={padL + innerW / 2} y={h - 5} fill="#93acc0" font-size="9" text-anchor="middle">
                {((minTs + maxTs) / 2000).toFixed(1)}s
              </text>
            )}
            <text x={w - padR} y={h - 5} fill="#93acc0" font-size="9" text-anchor="end">
              {(maxTs / 1000).toFixed(1)}s
            </text>

            {pts.length > 1 && cpuCurve.path && (
              <>
                <path d={cpuCurve.areaPath(toY(0))} fill="url(#psi-cpu-fill)" />
                <path
                  fill="none"
                  stroke="#ff7b89"
                  stroke-width="1.6"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  d={memCurve.path}
                  opacity="0.85"
                />
                <path
                  fill="none"
                  stroke="#f7c568"
                  stroke-width="1.8"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  d={ioCurve.path}
                  opacity="0.9"
                />
                <path
                  fill="none"
                  stroke="#6ee7c8"
                  stroke-width="2.1"
                  stroke-linecap="round"
                  stroke-linejoin="round"
                  d={cpuCurve.path}
                />
              </>
            )}

            {pts.length === 1 && activeX == null && (
              <circle
                cx={xs[0]}
                cy={toY(pts[0].cpu)}
                r={3.2}
                fill="#6ee7c8"
                stroke="#0b131c"
                stroke-width="1.2"
              />
            )}

            {activeX != null && (
              <g class="chart-ruler-group">
                <line
                  class="chart-ruler"
                  x1={activeX}
                  y1={padT}
                  x2={activeX}
                  y2={padT + innerH}
                />
                <circle cx={activeX} cy={toY(memVal)} r={3.6} fill="#ff7b89" stroke="#0b131c" stroke-width="1.2" />
                <circle cx={activeX} cy={toY(ioVal)} r={3.8} fill="#f7c568" stroke="#0b131c" stroke-width="1.2" />
                <circle cx={activeX} cy={toY(cpuVal)} r={4.6} fill="#eef4f8" stroke="#6ee7c8" stroke-width="1.8" />
              </g>
            )}
          </svg>

          <div class="chart-legend mono">
            <span class="bl-legend-item"><span class="dot" style="background:#6ee7c8" />CPU</span>
            <span class="bl-legend-item"><span class="dot" style="background:#f7c568" />IO</span>
            <span class="bl-legend-item"><span class="dot" style="background:#ff7b89" />MEM</span>
            <span class="chart-samples">{pts.length} 采样</span>
          </div>

          <div class="gantt-tip mono">
            {activeX != null
              ? `@${(activeTsMs / 1000).toFixed(2)}s · CPU ${Math.round(cpuVal)}% · IO ${Math.round(ioVal)}% · MEM ${Math.round(memVal)}%`
              : '在曲线上滑动或点击标尺查看任意时刻 CPU / IO / MEM'}
          </div>
        </>
      )}

      {topCpu.length > 0 && (
        <>
          <h3>高 CPU 占用进程</h3>
          <div class="cpu-list mono">
            {topCpu.map((c) => (
              <div key={c.comm} class="cpu-row">
                <div class="cpu-row-top">
                  <span class="cpu-name">{c.comm}</span>
                  <span class="cpu-pct">{c.pct.toFixed(1)}%</span>
                </div>
                <div class="cpu-track">
                  <div
                    class="cpu-fill"
                    style={`width:${Math.min(100, Math.max(3, (c.pct / maxProcPct) * 100)).toFixed(1)}%`}
                  />
                </div>
              </div>
            ))}
          </div>
        </>
      )}
    </section>
  );
}
