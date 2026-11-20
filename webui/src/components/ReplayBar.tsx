import { fmtMs, shortRunLabel } from '../format';
import type { ReplaySpeed, RunSummary } from '../types';

interface Props {
  runs: RunSummary[];
  selectedId: string;
  onSelect: (id: string) => void;
  includeBl: boolean;
  onToggleBl: (v: boolean) => void;
  includePreSf: boolean;
  onTogglePreSf: (v: boolean) => void;
  speed: ReplaySpeed;
  onSpeed: (s: ReplaySpeed) => void;
  onReplay: () => void;
  replaying: boolean;
  totalRuns: number;
}

const SPEEDS: ReplaySpeed[] = ['0.5', '1', '2', '4'];

export function ReplayBar(props: Props) {
  return (
    <section class="card">
      <div class="card-head">
        <h2>启动记录与屏幕回放</h2>
        <span class="pill mono">{props.totalRuns} 条记录</span>
      </div>

      <select
        class="select mono"
        aria-label="选择启动记录"
        value={props.selectedId}
        onChange={(e) => props.onSelect((e.target as HTMLSelectElement).value)}
      >
        {props.runs.map((r, i) => (
          <option key={r.boot_id} value={r.boot_id}>
            {shortRunLabel(r.boot_id, i, [
              r.mode || 'probe',
              r.bl_source !== 'NONE' ? `BL ${fmtMs(r.bl_total_ms)}` : '',
              r.finished_uptime_s > 0 ? `${r.finished_uptime_s.toFixed(1)}s` : '',
            ])}
          </option>
        ))}
      </select>

      <div class="replay-row">
        <div class="check-group">
          <label class="check-chip">
            <input
              type="checkbox"
              checked={props.includeBl}
              onChange={(e) => props.onToggleBl((e.target as HTMLInputElement).checked)}
            />
            <span>含 BL 阶段 (--bl)</span>
          </label>
          <label class="check-chip">
            <input
              type="checkbox"
              checked={props.includePreSf}
              onChange={(e) => props.onTogglePreSf((e.target as HTMLInputElement).checked)}
            />
            <span>含帧前阶段 (--pre-sf)</span>
          </label>
        </div>

        <div class="inline-row">
          <div class="speed-group" role="group" aria-label="回放倍速">
            {SPEEDS.map((sp) => (
              <button
                key={sp}
                type="button"
                class={`speed-btn ${props.speed === sp ? 'active' : ''}`}
                onClick={() => props.onSpeed(sp)}
              >
                {sp}x
              </button>
            ))}
          </div>
          <button
            type="button"
            class="btn primary"
            disabled={!props.selectedId || props.replaying}
            onClick={props.onReplay}
          >
            屏幕叠加回放
          </button>
        </div>
      </div>
    </section>
  );
}
