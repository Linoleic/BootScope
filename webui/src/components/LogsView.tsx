import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { fetchLogs, fetchRules, saveRules } from '../bridge';
import { copyTextToClipboard } from '../format';
import { RunSelect } from './RunSelect';
import type { LogEntry, LogGroupKey, LogLevelKey, LogOrder, LogsResponse, RunSummary } from '../types';

interface Props {
  runs: RunSummary[];
  selectedId: string;
  onSelectRun: (bootId: string) => void;
  refreshSeq?: number;
  onStatus: (msg: string, isErr?: boolean) => void;
}

interface GroupMeta {
  key: LogGroupKey;
  title: string;
  badge: string;
}

type TimelineItem =
  | { type: 'stage'; ts_ms: number; src: string; text: string }
  | { type: 'entry'; entry: LogEntry };

const GROUPS: GroupMeta[] = [
  { key: 'merr', title: '模块自身异常 (M)', badge: 'lvl-merr' },
  { key: 'fatal', title: '致命 / 崩溃 / I/O 异常 (F)', badge: 'lvl-fatal' },
  { key: 'init_fail', title: 'Init 命令 / 服务失败', badge: 'lvl-init' },
  { key: 'error', title: '系统与 HAL 错误 (E)', badge: 'lvl-error' },
  { key: 'stage', title: '启动阶段节点 (STAGE)', badge: 'lvl-stage' },
  { key: 'info', title: '一般运行信息 (INFO)', badge: 'lvl-info' },
];

const DEFAULT_EXPANDED: Record<LogGroupKey, boolean> = {
  merr: true,
  fatal: true,
  init_fail: true,
  error: false,
  stage: false,
  info: false,
};

function groupBadgeClass(g: LogGroupKey): string {
  if (g === 'merr') return 'lvl-merr';
  if (g === 'fatal') return 'lvl-fatal';
  if (g === 'init_fail') return 'lvl-init';
  if (g === 'stage') return 'lvl-stage';
  if (g === 'info') return 'lvl-info';
  return 'lvl-error';
}

function timelineBadgeText(g: LogGroupKey): string {
  if (g === 'merr') return 'M';
  if (g === 'fatal') return 'F';
  if (g === 'init_fail') return 'INIT';
  if (g === 'stage') return 'STAGE';
  if (g === 'info') return 'INFO';
  return 'E';
}

export function LogsView({ runs, selectedId, onSelectRun, refreshSeq = 0, onStatus }: Props) {
  const [viewMode, setViewMode] = useState<LogOrder>('group');
  const [level, setLevel] = useState<LogLevelKey>('all');
  const [activeTag, setActiveTag] = useState<string>('');
  const [queryInput, setQueryInput] = useState<string>('');
  const [appliedQuery, setAppliedQuery] = useState<string>('');
  const [limit, setLimit] = useState<number>(200);
  const [data, setData] = useState<LogsResponse | null>(null);
  const [loading, setLoading] = useState<boolean>(false);
  const [err, setErr] = useState<string>('');
  const [ignoring, setIgnoring] = useState<boolean>(false);
  const [expanded, setExpanded] = useState<Record<LogGroupKey, boolean>>({ ...DEFAULT_EXPANDED });
  const [copiedKey, setCopiedKey] = useState<string>('');
  const copyTimerRef = useRef<number | null>(null);
  const reqSeqRef = useRef<number>(0);

  useEffect(() => {
    return () => {
      if (copyTimerRef.current != null) window.clearTimeout(copyTimerRef.current);
    };
  }, []);

  async function handleCopyRow(rowKey: string, e: LogEntry) {
    if (window.getSelection()?.toString()) return;
    const formatted = `[${(e.ts_ms / 1000).toFixed(2)}s] [${e.src}] ${e.text}`;
    const ok = await copyTextToClipboard(formatted);
    if (!ok) {
      onStatus('复制失败，请长按手动选择文本', true);
      return;
    }
    setCopiedKey(rowKey);
    if (copyTimerRef.current != null) window.clearTimeout(copyTimerRef.current);
    copyTimerRef.current = window.setTimeout(() => setCopiedKey(''), 750);
    const preview = e.text.length > 42 ? `${e.text.slice(0, 42)}…` : e.text;
    onStatus(`已复制日志: ${preview}`);
  }

  useEffect(() => {
    if (!selectedId) return;
    const seq = ++reqSeqRef.current;
    setLoading(true);
    setErr('');
    fetchLogs({
      bootId: selectedId,
      level,
      tag: activeTag,
      query: appliedQuery,
      limit,
      order: viewMode,
    })
      .then((res) => {
        if (seq !== reqSeqRef.current) return;
        setData(res);
      })
      .catch((e: Error) => {
        if (seq !== reqSeqRef.current) return;
        setErr(e.message);
      })
      .finally(() => {
        if (seq === reqSeqRef.current) setLoading(false);
      });
  }, [selectedId, level, activeTag, appliedQuery, limit, viewMode, refreshSeq]);

  const grouped = useMemo(() => {
    const map: Record<LogGroupKey, LogEntry[]> = {
      merr: [],
      fatal: [],
      init_fail: [],
      error: [],
      stage: [],
      info: [],
    };
    if (!data) return map;
    for (const item of data.entries) {
      if (map[item.group]) map[item.group].push(item);
    }
    return map;
  }, [data]);

  const timelineItems = useMemo<TimelineItem[]>(() => {
    if (viewMode !== 'time' || !data) return [];
    const sortedEntries = [...data.entries].sort((a, b) => a.ts_ms - b.ts_ms);
    const stages = [...(data.stages ?? [])].sort((a, b) => a.ts_ms - b.ts_ms);
    const out: TimelineItem[] = [];
    let stageIdx = 0;
    let lastEmittedIdx = -1;

    for (const entry of sortedEntries) {
      if (entry.group === 'stage') {
        while (stageIdx < stages.length && stages[stageIdx].ts_ms <= entry.ts_ms) {
          lastEmittedIdx = stageIdx;
          stageIdx++;
        }
        out.push({ type: 'stage', ts_ms: entry.ts_ms, src: entry.src, text: entry.text });
        continue;
      }

      let latestIdx = -1;
      while (stageIdx < stages.length && stages[stageIdx].ts_ms <= entry.ts_ms) {
        latestIdx = stageIdx;
        stageIdx++;
      }
      if (latestIdx >= 0 && latestIdx > lastEmittedIdx) {
        lastEmittedIdx = latestIdx;
        const st = stages[latestIdx];
        out.push({ type: 'stage', ts_ms: st.ts_ms, src: st.src, text: st.text });
      }
      out.push({ type: 'entry', entry });
    }
    return out;
  }, [viewMode, data]);

  const isFiltering = level !== 'all' || activeTag !== '' || appliedQuery !== '';
  const canIgnoreTag = activeTag !== '' && activeTag !== 'init:cmd' && activeTag !== 'init:svc';

  function selectLevel(next: LogLevelKey) {
    setLevel(next);
    setLimit(200);
    if (next === 'critical') {
      setExpanded((prev) => ({ ...prev, merr: true, fatal: true, init_fail: true }));
    } else if (next !== 'all') {
      setExpanded((prev) => ({ ...prev, [next]: true }));
    }
  }

  function toggleTag(tag: string) {
    const next = activeTag === tag ? '' : tag;
    setActiveTag(next);
    setLimit(200);
    if (next) {
      setExpanded({
        merr: true,
        fatal: true,
        init_fail: true,
        error: true,
        stage: true,
        info: true,
      });
    }
  }

  function handleSearchSubmit(e: Event) {
    e.preventDefault();
    const q = queryInput.trim();
    setAppliedQuery(q);
    setLimit(200);
    if (q) {
      setExpanded({
        merr: true,
        fatal: true,
        init_fail: true,
        error: true,
        stage: true,
        info: true,
      });
    }
  }

  function handleReset() {
    setLevel('all');
    setActiveTag('');
    setQueryInput('');
    setAppliedQuery('');
    setLimit(200);
    setExpanded({ ...DEFAULT_EXPANDED });
  }

  function toggleGroup(k: LogGroupKey) {
    setExpanded((prev) => ({ ...prev, [k]: !prev[k] }));
  }

  async function handleIgnoreActiveTag() {
    if (!canIgnoreTag || ignoring) return;
    setIgnoring(true);
    try {
      const rules = await fetchRules();
      const lines = rules.content.split(/\r?\n/);
      const targetLine = `ignore_tag=${activeTag}`;
      const targetLower = targetLine.toLowerCase();
      if (lines.some((l) => l.trim().toLowerCase() === targetLower)) {
        onStatus(`rules.conf 已包含 ${targetLine}`);
        return;
      }
      const base = rules.content.replace(/\s+$/, '');
      const nextContent = base ? `${base}\n${targetLine}\n` : `${targetLine}\n`;
      await saveRules(nextContent);
      onStatus(`已添加 ${targetLine} 到 rules.conf（下次开机生效）`);
    } catch (e) {
      onStatus(`添加忽略规则失败: ${(e as Error).message}`, true);
    } finally {
      setIgnoring(false);
    }
  }

  const counts = data?.counts;
  const criticalCount = counts ? (counts.merr + counts.fatal + counts.init_fail) : undefined;
  const levelTabs: Array<{ key: LogLevelKey; label: string; count?: number }> = [
    { key: 'all', label: '全部', count: counts?.total },
    { key: 'critical', label: '关键', count: criticalCount },
    { key: 'merr', label: '模块 M', count: counts?.merr },
    { key: 'fatal', label: '致命 F', count: counts?.fatal },
    { key: 'init_fail', label: 'Init 失败', count: counts?.init_fail },
    { key: 'error', label: '错误 E', count: counts?.error },
    { key: 'stage', label: '阶段', count: counts?.stage },
    { key: 'info', label: '信息', count: counts?.info },
  ];

  const visibleGroups = GROUPS.filter((g) => {
    const rows = grouped[g.key];
    const total = counts ? counts[g.key] : 0;
    if (isFiltering) return rows.length > 0;
    return total > 0 || rows.length > 0;
  });

  const timelinePill = data
    ? (data.entries.length < data.matched_total
        ? `显示 ${data.entries.length} / 共 ${data.matched_total}`
        : `${data.entries.length}`)
    : '0';

  return (
    <div class="tab-pane">
      <section class="card">
        <div class="card-head">
          <h2>启动日志分析</h2>
          <div class="log-head-actions">
            <div class="view-toggle" role="group" aria-label="视图模式">
              <button
                type="button"
                class={`view-toggle-btn ${viewMode === 'group' ? 'active' : ''}`}
                onClick={() => setViewMode('group')}
              >
                分组
              </button>
              <button
                type="button"
                class={`view-toggle-btn ${viewMode === 'time' ? 'active' : ''}`}
                onClick={() => setViewMode('time')}
              >
                时间线
              </button>
            </div>
            <span class="pill mono">{loading ? '加载中…' : `${counts?.total ?? 0} 条`}</span>
          </div>
        </div>

        <RunSelect
          runs={runs}
          value={selectedId}
          onChange={onSelectRun}
          ariaLabel="选择日志所属启动记录"
        />

        <form class="log-search-row" onSubmit={handleSearchSubmit}>
          <input
            type="search"
            class="input mono log-search-input"
            placeholder="按关键词 / Tag 搜索完整日志..."
            value={queryInput}
            onInput={(e) => setQueryInput((e.target as HTMLInputElement).value)}
          />
          <button type="submit" class="btn primary">搜索</button>
          {isFiltering && (
            <button type="button" class="btn ghost" onClick={handleReset}>重置</button>
          )}
        </form>

        <div class="level-chips mono" role="group" aria-label="日志级别筛选">
          {levelTabs.map((item) => (
            <button
              key={item.key}
              type="button"
              class={`level-chip ${level === item.key ? 'active' : ''}`}
              onClick={() => selectLevel(item.key)}
            >
              <span>{item.label}</span>
              {item.count != null && <span class="chip-cnt">{item.count}</span>}
            </button>
          ))}
        </div>

        {data && data.top_tags.length > 0 && (
          <div class="tag-section">
            <div class="tag-section-head">
              <span class="stat-k">高频错误 Tag</span>
              {canIgnoreTag && (
                <button
                  type="button"
                  class="btn danger-soft tag-ignore-btn mono"
                  disabled={ignoring}
                  onClick={handleIgnoreActiveTag}
                >
                  + 忽略 {activeTag}
                </button>
              )}
            </div>
            <div class="tag-chips mono">
              {data.top_tags.map((t) => (
                <button
                  key={t.tag}
                  type="button"
                  class={`tag-chip ${activeTag.toLowerCase() === t.tag.toLowerCase() ? 'active' : ''} ${t.fatal > 0 ? 'has-fatal' : ''}`}
                  onClick={() => toggleTag(t.tag)}
                >
                  <span>{t.tag}</span>
                  <span class="chip-cnt">{t.count}</span>
                </button>
              ))}
            </div>
          </div>
        )}

        {err && <p class="status-line err mono">{err}</p>}
      </section>

      {!data && loading && (
        <section class="card compare-placeholder">
          <div class="sub mono">正在解析启动日志…</div>
        </section>
      )}

      {viewMode === 'time' && data && data.entries.length === 0 && (
        <section class="card">
          <div class="sub mono">
            {isFiltering ? '当前筛选条件下无匹配日志。' : '本次启动无日志记录。'}
          </div>
        </section>
      )}

      {viewMode === 'time' && data && data.entries.length > 0 && (
        <section class="card">
          <div class="card-head">
            <h2>时间线混排</h2>
            <span class="pill mono">{timelinePill}</span>
          </div>
          <div class="log-scroll log-timeline-scroll">
            <div class="log-list mono">
              {timelineItems.map((it, idx) => {
                if (it.type === 'stage') {
                  return (
                    <div key={`st-${it.ts_ms}-${idx}`} class="timeline-stage-divider">
                      <span class="timeline-stage-ts">{(it.ts_ms / 1000).toFixed(2)}s</span>
                      <span class="timeline-stage-label">── {it.text} ──</span>
                    </div>
                  );
                }
                const e = it.entry;
                const rowKey = `tm-${e.ts_ms}-${idx}`;
                return (
                  <div
                    key={rowKey}
                    class={`log-row ${groupBadgeClass(e.group)} ${copiedKey === rowKey ? 'is-copied' : ''}`}
                    title="点击复制完整日志"
                    onClick={() => handleCopyRow(rowKey, e)}
                  >
                    <span class="log-ts">{(e.ts_ms / 1000).toFixed(2)}s</span>
                    <span class="log-badge-tag">{timelineBadgeText(e.group)}</span>
                    <span class="log-msg">{e.text}</span>
                  </div>
                );
              })}
            </div>
          </div>
          {data.matched_total > data.entries.length && limit < 2000 && (
            <div class="log-more-row">
              <button
                type="button"
                class="btn ghost log-more-btn"
                disabled={loading}
                onClick={() => setLimit((n) => Math.min(2000, n + 400))}
              >
                加载更多（当前上限 {limit}）
              </button>
            </div>
          )}
        </section>
      )}

      {viewMode === 'group' && data && visibleGroups.length === 0 && (
        <section class="card">
          <div class="sub mono">
            {isFiltering ? '当前筛选条件下无匹配日志。' : '本次启动无日志记录。'}
          </div>
        </section>
      )}

      {viewMode === 'group' && data && visibleGroups.map((g) => {
        const rows = grouped[g.key];
        const groupTotal = isFiltering ? rows.length : (counts?.[g.key] ?? rows.length);
        const showTotalPill = !isFiltering && rows.length < groupTotal
          ? `显示 ${rows.length} / 共 ${groupTotal}`
          : `${rows.length}`;
        const isOpen = expanded[g.key];
        const canLoadMore = !isFiltering && rows.length < groupTotal && limit < 2000;
        const canLoadMoreFiltered = isFiltering && data.matched_total > data.entries.length && limit < 2000;

        return (
          <section key={g.key} class={`card log-group-card ${g.badge}`}>
            <button
              type="button"
              class="log-group-head"
              aria-expanded={isOpen}
              onClick={() => toggleGroup(g.key)}
            >
              <span class="log-group-title">
                <span class={`log-group-dot ${g.badge}`} />
                <span>{g.title}</span>
              </span>
              <span class="log-group-right">
                <span class="pill mono">{showTotalPill}</span>
                <svg
                  class={`log-group-chevron ${isOpen ? 'is-open' : ''}`}
                  width="14"
                  height="14"
                  viewBox="0 0 16 16"
                  aria-hidden="true"
                >
                  <path
                    d="M4 6l4 4 4-4"
                    fill="none"
                    stroke="currentColor"
                    stroke-width="1.8"
                    stroke-linecap="round"
                    stroke-linejoin="round"
                  />
                </svg>
              </span>
            </button>

            {isOpen && (
              <div class="log-group-body">
                <div class="log-scroll">
                  <div class="log-list mono">
                    {rows.map((e, idx) => {
                      const rowKey = `${g.key}-${e.ts_ms}-${idx}`;
                      return (
                        <div
                          key={rowKey}
                          class={`log-row ${g.badge} ${copiedKey === rowKey ? 'is-copied' : ''}`}
                          title="点击复制完整日志"
                          onClick={() => handleCopyRow(rowKey, e)}
                        >
                          <span class="log-ts">{(e.ts_ms / 1000).toFixed(2)}s</span>
                          <span class="log-src">{e.src}</span>
                          <span class="log-msg">{e.text}</span>
                        </div>
                      );
                    })}
                  </div>
                </div>
                {(canLoadMore || canLoadMoreFiltered) && (
                  <div class="log-more-row">
                    <button
                      type="button"
                      class="btn ghost log-more-btn"
                      disabled={loading}
                      onClick={() => setLimit((n) => Math.min(2000, n + 400))}
                    >
                      加载更多（当前上限 {limit}）
                    </button>
                  </div>
                )}
              </div>
            )}
          </section>
        );
      })}
    </div>
  );
}
