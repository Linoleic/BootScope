export function fmtMs(ms: number | null | undefined): string {
  if (ms == null || !Number.isFinite(ms) || ms < 0) return '--';
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)}s`;
  return `${Math.round(ms)}ms`;
}

export function fmtDelta(deltaMs: number, hasDelta: boolean): { text: string; cls: string } {
  if (!hasDelta) return { text: 'N/A', cls: '' };
  const sign = deltaMs > 0 ? '+' : '';
  const text = `${sign}${deltaMs}ms`;
  const cls = deltaMs <= -5 ? 'delta-good' : deltaMs >= 5 ? 'delta-bad' : '';
  return { text, cls };
}

export function shortRunLabel(bootId: string, idx: number, extra: string[]): string {
  const tag = idx === 0 ? '[最新] ' : '';
  const meta = extra.filter(Boolean).join(' · ');
  return `${tag}${bootId.slice(0, 8)}${meta ? ` · ${meta}` : ''}`;
}

export function fmtEpochShort(epochSec: number): string {
  if (!epochSec || !Number.isFinite(epochSec) || epochSec <= 0) return '';
  const d = new Date(epochSec * 1000);
  if (Number.isNaN(d.getTime())) return '';
  const pad = (n: number) => String(n).padStart(2, '0');
  return `${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
}
