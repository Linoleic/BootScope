# BootScope

[中文](README.md) | **English**

[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-blue.svg)](https://developer.android.com)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

An early-boot on-screen HUD, full-chain boot profiler, and KernelSU WebUI dashboard for modern `arm64-v8a` Android devices. It renders critical stages, error highlights, and live resource pressure during boot, then archives structured traces for post-boot inspection and run-to-run comparison.

BootScope aims to give rooted users and developers immediate visibility into slow boots, bootloader variance, stalled daemons, or module errors without requiring USB debugging or manual log stitching across subsystems.

Nothing in `/system` or `/vendor` is modified, and stock `bootanimation` remains untouched. Sampling and rendering stay self-contained inside the module's native binary and private data directories, and the visual overlay can be toggled off or cleanly uninstalled at any time.

## Features

- Launches an early native `SurfaceFlinger` HUD overlay during `late-fs`, showing boot phases, fault summaries, core service states, and CPU/IO/MEM pressure.
- Parses bootloader timings from Qualcomm UEFI/KPI logs, `ro.boot.boottime*` properties, and MediaTek `/proc/bootprof`, aligned to the system timeline.
- Merges dense early-trace metrics with tail `psi.tsv` samples and renders smooth Fritsch-Carlson monotone cubic PSI curves with a continuous pixel-scrubbing ruler.
- Tracks multi-run boot duration trends and provides a dedicated Boot Logs tab with severity-grouped and stage-interleaved chronological inspection views.
- Performs A/B stage and bootloader delta comparisons (`B - A`), and triggers full-timeline on-screen replays (`--bl --pre-sf`) directly on the device.
- Supports toggling the HUD overlay, pruning archived runs, and editing persistent `rules.conf` filtering/highlight/redaction rules inside WebUI.

## Requirements

- `arm64-v8a` device running **Android 13 or later (API 33+)**.
- Root access; **KernelSU / SukiSU-Ultra** provides full capabilities (early `initrc`, WebUI manager integration, and rebootless hot installs). Fallback paths are retained for late-start collection on other root setups.
- Verified on Xiaomi (Qualcomm, Android 16, SukiSU-Ultra) and Google Pixel 7 Pro (Tensor, Android 17, KernelSU); bootloader log granularity varies across SoCs and OEMs.

## Installation & Usage

1. Download the module ZIP from [Releases](https://github.com/Linoleic/BootScope/releases) (or build from source via [BUILD_EN.md](BUILD_EN.md)).
2. Flash the ZIP in your KernelSU manager. Reboot once after initial installation to activate `initrc/bootscope.rc`; subsequent updates support rebootless hot installs when supported by the manager.
3. During reboot, the early HUD overlay appears above `bootanimation` and exits automatically once boot finishes.
4. Open **BootScope → WebUI** in your KernelSU manager to examine the Gantt timeline, smooth PSI curves, boot logs, and A/B comparisons.
5. To review the boot animation overlay again after booting, tap **Action** on the module card to run an on-screen `--bl --pre-sf` replay.

```text
Initial install + reboot -> Early native HUD & full-boot sampling
Post-boot WebUI          -> Gantt timeline / smooth PSI / grouped & timeline logs / A/B diff
```

## How It Works

### Non-Intrusive Early Rendering & Symbol Fallback

At `on late-fs`, KernelSU `initrc` starts the single static-libstdc++ `bootscope early` binary. Rather than linking against version-locked AOSP headers, it resolves `libbinder.so`, `libutils.so`, and `libgui.so` via `dlopen` with multi-signature symbol fallbacks across Android 13–17.

Live HUD frames refresh at ~3 FPS (`333 ms`) while recording self-overhead (`self_cpu_ms`, `self_rss_kb`). If early rendering could not start before `late_start` while `bootanimation` is still running, `display.sh` attempts a `late_native` fallback automatically.

### Zero-Fork Native Collection

`bootscope collect` reads `__system_property_foreach`, `/proc/pressure/*`, `/proc/[pid]/stat`, and kernel/event logs directly inside one native process, avoiding repeated shell/sub-process forks that would skew boot timing measurements.

### In-Place Redaction That Preserves Context

Instead of dropping entire fault lines containing sensitive substrings (`imei`, `android_id`, `token=`, `password`, etc.), `redact_key` rules replace sensitive values in-place with `***`, and Java `Caused by:` root-cause lines are retained in `early-trace.tsv` for post-boot inspection.

### Comparison with Alternatives

| Approach | Primary Maintenance Cost | BootScope's Choice |
| --- | --- | --- |
| Host `adb logcat` / `dmesg` | Requires pre-configured USB/Wi-Fi ADB; misses pre-ADB stages | Autonomous on-device capture from `late-fs` onward |
| `bootchart` / Perfetto traces | Manual setup and PC-side trace viewers required | Zero-config background recording with phone WebUI analysis |
| Post-boot shell scripts | Starts too late for early `late-fs` services and faults | Native C++ binary injected early (~2–4s uptime) via KernelSU `initrc` |
| BootScope | Root / KernelSU setup and optional `rules.conf` tuning | Self-contained probe, early HUD, and mobile WebUI |

Each route trades setup requirements for visibility. BootScope's early overlay relies on `SurfaceFlinger` private ABI compatibility and may fall back to background-only profiling on heavily customized OEM frameworks.

## WebUI

| Tab | Purpose |
| --- | --- |
| Overview | Boot summary, bootloader stage breakdown, Gantt timeline, smooth PSI curves, and historical trend |
| Boot Logs | Severity-grouped (`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`) or stage-interleaved chronological view with one-tap copy |
| Compare | Side-by-side milestone deltas and `Bootloader stage deltas (B - A)` across two runs |
| Settings | Toggle HUD overlay, configure on-screen `--replay`, prune archived runs, and edit `rules.conf` |

## Updates & Recovery

**Updating the Module:** Flash the new ZIP in your KernelSU manager or check updates via `updateJson`. When KernelSU hot install is supported and `bootscope early` is already active in `modules.rc`, updates apply without requiring an immediate reboot.

**Disabling the On-Screen Overlay:** Toggle the switch inside WebUI **Settings**, or create the flag file manually (data collection and WebUI remain active):

```sh
mkdir -p /data/adb/bootscope
touch /data/adb/bootscope/disable_display
```

Remove the file or toggle the switch back on to restore display.

**Uninstalling:** Removing the module in your manager runs `uninstall.sh` on reboot to clean up `/metadata/vold/ksu/bootscope*` and `/data/adb/bootscope/`.

## Storage Locations

| Item | Path | Notes |
| --- | --- | --- |
| **Per-Boot Run Archive** | `/data/adb/bootscope/<boot_id>/` | Stores `early-trace.tsv`, `samples.txt`, `psi.tsv`, `proc.tsv`, bootloader dumps, and display diagnostics (`0700`) |
| **Custom Rules Override** | `/data/adb/bootscope/rules.conf` | Persistent filter, highlight, and redaction rules preserved across module upgrades |
| **Display Disable Flag** | `/data/adb/bootscope/disable_display` | Presence suppresses the on-screen HUD without halting collection or WebUI |
| **Early Runtime Sync** | `/metadata/vold/ksu/bootscope*` | Native binary, rules copy, and display flag read by `bootscope_early` at `late-fs` |
| **Volatile Early Staging** | `/dev/bootscope_early*` | Temporary early trace and renderer state files archived into the run directory at `late_start` |

## FAQ

**1. Why didn't the HUD appear during boot?**

Ensure the device uses KernelSU / SukiSU-Ultra and has rebooted at least once after installation so `initrc/bootscope.rc` takes effect. Check `disable_display` in Settings or inspect `display-status.txt` in the run directory.

**2. Is it normal for Bootloader timings to show `N/A`?**

Yes. Access to `UefiLog0.txt`, `boot_stats`, `/proc/bootprof`, or `ro.boot.boottime*` depends on SoC and vendor kernel permissions. Missing bootloader metrics do not affect Android system-stage profiling.

**3. Does BootScope slow down booting?**

Overhead is kept low by running one zero-fork native process, pacing live rendering to ~3 FPS, and tracking `self_cpu_ms` / `self_rss_kb` directly in WebUI. If zero graphical overhead is preferred, enable `disable_display`.

**4. Can it run on Magisk or APatch?**

Background collection, late `bootanimation` fallback (`late_native`), and `action.sh` replay still function where supported, while `~2–4s` early `initrc` HUD injection and the built-in module WebUI require KernelSU.

**5. What should I include when reporting compatibility issues?**

Please attach your device model, Android version, root manager version, and redacted copies of `display-status.txt` / `display-renderer.txt` from the affected run directory.

## Disclaimer

BootScope runs privileged native code during early Android boot. Although system partitions remain untouched and safety mechanisms (`disable_display`, `0700` permissions, and in-place redaction) are built in, please verify compatibility for your device and keep backups. Disable or uninstall the module from recovery/manager if boot issues occur.

Source code is licensed under **[GNU General Public License v3.0](LICENSE)** with no warranty; the compiled WebUI bundle includes [Preact](https://preactjs.com/) under the MIT License.

## Related Docs

- [Changelog](CHANGELOG.md)
- [Architecture & Developer Guide](docs/ARCHITECTURE_EN.md)
- [Building & Releasing (Local / GitHub Actions)](BUILD_EN.md)
- [GNU GPL v3.0 License](LICENSE)
