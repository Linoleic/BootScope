import { useEffect, useState } from 'preact/hooks';
import { fetchRules, pruneRuns, saveRules, updateConfig } from '../bridge';
import type { ConfigState } from '../types';

interface Props {
  config: ConfigState;
  diskKb: number;
  totalRuns: number;
  onConfigUpdated: (cfg: ConfigState) => void;
  onPruned: () => void;
  onStatus: (msg: string, isErr?: boolean) => void;
}

export function SettingsView(props: Props) {
  const [maxHistory, setMaxHistory] = useState<string>(String(props.config.max_history));
  const [rulesSource, setRulesSource] = useState<string>('--');
  const [rulesText, setRulesText] = useState<string>('');
  const [saving, setSaving] = useState<boolean>(false);

  useEffect(() => {
    setMaxHistory(String(props.config.max_history));
  }, [props.config.max_history]);

  useEffect(() => {
    fetchRules()
      .then((res) => {
        setRulesSource(res.source);
        setRulesText(res.content);
      })
      .catch((e: Error) => props.onStatus(`读取 rules.conf 失败: ${e.message}`, true));
  }, []);

  async function toggleDisplay(checked: boolean) {
    try {
      const next = await updateConfig('display_enabled', checked ? '1' : '0');
      props.onConfigUpdated(next);
      props.onStatus(checked ? '已开启下次开机实时屏幕显示' : '已关闭下次开机实时屏幕显示');
    } catch (e) {
      props.onStatus(`切换失败: ${(e as Error).message}`, true);
    }
  }

  async function saveRetention() {
    try {
      const next = await updateConfig('max_history', maxHistory);
      props.onConfigUpdated(next);
      props.onStatus(`已保存历史保留上限: ${next.max_history} 次`);
    } catch (e) {
      props.onStatus(`保存失败: ${(e as Error).message}`, true);
    }
  }

  async function runPruneNow() {
    try {
      const keep = Number(maxHistory) || 10;
      const res = await pruneRuns(keep);
      props.onStatus(`清理完成：保留 ${res.kept} 条，删除 ${res.deleted} 条`);
      props.onPruned();
    } catch (e) {
      props.onStatus(`清理失败: ${(e as Error).message}`, true);
    }
  }

  async function handleSaveRules(reset: boolean) {
    setSaving(true);
    try {
      const res = await saveRules(reset ? '__RESET__' : rulesText);
      setRulesSource(res.source);
      setRulesText(res.content);
      props.onStatus(reset ? '已恢复内置默认规则' : `已保存 rules.conf (${res.source})`);
    } catch (e) {
      props.onStatus(`规则保存失败: ${(e as Error).message}`, true);
    } finally {
      setSaving(false);
    }
  }

  return (
    <div style="display:flex;flex-direction:column;gap:12px">
      <section class="card">
        <div class="card-head">
          <h2>开机探针与存储管理</h2>
          <span class="pill mono">{props.config.ksu_early_installed ? 'KSU LATE-FS READY' : 'LATE START'}</span>
        </div>

        <label class="switch-row">
          <div>
            <div style="font-size:13px;font-weight:600">开机屏幕实时显示</div>
            <div class="sub">开启后在开机画面叠加关键路径与实时资源指标</div>
          </div>
          <input
            type="checkbox"
            checked={props.config.display_enabled}
            onChange={(e) => toggleDisplay((e.target as HTMLInputElement).checked)}
          />
        </label>

        <div class="inline-row" style="justify-content:space-between">
          <div class="inline-row">
            <span class="sub">自动保留最近</span>
            <input
              type="number"
              class="input mono"
              style="width:76px"
              min="1"
              max="200"
              value={maxHistory}
              onInput={(e) => setMaxHistory((e.target as HTMLInputElement).value)}
            />
            <span class="sub">次启动（当前 {props.totalRuns} 条 · {(props.diskKb / 1024).toFixed(2)} MB）</span>
          </div>
          <div class="inline-row">
            <button type="button" class="btn" onClick={saveRetention}>保存上限</button>
            <button type="button" class="btn danger-soft" onClick={runPruneNow}>立即清理</button>
          </div>
        </div>
      </section>

      <section class="card">
        <div class="card-head">
          <h2>过滤与脱敏规则 (`rules.conf`)</h2>
          <span class="pill mono">SOURCE: {rulesSource}</span>
        </div>
        <textarea
          class="textarea mono"
          value={rulesText}
          onInput={(e) => setRulesText((e.target as HTMLTextAreaElement).value)}
        />
        <div class="inline-row" style="margin-top:10px;justify-content:flex-end">
          <button type="button" class="btn danger-soft" disabled={saving} onClick={() => handleSaveRules(true)}>
            恢复默认规则
          </button>
          <button type="button" class="btn primary" disabled={saving} onClick={() => handleSaveRules(false)}>
            保存规则
          </button>
        </div>
      </section>
    </div>
  );
}
