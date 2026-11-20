export interface DeviceMeta {
  model: string;
  device: string;
  android_version: string;
  sdk: string;
  abi: string;
  slot: string;
}

export interface ConfigState {
  display_enabled: boolean;
  max_history: number;
  ksu_early_installed: boolean;
}

export interface RunSummary {
  boot_id: string;
  mtime_epoch: number;
  android_version: string;
  sdk: string;
  mode: string;
  bl_source: string;
  bl_total_ms: number;
  bl_wait_ms: number;
  first_frame_ms: number;
  released_ms: number;
  boot_done_ms: number;
  finished_uptime_s: number;
  boot_completed: string;
  error_count: number;
}

export interface ListResponse {
  device: DeviceMeta;
  config: ConfigState;
  total_runs: number;
  disk_kb: number;
  runs: RunSummary[];
}

export interface BlStage {
  id: string;
  start_ms: number;
  dur_ms: number;
  wait_only: boolean;
  detail: string;
}

export interface BlEvent {
  por_ms: number;
  major: boolean;
  text: string;
}

export interface BootloaderDetail {
  source: string;
  summary: string;
  meta: string;
  total_ms: number;
  timeline_ms: number;
  wait_ms: number;
  stages: BlStage[];
  events: BlEvent[];
}

export interface PhaseInfo {
  id: string;
  state: 'pending' | 'running' | 'done';
  start_ms: number;
  end_ms: number;
  dur_ms: number;
  detail: string;
}

export interface InitMilestone {
  name: string;
  ts_ms: number;
}

export interface InitServiceTiming {
  name: string;
  ts_ms: number;
  dur_ms: number;
  detail: string;
}

export interface BootEvent {
  tag: string;
  ms: number;
}

export interface PsiPoint {
  ts_ms: number;
  cpu: number;
  io: number;
  mem: number;
}

export interface TopCpuProc {
  comm: string;
  pct: number;
}

export type LogLevelKey = 'all' | 'critical' | 'merr' | 'fatal' | 'init_fail' | 'error' | 'stage' | 'info';
export type LogGroupKey = Exclude<LogLevelKey, 'all' | 'critical'>;
export type LogOrder = 'group' | 'time';

export interface LogCounts {
  merr: number;
  fatal: number;
  init_fail: number;
  error: number;
  stage: number;
  info: number;
  total: number;
}

export interface LogTagStat {
  tag: string;
  count: number;
  fatal: number;
}

export interface LogStageAnchor {
  ts_ms: number;
  src: string;
  text: string;
}

export interface LogEntry {
  ts_ms: number;
  src: string;
  kind: string;
  group: LogGroupKey;
  tag: string;
  text: string;
}

export interface LogsResponse {
  ok: boolean;
  boot_id: string;
  level: LogLevelKey;
  tag: string;
  query: string;
  limit: number;
  order?: LogOrder;
  matched_total: number;
  counts: LogCounts;
  stages?: LogStageAnchor[];
  top_tags: LogTagStat[];
  entries: LogEntry[];
}

export interface TraceError {
  ts_ms: number;
  src: string;
  kind: string;
  group?: LogGroupKey;
  text: string;
}

export interface InspectResponse {
  ok: boolean;
  boot_id: string;
  mtime_epoch: number;
  metadata: Record<string, string>;
  display: {
    mode: string;
    started_ms: number;
    first_frame_ms: number;
    released_ms: number;
    layout: string;
    exit: string;
    self_cpu_ms?: number;
    self_rss_kb?: number;
    frames?: number;
  };
  bootloader: BootloaderDetail;
  phases: PhaseInfo[];
  init_milestones: InitMilestone[];
  top_init_services: InitServiceTiming[];
  boot_events: BootEvent[];
  psi_series: PsiPoint[];
  top_cpu: TopCpuProc[];
  log_counts?: LogCounts;
  errors_total?: number;
  errors: TraceError[];
}

export interface DeltaRow {
  id?: string;
  name?: string;
  a_ms: number;
  b_ms: number;
  delta_ms: number;
  has_delta: boolean;
}

export interface CompareResponse {
  ok: boolean;
  run_a: {
    boot_id: string;
    mtime_epoch: number;
    bl_source: string;
    bl_total_ms: number;
    first_frame_ms: number;
    released_ms: number;
    boot_done_ms: number;
  };
  run_b: {
    boot_id: string;
    mtime_epoch: number;
    bl_source: string;
    bl_total_ms: number;
    first_frame_ms: number;
    released_ms: number;
    boot_done_ms: number;
  };
  bl_deltas: DeltaRow[];
  phase_deltas: DeltaRow[];
  milestone_deltas: DeltaRow[];
  service_deltas: DeltaRow[];
}

export interface RulesResponse {
  ok: boolean;
  source: string;
  content: string;
}

export type ReplaySpeed = '0.5' | '1' | '2' | '4';
