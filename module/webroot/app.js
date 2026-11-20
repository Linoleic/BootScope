const BOOT_ID_RE = /^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$/i;
const VALID_SPEEDS = new Set(['0.5', '1', '2', '4']);
const SEG_COLORS = ['#6ee7c8', '#68b0ff', '#f7c568', '#c792ea', '#ff8c7b', '#8bd3dd'];

const el = (id) => document.getElementById(id);

let callbackSeq = 0;
let selectedSpeed = '2';

function injectKsuInsets() {
  if (typeof window !== 'undefined' && window.ksu) {
    const link = document.createElement('link');
    link.rel = 'stylesheet';
    link.href = '/internal/insets.css';
    document.head.appendChild(link);
  }
}

function ksuExec(command, timeoutMs = 15000) {
  if (typeof window === 'undefined' || !window.ksu || typeof window.ksu.exec !== 'function') {
    return Promise.reject(new Error('bridge_unavailable'));
  }
  const cbName = `__bootscopeKsu_${Date.now().toString(36)}_${callbackSeq++}`;
  return new Promise((resolve, reject) => {
    let done = false;
    const timer = setTimeout(() => {
      if (done) return;
      done = true;
      delete window[cbName];
      reject(new Error('exec_timeout'));
    }, timeoutMs);

    window[cbName] = (errno, stdout, stderr) => {
      if (done) return;
      done = true;
      clearTimeout(timer);
      delete window[cbName];
      resolve({ errno, stdout: String(stdout || ''), stderr: String(stderr || '') });
    };

    try {
      window.ksu.exec(command, JSON.stringify({}), cbName);
    } catch (err) {
      if (!done) {
        done = true;
        clearTimeout(timer);
        delete window[cbName];
        reject(err);
      }
    }
  });
}

function esc(text) {
  return String(text ?? '')
    .replaceAll('&', '&amp;')
    .replaceAll('<', '&lt;')
    .replaceAll('>', '&gt;')
    .replaceAll('"', '&quot;')
    .replaceAll("'", '&#39;');
}

function fmtMs(ms) {
  if (ms == null || !Number.isFinite(ms) || ms < 0) return '--';
  if (ms >= 1000) return `${(ms / 1000).toFixed(2)} s`;
  return `${Math.round(ms)} ms`;
}

function parseKvLines(text) {
  const map = {};
  for (const raw of text.split(/\r?\n/)) {
    const idx = raw.indexOf('=');
    if (idx > 0) {
      map[raw.slice(0, idx).trim()] = raw.slice(idx + 1).trim();
    }
  }
  return map;
}

function splitSections(raw) {
  const sections = {};
  let current = null;
  for (const line of raw.split(/\r?\n/)) {
    const m = /^===([A-Z0-9_]+)===$/.exec(line);
    if (m) {
      current = m[1];
      sections[current] = [];
    } else if (current) {
      sections[current].push(line);
    }
  }
  const joined = {};
  for (const [k, lines] of Object.entries(sections)) {
    joined[k] = lines.join('\n');
  }
  return joined;
}

function parseBootTimeProps(btStr, btExtraStr) {
  const kv = {};
  for (const src of [btStr, btExtraStr]) {
    if (!src) continue;
    for (const part of src.split(',')) {
      const [k, v] = part.split(':').map((x) => x.trim());
      if (k && v && /^\d+$/.test(v)) kv[k.toUpperCase()] = Number(v);
    }
  }
  if (!Object.keys(kv).length) return null;

  const stages = [];
  let cur = 0;
  const add = (id, dur, detail, waitOnly = false) => {
    if (!dur || dur <= 0) return;
    stages.push({ id, start_ms: cur, dur_ms: dur, detail, wait_only: waitOnly });
    if (!waitOnly) cur += dur;
  };

  if ('0BLE' in kv || '2BLE' in kv || '3BLE' in kv) {
    add('BL0', kv['0BLE'] || 0, 'BOOT ROM');
    add('BL1', (kv['1BLL'] || 0) + (kv['1BLE'] || 0), `LOAD:${kv['1BLL'] || 0} EXEC:${kv['1BLE'] || 0}`);
    add('BL2', (kv['2BLL'] || 0) + (kv['2BLE'] || 0), `DDR:${kv['DRAM_TRAIN'] || 0}ms`);
    add('BL3', (kv['3BLL'] || 0) + (kv['3BLE'] || 0), `DISP:${kv['DISP'] || 0}ms`);
    add('AVB', kv.AVB || 0, 'VERIFIED BOOT');
    add('KL', (kv.KL || 0) + (kv.KD || 0), `BOOT:${kv.KL_BOOT || 0} VDRB:${kv.KL_VDRB || 0}`);
    add('ODT', kv.ODT || 0, 'DT OVERLAY');
    add('FWL', kv.FWL || 0, `PKD:${kv.PKD || 0}`);
    if (kv.SW > 0) add('SW', kv.SW, 'UNLOCK WARNING WAIT', true);
  } else {
    for (const [k, v] of Object.entries(kv)) {
      add(k, v, k, k === 'SW');
    }
  }
  return { source: 'PROP', total_ms: cur, wait_ms: kv.SW || 0, meta: '', stages };
}

function parseUefiText(text) {
  if (!text.trim()) return null;
  let apblUs = -1, sblStart = -1, sblEnd = -1, sblDelta = -1, ddrMhz = -1;
  let uefiStart = -1, uefiTotal = -1, logoMs = -1, ebsMs = -1;
  let bootMs = -1, vbootMs = -1, ovlMs = -1, dtMs = -1;
  let reason = '', slot = '', bootDev = '';

  for (const raw of text.split(/\r?\n/)) {
    const s = raw.trim();
    let m;
    if ((m = /^APBL Exit:\s*(\d+)/.exec(s))) apblUs = Number(m[1]);
    else if ((m = /^B\s*-\s*(\d+)\s*-\s*(.+)$/.exec(s))) {
      const us = Number(m[1]);
      if (m[2].includes('SBL1, Start')) sblStart = us;
      else if (m[2].includes('SBL1, End')) sblEnd = us;
      else if ((m = /Max enabled DDR Freq = (\d+)/.exec(s))) ddrMhz = Number(m[1]);
      else if ((m = /PM: Reset by (.+)$/.exec(s))) reason = m[1].trim();
    } else if ((m = /^D\s*-\s*(\d+)\s*-\s*SBL1, Delta/.exec(s))) sblDelta = Number(m[1]);
    else if ((m = /^S - DDR Frequency,\s*(\d+)/.exec(s)) && ddrMhz < 0) ddrMhz = Number(m[1]);
    else if ((m = /^UEFI Start\s*\[\s*(\d+)\]/.exec(s))) uefiStart = Number(m[1]);
    else if ((m = /^UEFI Total\s*:\s*(\d+)/.exec(s))) uefiTotal = Number(m[1]);
    else if ((m = /DeCompressLogoData:.*use time:\s*(\d+)/.exec(s))) logoMs = Number(m[1]);
    else if ((m = /^Booting from slot \(([^)]+)\)/.exec(s))) slot = m[1].toUpperCase();
    else if ((m = /^Boot Device\s*:\s*(\S+)/.exec(s))) bootDev = m[1].toUpperCase();
    else if ((m = /^Load Image boot(?:_[ab])?\s+total time:\s*(\d+)/.exec(s))) bootMs = Number(m[1]);
    else if ((m = /^Load Image vendor_boot(?:_[ab])?\s+total time:\s*(\d+)/.exec(s))) vbootMs = Number(m[1]);
    else if ((m = /^Apply Overlay total time:\s*(\d+)/.exec(s))) ovlMs = Number(m[1]);
    else if ((m = /^Update Device Tree total time:\s*(\d+)/.exec(s))) dtMs = Number(m[1]);
    else if ((m = /^(?:Shutting Down UEFI Boot Services:|Start EBS\s*\[)\s*(\d+)/.exec(s))) ebsMs = Number(m[1]);
  }
  if (ebsMs <= 0 && sblDelta <= 0 && uefiTotal <= 0) return null;

  const stages = [];
  if (apblUs > 0) stages.push({ id: 'PBL', start_ms: 0, dur_ms: Math.round(apblUs / 1000), detail: 'APBL MEDIA+AUTH', wait_only: false });
  if (sblStart >= 0 && (sblDelta > 0 || sblEnd > sblStart)) {
    const dur = sblDelta > 0 ? Math.round(sblDelta / 1000) : Math.round((sblEnd - sblStart) / 1000);
    stages.push({ id: 'SBL1', start_ms: Math.round(sblStart / 1000), dur_ms: dur, detail: ddrMhz > 0 ? `DDR ${ddrMhz}MHz` : 'XBL/DDR', wait_only: false });
  }
  if (uefiStart >= 0 && uefiTotal > 0) {
    stages.push({ id: 'UEFI', start_ms: uefiStart, dur_ms: uefiTotal, detail: logoMs >= 0 ? `LOGO:${logoMs}ms` : 'DXE INIT', wait_only: false });
  }
  const ablStart = (uefiStart >= 0 && uefiTotal > 0) ? (uefiStart + uefiTotal) : Math.max(0, Math.round(sblEnd / 1000));
  if (ebsMs > ablStart) {
    const det = [
      bootMs >= 0 ? `KL:${bootMs}` : '',
      vbootMs >= 0 ? `VDR:${vbootMs}` : '',
      ovlMs >= 0 ? `OVL:${ovlMs}` : '',
      dtMs >= 0 ? `DT:${dtMs}` : '',
    ].filter(Boolean).join(' ') || 'ABL LOAD';
    stages.push({ id: 'ABL', start_ms: ablStart, dur_ms: ebsMs - ablStart, detail: det, wait_only: false });
  }
  const meta = [reason && `POR:${reason}`, bootDev, ddrMhz > 0 && `DDR:${ddrMhz}M`, slot && `SLOT:${slot}`].filter(Boolean).join(' ');
  const total = ebsMs > 0 ? ebsMs : stages.reduce((acc, s) => acc + s.dur_ms, 0);
  return { source: 'UEFILOG', total_ms: total, wait_ms: 0, meta, stages };
}

function parseQcomKpiText(text) {
  const raw = {};
  for (const line of text.split(/\r?\n/)) {
    const [k, v] = line.split('\t');
    if (k && v && /^\d+$/.test(v.trim())) raw[k.trim()] = Number(v.trim());
  }
  const freq = raw.mpm_freq || 0;
  if (!freq || raw.bl_start == null || raw.bl_end == null || raw.bl_end < raw.bl_start) return null;
  const preBl = Math.round((raw.bl_start * 1000) / freq);
  const ablMs = Math.round(((raw.bl_end - raw.bl_start) * 1000) / freq);
  const stages = [];
  if (preBl > 0) stages.push({ id: 'PRE_BL', start_ms: 0, dur_ms: preBl, detail: 'PBL+SBL+UEFI', wait_only: false });
  const klRaw = raw.bl_load_kernel;
  let klMs = -1;
  let ablPreMs = -1;
  if (klRaw != null) {
    if (klRaw >= raw.bl_start && klRaw <= raw.bl_end) {
      ablPreMs = Math.round(((klRaw - raw.bl_start) * 1000) / freq);
      klMs = Math.round(((raw.bl_end - klRaw) * 1000) / freq);
    } else if (klRaw > 0 && klRaw <= (raw.bl_end - raw.bl_start)) {
      klMs = Math.round((klRaw * 1000) / freq);
      ablPreMs = Math.max(0, ablMs - klMs);
    }
  }
  if (klMs >= 0) {
    stages.push({ id: 'ABL', start_ms: preBl, dur_ms: ablPreMs, detail: 'ABL INIT+DT', wait_only: false });
    stages.push({ id: 'KL', start_ms: preBl + ablPreMs, dur_ms: klMs, detail: 'KERNEL LOAD', wait_only: false });
  } else {
    stages.push({ id: 'ABL', start_ms: preBl, dur_ms: ablMs, detail: 'ABL BOOTLOADER', wait_only: false });
  }
  const total = preBl > 0 ? Math.round((raw.bl_end * 1000) / freq) : ablMs;
  const handoff = raw.kernel_mpm > raw.bl_end ? Math.round(((raw.kernel_mpm - raw.bl_end) * 1000) / freq) : -1;
  return {
    source: 'QCOM_KPI',
    total_ms: total,
    wait_ms: 0,
    meta: `MPM:${freq}Hz` + (handoff >= 0 ? ` HANDOFF:${handoff}ms` : ''),
    stages,
  };
}

function parseBootprofText(text) {
  if (!text.trim()) return null;
  let pl = -1;
  let lk = -1;
  for (const raw of text.split(/\r?\n/)) {
    const s = raw.trim();
    let m;
    if ((m = /PL Boot Time\s*:\s*(\d+)/.exec(s))) pl = Number(m[1]);
    else if ((m = /LK Boot Time\s*:\s*(\d+)/.exec(s))) lk = Number(m[1]);
    else if (pl < 0 && /preloader/i.test(s) && (m = /^(\d+)\b/.exec(s))) pl = Number(m[1]);
    else if (lk < 0 && /(?:lk boot|little kernel)/i.test(s) && (m = /^(\d+)\b/.exec(s))) lk = Number(m[1]);
  }
  if (pl < 0 && lk < 0) return null;
  const stages = [];
  let cur = 0;
  if (pl > 0) {
    stages.push({ id: 'PL', start_ms: cur, dur_ms: pl, detail: 'MTK PRELOADER', wait_only: false });
    cur += pl;
  }
  if (lk > 0) {
    stages.push({ id: 'LK', start_ms: cur, dur_ms: lk, detail: 'MTK LITTLE KERNEL', wait_only: false });
    cur += lk;
  }
  return { source: 'BOOTPROF', total_ms: cur, wait_ms: 0, meta: 'MediaTek Bootprof', stages };
}

function parseTraceBl(text) {
  let profile = null;
  let blTotal = -1;
  const phases = {};
  for (const line of text.split(/\r?\n/)) {
    const cols = line.split('\t');
    if (cols.length < 3) continue;
    const type = cols[1];
    if (type === 'BL' && cols.length >= 4) {
      blTotal = Number(cols[2]);
    } else if (type === 'BLMETA' && cols.length >= 5) {
      profile = {
        source: cols[2] || 'TRACE',
        total_ms: Number(cols[3]) >= 0 ? Number(cols[3]) : blTotal,
        wait_ms: Number(cols[4]) || 0,
        meta: cols[5] || '',
        stages: [],
      };
    } else if (type === 'BLSTG' && profile && cols.length >= 7) {
      profile.stages.push({
        id: cols[2],
        start_ms: Number(cols[3]) || 0,
        dur_ms: Number(cols[4]) || 0,
        wait_only: cols[5] === '1',
        detail: cols[6] || '',
      });
    } else if (type === 'PHASE' && cols.length >= 7) {
      const idx = Number(cols[2]);
      phases[idx] = {
        idx,
        state: Number(cols[3]),
        start_ms: Number(cols[4]),
        end_ms: Number(cols[5]),
        detail: cols[6],
      };
    }
  }
  if (profile && profile.stages.length && (profile.total_ms < 0)) {
    profile.total_ms = profile.stages.filter((s) => !s.wait_only).reduce((a, b) => a + b.dur_ms, 0);
  }
  return { profile: profile && profile.stages.length ? profile : null, phases };
}

function setStatus(msg, isError = false) {
  const s = el('actionStatus');
  s.textContent = msg;
  s.style.color = isError ? 'var(--danger)' : 'var(--accent)';
}

function renderBlProfile(profile) {
  const badge = el('blSourceBadge');
  const metaEl = el('blMetaLine');
  const barEl = el('blBar');
  const legendEl = el('blLegend');
  const wrapEl = el('blTableWrap');

  if (!profile || !profile.stages.length) {
    badge.textContent = 'N/A';
    metaEl.textContent = '此次记录未包含细分 Bootloader 阶段数据';
    barEl.innerHTML = '';
    legendEl.innerHTML = '';
    wrapEl.innerHTML = '';
    return;
  }

  badge.textContent = profile.source;
  metaEl.textContent = profile.meta || `总硬件阶段时长 ${fmtMs(profile.total_ms)}`;
  const activeStages = profile.stages.filter((s) => !s.wait_only && s.dur_ms > 0);
  const sumDur = Math.max(1, activeStages.reduce((a, b) => a + b.dur_ms, 0));

  barEl.innerHTML = activeStages.map((s, i) => {
    const pct = ((s.dur_ms / sumDur) * 100).toFixed(2);
    const col = SEG_COLORS[i % SEG_COLORS.length];
    return `<div class="bl-seg" style="width:${pct}%;background:${col}" title="${esc(s.id)}: ${s.dur_ms}ms"></div>`;
  }).join('');

  legendEl.innerHTML = activeStages.map((s, i) => {
    const col = SEG_COLORS[i % SEG_COLORS.length];
    return `<span class="bl-legend-item"><span class="dot" style="background:${col}"></span>${esc(s.id)} ${s.dur_ms}ms</span>`;
  }).join('');

  const rows = profile.stages.map((s) => `
    <tr>
      <td class="mono">${esc(s.id)}${s.wait_only ? ' (等待)' : ''}</td>
      <td class="mono">${s.start_ms} ms</td>
      <td class="mono">${s.dur_ms} ms</td>
      <td class="mono">${esc(s.detail)}</td>
    </tr>
  `).join('');
  wrapEl.innerHTML = `
    <table>
      <thead><tr><th>阶段</th><th>POR 起点</th><th>耗时</th><th>详情</th></tr></thead>
      <tbody>${rows}</tbody>
    </table>`;
}

function renderKvRows(containerId, items) {
  const node = el(containerId);
  if (!items.length) {
    node.innerHTML = '<div class="kv-row"><span class="k">无数据</span><span class="v">-</span></div>';
    return;
  }
  node.innerHTML = items.map((item) => `
    <div class="kv-row ${item.err ? 'err' : ''}">
      <span class="k" title="${esc(item.k)}">${esc(item.k)}</span>
      <span class="v">${esc(item.v)}</span>
    </div>
  `).join('');
}

async function refreshBootstrap() {
  setStatus('正在扫描设备记录...');
  const cmd = [
    'MODEL=$(getprop ro.product.model 2>/dev/null);',
    'SDK=$(getprop ro.build.version.sdk 2>/dev/null);',
    'DIS=0;',
    '[ -e /data/adb/bootscope/disable_display ] || [ -e /metadata/vold/ksu/bootscope_disable_display ] && DIS=1;',
    'echo "DEV" >/dev/null;',
    'printf "DEV\\t%s\\t%s\\t%s\\n" "$MODEL" "$SDK" "$DIS";',
    'COUNT=0;',
    'for d in $(ls -1dt /data/adb/bootscope/*/ 2>/dev/null); do',
    '  d=${d%/}; bid=${d##*/};',
    '  case "$bid" in ????????-????-????-????-????????????) ;; *) continue ;; esac;',
    '  COUNT=$((COUNT+1)); [ "$COUNT" -le 30 ] || continue;',
    '  UP=$(grep -m1 "^finished_uptime=" "$d/metadata.txt" 2>/dev/null | cut -d= -f2);',
    '  MODE=$(grep -m1 "^mode=" "$d/display-status.txt" 2>/dev/null | cut -d= -f2);',
    '  HAS_UEFI=0; [ -s "$d/bootloader-uefi.txt" ] && HAS_UEFI=1;',
    '  printf "RUN\\t%s\\t%s\\t%s\\t%s\\n" "$bid" "$UP" "$MODE" "$HAS_UEFI";',
    'done;',
    'printf "TOTAL\\t%s\\n" "$COUNT"',
  ].join('\n');

  const res = await ksuExec(cmd);
  if (res.errno !== 0) {
    setStatus(`读取设备状态失败 (rc=${res.errno})`, true);
    return;
  }

  let totalRuns = 0;
  const runs = [];
  for (const line of res.stdout.split(/\r?\n/)) {
    const p = line.split('\t');
    if (p[0] === 'DEV') {
      el('deviceBadge').textContent = `${p[1] || 'Android'} · API ${p[2] || '?'}`;
      el('bootDisplayToggle').checked = p[3] !== '1';
    } else if (p[0] === 'RUN' && BOOT_ID_RE.test(p[1] || '')) {
      runs.push({ id: p[1], uptime: p[2] || '', mode: p[3] || '', uefi: p[4] === '1' });
    } else if (p[0] === 'TOTAL') {
      totalRuns = Number(p[1]) || runs.length;
    }
  }

  el('runStatsBadge').textContent = `${totalRuns} 条记录`;
  const select = el('runSelect');
  const prev = select.value;
  select.innerHTML = '';
  runs.forEach((r, idx) => {
    const opt = document.createElement('option');
    opt.value = r.id;
    const tag = idx === 0 ? '[最新] ' : '';
    const extra = [r.mode || 'probe', r.uptime && `${r.uptime}s`, r.uefi && 'UEFI'].filter(Boolean).join(' · ');
    opt.textContent = `${tag}${r.id.slice(0, 13)}… (${extra})`;
    select.appendChild(opt);
  });

  if (!runs.length) {
    setStatus('尚未在 /data/adb/bootscope 发现启动记录', true);
    return;
  }
  select.value = runs.some((r) => r.id === prev) ? prev : runs[0].id;
  await loadSelectedRun();
}

async function loadSelectedRun() {
  const bootId = el('runSelect').value;
  if (!BOOT_ID_RE.test(bootId)) return;
  setStatus(`正在加载 ${bootId.slice(0, 8)}…`);
  const d = `/data/adb/bootscope/${bootId}`;

  const cmd = [
    `D="${d}";`,
    'echo "===META==="; cat "$D/metadata.txt" 2>/dev/null;',
    'echo "===DSTAT==="; cat "$D/display-status.txt" 2>/dev/null;',
    'echo "===DREND==="; cat "$D/display-renderer.txt" 2>/dev/null;',
    'echo "===KPI==="; cat "$D/bootloader-kpi.tsv" 2>/dev/null;',
    'echo "===UEFI==="; grep -E "APBL Exit:|SBL1, Start|SBL1, End|SBL1, Delta|DDR Frequency|Max enabled DDR|Reset by|Boot Device|Booting from slot|UEFI Start|UEFI Total|DeCompressLogoData|Load Image |Apply Overlay|Update Device Tree|Shutting Down UEFI|Start EBS" "$D/bootloader-uefi.txt" 2>/dev/null;',
    'echo "===BOOTPROF==="; head -n 60 "$D/bootloader-bootprof.txt" 2>/dev/null;',
    'echo "===PROPS==="; grep -m 4 -E "^\\[ro\\.boot\\.boottime(_extra)?\\]:" "$D/samples.txt" 2>/dev/null;',
    'echo "===TRACE_BL==="; awk -F "\\t" \'$2~/^(BL|BLMETA|BLSTG|PHASE)$/\' "$D/early-trace.tsv" 2>/dev/null | head -n 80;',
    'echo "===TRACE_ERR==="; awk -F "\\t" \'$2=="TAIL" && ($4=="ERR" || $4=="MERR")\' "$D/early-trace.tsv" 2>/dev/null | tail -n 20;',
    'echo "===BEV==="; cat "$D/boot-events.tsv" 2>/dev/null;',
    'echo "===INIT==="; awk -F "\\t" \'$2=="exit" && $4~/took=/ { tk=$4; sub(/^.*took=/,"",tk); sub(/s.*$/,"",tk); printf "%.3f\\t%s\\t%s\\n", tk+0, $1, $3 }\' "$D/init-events.tsv" 2>/dev/null | sort -rn | head -n 10;',
  ].join('\n');

  const res = await ksuExec(cmd);
  if (res.errno !== 0) {
    setStatus(`加载记录失败 (rc=${res.errno})`, true);
    return;
  }

  const sec = splitSections(res.stdout);
  const meta = parseKvLines(sec.META || '');
  const dstat = parseKvLines(sec.DSTAT || '');
  const drend = parseKvLines(sec.DREND || '');

  const ffMs = drend.first_frame_submitted_ms ? Number(drend.first_frame_submitted_ms) : -1;
  const relMs = drend.released_ms ? Number(drend.released_ms) : -1;
  el('statSf').textContent = fmtMs(ffMs);
  el('statSfSub').textContent = `mode=${dstat.mode || '-'}  rel=${fmtMs(relMs)}`;

  const bevRows = (sec.BEV || '').split(/\r?\n/).filter(Boolean).map((line) => {
    const [tag, ms] = line.split('\t');
    return { k: tag, v: `${ms} ms`, ms: Number(ms) };
  });
  const animDone = bevRows.find((r) => r.k === 'wm_boot_animation_done') || bevRows[bevRows.length - 1];
  const finSec = meta.finished_uptime ? Number(meta.finished_uptime) : -1;
  el('statDone').textContent = animDone ? fmtMs(animDone.ms) : (finSec > 0 ? `${finSec.toFixed(2)} s` : '--');
  el('statDoneSub').textContent = `SDK ${meta.sdk || '?'} · boot_completed=${meta.boot_completed || '?'}`;

  const traceParsed = parseTraceBl(sec.TRACE_BL || '');
  let propsBt = '';
  let propsBtExtra = '';
  for (const line of (sec.PROPS || '').split(/\r?\n/)) {
    const m = /^\[([^\]]+)\]:\s*\[([^\]]*)\]/.exec(line);
    if (m) {
      if (m[1] === 'ro.boot.boottime') propsBt = m[2];
      else if (m[1] === 'ro.boot.boottime_extra') propsBtExtra = m[2];
    }
  }
  const blProfile = traceParsed.profile
    || parseUefiText(sec.UEFI || '')
    || parseBootTimeProps(propsBt, propsBtExtra)
    || parseBootprofText(sec.BOOTPROF || '')
    || parseQcomKpiText(sec.KPI || '');

  el('statBl').textContent = blProfile ? fmtMs(blProfile.total_ms) : '--';
  el('statBlSub').textContent = blProfile
    ? (`src=${blProfile.source}` + (blProfile.wait_ms > 0 ? ` · wait=${fmtMs(blProfile.wait_ms)}` : ''))
    : '无 BL 数据';

  renderBlProfile(blProfile);

  const phaseNames = ['BL', 'INIT', 'ZYGOTE', 'PRELOAD', 'PMS', 'AMS'];
  const phaseItems = [];
  for (let i = 0; i < 6; i++) {
    const p = traceParsed.phases[i];
    if (p) {
      const dur = (p.start_ms >= 0 && p.end_ms >= p.start_ms) ? fmtMs(p.end_ms - p.start_ms) : (p.state === 2 ? 'DONE' : 'RUN');
      phaseItems.push({ k: `${phaseNames[i]} · ${p.detail || '-'}`, v: dur });
    }
  }
  renderKvRows('phaseList', phaseItems);
  renderKvRows('bootEventsList', bevRows.map((r) => ({ k: r.k, v: r.v })));

  const initItems = (sec.INIT || '').split(/\r?\n/).filter(Boolean).map((line) => {
    const [took, ts, name] = line.split('\t');
    return { k: `${name} (@${Number(ts).toFixed(2)}s)`, v: `${Number(took).toFixed(3)} s` };
  });
  renderKvRows('initTopList', initItems);

  const diagItems = [];
  if (drend.layout) diagItems.push({ k: 'layout', v: drend.layout });
  if (drend.exit) diagItems.push({ k: 'renderer exit', v: drend.exit });
  for (const line of (sec.TRACE_ERR || '').split(/\r?\n/).filter(Boolean)) {
    const cols = line.split('\t');
    diagItems.push({ k: `${cols[2] || 'E'} ${cols[4] || ''}`, v: `${(Number(cols[0]) / 1000).toFixed(2)}s`, err: true });
  }
  renderKvRows('diagList', diagItems);
  setStatus(`已加载 ${bootId.slice(0, 8)} (${meta.android_version ? `Android ${meta.android_version}` : 'ready'})`);
}

async function toggleBootDisplay(enabled) {
  const cmd = enabled
    ? 'rm -f /data/adb/bootscope/disable_display /metadata/vold/ksu/bootscope_disable_display; echo ok'
    : 'mkdir -p /data/adb/bootscope; : > /data/adb/bootscope/disable_display; [ -d /metadata/vold/ksu ] && : > /metadata/vold/ksu/bootscope_disable_display; echo ok';
  const res = await ksuExec(cmd);
  if (res.errno === 0) {
    setStatus(enabled ? '已开启下次开机屏幕显示' : '已关闭下次开机屏幕显示');
  } else {
    setStatus(`切换失败 (rc=${res.errno})`, true);
  }
}

async function launchReplay() {
  const bootId = el('runSelect').value;
  if (!BOOT_ID_RE.test(bootId)) {
    setStatus('无效的记录 ID', true);
    return;
  }
  if (!VALID_SPEEDS.has(selectedSpeed)) selectedSpeed = '1';
  const flags = [];
  if (el('optBl').checked) flags.push('--bl');
  if (el('optPreSf').checked) flags.push('--pre-sf');
  flags.push('--speed', selectedSpeed);

  const cmd = [
    'BIN=/data/adb/modules/bootscope/bootscope_early;',
    '[ -x "$BIN" ] || { echo NO_BIN; exit 1; };',
    'if ps -ef 2>/dev/null | grep -q "[b]ootscope_early --replay"; then echo ALREADY_RUNNING; exit 0; fi;',
    `nohup "$BIN" --replay "/data/adb/bootscope/${bootId}" ${flags.join(' ')} </dev/null >/dev/null 2>&1 &`,
    'echo STARTED',
  ].join('\n');

  el('replayBtn').disabled = true;
  try {
    const res = await ksuExec(cmd);
    if (res.stdout.includes('STARTED')) {
      setStatus(`已启动屏幕回放 (${flags.join(' ')}) — 请切换到系统桌面观看`);
    } else if (res.stdout.includes('ALREADY_RUNNING')) {
      setStatus('已有回放正在运行中，请稍候完成', true);
    } else {
      setStatus(`启动回放失败: ${res.stdout || res.stderr}`, true);
    }
  } finally {
    el('replayBtn').disabled = false;
  }
}

async function pruneOldRuns() {
  const cmd = [
    'N=0;',
    'for d in $(ls -1dt /data/adb/bootscope/*/ 2>/dev/null); do',
    '  d=${d%/}; bid=${d##*/};',
    '  case "$bid" in ????????-????-????-????-????????????) ;; *) continue ;; esac;',
    '  N=$((N+1));',
    '  if [ "$N" -gt 10 ]; then rm -rf "$d"; fi;',
    'done;',
    'echo "PRUNED $N"',
  ].join('\n');
  const res = await ksuExec(cmd);
  if (res.errno === 0) {
    setStatus('已清理旧记录，最多保留最近 10 次启动');
    await refreshBootstrap();
  } else {
    setStatus(`清理失败 (rc=${res.errno})`, true);
  }
}

function init() {
  injectKsuInsets();

  el('refreshBtn').addEventListener('click', () => refreshBootstrap().catch((e) => setStatus(e.message, true)));
  el('runSelect').addEventListener('change', () => loadSelectedRun().catch((e) => setStatus(e.message, true)));
  el('bootDisplayToggle').addEventListener('change', (e) => toggleBootDisplay(e.target.checked).catch((err) => setStatus(err.message, true)));
  el('replayBtn').addEventListener('click', () => launchReplay().catch((e) => setStatus(e.message, true)));
  el('pruneBtn').addEventListener('click', () => pruneOldRuns().catch((e) => setStatus(e.message, true)));

  document.querySelectorAll('.speed-btn').forEach((btn) => {
    btn.addEventListener('click', () => {
      const sp = btn.dataset.speed;
      if (!VALID_SPEEDS.has(sp)) return;
      selectedSpeed = sp;
      document.querySelectorAll('.speed-btn').forEach((b) => b.classList.toggle('active', b === btn));
    });
  });

  if (typeof window === 'undefined' || !window.ksu || typeof window.ksu.exec !== 'function') {
    el('bridgeBanner').classList.remove('hidden');
    el('deviceBadge').textContent = 'WebUI 预览模式（无 Root 桥接）';
    return;
  }
  refreshBootstrap().catch((e) => setStatus(`初始化异常: ${e.message}`, true));
}

init();
