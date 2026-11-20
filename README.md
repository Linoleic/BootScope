# BootScope

**中文** | [English](README_EN.md)

[![Platform](https://img.shields.io/badge/Platform-Android%20%28arm64--v8a%29-blue.svg)](https://developer.android.com)
[![License: GPL v3](https://img.shields.io/badge/License-GPLv3-blue.svg)](LICENSE)

面向 `arm64-v8a` Android 设备的早期启动状态 HUD、全链路启动剖析与 KernelSU WebUI 分析工具，用于观测从 Bootloader（POR）到 `sys.boot_completed` 之间的启动阶段耗时、内核/框架异常与系统资源瓶颈。

全模块以单一静态链接 `C++17` 原生二进制（`bootscope`）配合 Preact WebUI 构成：不改动系统分区，不替换原版 `bootanimation`，通过零 `fork` 原生采集与低开销早期渲染完成观测，并支持安全禁用显示或完整卸载。

## 核心能力

- **早期原生屏幕 HUD**：在 KernelSU `on late-fs`（约开机 2–4 秒）启动独立 `SurfaceFlinger` 浮层，以 ~3 FPS 低频刷新展示关键启动路径、`*:E` 异常、核心服务与实时负载，并记录模块自身耗时（`self_cpu_ms`、`self_rss_kb`）。
- **多平台 Bootloader 分段解析**：读取高通 UEFI/KPI、`ro.boot.boottime*` 与联发科 `/proc/bootprof`，与 Android init 及框架阶段对齐至同一时间轴。
- **高密度资源曲线与趋势分析**：合并 `early-trace.tsv`（`METRIC`）与后期 `psi.tsv` 采样，使用 Fritsch-Carlson 单调三次样条绘制平滑 CPU / IO / MEM 曲线，配合连续滑动标尺定位任意时刻占比，并在多轮重启间追踪耗时波动。
- **独立「启动日志」视图**：支持按严重程度分组（`F` / `E` / `M ERR` / `INIT` / `STAGE` / `INFO`）与按启动阶段分割线时间线混排，完整保留 Java `Caused by:` 堆栈根因并支持一键复制。
- **A/B 启动对比与屏幕回放**：支持任意两次启动的阶段差值与 `Bootloader 分段耗时差异 (B - A)`，支持在真机直接触发 `--bl --pre-sf` 全链路屏幕回放（`action.sh`）。

## 观测数据源与覆盖范围

| 观测维度 | 设备端数据源 | 说明 |
| --- | --- | --- |
| **Bootloader 耗时** | `/dev/logfs/UefiLog0.txt`、`boot_stats` KPI、`ro.boot.boottime*`、`/proc/bootprof` | 随芯片与内核暴露节点自动匹配并对齐至通电基准；缺失时安全降级 |
| **早期轨迹与状态机** | `dmesg`、`events`、`/dev/bootscope_early_trace.tsv` | 记录 `late-fs` 起的关键阶段转换、早期事件与高精度 `METRIC` 采样 |
| **系统属性与服务** | `__system_property_foreach` | 单进程无 `fork` 轮询 `init.svc.*`、`ro.boottime.*` 与启动完成标志 |
| **资源压力与热点进程** | `/proc/pressure/{cpu,io,memory}`、`/proc/[pid]/stat` | 统计开机期间 CPU / IO / MEM PSI 压力波动及 Top CPU 进程占用 |
| **异常日志与脱敏** | `logcat *:E`、`init` 错误、`rules.conf` | 按 `redact_key` 对敏感键值做**就地脱敏（`***`）**，保留故障上下文 |

## WebUI 与命令行

### WebUI 页面

| 页面 | 功能 |
| --- | --- |
| 总览 | 单次启动耗时摘要、Bootloader 分段剖析、全链路甘特图、平滑 PSI 曲线与历史趋势 |
| 启动日志 | 分组归纳与时间线混排双视角排查视图，支持等级筛选、关键词检索与一键复制 |
| 对比 | 选定两次运行执行关键节点差值与 `Bootloader 分段耗时差异 (B - A)` 对比 |
| 设置 | 开关开机浮层（`disable_display`）、配置 `--replay` 参数、清理历史运行及编辑 `rules.conf` |

### 常用命令行（Root Shell）

```sh
bootscope summary latest                         # 输出最近一次开机的文本摘要
bootscope inspect-json latest                    # 输出供分析/脚本消费的完整 JSON
bootscope logs-json latest --level critical      # 筛选致命异常、模块错误与 init 失败日志
bootscope replay latest --bl --pre-sf            # 在屏幕回放含 BL 与帧前阶段的全链路启动
bootscope version                                # 查看包版本（-rN）与内嵌完整 Commit SHA
```

## 环境兼容与安装

- **系统与架构**：`arm64-v8a`，Android 13+（API 33+）。
- **Root 环境**：完整能力需要 **KernelSU / SukiSU-Ultra**（用于 `initrc` 早期拉起、管理器 WebUI 与条件免重启热更新）。Magisk / APatch 环境下仍可使用后台采集与 `late_native` 后期回退显示，但不支持 `late-fs` 早期浮层；样本亦可导出到电脑通过仓库内 Python 脚本离线分析。
- **已实测平台**：小米（Qualcomm，Android 16，SukiSU-Ultra）；Google Pixel 7 Pro（Tensor，Android 17，KernelSU）。

### 安装与开关说明

1. 从 [Releases](https://github.com/Linoleic/BootScope/releases) 获取模块压缩包（或按 [BUILD.md](BUILD.md) 从源码构建）并在 KernelSU 管理器安装。
2. 首次安装后重启一次以激活 `initrc/bootscope.rc`；后续若 `KSU_HAS_HOT_INSTALL=true` 且早期入口已激活，升级会自动请求免重启热安装。
3. **关闭屏幕浮层但保留采集与 WebUI**：在 WebUI「设置」页关闭开关，或创建空文件 `/data/adb/bootscope/disable_display`。
4. **路径与卸载清理**：`rules.conf` 与历史开机记录（`<boot_id>/`，权限 `0700`）均存放在 `/data/adb/bootscope/`；KernelSU 早期运行副本位于 `/metadata/vold/ksu/bootscope*`。卸载模块并重启后，`uninstall.sh` 会自动清理 `/metadata/vold/ksu/bootscope*` 注入文件；`/data/adb/bootscope/` 默认保留以便重装复用历史样本与规则，如需彻底清空可执行 `rm -rf /data/adb/bootscope`。

## 已知边界与排障

- **未见早期 HUD 或显示为 `late_native` / `none`**：`bootscope early` 依赖运行时 `dlopen` 与 `SurfaceFlinger` 符号匹配，部分定制 ROM 可能存在私有符号差异；可检查 `/data/adb/bootscope/<boot_id>/display-status.txt` 与 `display-renderer.txt`。
- **Bootloader 阶段展示不完整或显示 `N/A`**：取决于 OEM 内核是否向用户态保留 UEFI/KPI/`bootprof` 读取权限，不影响 Android 用户态启动剖析。
- **反馈兼容性问题**：提交 Issue 时请附带机型、Android 版本、Root 框架版本，以及脱敏确认后的 `display-status.txt` 与 `display-renderer.txt`。

## 许可与相关文档

BootScope 源码基于 **[GNU General Public License v3.0](LICENSE)** 开源（不附带任何担保）；前端 WebUI 编译产物内包含按 MIT 协议分发的 [Preact](https://preactjs.com/)。本项目涉及早期启动渲染与 Root 权限操作，刷入前请自行评估设备兼容性并做好备份。

- [更新日志 (CHANGELOG)](CHANGELOG.md)
- [架构设计与接口契约 (ARCHITECTURE)](docs/ARCHITECTURE.md)
- [源码构建、测试与 CI 发布 (BUILD)](BUILD.md)
- [GNU GPL v3.0 许可证](LICENSE)
