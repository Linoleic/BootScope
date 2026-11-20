# BootScope

**中文** | [English](README_EN.md)

[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-blue.svg)](https://developer.android.com)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

面向现代 `arm64-v8a` Android 设备的早期启动状态 HUD、全链路启动剖析与 KernelSU WebUI 管理模块。在开机过程中原位显示关键阶段、异常日志与系统压力，并在开机后提供完整的数据回看与对比能力。

本项目希望让已经配置 Root 环境的开发者与折腾用户，无需连接电脑或手动抓取多份系统日志，就能直观定位开机变慢、Bootloader 耗时波动、守护进程阻塞或模块报错的真实原因。

工作过程不修改系统分区，也不替换原版开机动画。采集与显示全部在模块自研的独立原生层与私有目录内完成，可随时开关浮层显示或卸载清理。

## 功能

- 在开机 `late-fs` 阶段拉起早期原生屏幕浮层，实时展示阶段进度、错误摘要、核心服务与资源压力。
- 解析 Qualcomm、MediaTek 与 `ro.boot.boottime*` 的 Bootloader 耗时，并与系统启动甘特时间线对齐。
- 合并高频早期压力采样与后期 `psi.tsv`，在 WebUI 中渲染平滑 CPU / IO / MEM 曲线并用连续滑动标尺查看任意时刻指标。
- 追踪多次开机的历史耗时趋势，支持按严重程度分组与按启动阶段混排的独立「启动日志」排查视图。
- 对两次启动执行 A/B 分段耗时与 Bootloader 差异对比，并在设备上直接执行全链路屏幕回放（`--bl --pre-sf`）。
- 支持在线开关显示浮层、清理历史运行目录以及自定义 `rules.conf` 过滤、模块高亮与就地脱敏规则。

## 使用要求

- `arm64-v8a` 设备，系统版本为 **Android 13 及以上（API 33+）**。
- 已获取 Root 权限；**KernelSU / SukiSU-Ultra** 具备完整能力（早期 `initrc`、WebUI 与免重启热更新），其他 Root 管理器保留后期 `late_native` 显示与后台采集降级路径。
- 已在小米（Qualcomm，Android 16，SukiSU-Ultra）与 Google Pixel 7 Pro（Tensor，Android 17，KernelSU）实机验证；不同 ROM 或内核的 Bootloader 原始日志完整度可能存在差异。

## 安装与使用

1. 从本项目的 [Releases](https://github.com/Linoleic/BootScope/releases) 下载模块 ZIP（或按 [BUILD.md](BUILD.md) 自行构建）。
2. 在 KernelSU 管理器中选择模块压缩包安装。首次安装后需重启一次设备，以激活 `initrc/bootscope.rc` 早期入口；后续版本升级支持免重启热安装。
3. 重启开机过程中，屏幕上方将显示早期状态 HUD 浮层并在桌面就绪时自动退出。
4. 进入桌面后打开 KernelSU 管理器中的 **BootScope → WebUI**，查看本次开机的甘特图、平滑 PSI 曲线、启动日志与历史对比。
5. 需要重新观察本次启动画面时，可在模块卡片点击 **Action** 触发 `--bl --pre-sf` 屏幕回放。

```text
首次安装重启 -> 开机 early_native 屏幕显示 + 全程采样落盘
开机后进入 WebUI -> 查看甘特图 / 平滑压力曲线 / 启动日志 / A/B 对比
```

## 实现方式与特点

### 无侵入早期显示与运行时适配

模块在 `on late-fs` 由 KernelSU `initrc` 启动单一原生二进制 `bootscope early`，通过运行时 `dlopen` 与多版本符号回退连接 `SurfaceComposerClient` 创建独立浮层窗口，不改动原版 `bootanimation` 与系统镜像。

实际开机阶段按约 3 FPS（`333ms`）低频率刷新并记录自身开销（`self_cpu_ms`、`self_rss_kb`）；当早期帧未能触发且动画仍在运行时，由后期脚本自动尝试 `late_native` 回退显示。

### 零 `fork` 原生采集与全链路对齐

`bootscope collect` 在后台直接通过 `__system_property_foreach`、`/proc/pressure/*`、`/proc/[pid]/stat` 与内核/事件日志完成周期采集，避免 shell 频繁派生进程影响开机耗时本身。

结合早期记录的 `early-trace.tsv` 与后期样本，WebUI 可还原从通电（Bootloader）、Init、SurfaceFlinger 到 `boot_completed` 的连续时间轴与毫秒级资源占用变化。

### 保留诊断现场的就地脱敏

针对 `imei`、`android_id`、`token=`、`password` 等敏感配置，`redact_key` 采用 **就地脱敏**（将其值替换为 `***`），而不直接丢弃整行故障日志，避免排查启动错误时失去关键上下文，并完整保留 Java `Caused by:` 根因行供「启动日志」页追踪。

### 与其他定位方式的区别

| 方式 | 用户需要维护的主要成本 | 本项目的选择 |
| --- | --- | --- |
| 电脑连接 `adb logcat` / `dmesg` | 需提前接线配对，无法捕获早期无 ADB 阶段画面 | 设备本地自主记录早期轨迹与历史开机档 |
| 手动抓取 `bootchart` / Perfetto | 配置门槛高，需导出到电脑专用工具查看 | 开机过程零操作，手机端 WebUI 直接可视化分析 |
| 传统后期 `service.sh` 脚本 | 起步过晚，错过早期 `late-fs` 服务与错误现场 | 通过 KernelSU `initrc` 约 2–4 秒内切入渲染与追踪 |
| BootScope | 保持 Root / KernelSU 环境及按需配置 `rules.conf` | 原生采集 + 早期 HUD + 手机端 WebUI 一体闭环 |

各方式的具体适用场景取决于设备形态。BootScope 的早期浮层与 `SurfaceFlinger` 符号解析同样受制于厂商系统版本演变，部分机型可能回退为仅提供后期采集与 WebUI 分析。

## WebUI

| 页面 | 用途 |
| --- | --- |
| 总览 | 查看启动摘要、Bootloader 剖析、甘特时间轴、平滑 PSI 曲线与历史趋势 |
| 启动日志 | 按严重程度分组（`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`）或按阶段时间线排查并一键复制 |
| 对比 | 选定两次历史启动执行 A/B 关键阶段差值与 Bootloader 分段差异对比 |
| 设置 | 开关屏幕 HUD 显示、调节全链路回放参数、清理历史记录及在线编辑 `rules.conf` |

## 更新与恢复

**更新模块：** 在 KernelSU 管理器中直接安装新版 ZIP，或在支持 `updateJson` 的管理器中直接检查更新；满足热安装条件时无需重启设备即可更新工具与 WebUI 资产（新增早期底层特性后建议重启一次以验证实机开机采集）。

**临时关闭屏幕显示：** 若不希望在开机时出现屏幕 HUD（仍保留后台采样与 WebUI 分析），可在 WebUI「设置」页切换开关，或执行：

```sh
mkdir -p /data/adb/bootscope
touch /data/adb/bootscope/disable_display
```

删除该文件或重新打开开关即可恢复显示。

**卸载模块：** 在管理器中点击卸载模块并重启后，`uninstall.sh` 会自动清理 `/metadata/vold/ksu/bootscope*` 注入文件与 `/data/adb/bootscope/` 运行数据。

## 数据与配置存储位置

模块运行与持久化配置使用的设备端路径如下：

| 类型 | 存储路径 | 说明 |
| --- | --- | --- |
| **单次运行采样目录** | `/data/adb/bootscope/<boot_id>/` | 保存 `early-trace.tsv`、`samples.txt`、`psi.tsv`、`proc.tsv`、Bootloader 日志与显示诊断状态（权限 `0700`） |
| **自定义过滤与脱敏规则** | `/data/adb/bootscope/rules.conf` | 持久化的过滤/高亮/脱敏规则覆盖配置，模块升级时保留；WebUI 在线编辑也会写入此处 |
| **显示开关标志文件** | `/data/adb/bootscope/disable_display` | 存在该文件时禁用开机屏幕浮层，不影响采集与 WebUI |
| **早期注入二进制与状态同步** | `/metadata/vold/ksu/bootscope*` | KernelSU `initrc` 早期入口使用的原生二进制、规则副本与显示禁用标记 |
| **当次开机早期内存轨迹** | `/dev/bootscope_early*` | `late-fs` 阶段写入 `/dev` 临时节点的轨迹日志与状态文件，随后在 `service.sh` 阶段归档至运行目录 |

## 常见问题（FAQ）

**1. 开机时没有看到屏幕浮层怎么办？**

先确认设备为 KernelSU / SukiSU-Ultra，且安装后已重启过至少一次以激活 `initrc/bootscope.rc`；也可打开 WebUI 或检查运行时目录中的 `display-status.txt`，排查是否开启了 `disable_display` 或触发了 `SurfaceFlinger` 符号兼容限制。

**2. Bootloader 耗时或分段显示为 `N/A` 正常吗？**

属于常见现象。不同芯片与厂商（高通 UEFI/KPI、`ro.boot.boottime*`、联发科 `bootprof`）暴露给内核或属性的 Bootloader 节点权限与格式各不相同，未读取到对应平台节点时会降级展示，不影响 Android 系统阶段的采集与分析。

**3. 安装后会不会明显拖慢开机速度？**

不会产生明显负担。实际开机期间 HUD 刷新率主动限制在约 3 FPS 且采用零 `fork` 采样，可在 WebUI 运行列表中直接核对每次开机的自身耗时（`self_cpu_ms`）与内存占用（`self_rss_kb`）。如仍希望零图形开销，关闭显示开关即可。

**4. Magisk 或 APatch 能否使用？**

后台采集、屏幕后期 `late_native` 显示（若动画尚未退出）与屏幕回放脚本均可运行，但约开机 2–4 秒介入的 `initrc` 早期浮层及原生模块 WebUI 管理器入口需依赖 KernelSU 环境；非 KernelSU 用户也可将样本导出至电脑配合离线脚本分析。

**5. 在哪里反馈新机型适配或界面问题？**

可在仓库 Issues 中附上机型、Android 版本、Root 框架版本，以及对应运行目录中的 `display-status.txt`、`display-renderer.txt`（请留意脱敏检查）。

## 使用声明

BootScope 主要在早期启动、原生渲染进程与 Root 权限上下文中工作。尽管模块不改动系统分区，并内置 `disable_display` 安全开关、权限隔离与脱敏机制，刷新模块前仍请评估设备兼容性并保留备份；若出现异常，可在管理器停用或移除模块后重启恢复。

本项目源码按 **GNU General Public License v3.0** 许可分发，不附带担保（详见 [LICENSE](LICENSE)）；前端 WebUI 编译产物中包含按 **MIT 许可证** 分发的 [Preact](https://preactjs.com/)。

## 相关文档

- [更新日志](CHANGELOG.md)
- [技术架构与开发者指南](docs/ARCHITECTURE.md)
- [自行构建与发布说明（本地 / GitHub Actions）](BUILD.md)
- [GNU GPL v3.0 许可证](LICENSE)
