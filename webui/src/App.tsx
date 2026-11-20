import { useEffect, useRef, useState } from 'preact/hooks';
import {
  fetchInspect,
  fetchList,
  hasKsuBridge,
  injectKsuInsets,
  launchReplay,
} from './bridge';
import { BlCard } from './components/BlCard';
import { BootTrendCard } from './components/BootTrendCard';
import { CompareView } from './components/CompareView';
import { GanttTimeline } from './components/GanttTimeline';
import { LogsView } from './components/LogsView';
import { PsiCpuCard } from './components/PsiCpuCard';
import { ReplayBar } from './components/ReplayBar';
import { SettingsView } from './components/SettingsView';
import { copyTextToClipboard, fmtMs } from './format';
import type {
  ConfigState,
  InspectResponse,
  ListResponse,
  LogGroupKey,
  ReplaySpeed,
} from './types';

type Tab = 'overview' | 'logs' | 'compare' | 'settings';

function groupClass(g?: LogGroupKey): string {
  if (g === 'merr') return 'lvl-merr';
  if (g === 'fatal') return 'lvl-fatal';
  if (g === 'init_fail') return 'lvl-init';
  if (g === 'stage') return 'lvl-stage';
  if (g === 'info') return 'lvl-info';
  return 'lvl-error';
}

export function App() {
  const [tab, setTab] = useState<Tab>('overview');
  const [visited, setVisited] = useState<{ logs: boolean; compare: boolean; settings: boolean }>({
    logs: false,
    compare: false,
    settings: false,
  });
  const [listData, setListData] = useState<ListResponse | null>(null);
  const [selectedId, setSelectedId] = useState<string>('');
  const [inspectData, setInspectData] = useState<InspectResponse | null>(null);
  const [refreshSeq, setRefreshSeq] = useState<number>(0);
  const [includeBl, setIncludeBl] = useState<boolean>(true);
  const [includePreSf, setIncludePreSf] = useState<boolean>(true);
  const [speed, setSpeed] = useState<ReplaySpeed>('2');
  const [replaying, setReplaying] = useState<boolean>(false);
  const [statusMsg, setStatusMsg] = useState<string>('');
  const [statusErr, setStatusErr] = useState<boolean>(false);
  const [copiedIdx, setCopiedIdx] = useState<number>(-1);
  const copyTimerRef = useRef<number | null>(null);

  const setStatus = (msg: string, err = false) => {
    setStatusMsg(msg);
    setStatusErr(err);
  };

  useEffect(() => {
    return () => {
      if (copyTimerRef.current != null) window.clearTimeout(copyTimerRef.current);
    };
  }, []);

  async function handleCopyPreview(idx: number, e: { ts_ms: number; src: string; text: string }) {
    if (window.getSelection()?.toString()) return;
    const formatted = `[${(e.ts_ms / 1000).toFixed(2)}s] [${e.src}] ${e.text}`;
    const ok = await copyTextToClipboard(formatted);
    if (!ok) {
      setStatus('复制失败，请长按手动选择文本', true);
      return;
    }
    setCopiedIdx(idx);
    if (copyTimerRef.current != null) window.clearTimeout(copyTimerRef.current);
    copyTimerRef.current = window.setTimeout(() => setCopiedIdx(-1), 750);
    const preview = e.text.length > 42 ? `${e.text.slice(0, 42)}…` : e.text;
    setStatus(`已复制日志: ${preview}`);
  }

  function switchTab(next: Tab) {
    setTab(next);
    if (next !== 'overview') {
      setVisited((prev) => (prev[next] ? prev : { ...prev, [next]: true }));
    }
  }

  async function reloadList(preferredId?: string) {
    try {
      const data = await fetchList();
      setListData(data);
      const nextId =
        (preferredId && data.runs.some((r) => r.boot_id === preferredId) ? preferredId : '')
        || (selectedId && data.runs.some((r) => r.boot_id === selectedId) ? selectedId : '')
        || data.runs[0]?.boot_id
        || '';
      setSelectedId(nextId);
      if (nextId) {
        const detail = await fetchInspect(nextId);
        setInspectData(detail);
      } else {
        setInspectData(null);
      }
      setRefreshSeq((n) => n + 1);
      setStatus(`已加载 ${data.total_runs} 条启动记录`);
    } catch (e) {
      setStatus(`加载失败: ${(e as Error).message}`, true);
    }
  }

  useEffect(() => {
    injectKsuInsets();
    reloadList();
  }, []);

  async function handleSelectRun(bootId: string) {
    setSelectedId(bootId);
    try {
      const detail = await fetchInspect(bootId);
      setInspectData(detail);
      setStatus(`已切换至 ${bootId.slice(0, 8)}`);
    } catch (e) {
      setStatus(`读取详情失败: ${(e as Error).message}`, true);
    }
  }

  async function handleReplay() {
    if (!selectedId) return;
    setReplaying(true);
    try {
      const res = await launchReplay({
        bootId: selectedId,
        bl: includeBl,
        preSf: includePreSf,
        speed,
      });
      if (res === 'already_running') {
        setStatus('已有回放正在运行，请稍候完成', true);
      } else {
        setStatus(`已触发屏幕叠加回放 (${speed}x) — 请切回桌面查看`);
      }
    } catch (e) {
      setStatus(`拉起回放失败: ${(e as Error).message}`, true);
    } finally {
      setReplaying(false);
    }
  }

  function handleConfigUpdated(cfg: ConfigState) {
    if (!listData) return;
    setListData({ ...listData, config: cfg });
  }

  const dev = listData?.device;
  const bootDone = inspectData
    ? (inspectData.boot_events.find((b) => b.tag === 'wm_boot_animation_done')?.ms
      ?? inspectData.boot_events[inspectData.boot_events.length - 1]?.ms
      ?? (inspectData.metadata.finished_uptime ? Number(inspectData.metadata.finished_uptime) * 1000 : -1))
    : -1;
  const lc = inspectData?.log_counts;
  const hasCriticalAlert = ((lc?.merr ?? 0) + (lc?.fatal ?? 0)) > 0;
  const errSummaryTotal = inspectData?.errors_total ?? inspectData?.errors.length ?? 0;

  return (
    <main class="shell">
      <header class="topbar">
        <div class="brand">
          <div class="brand-mark" />
          <div>
            <h1>BOOTSCOPE</h1>
            <p class="sub mono">
              {dev ? `${dev.model} · Android ${dev.android_version} (API ${dev.sdk})` : '加载中...'}
            </p>
          </div>
        </div>
        <button type="button" class="btn ghost" onClick={() => reloadList()}>
          刷新
        </button>
      </header>

      {!hasKsuBridge() && (
        <div class="banner">
          未检测到 KernelSU <code>window.ksu</code> 桥接，当前处于桌面模拟数据预览模式。
        </div>
      )}

      <nav class="tabs">
        <button type="button" class={`tab-btn ${tab === 'overview' ? 'active' : ''}`} onClick={() => switchTab('overview')}>
          总览
        </button>
        <button type="button" class={`tab-btn ${tab === 'logs' ? 'active' : ''}`} onClick={() => switchTab('logs')}>
          <span>启动日志</span>
          {hasCriticalAlert && <span class="tab-dot" aria-hidden="true" />}
        </button>
        <button type="button" class={`tab-btn ${tab === 'compare' ? 'active' : ''}`} onClick={() => switchTab('compare')}>
          A/B 对比
        </button>
        <button type="button" class={`tab-btn ${tab === 'settings' ? 'active' : ''}`} onClick={() => switchTab('settings')}>
          配置
        </button>
      </nav>

      <p class={`status-line mono ${statusErr ? 'err' : ''}`}>{statusMsg || '\u00a0'}</p>

      {listData && (
        <div class={`tab-pane ${tab === 'overview' ? '' : 'is-hidden'}`}>
          <ReplayBar
            runs={listData.runs}
            selectedId={selectedId}
            onSelect={handleSelectRun}
            includeBl={includeBl}
            onToggleBl={setIncludeBl}
            includePreSf={includePreSf}
            onTogglePreSf={setIncludePreSf}
            speed={speed}
            onSpeed={setSpeed}
            onReplay={handleReplay}
            replaying={replaying}
            totalRuns={listData.total_runs}
          />

          {inspectData && (
            <>
              <section class="summary-grid">
                <article class="stat-card">
                  <div class="stat-k">BL 耗时</div>
                  <div class="stat-v mono">{fmtMs(inspectData.bootloader.total_ms)}</div>
                  <div class="stat-s mono">
                    {inspectData.bootloader.source}
                    {inspectData.bootloader.wait_ms > 0 ? ` · 等 ${fmtMs(inspectData.bootloader.wait_ms)}` : ''}
                  </div>
                </article>
                <article class="stat-card">
                  <div class="stat-k">浮层首帧</div>
                  <div class="stat-v mono">{fmtMs(inspectData.display.first_frame_ms)}</div>
                  <div class="stat-s mono">
                    {(inspectData.display.self_cpu_ms ?? -1) >= 0
                      ? `自耗 ${inspectData.display.self_cpu_ms}ms${(inspectData.display.self_rss_kb ?? 0) > 0 ? ` · ${(inspectData.display.self_rss_kb! / 1024).toFixed(1)}MB` : ''}`
                      : `${inspectData.display.mode} · 退 ${fmtMs(inspectData.display.released_ms)}`}
                  </div>
                </article>
                <article class="stat-card">
                  <div class="stat-k">启动完成</div>
                  <div class="stat-v mono">{fmtMs(bootDone)}</div>
                  <div class="stat-s mono">
                    {inspectData.metadata.boot_completed === '1'
                      ? '已完成'
                      : `状态=${inspectData.metadata.boot_completed || '?'}`}
                    {inspectData.metadata.bootreason ? ` · ${inspectData.metadata.bootreason}` : ''}
                  </div>
                </article>
              </section>

              <GanttTimeline run={inspectData} />
              <BlCard bl={inspectData.bootloader} />
              <PsiCpuCard psi={inspectData.psi_series} topCpu={inspectData.top_cpu} />
              <BootTrendCard runs={listData.runs} selectedId={selectedId} onSelect={handleSelectRun} />

              <section class="two-col">
                <article class="card">
                  <div class="card-head">
                    <h2>关键路径与框架事件</h2>
                  </div>
                  <div class="phase-list mono">
                    {inspectData.phases.map((p) => (
                      <div key={p.id} class="phase-row">
                        <div class="phase-top">
                          <span class="phase-id">{p.id}</span>
                          <span class="phase-dur">{p.dur_ms >= 0 ? fmtMs(p.dur_ms) : p.state.toUpperCase()}</span>
                        </div>
                        <div class="phase-detail">{p.detail || '-'}</div>
                      </div>
                    ))}
                  </div>
                  <h3>Framework 事件</h3>
                  <div class="kv-scroll">
                    <div class="kv-scroll-inner mono">
                      {inspectData.boot_events.map((b) => (
                        <div key={b.tag} class="kv-row">
                          <span class="k">{b.tag}</span>
                          <span class="v">{b.ms}ms</span>
                        </div>
                      ))}
                    </div>
                  </div>
                </article>

                <article class="card">
                  <div class="card-head">
                    <h2>Init 耗时服务与错误摘要</h2>
                  </div>
                  <div class="kv-scroll">
                    <div class="kv-scroll-inner mono">
                      {inspectData.top_init_services.slice(0, 10).map((s) => (
                        <div key={`${s.name}-${s.ts_ms}`} class="kv-row">
                          <span class="k">
                            {s.name} <span class="muted">@{(s.ts_ms / 1000).toFixed(2)}s</span>
                          </span>
                          <span class="v">{fmtMs(s.dur_ms)}</span>
                        </div>
                      ))}
                    </div>
                  </div>
                  <h3>错误摘要 ({errSummaryTotal})</h3>
                  {lc && (
                    <div class="err-summary-grid mono">
                      <button
                        type="button"
                        class={`err-chip lvl-merr ${lc.merr > 0 ? 'is-alert' : ''}`}
                        onClick={() => switchTab('logs')}
                      >
                        <span class="err-chip-k">模块 M</span>
                        <span class="err-chip-v">{lc.merr}</span>
                      </button>
                      <button
                        type="button"
                        class={`err-chip lvl-fatal ${lc.fatal > 0 ? 'is-alert' : ''}`}
                        onClick={() => switchTab('logs')}
                      >
                        <span class="err-chip-k">致命 F</span>
                        <span class="err-chip-v">{lc.fatal}</span>
                      </button>
                      <button
                        type="button"
                        class={`err-chip lvl-init ${lc.init_fail > 0 ? 'is-alert' : ''}`}
                        onClick={() => switchTab('logs')}
                      >
                        <span class="err-chip-k">Init 失败</span>
                        <span class="err-chip-v">{lc.init_fail}</span>
                      </button>
                      <button
                        type="button"
                        class="err-chip lvl-error"
                        onClick={() => switchTab('logs')}
                      >
                        <span class="err-chip-k">错误 E</span>
                        <span class="err-chip-v">{lc.error}</span>
                      </button>
                    </div>
                  )}
                  {inspectData.errors.length === 0 ? (
                    <div class="kv-list mono">
                      <div class="kv-row"><span class="k">无错误记录</span><span class="v">OK</span></div>
                    </div>
                  ) : (
                    <div class="log-scroll log-preview">
                      <div class="log-list mono">
                        {inspectData.errors.map((e, idx) => (
                          <div
                            key={idx}
                            class={`log-row ${groupClass(e.group)} ${copiedIdx === idx ? 'is-copied' : ''}`}
                            title="点击复制完整日志"
                            onClick={() => handleCopyPreview(idx, e)}
                          >
                            <span class="log-ts">{(e.ts_ms / 1000).toFixed(2)}s</span>
                            <span class="log-src">{e.src}</span>
                            <span class="log-msg">{e.text}</span>
                          </div>
                        ))}
                      </div>
                    </div>
                  )}
                  <button type="button" class="btn ghost full-width" onClick={() => switchTab('logs')}>
                    进入启动日志分级分析 →
                  </button>
                </article>
              </section>
            </>
          )}
        </div>
      )}

      {listData && visited.logs && (
        <div class={`tab-pane ${tab === 'logs' ? '' : 'is-hidden'}`}>
          <LogsView
            runs={listData.runs}
            selectedId={selectedId}
            onSelectRun={handleSelectRun}
            refreshSeq={refreshSeq}
            onStatus={setStatus}
          />
        </div>
      )}

      {listData && visited.compare && (
        <div class={`tab-pane ${tab === 'compare' ? '' : 'is-hidden'}`}>
          <CompareView runs={listData.runs} refreshSeq={refreshSeq} />
        </div>
      )}

      {listData && visited.settings && (
        <div class={`tab-pane ${tab === 'settings' ? '' : 'is-hidden'}`}>
          <SettingsView
            config={listData.config}
            diskKb={listData.disk_kb}
            totalRuns={listData.total_runs}
            onConfigUpdated={handleConfigUpdated}
            onPruned={() => reloadList()}
            onStatus={setStatus}
          />
        </div>
      )}
    </main>
  );
}
