# BootScope

BootScope 是 Android 启动状态可视化及分析项目（目标平台：现代 `arm64-v8a` KernelSU 设备，并在脚本中为其他 root 框架保留接入钩子）。项目采用**单一 C++17 核心二进制 + TypeScript/Preact WebUI + PC Python 分析套件**架构。

## 构建模块

需要 Android NDK、Node.js/npm 及 Python 3：

```powershell
python .\build_module.py
```

构建脚本会：
1. 使用 NDK（C++17 + `-static-libstdc++`）编译 `core/*.cpp` 为单一可执行文件 `bootscope`。
2. 在 `webui/` 运行 `tsc --noEmit && vite build` 生成静态资源到 `dist/webroot/`（可用 `--skip-webui` 复用现有产物）。
3. 打包为 `dist/bootscope-0.5.0.zip`。支持 KernelSU 热安装（`KSU_HAS_HOT_INSTALL=true`）且 `initrc/bootscope.rc` 入口已在 `/metadata/vold/ksu/modules.rc` 激活时，[customize.sh](module/customize.sh) 会自动请求免重启更新（`MODULE_HOT_INSTALL_REQUEST=true`）。

## 架构概览

1. **核心原生二进制 `bootscope`（`core/`）**：
   - `bootscope early`：KernelSU `on late-fs`（`initrc/bootscope.rc`）拉起早期 SurfaceFlinger 浮层，捕获 `early-trace.tsv`（含框架/Init/BL/资源/PSI 快照）。`display.sh` 在缺失早期帧时可自动降级为 `late_native`。
   - `bootscope collect`（[probe.sh](module/probe.sh) 调度）：零 `fork` 遍历 `__system_property_foreach`、`/proc/pressure/*`、`/proc/[pid]/stat`、`/dev/kmsg` 与多平台 Bootloader 数据（`ro.boot.boottime*`、`/dev/logfs/UefiLog0.txt`、`/proc/bootprof`、高通 `boot_stats` KPI），按 `max_history` 自动轮转清理旧目录。
   - `bootscope replay <target> [--bl] [--pre-sf] [--speed <x>]`：屏幕叠加全链路回放（支持 POR 通电起的 BL 阶段与帧前 Init 阶段）。
   - `bootscope list-json` / `inspect-json` / `compare-json` / `summary` / `prune` / `config-get` / `config-set` / `rules-get` / `rules-set`：统一后端 JSON/CLI 契约。
2. **KernelSU WebUI（`webui/`）**：
   - Vite + TypeScript + Preact 构建，通过 `window.ksu.exec` 直接消费 `bootscope` JSON API，提供全链路 SVG 甘特时间轴、BL 分段剖析、A/B 两次启动耗时对比、屏幕回放控制台，以及开关/历史清理/`rules.conf` 在线编辑。

## PC 端分析与单元测试

```powershell
python .\analyze_probe.py .\results\bootA\samples.txt --compare .\results\bootB\samples.txt --html .\results\report.html
python -m unittest discover -s tests -v
```
