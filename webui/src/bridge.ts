import type {
  CompareResponse,
  ConfigState,
  InspectResponse,
  ListResponse,
  ReplaySpeed,
  RulesResponse,
} from './types';

declare global {
  interface Window {
    ksu?: {
      exec?: (cmd: string, options: string, callback: string) => void;
    };
    [key: string]: unknown;
  }
}

const BIN = '/data/adb/modules/bootscope/bootscope';
const UUID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const VALID_SPEEDS = new Set<ReplaySpeed>(['0.5', '1', '2', '4']);
let cbSeq = 0;

export function hasKsuBridge(): boolean {
  return typeof window !== 'undefined' && typeof window.ksu?.exec === 'function';
}

export function injectKsuInsets(): void {
  if (typeof window === 'undefined' || !window.ksu) return;
  if (document.querySelector('link[href="/internal/insets.css"]')) return;
  const link = document.createElement('link');
  link.rel = 'stylesheet';
  link.href = '/internal/insets.css';
  document.head.appendChild(link);
}

export function ksuExec(
  command: string,
  timeoutMs = 15000,
): Promise<{ errno: number; stdout: string; stderr: string }> {
  const bridge = typeof window !== 'undefined' ? window.ksu : undefined;
  if (!bridge || typeof bridge.exec !== 'function') {
    return Promise.reject(new Error('bridge_unavailable'));
  }
  const cbName = `__bsKsu_${Date.now().toString(36)}_${cbSeq++}`;
  return new Promise((resolve, reject) => {
    let settled = false;
    const timer = window.setTimeout(() => {
      if (settled) return;
      settled = true;
      delete window[cbName];
      reject(new Error('exec_timeout'));
    }, timeoutMs);

    window[cbName] = (errno: number, stdout: string, stderr: string) => {
      if (settled) return;
      settled = true;
      window.clearTimeout(timer);
      delete window[cbName];
      resolve({ errno, stdout: String(stdout ?? ''), stderr: String(stderr ?? '') });
    };

    try {
      if (typeof bridge.exec !== 'function') {
        throw new Error('bridge_unavailable');
      }
      bridge.exec(command, JSON.stringify({}), cbName);
    } catch (err) {
      if (!settled) {
        settled = true;
        window.clearTimeout(timer);
        delete window[cbName];
        reject(err);
      }
    }
  });
}

async function execJson<T>(subcmd: string): Promise<T> {
  const res = await ksuExec(`${BIN} ${subcmd}`);
  if (!res.stdout.trim()) {
    throw new Error(`命令无输出 (${subcmd}, rc=${res.errno})`);
  }
  return JSON.parse(res.stdout) as T;
}

const MOCK_LIST: ListResponse = {
  device: {
    model: 'Pixel 7 Pro (Preview)',
    device: 'cheetah',
    android_version: '17',
    sdk: '37',
    abi: 'arm64-v8a',
    slot: '_b',
  },
  config: { display_enabled: true, max_history: 20, ksu_early_installed: true },
  total_runs: 2,
  disk_kb: 1280,
  runs: [
    {
      boot_id: '11111111-2222-4333-8444-555555555555',
      mtime_epoch: 1795000000,
      android_version: '17',
      sdk: '37',
      mode: 'early_native',
      bl_source: 'UEFILOG',
      bl_total_ms: 4728,
      bl_wait_ms: 0,
      first_frame_ms: 3660,
      released_ms: 21500,
      boot_done_ms: 21100,
      finished_uptime_s: 24.5,
      boot_completed: '1',
      error_count: 1,
    },
    {
      boot_id: 'aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee',
      mtime_epoch: 1794990000,
      android_version: '17',
      sdk: '37',
      mode: 'early_native',
      bl_source: 'PROP',
      bl_total_ms: 1322,
      bl_wait_ms: 10032,
      first_frame_ms: 2960,
      released_ms: 22205,
      boot_done_ms: 21900,
      finished_uptime_s: 27.2,
      boot_completed: '1',
      error_count: 0,
    },
  ],
};

const MOCK_INSPECT: InspectResponse = {
  ok: true,
  boot_id: MOCK_LIST.runs[0].boot_id,
  mtime_epoch: 1795000000,
  metadata: { android_version: '17', sdk: '37', boot_completed: '1', bootreason: 'reboot' },
  display: {
    mode: 'early_native',
    started_ms: 2338,
    first_frame_ms: 3660,
    released_ms: 21500,
    layout: '1440x3120@120',
    exit: 'animation_ended',
    self_cpu_ms: 142,
    self_rss_kb: 6144,
    frames: 74,
  },
  bootloader: {
    source: 'UEFILOG',
    summary: 'PBL:140 SBL1:1329 UEFI:1264 ABL:1404',
    meta: 'POR:PSHOLD COLD UFS DDR:4224M SLOT:_A',
    total_ms: 4728,
    timeline_ms: 4728,
    wait_ms: 0,
    stages: [
      { id: 'PBL', start_ms: 0, dur_ms: 140, wait_only: false, detail: 'APBL MEDIA+AUTH' },
      { id: 'SBL1', start_ms: 150, dur_ms: 1329, wait_only: false, detail: 'SBL1/XBL DDR:4224MHZ' },
      { id: 'UEFI', start_ms: 1744, dur_ms: 1264, wait_only: false, detail: 'DISPLAYDXE LOGO:45MS' },
      { id: 'ABL', start_ms: 3008, dur_ms: 1404, wait_only: false, detail: 'KL:55 VDR:28 OVL:300 DT:46' },
      { id: 'EBS', start_ms: 4412, dur_ms: 316, wait_only: false, detail: 'EBS -> KERNEL ENTRY' },
    ],
    events: [
      { por_ms: 0, major: true, text: 'APBL START' },
      { por_ms: 150, major: true, text: 'SBL1 START' },
      { por_ms: 1744, major: true, text: 'UEFI START' },
      { por_ms: 3008, major: true, text: 'ABL LINUXLOADER' },
    ],
  },
  phases: [
    { id: 'BL', state: 'done', start_ms: 0, end_ms: 4728, dur_ms: 4728, detail: 'PBL:140 SBL1:1329 UEFI:1264 ABL:1404' },
    { id: 'INIT', state: 'done', start_ms: 0, end_ms: 6120, dur_ms: 6120, detail: 'ACT:ZYGOTE-START' },
    { id: 'ZYG', state: 'done', start_ms: 6120, end_ms: 10940, dur_ms: 4820, detail: 'SYSTEM_SERVER STARTED' },
    { id: 'PRE', state: 'done', start_ms: 11200, end_ms: 13100, dur_ms: 1900, detail: 'PRELOADED' },
    { id: 'PMS', state: 'done', start_ms: 13100, end_ms: 17850, dur_ms: 4750, detail: 'PMS_READY' },
    { id: 'AMS', state: 'done', start_ms: 17850, end_ms: 21100, dur_ms: 3250, detail: 'ANIM_DONE' },
  ],
  init_milestones: [
    { name: 'early-init', ts_ms: 1420 },
    { name: 'init', ts_ms: 2740 },
    { name: 'late-fs', ts_ms: 3120 },
    { name: 'post-fs-data', ts_ms: 4210 },
    { name: 'zygote-start', ts_ms: 6120 },
  ],
  top_init_services: [
    { name: 'apexd-bootstrap', ts_ms: 1850, dur_ms: 420, detail: 'status=0;took=0.420s' },
    { name: 'vdc', ts_ms: 4320, dur_ms: 310, detail: 'status=0;took=0.310s' },
  ],
  boot_events: [
    { tag: 'boot_progress_start', ms: 10940 },
    { tag: 'boot_progress_preload_end', ms: 13100 },
    { tag: 'boot_progress_pms_ready', ms: 17850 },
    { tag: 'wm_boot_animation_done', ms: 21100 },
  ],
  psi_series: [
    { ts_ms: 5000, cpu: 28, io: 42, mem: 0 },
    { ts_ms: 12000, cpu: 65, io: 18, mem: 2 },
    { ts_ms: 20000, cpu: 34, io: 8, mem: 0 },
  ],
  top_cpu: [{ comm: 'SYSTEM_SERVER', pct: 48.2 }],
  errors: [],
};

const MOCK_COMPARE: CompareResponse = {
  ok: true,
  run_a: { boot_id: MOCK_LIST.runs[1].boot_id, mtime_epoch: 1794990000, bl_source: 'PROP', bl_total_ms: 1322, first_frame_ms: 2960, released_ms: 22205, boot_done_ms: 21900 },
  run_b: { boot_id: MOCK_LIST.runs[0].boot_id, mtime_epoch: 1795000000, bl_source: 'UEFILOG', bl_total_ms: 4728, first_frame_ms: 3660, released_ms: 21500, boot_done_ms: 21100 },
  bl_deltas: [],
  phase_deltas: [
    { id: 'BL', a_ms: 1322, b_ms: 4728, delta_ms: 3406, has_delta: true },
    { id: 'INIT', a_ms: 6400, b_ms: 6120, delta_ms: -280, has_delta: true },
    { id: 'PMS', a_ms: 5200, b_ms: 4750, delta_ms: -450, has_delta: true },
  ],
  milestone_deltas: [
    { name: 'display:first_frame_submitted_ms', a_ms: 2960, b_ms: 3660, delta_ms: 700, has_delta: true },
    { name: 'events:wm_boot_animation_done', a_ms: 21900, b_ms: 21100, delta_ms: -800, has_delta: true },
  ],
  service_deltas: [
    { name: 'apexd-bootstrap', a_ms: 510, b_ms: 420, delta_ms: -90, has_delta: true },
  ],
};

const MOCK_RULES: RulesResponse = {
  ok: true,
  source: 'DEF',
  content: '# BootScope filter rules\nmodule_key=bootscope\nredact_key=android_id\n',
};

export async function fetchList(): Promise<ListResponse> {
  if (!hasKsuBridge()) return MOCK_LIST;
  return execJson<ListResponse>('list-json --limit 30');
}

export async function fetchInspect(bootId: string): Promise<InspectResponse> {
  if (!hasKsuBridge()) return MOCK_INSPECT;
  if (!UUID_RE.test(bootId)) throw new Error('invalid_boot_id');
  return execJson<InspectResponse>(`inspect-json ${bootId}`);
}

export async function fetchCompare(aId: string, bId: string): Promise<CompareResponse> {
  if (!hasKsuBridge()) return MOCK_COMPARE;
  if (!UUID_RE.test(aId) || !UUID_RE.test(bId)) throw new Error('invalid_boot_id');
  return execJson<CompareResponse>(`compare-json ${aId} ${bId}`);
}

export async function updateConfig(
  key: 'display_enabled' | 'max_history',
  value: string,
): Promise<ConfigState> {
  if (!hasKsuBridge()) {
    return {
      ...MOCK_LIST.config,
      [key]: key === 'display_enabled' ? value === '1' : Number(value),
    };
  }
  const safeVal = key === 'display_enabled' ? (value === '1' ? '1' : '0') : String(Number(value));
  const res = await execJson<{ ok: boolean; config?: ConfigState; error?: string }>(
    `config-set ${key} ${safeVal}`,
  );
  if (!res.ok || !res.config) throw new Error(res.error || 'config_set_failed');
  return res.config;
}

export async function pruneRuns(keep: number): Promise<{ kept: number; deleted: number }> {
  if (!hasKsuBridge()) return { kept: 2, deleted: 0 };
  const n = Math.max(1, Math.min(200, Math.round(keep)));
  const res = await execJson<{ ok: boolean; kept: number; deleted: number }>(`prune --keep ${n}`);
  return { kept: res.kept, deleted: res.deleted };
}

export async function fetchRules(): Promise<RulesResponse> {
  if (!hasKsuBridge()) return MOCK_RULES;
  return execJson<RulesResponse>('rules-get');
}

function encodeUtf8Base64(text: string): string {
  const bytes = new TextEncoder().encode(text);
  let bin = '';
  for (let i = 0; i < bytes.length; i++) {
    bin += String.fromCharCode(bytes[i]);
  }
  return btoa(bin);
}

export async function saveRules(content: string): Promise<RulesResponse> {
  if (!hasKsuBridge()) return { ok: true, source: 'USR', content };
  const b64 = encodeUtf8Base64(content);
  const res = await ksuExec(`printf '%s' '${b64}' | base64 -d | ${BIN} rules-set`);
  if (!res.stdout.trim()) throw new Error('rules_set_no_output');
  return JSON.parse(res.stdout) as RulesResponse;
}

export async function launchReplay(opts: {
  bootId: string;
  bl: boolean;
  preSf: boolean;
  speed: ReplaySpeed;
}): Promise<'started' | 'already_running'> {
  if (!UUID_RE.test(opts.bootId)) throw new Error('invalid_boot_id');
  if (!VALID_SPEEDS.has(opts.speed)) throw new Error('invalid_speed');
  if (!hasKsuBridge()) return 'started';

  const flags: string[] = ['replay', opts.bootId];
  if (opts.bl) flags.push('--bl');
  if (opts.preSf) flags.push('--pre-sf');
  flags.push('--speed', opts.speed);

  const cmd = [
    `[ -x "${BIN}" ] || { echo NO_BIN; exit 1; };`,
    'if ps -ef 2>/dev/null | grep -q "[b]ootscope replay"; then echo ALREADY_RUNNING; exit 0; fi;',
    `nohup "${BIN}" ${flags.join(' ')} </dev/null >/dev/null 2>&1 &`,
    'echo STARTED',
  ].join('\n');

  const res = await ksuExec(cmd);
  if (res.stdout.includes('ALREADY_RUNNING')) return 'already_running';
  if (res.stdout.includes('STARTED')) return 'started';
  throw new Error(res.stderr || res.stdout || 'replay_launch_failed');
}
