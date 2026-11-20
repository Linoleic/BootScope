import { useState } from 'preact/hooks';
import type { InspectResponse } from '../types';

export function GanttTimeline({ run }: { run: InspectResponse }) {
  const [selected, setSelected] = useState<string>('');

  const blSpan = run.bootloader.timeline_ms > 0 ? run.bootloader.timeline_ms : 0;
  const maxKernelMs = Math.max(
    1000,
    ...run.phases.filter((p) => p.id !== 'BL').map((p) => Math.max(p.start_ms, p.end_ms)),
    ...run.boot_events.map((b) => b.ms),
    run.display.released_ms > 0 ? run.display.released_ms : 0,
  );
  const totalAxis = blSpan + maxKernelMs;

  const padL = 96;
  const padR = 16;
  const width = 760;
  const innerW = width - padL - padR;
  const toX = (absMs: number) => padL + Math.max(0, Math.min(1, absMs / totalAxis)) * innerW;
  const toW = (durMs: number) => Math.max(3, (durMs / totalAxis) * innerW);

  const tracks = ['Bootloader', 'Critical Path', 'Init / 阶段', 'Framework', 'Display'];
  const rowH = 34;
  const height = 24 + tracks.length * rowH + 24;

  return (
    <section class="card">
      <div class="card-head">
        <h2>全链路启动时间轴 (POR → 桌面完成)</h2>
        <span class="pill mono">{(totalAxis / 1000).toFixed(2)} s</span>
      </div>

      <div class="svg-wrap">
        <svg class="gantt-svg" viewBox={`0 0 ${width} ${height}`}>
          {blSpan > 0 && (
            <>
              <line x1={toX(blSpan)} y1={14} x2={toX(blSpan)} y2={height - 20} stroke="#6ee7c8" stroke-dasharray="4 3" stroke-width="1" />
              <text x={toX(blSpan)} y={height - 6} fill="#6ee7c8" font-size="10" text-anchor="middle">
                Kernel 0s
              </text>
            </>
          )}

          {tracks.map((name, i) => {
            const y = 20 + i * rowH;
            return (
              <g key={name}>
                <text x={8} y={y + 18} fill="#93acc0" font-size="11">{name}</text>
                <line x1={padL} y1={y + 14} x2={width - padR} y2={y + 14} stroke="#1d2f42" stroke-width="1" />
              </g>
            );
          })}

          {run.bootloader.stages
            .filter((s) => !s.wait_only && s.dur_ms > 0)
            .map((s) => (
              <rect
                key={`bl-${s.id}`}
                x={toX(s.start_ms)}
                y={20 + 4}
                width={toW(s.dur_ms)}
                height={18}
                rx={3}
                fill="#68b0ff"
                opacity="0.88"
                style="cursor:pointer"
                onClick={() => setSelected(`BL/${s.id}: POR +${s.start_ms}ms · 耗时 ${s.dur_ms}ms (${s.detail})`)}
              />
            ))}

          {run.phases
            .filter((p) => p.id !== 'BL' && p.start_ms >= 0 && p.end_ms > p.start_ms)
            .map((p) => (
              <rect
                key={`ph-${p.id}`}
                x={toX(blSpan + p.start_ms)}
                y={20 + rowH + 4}
                width={toW(p.end_ms - p.start_ms)}
                height={18}
                rx={3}
                fill="#6ee7c8"
                opacity="0.85"
                style="cursor:pointer"
                onClick={() => setSelected(`阶段 ${p.id}: ${(p.start_ms / 1000).toFixed(2)}s ~ ${(p.end_ms / 1000).toFixed(2)}s · 耗时 ${p.dur_ms}ms`)}
              />
            ))}

          {run.init_milestones.map((m) => (
            <circle
              key={`im-${m.name}`}
              cx={toX(blSpan + m.ts_ms)}
              cy={20 + rowH * 2 + 14}
              r={4.5}
              fill="#f7c568"
              style="cursor:pointer"
              onClick={() => setSelected(`Init 动作 ${m.name}: +${(m.ts_ms / 1000).toFixed(3)}s`)}
            />
          ))}

          {run.boot_events.map((ev) => (
            <circle
              key={`fe-${ev.tag}`}
              cx={toX(blSpan + ev.ms)}
              cy={20 + rowH * 3 + 14}
              r={4.5}
              fill="#c792ea"
              style="cursor:pointer"
              onClick={() => setSelected(`Framework ${ev.tag}: +${ev.ms}ms`)}
            />
          ))}

          {run.display.first_frame_ms > 0 && run.display.released_ms > run.display.first_frame_ms && (
            <rect
              x={toX(blSpan + run.display.first_frame_ms)}
              y={20 + rowH * 4 + 4}
              width={toW(run.display.released_ms - run.display.first_frame_ms)}
              height={18}
              rx={3}
              fill="#ff8c7b"
              opacity="0.85"
              style="cursor:pointer"
              onClick={() => setSelected(`BootScope 浮层 (${run.display.mode}): 首帧 ${(run.display.first_frame_ms / 1000).toFixed(2)}s → 退场 ${(run.display.released_ms / 1000).toFixed(2)}s`)}
            />
          )}
        </svg>
      </div>

      <div class="gantt-tip mono">
        {selected || '点击时间轴上的色块或节点查看精确时间戳'}
      </div>
    </section>
  );
}
