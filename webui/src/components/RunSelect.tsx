import { useEffect, useMemo, useRef, useState } from 'preact/hooks';
import { fmtEpochShort, fmtMs } from '../format';
import type { RunSummary } from '../types';

interface Props {
  runs: RunSummary[];
  value: string;
  onChange: (bootId: string) => void;
  ariaLabel: string;
}

interface MenuPos {
  left: number;
  width: number;
  maxHeight: number;
  placement: 'bottom' | 'top';
  top?: number;
  bottom?: number;
}

function resolveDoneMs(r: RunSummary): number {
  if (r.boot_done_ms >= 0) return r.boot_done_ms;
  if (r.finished_uptime_s > 0) return r.finished_uptime_s * 1000;
  return -1;
}

export function RunSelect({ runs, value, onChange, ariaLabel }: Props) {
  const [open, setOpen] = useState(false);
  const [pos, setPos] = useState<MenuPos | null>(null);
  const triggerRef = useRef<HTMLButtonElement>(null);
  const menuRef = useRef<HTMLDivElement>(null);

  const selectedIdx = useMemo(
    () => runs.findIndex((r) => r.boot_id === value),
    [runs, value],
  );
  const selected = selectedIdx >= 0 ? runs[selectedIdx] : runs[0];

  function toggleMenu() {
    if (open) {
      setOpen(false);
      return;
    }
    const el = triggerRef.current;
    if (!el) return;
    const rect = el.getBoundingClientRect();
    const vw = window.innerWidth;
    const vh = window.innerHeight;
    const width = Math.min(Math.max(rect.width, 200), vw - 16);
    const left = Math.max(8, Math.min(rect.left, vw - width - 8));
    const spaceBelow = vh - rect.bottom - 10;
    const spaceAbove = rect.top - 10;
    if (spaceBelow < 180 && spaceAbove > spaceBelow) {
      setPos({
        left,
        width,
        maxHeight: Math.min(320, Math.max(120, spaceAbove)),
        placement: 'top',
        bottom: vh - rect.top + 6,
      });
    } else {
      setPos({
        left,
        width,
        maxHeight: Math.min(320, Math.max(120, spaceBelow)),
        placement: 'bottom',
        top: rect.bottom + 6,
      });
    }
    setOpen(true);
  }

  useEffect(() => {
    if (!open) return;

    const menu = menuRef.current;
    if (menu) {
      const activeEl = menu.querySelector('.run-select-opt.active') as HTMLElement | null;
      if (activeEl) {
        const targetTop = activeEl.offsetTop - Math.max(0, (menu.clientHeight - activeEl.offsetHeight) / 2);
        menu.scrollTop = Math.max(0, targetTop);
      }
    }

    const onKeyDown = (e: KeyboardEvent) => {
      if (e.key === 'Escape') {
        setOpen(false);
        triggerRef.current?.focus();
      }
    };
    const onViewportChange = () => setOpen(false);

    window.addEventListener('keydown', onKeyDown);
    window.addEventListener('resize', onViewportChange);
    window.addEventListener('scroll', onViewportChange, false);
    return () => {
      window.removeEventListener('keydown', onKeyDown);
      window.removeEventListener('resize', onViewportChange);
      window.removeEventListener('scroll', onViewportChange, false);
    };
  }, [open]);

  if (!selected) {
    return (
      <button type="button" class="run-select-trigger mono" disabled aria-label={ariaLabel}>
        <span class="run-select-sub">暂无启动记录</span>
      </button>
    );
  }

  const selTime = fmtEpochShort(selected.mtime_epoch);
  const selDone = resolveDoneMs(selected);
  const selSummary = [
    selTime,
    selected.bl_source !== 'NONE' ? `BL ${fmtMs(selected.bl_total_ms)}` : '',
    selDone >= 0 ? `完成 ${fmtMs(selDone)}` : '',
  ].filter(Boolean).join(' · ');

  const menuStyle = pos
    ? `left:${pos.left}px;width:${pos.width}px;max-height:${pos.maxHeight}px;${
        pos.placement === 'top' && pos.bottom != null
          ? `bottom:${pos.bottom}px;`
          : `top:${pos.top ?? 0}px;`
      }`
    : '';

  return (
    <>
      <button
        ref={triggerRef}
        type="button"
        class={`run-select-trigger mono ${open ? 'is-open' : ''}`}
        role="combobox"
        aria-haspopup="listbox"
        aria-expanded={open}
        aria-label={ariaLabel}
        onClick={toggleMenu}
      >
        <span class="run-select-val">
          {selectedIdx === 0 && <span class="run-select-badge">最新</span>}
          <span class="run-select-id">{selected.boot_id.slice(0, 8)}</span>
          {selSummary && <span class="run-select-sub">{selSummary}</span>}
        </span>
        <svg class="run-select-arrow" width="14" height="14" viewBox="0 0 16 16" aria-hidden="true">
          <path
            d="M4 6l4 4 4-4"
            fill="none"
            stroke="currentColor"
            stroke-width="1.8"
            stroke-linecap="round"
            stroke-linejoin="round"
          />
        </svg>
      </button>

      {open && pos && (
        <>
          <div class="run-select-backdrop" onClick={() => setOpen(false)} />
          <div
            ref={menuRef}
            class={`run-select-menu mono ${pos.placement === 'top' ? 'from-top' : ''}`}
            role="listbox"
            aria-label={ariaLabel}
            style={menuStyle}
          >
            {runs.map((r, idx) => {
              const active = r.boot_id === value;
              const timeStr = fmtEpochShort(r.mtime_epoch);
              const modeStr = r.mode === 'early_native' ? 'early' : (r.mode || 'probe');
              const doneMs = resolveDoneMs(r);
              const metaStr = [
                modeStr,
                r.bl_source !== 'NONE' ? `BL ${fmtMs(r.bl_total_ms)}` : '',
                doneMs >= 0 ? `完成 ${fmtMs(doneMs)}` : '',
                r.error_count > 0 ? `${r.error_count} ERR` : '',
              ].filter(Boolean).join(' · ');

              return (
                <button
                  key={r.boot_id}
                  type="button"
                  role="option"
                  aria-selected={active}
                  class={`run-select-opt ${active ? 'active' : ''}`}
                  onClick={() => {
                    setOpen(false);
                    if (r.boot_id !== value) onChange(r.boot_id);
                  }}
                >
                  <div class="run-opt-top">
                    <span class="run-opt-left">
                      {idx === 0 && <span class="run-select-badge">最新</span>}
                      <span>{r.boot_id.slice(0, 8)}</span>
                    </span>
                    <span class="run-opt-right">
                      {timeStr && <span class="run-opt-time">{timeStr}</span>}
                      {active && (
                        <svg class="run-opt-check" width="13" height="13" viewBox="0 0 16 16" aria-hidden="true">
                          <path
                            d="M3.5 8.5l3 3 6-6"
                            fill="none"
                            stroke="currentColor"
                            stroke-width="2"
                            stroke-linecap="round"
                            stroke-linejoin="round"
                          />
                        </svg>
                      )}
                    </span>
                  </div>
                  <div class="run-opt-meta">{metaStr}</div>
                </button>
              );
            })}
          </div>
        </>
      )}
    </>
  );
}
