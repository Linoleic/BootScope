# BootScope

**中文** | [English](README_EN.md)

[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

BootScope 是一个面向现代 `arm64-v8a` Android 设备的**早期启动状态 HUD、全链路启动剖析与 KernelSU WebUI 可视化工具**，采用 **单一原生 C++17 核心二进制 + TypeScript/Preact WebUI + PC Python 离线分析套件** 架构。

> 开发者文档：**[架构设计说明 (中文)](docs/ARCHITECTURE.md)** | **[Architecture Guide (English)](docs/ARCHITECTURE_EN.md)**

---

## 核心特性

- **早期原生 SurfaceFlinger 屏幕浮层（`bootscope early`）**
  - 在 KernelSU `on late-fs` 阶段（`initrc/bootscope.rc`）约开机 **2–4 秒** 时拉起低开销原生 HUD（真实开机约 3 FPS），直接呈现启动关键路径、`*:E` 错误摘要、核心服务状态与实时 CPU/IO/MEM 压力，并记录自耗指标（`self_cpu_ms`、`self_rss_kb`）与完整 `early-trace.tsv`。
  - 支持横屏/竖屏自适应布局；若设备不支持 KernelSU `initrc`，`display.sh` 可自动降级为 `late_native` 在 `service.sh` 阶段拉起。
- **零 `fork` 原生采集器（`bootscope collect`）**
  - 直接通过 `__system_property_foreach`、`/proc/pressure/*`、`/proc/[pid]/stat`、`/dev/kmsg` 与事件日志收集完整开机采样并自动按 `max_history` 轮转清理。
- **多平台 Bootloader 分段解析**
  - 自动解析 Qualcomm `UefiLog0.txt` + 内核 `boot_stats` KPI、`ro.boot.boottime*` 属性及 MediaTek `/proc/bootprof`，并与 POR 通电时间线对齐。
- **KernelSU WebUI 全功能可视化界面**
  - **总览与甘特图**：包含 Bootloader 分段解析、系统启动阶段甘特时间轴与关键里程碑。
  - **平滑资源压力曲线与连续标尺**：合并早期高频 `early-trace.tsv`（`METRIC`）与后期 `psi.tsv` 采样（最高 120 点），使用 Fritsch-Carlson 单调三次样条渲染平滑 CPU / IO / MEM 曲线，支持手势连续无级滑动查看任意时刻精确占比。
  - **多轮启动耗时趋势图**：展示历史各次启动耗时波动，支持标尺定位与一键跳转查看详情。
  - **独立「启动日志」页**：支持按严重程度分类的「分组视角（`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`）」与按启动阶段分割线混排的「时间线视角」，支持 Java `Caused by:` 根因完整追踪与一键复制排查文本。
  - **A/B 两次启动对比**：直观对比总耗时与 `Bootloader 分段耗时差异 (B - A)`。
  - **设置与在线规则控制**：在线开关 HUD 浮层、触发屏幕 `--replay` 回放、清理历史记录以及直接编辑持久化 `/data/adb/bootscope/rules.conf`。
- **隐私脱敏与安全安装**
  - `redact_key` 默认采用**就地脱敏**（将敏感键值替换为 `***`），在保护 `imei`/`android_id`/`token`/`password` 等隐私的同时保留原始故障上下文。
  - 支持 KernelSU 免重启热更新（当 `KSU_HAS_HOT_INSTALL=true` 且早期入口已激活时自动请求热安装）。

---

## 环境要求与兼容性

| 项目 | 要求 / 状态 |
| --- | --- |
| CPU 架构 | `arm64-v8a` |
| 系统版本 | Android 13+（API 33+） |
| Root 框架 | **KernelSU / SukiSU-Ultra**（完整支持早期 `initrc` + WebUI + 热更新）；脚本同时保留其他 root 框架的后期降级兼容路径 |
| 已验证机型 | 小米（Qualcomm，Android 16，SukiSU-Ultra）；Google Pixel 7 Pro（Tensor，Android 17，KernelSU） |

---

## 安装与使用

1. 从 [Releases](https://github.com/Linoleic/BootScope/releases) 下载 `bootscope-<version>.zip`（或从源码构建）。
2. 在 KernelSU 管理器中选择模块压缩包安装；首次安装建议重启一次以激活 `initrc/bootscope.rc` 早期入口（后续升级支持免重启热安装）。
3. **打开 WebUI**：开机完成后在 KernelSU 模块列表中点击 **BootScope → WebUI** 查看甘特图、平滑 PSI 曲线、启动日志与 A/B 对比。
4. **屏幕回放（`action.sh`）**：在模块页面点击 **Action** 按钮，默认执行 `--bl --pre-sf` 全链路屏幕回放。

### 安全开关与恢复

- **临时/永久关闭开机屏幕 HUD 浮层**（不影响后台数据采集与 WebUI）：
  ```sh
  mkdir -p /data/adb/bootscope
  touch /data/adb/bootscope/disable_display
  ```
  或直接在 WebUI「设置」页切换开关；删除该文件即可恢复显示。
- **自定义过滤 / 高亮 / 脱敏规则**：复制 `rules.conf` 到 `/data/adb/bootscope/rules.conf` 或在 WebUI「设置」页在线保存，升级模块时会自动保留。
- **卸载清理**：卸载模块并通过管理器重启后，`uninstall.sh` 会自动清理 `/metadata/vold/ksu/bootscope*` 与数据目录。

---

## 从源码构建

构建依赖：**Android NDK（API 33+，环境变量 `ANDROID_HOME` 或 `ANDROID_NDK_HOME`）**、**Node.js 18+ / npm** 与 **Python 3.10+**。

```sh
python build_module.py
```

构建脚本将依次执行：
1. 使用 NDK C++17（`-O2 -Wall -Wextra -static-libstdc++`）编译 `core/*.cpp` 生成单文件核心 `dist/bootscope-bin/bootscope`。
2. 在 `webui/` 运行 `tsc --noEmit && vite build` 输出前端静态资源至 `dist/webroot/`（添加 `--skip-webui` 可复用现有产物）。
3. 打包生成带标准 Unix 权限与 LF 换行符的模块包 `dist/bootscope-<version>.zip`。

---

## PC 离线分析与单元测试

本仓库同时提供 PC 端辅助分析脚本（用于导出/离线对照样本）与自动化单元测试套件：

- [analyze_probe.py](analyze_probe.py)：离线分析 `samples.txt` / `psi.tsv` / `proc.tsv` 并生成 A/B HTML 报告：
  ```sh
  python analyze_probe.py results/bootA/samples.txt --compare results/bootB/samples.txt --html results/report.html
  ```
- [bootscope.py](bootscope.py) / [replay.py](replay.py)：基于 ADB 的桌面端服务快照监听与样本回放工具。
- 运行单元测试：
  ```sh
  python -m unittest discover -s tests -v
  ```

---

## 开源协议与免责声明

- 本项目基于 **[GNU General Public License v3.0](LICENSE)** 开源。
- BootScope 涉及早期 Boot / `SurfaceFlinger` 原生注入与 Root 权限操作。尽管已内置安全开关与异常回退机制，刷入与使用模块仍需用户自行评估设备兼容性并承担相应风险。
