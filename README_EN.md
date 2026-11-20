# BootScope

[中文](README.md) | **English**

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

BootScope is an **early-boot on-screen HUD overlay, full-chain boot profiler, and KernelSU WebUI dashboard** for modern `arm64-v8a` Android devices. It is built around a **single native C++17 binary (`bootscope`) + a TypeScript/Preact KernelSU WebUI + a Python PC offline analysis suite**.

> Developer Docs: **[Architecture Guide (English)](docs/ARCHITECTURE_EN.md)** | **[架构设计说明 (中文)](docs/ARCHITECTURE.md)**

---

## Key Features

- **Early-Boot Native SurfaceFlinger HUD (`bootscope early`)**
  - Starts on KernelSU `on late-fs` (`initrc/bootscope.rc`) around **2–4s uptime** with low overhead (~3 FPS live updates), rendering critical boot phases, `*:E` error highlights, core daemon status, and live CPU/IO/MEM load while recording self-overhead telemetry (`self_cpu_ms`, `self_rss_kb`) and `early-trace.tsv`.
  - Adapts automatically to portrait and landscape screens, and falls back to `late_native` via `service.sh` (`display.sh`) when early `initrc` execution is unavailable.
- **Zero-Fork Native Collector (`bootscope collect`)**
  - Reads properties via `__system_property_foreach`, kernel PSI (`/proc/pressure/*`), per-process CPU/IO (`/proc/[pid]/stat`), `/dev/kmsg`, and system events with automatic run rotation (`max_history`).
- **Multi-Platform Bootloader Profiling**
  - Parses Qualcomm `UefiLog0.txt` + kernel `boot_stats` KPI timestamps, `ro.boot.boottime*` properties, and MediaTek `/proc/bootprof` aligned to the power-on timeline.
- **Interactive KernelSU WebUI**
  - **Overview & Gantt Timeline**: Power-on to boot-completed Gantt breakdown across bootloader and Android init/framework stages, milestones, and `top_cpu` processes.
  - **Smooth PSI Pressure Curve & Continuous Scrubber**: Merges dense early `early-trace.tsv` `METRIC` samples with tail `psi.tsv` data (up to 120 points) and fits Fritsch-Carlson monotone cubic Bezier curves with a continuous pixel-level pointer scrubbing ruler.
  - **Multi-Run Boot Trend**: Visualizes historical boot durations with a scrubbing ruler and one-tap navigation into any run.
  - **Dedicated Boot Logs Tab**: Fault-first grouped inspection (`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`) and interleaved chronological timeline view with phase dividers, preserving Java `Caused by:` stack causes and one-tap copy.
  - **A/B Comparison & Settings**: Side-by-side run comparison including `Bootloader stage deltas (B - A)`, live HUD toggle, on-screen `--replay` launch, history cleanup, and online `/data/adb/bootscope/rules.conf` editing.
- **Privacy & Safe Updates**
  - In-place `redact_key` masking replaces sensitive values (`imei`, `android_id`, `token=`, `password`, etc.) with `***` without discarding diagnostic error context.
  - Supports KernelSU rebootless hot installation when `KSU_HAS_HOT_INSTALL=true` and early `initrc` entry is already active.

---

## Requirements & Compatibility

| Item | Requirement / Status |
| --- | --- |
| Architecture | `arm64-v8a` |
| Android Version | Android 13+ (API 33+) |
| Root Solution | **KernelSU / SukiSU-Ultra** (full early `initrc`, WebUI, and hot-install support); late fallback hooks retained for other root managers |
| Verified Devices | Xiaomi (Qualcomm, Android 16, SukiSU-Ultra); Google Pixel 7 Pro (Tensor, Android 17, KernelSU) |

---

## Installation & Usage

1. Download `bootscope-<version>.zip` from [Releases](https://github.com/Linoleic/BootScope/releases) (or build from source).
2. Flash the ZIP in your KernelSU manager. Reboot once after initial installation to activate `initrc/bootscope.rc`; subsequent upgrades can use rebootless hot install.
3. **Open WebUI**: After booting, open **BootScope → WebUI** in your KernelSU manager to inspect timelines, smooth PSI curves, logs, and A/B comparisons.
4. **On-Screen Replay (`action.sh`)**: Tap **Action** in the module card to trigger a `--bl --pre-sf` screen replay of the latest run.

### Safe Disable & Customization

- **Disable on-screen HUD overlay** (collection and WebUI continue to work):
  ```sh
  mkdir -p /data/adb/bootscope
  touch /data/adb/bootscope/disable_display
  ```
  Or toggle it inside the WebUI **Settings** tab. Remove the file to re-enable.
- **Custom Rules**: Persist custom `rules.conf` overrides in `/data/adb/bootscope/rules.conf` (or edit directly from WebUI Settings); preserved across module upgrades.
- **Uninstall**: Removing the module via the manager runs `uninstall.sh` to clean up `/metadata/vold/ksu/bootscope*` and runtime data.

---

## Building from Source

Prerequisites: **Android NDK (API 33+, via `ANDROID_HOME` or `ANDROID_NDK_HOME`)**, **Node.js 18+ / npm**, and **Python 3.10+**.

```sh
python build_module.py
```

The build script compiles `core/*.cpp` with C++17 (`-O2 -Wall -Wextra -static-libstdc++`), runs `tsc --noEmit && vite build` in `webui/` (`--skip-webui` reuses existing webroot artifacts), and packages `dist/bootscope-<version>.zip` with LF line endings and Unix file modes.

---

## PC Offline Analysis & Unit Tests

- [analyze_probe.py](analyze_probe.py): Offline CLI analyzer and HTML A/B comparator for exported `samples.txt` / `psi.tsv` / `proc.tsv`:
  ```sh
  python analyze_probe.py results/bootA/samples.txt --compare results/bootB/samples.txt --html results/report.html
  ```
- [bootscope.py](bootscope.py) / [replay.py](replay.py): Host-side ADB init property monitor and sample replay utilities.
- Run the unit test suite:
  ```sh
  python -m unittest discover -s tests -v
  ```

---

## License & Disclaimer

- Licensed under the **[GNU General Public License v3.0](LICENSE)**.
- BootScope interacts with early Android boot stages, `SurfaceFlinger`, and root privileges. Review compatibility for your device and maintain backups before flashing.
