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
  const [listData, setListData] = useState<ListResponse | null>(null);
  const [selectedId, setSelectedId] = useState<string>('');
  const [inspectData, setInspectData] = useState<InspectResponse | null>(null);
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
          未检测到 KernelSU `window.ksu` 桥接，当前处于桌面模拟数据预览模式。
        </div>
      )}

      <nav class="tabs">
        <button type="button" class={`tab-btn ${tab === 'overview' ? 'active' : ''}`} onClick={() => setTab('overview')}>
          总览与时间轴
        </button>
        <button type="button" class={`tab-btn ${tab === 'compare' ? 'active' : ''}`} onClick={() => setTab('compare')}>
          A/B 对比
        </button>
        <button type="button" class={`tab-btn ${tab === 'settings' ? 'active' : ''}`} onClick={() => setTab('settings')}>
          配置与规则
        </button>
      </nav>

      {statusMsg && <p class={`status-line mono ${statusErr ? 'err' : ''}`}>{statusMsg}</p>}

      {tab === 'overview' && listData && (
        <>
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
                  <div class="stat-k">硬件 BL 耗时</div>
                  <div class="stat-v mono">{fmtMs(inspectData.bootloader.total_ms)}</div>
                  <div class="stat-s mono">
                    src={inspectData.bootloader.source}
                    {inspectData.bootloader.wait_ms > 0 ? ` · wait=${fmtMs(inspectData.bootloader.wait_ms)}` : ''}
                  </div>
                </article>
                <article class="stat-card">
                  <div class="stat-k">浮层首帧时刻</div>
                  <div class="stat-v mono">{fmtMs(inspectData.display.first_frame_ms)}</div>
                  <div class="stat-s mono">
                    mode={inspectData.display.mode} · rel={fmtMs(inspectData.display.released_ms)}
                  </div>
                </article>
                <article class="stat-card">
                  <div class="stat-k">系统启动完成</div>
                  <div class="stat-v mono">{fmtMs(bootDone)}</div>
                  <div class="stat-s mono">
                    boot_completed={inspectData.metadata.boot_completed || '?'} · {inspectData.metadata.bootreason || '-'}
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
                  <div class="kv-list mono">
                    {inspectData.phases.map((p) => (
                      <div key={p.id} class="kv-row">
                        <span class="k">{p.id} · {p.detail}</span>
                        <span class="v">{p.dur_ms >= 0 ? fmtMs(p.dur_ms) : p.state.toUpperCase()}</span>
                      </div>
                    ))}
                  </div>
                  <h3>Framework 事件</h3>
                  <div class="kv-list mono">
                    {inspectData.boot_events.map((b) => (
                      <div key={b.tag} class="kv-row">
                        <span class="k">{b.tag}</span>
                        <span class="v">{b.ms} ms</span>
                      </div>
                    ))}
                  </div>
                </article>

                <article class="card">
                  <div class="card-head">
                    <h2>Init 耗时服务与错误诊断</h2>
                  </div>
                  <div class="kv-list mono">
                    {inspectData.top_init_services.slice(0, 10).map((s) => (
                      <div key={`${s.name}-${s.ts_ms}`} class="kv-row">
                        <span class="k">{s.name} (@{(s.ts_ms / 1000).toFixed(2)}s)</span>
                        <span class="v">{fmtMs(s.dur_ms)}</span>
                      </div>
                    ))}
                  </div>
                  <h3>错误 / 异常日志 ({inspectData.errors.length})</h3>
                  <div class="kv-list mono">
                    {inspectData.errors.length === 0 ? (
                      <div class="kv-row"><span class="k">无错误记录</span><span class="v">OK</span></div>
                    ) : (
                      inspectData.errors.map((e, idx) => (
                        <div key={idx} class="kv-row err">
                          <span class="k">{e.src} {e.text}</span>
                          <span class="v">{(e.ts_ms / 1000).toFixed(2)}s</span>
                        </div>
                      ))
                    )}
                  </div>
                </article>
              </section>
            </>
          )}
        </>
      )}

      {tab === 'compare' && listData && <CompareView runs={listData.runs} />}

      {tab === 'settings' && listData && (
        <SettingsView
          config={listData.config}
          diskKb={listData.disk_kb}
          totalRuns={listData.total_runs}
          onConfigUpdated={handleConfigUpdated}
          onPruned={() => reloadList()}
          onStatus={setStatus}
        />
      )}
    </main>
  );
}
