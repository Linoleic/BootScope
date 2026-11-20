# Changelog

## v0.5.0

- **Unified Native C++17 Core (`bootscope`)**: Consolidated early-boot HUD rendering, sysfs/proc/property/logcat collection, multi-platform bootloader parsing, screen replay, and WebUI JSON APIs into a single static-libstdc++ `arm64-v8a` binary.
- **Early-Boot SurfaceFlinger HUD**: Launches on `late-fs` via KernelSU `initrc` (`bootscope_early`) at ~2–4s uptime with adaptive landscape/portrait rendering, ~3 FPS low-overhead live updates, self-overhead telemetry (`self_cpu_ms`, `self_rss_kb`), and automatic `late_native` fallback when early frames are unavailable.
- **Multi-Platform Bootloader Profiling**: Parses `ro.boot.boottime*`, Qualcomm `UefiLog0.txt` + kernel `boot_stats` KPI timestamps, and MediaTek `/proc/bootprof` into aligned power-on timeline stages.
- **KernelSU WebUI (Preact + TypeScript + Vite)**:
  - **Run Overview**: Full-boot Gantt timeline (including Bootloader stages), kernel/system milestone cards, and `top_cpu` process bars.
  - **Smooth PSI Curves & Continuous Ruler**: Merges `early-trace.tsv` `METRIC` samples with `psi.tsv` (up to 120 samples) and renders Fritsch-Carlson monotone cubic Bezier curves with a continuous pixel-scrubbing crosshair.
  - **Multi-Run Boot Trend**: Tracks historical boot durations across runs with an interactive scrubbing ruler and quick-jump inspection.
  - **Dedicated Boot Logs Tab**: Fault-first grouped summary view (`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`), interleaved chronological `时间线` view with phase dividers, and one-tap clipboard copy.
  - **A/B Boot Compare & Settings**: Side-by-side run comparison including `Bootloader 分段耗时差异 (B - A)`, live toggle for display HUD, on-screen `--replay`, log retention cleanup, and online `rules.conf` editor.
- **Privacy & Safety**: Local in-place key/value redaction (`redact_key` masks sensitive values as `***` while preserving surrounding diagnostic context), automatic error stack root-cause preservation (`Caused by:`), and conditional rebootless KernelSU hot install.
