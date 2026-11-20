import { useEffect, useState } from 'preact/hooks';
import {
  fetchInspect,
  fetchList,
  hasKsuBridge,
  injectKsuInsets,
  launchReplay,
} from './bridge';
import { BlCard } from './components/BlCard';
import { CompareView } from './components/CompareView';
import { GanttTimeline } from './components/GanttTimeline';
import { ReplayBar } from './components/ReplayBar';
import { SettingsView } from './components/SettingsView';
import { fmtMs } from './format';
import type {
  ConfigState,
  InspectResponse,
  ListResponse,
  ReplaySpeed,
} from './types';

type Tab = 'overview' | 'compare' | 'settings';

export function App() {
  const [tab, setTab] = useState<Tab>('overview');
  const [visited, setVisited] = useState<{ compare: boolean; settings: boolean }>({
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

  const setStatus = (msg: string, err = false) => {
    setStatusMsg(msg);
    setStatusErr(err);
  };

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
          总览与时间轴
        </button>
        <button type="button" class={`tab-btn ${tab === 'compare' ? 'active' : ''}`} onClick={() => switchTab('compare')}>
          A/B 对比
        </button>
        <button type="button" class={`tab-btn ${tab === 'settings' ? 'active' : ''}`} onClick={() => switchTab('settings')}>
          配置与规则
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
                    <h2>Init 耗时服务与错误诊断</h2>
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
                  <h3>
                    错误 / 异常日志 (
                    {inspectData.errors_total != null && inspectData.errors_total > inspectData.errors.length
                      ? `最近 ${inspectData.errors.length} / 共 ${inspectData.errors_total}`
                      : inspectData.errors.length}
                    )
                  </h3>
                  {inspectData.errors.length === 0 ? (
                    <div class="kv-list mono">
                      <div class="kv-row"><span class="k">无错误记录</span><span class="v">OK</span></div>
                    </div>
                  ) : (
                    <div class="log-scroll">
                      <div class="log-list mono">
                        {inspectData.errors.map((e, idx) => (
                          <div key={idx} class="log-row">
                            <span class="log-ts">{(e.ts_ms / 1000).toFixed(2)}s</span>
                            <span class="log-src">{e.src}</span>
                            <span class="log-msg">{e.text}</span>
                          </div>
                        ))}
                      </div>
                    </div>
                  )}
                </article>
              </section>
            </>
          )}
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
