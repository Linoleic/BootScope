# 更新日志

本文档记录各版本的重要变更。
格式基于 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，版本号遵循[语义化版本 2.0.0](https://semver.org/lang/zh-CN/)。本项目的版本标题省略发布日期。

## [Unreleased]

## [v0.5.0]

### 新增

- 统一原生 C++17 核心二进制（`bootscope`），整合早期屏幕 HUD、原生采集守护进程、多平台 Bootloader 解析、屏幕回放与 WebUI JSON API。
- 通过 KernelSU `on late-fs`（`initrc/bootscope.rc`）实现早期 `SurfaceFlinger` 屏幕浮层，支持横竖屏自适应布局、~3 FPS 低开销刷新、`early-trace.tsv` 全程轨迹记录、模块自耗遥测（`self_cpu_ms`、`self_rss_kb`）以及无早期帧时的 `late_native` 自动回退。
- 支持 Qualcomm `UefiLog0.txt` + `boot_stats` KPI、`ro.boot.boottime*` 与 MediaTek `/proc/bootprof` 分段解析并对齐通电时间线。
- 推出 Preact + TypeScript + Vite WebUI：包含全链路甘特图、单调三次样条平滑 PSI 曲线与连续滑动标尺、历史启动耗时趋势图、分组/时间线双视角「启动日志」页、含 Bootloader 分段差异（`bl_deltas`）的 A/B 对比，以及支持屏幕回放、`disable_display` 开关和在线编辑 `rules.conf` 的设置页。
- 支持发布包版本号自动附加 `-r<commit数>` 与 Git 提交校验标识（`bootscope version`），随包打包 `LICENSE`，并配置独立构建验证与发版自动化工作流。

### 安全

- 默认启用基于 `redact_key` 的就地脱敏（将敏感键值替换为 `***`）并限制运行目录权限为 `0700`，在保留故障上下文和 Java `Caused by:` 堆栈根因的同时避免隐私泄露。
- 模块更新时支持条件免重启热安装，无需额外停用原有系统组件或修改系统分区。
