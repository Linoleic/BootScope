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

export async function copyTextToClipboard(text: string): Promise<boolean> {
  if (typeof navigator !== 'undefined' && navigator.clipboard?.writeText) {
    try {
      await navigator.clipboard.writeText(text);
      return true;
    } catch {
      // Fallback below for non-HTTPS WebView contexts.
    }
  }
  if (typeof document === 'undefined' || !document.body) return false;
  const ta = document.createElement('textarea');
  ta.value = text;
  ta.setAttribute('readonly', '');
  ta.style.position = 'fixed';
  ta.style.left = '-9999px';
  ta.style.top = '0';
  ta.style.opacity = '0';
  document.body.appendChild(ta);
  ta.focus();
  ta.select();
  ta.setSelectionRange(0, text.length);
  let ok = false;
  try {
    ok = document.execCommand('copy');
  } catch {
    ok = false;
  } finally {
    document.body.removeChild(ta);
  }
  return ok;
}

export function nearestChartIndex(
  svg: SVGSVGElement,
  clientX: number,
  viewWidth: number,
  xs: number[],
): number {
  if (xs.length === 0) return -1;
  const rect = svg.getBoundingClientRect();
  if (rect.width <= 0) return 0;
  const svgX = ((clientX - rect.left) / rect.width) * viewWidth;
  let bestIdx = 0;
  let bestDist = Math.abs(xs[0] - svgX);
  for (let i = 1; i < xs.length; i++) {
    const d = Math.abs(xs[i] - svgX);
    if (d < bestDist) {
      bestDist = d;
      bestIdx = i;
    }
  }
  return bestIdx;
}

export function svgXFromPointer(
  svg: SVGSVGElement,
  clientX: number,
  viewWidth: number,
  minX: number,
  maxX: number,
): number {
  const rect = svg.getBoundingClientRect();
  if (rect.width <= 0) return minX;
  const raw = ((clientX - rect.left) / rect.width) * viewWidth;
  return Math.min(maxX, Math.max(minX, raw));
}

export interface MonotoneCubicCurve {
  path: string;
  areaPath: (baseY: number) => string;
  evalAt: (x: number) => number;
}

export function buildMonotoneCubic(
  xs: number[],
  vals: number[],
  toY: (v: number) => number,
  clampMin = 0,
  clampMax = 100,
): MonotoneCubicCurve {
  const clampV = (v: number) => Math.min(clampMax, Math.max(clampMin, v));
  const ptsX: number[] = [];
  const ptsV: number[] = [];
  const len = Math.min(xs.length, vals.length);
  for (let i = 0; i < len; i++) {
    const x = xs[i];
    const v = clampV(vals[i]);
    if (!Number.isFinite(x) || !Number.isFinite(v)) continue;
    if (ptsX.length === 0 || x > ptsX[ptsX.length - 1] + 1e-6) {
      ptsX.push(x);
      ptsV.push(v);
    }
  }

  const n = ptsX.length;
  if (n === 0) {
    return { path: '', areaPath: () => '', evalAt: () => clampMin };
  }
  if (n === 1) {
    const v0 = ptsV[0];
    return { path: '', areaPath: () => '', evalAt: () => v0 };
  }

  const h = new Array<number>(n - 1);
  const d = new Array<number>(n - 1);
  for (let i = 0; i < n - 1; i++) {
    h[i] = ptsX[i + 1] - ptsX[i];
    d[i] = (ptsV[i + 1] - ptsV[i]) / h[i];
  }

  const m = new Array<number>(n).fill(0);
  if (n === 2) {
    m[0] = d[0];
    m[1] = d[0];
  } else {
    for (let i = 1; i < n - 1; i++) {
      if (d[i - 1] * d[i] <= 0) {
        m[i] = 0;
      } else {
        const w1 = 2 * h[i] + h[i - 1];
        const w2 = h[i] + 2 * h[i - 1];
        m[i] = (w1 + w2) / (w1 / d[i - 1] + w2 / d[i]);
      }
    }

    let s0 = ((2 * h[0] + h[1]) * d[0] - h[0] * d[1]) / (h[0] + h[1]);
    if (s0 * d[0] <= 0) s0 = 0;
    else if (Math.abs(s0) > 3 * Math.abs(d[0])) s0 = 3 * d[0];
    m[0] = s0;

    const k = n - 2;
    let sn = ((2 * h[k] + h[k - 1]) * d[k] - h[k] * d[k - 1]) / (h[k] + h[k - 1]);
    if (sn * d[k] <= 0) sn = 0;
    else if (Math.abs(sn) > 3 * Math.abs(d[k])) sn = 3 * d[k];
    m[n - 1] = sn;

    for (let i = 0; i < n - 1; i++) {
      if (d[i] === 0) {
        m[i] = 0;
        m[i + 1] = 0;
      } else {
        const alpha = m[i] / d[i];
        const beta = m[i + 1] / d[i];
        const sq = alpha * alpha + beta * beta;
        if (sq > 9) {
          const tau = 3 / Math.sqrt(sq);
          m[i] = tau * alpha * d[i];
          m[i + 1] = tau * beta * d[i];
        }
      }
    }
  }

  let path = `M ${ptsX[0].toFixed(1)} ${toY(ptsV[0]).toFixed(1)}`;
  for (let i = 0; i < n - 1; i++) {
    const cp1x = ptsX[i] + h[i] / 3;
    const cp1y = toY(ptsV[i] + (m[i] * h[i]) / 3);
    const cp2x = ptsX[i + 1] - h[i] / 3;
    const cp2y = toY(ptsV[i + 1] - (m[i + 1] * h[i]) / 3);
    const x1 = ptsX[i + 1];
    const y1 = toY(ptsV[i + 1]);
    path += ` C ${cp1x.toFixed(1)} ${cp1y.toFixed(1)}, ${cp2x.toFixed(1)} ${cp2y.toFixed(1)}, ${x1.toFixed(1)} ${y1.toFixed(1)}`;
  }

  const areaPath = (baseY: number) =>
    `${path} L ${ptsX[n - 1].toFixed(1)} ${baseY.toFixed(1)} L ${ptsX[0].toFixed(1)} ${baseY.toFixed(1)} Z`;

  const evalAt = (x: number) => {
    if (x <= ptsX[0]) return ptsV[0];
    if (x >= ptsX[n - 1]) return ptsV[n - 1];
    let lo = 0;
    let hi = n - 2;
    while (lo < hi) {
      const mid = (lo + hi + 1) >> 1;
      if (ptsX[mid] <= x) lo = mid;
      else hi = mid - 1;
    }
    const segH = h[lo];
    if (segH <= 0) return ptsV[lo];
    const t = (x - ptsX[lo]) / segH;
    const t2 = t * t;
    const t3 = t2 * t;
    const h00 = 2 * t3 - 3 * t2 + 1;
    const h10 = t3 - 2 * t2 + t;
    const h01 = -2 * t3 + 3 * t2;
    const h11 = t3 - t2;
    return clampV(h00 * ptsV[lo] + h10 * segH * m[lo] + h01 * ptsV[lo + 1] + h11 * segH * m[lo + 1]);
  };

  return { path, areaPath, evalAt };
}
