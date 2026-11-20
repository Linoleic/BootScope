import { RunSelect } from './RunSelect';
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

      <RunSelect
        runs={props.runs}
        value={props.selectedId}
        onChange={props.onSelect}
        ariaLabel="选择启动记录"
      />

      <div class="replay-row">
        <div class="check-group">
          <label class="check-chip">
            <input
              type="checkbox"
              checked={props.includeBl}
              onChange={(e) => props.onToggleBl((e.target as HTMLInputElement).checked)}
            />
            <span>含 BL (--bl)</span>
          </label>
          <label class="check-chip">
            <input
              type="checkbox"
              checked={props.includePreSf}
              onChange={(e) => props.onTogglePreSf((e.target as HTMLInputElement).checked)}
            />
            <span>含帧前 (--pre-sf)</span>
          </label>
        </div>

        <div class="replay-controls">
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
