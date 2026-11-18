# BootScope

BootScope 是 Android 启动状态可视化及分析项目。当前包含统一打包的 KernelSU 模块（数据采集探针 + 实验性开机显示面板）以及电脑端的快照监控与启动回放工具。

## KernelSU 模块（采集探针 + 开机显示）

`module/` 与 `display/` 合并构建为单一 KernelSU 模块（`id=bootscope`）。

```powershell
python .\build_module.py
```

使用 JDK、Android SDK（API ≥ 33，优先 `android-36`）和 Build Tools（优先 `36.0.0`）编译 `display/*.java` 为 `classes.dex`，并生成完整模块 `dist/bootscope-0.3.0.zip`。如果只需纯 shell 数据采集（不编译显示面板），可运行：

```powershell
python .\build_module.py --no-display
```

生成仅包含采集脚本的 `dist/bootscope-0.3.0-headless.zip`。若之前曾安装拆分的旧模块（`bootscope_probe` / `bootscope_display_probe`），升级前建议先卸载：

```powershell
adb -s <设备序列号> shell "su -c 'ksud module uninstall bootscope_probe; ksud module uninstall bootscope_display_probe'"
```

### 运行机制

`module/service.sh` 非阻塞地分别拉起两个独立后台任务，彼此互不影响：

1. **采集探针（`probe.sh`）**：每轮采样 init 服务状态、首次启动时间及开机动画和启动完成属性，最多 60 次；连续 3 次检测到 `sys.boot_completed=1` 后，限时读取当前 `events` 环形缓冲，仅保存允许的启动事件标签和毫秒数值。快照记录观测时刻，事件字段使用各写入方定义的时钟。
2. **开机显示（`display.sh` + `DisplayProbe`）**：在模块内存在 `classes.dex` 且未配置禁用开关时，通过 `app_process` 在系统动画期间向 SurfaceFlinger 提交状态叠加层，动态跟踪分辨率与刷新率变化并避让顶部挖孔区域，展示启动进度条、当前阶段事件及关键服务状态。动画退出、系统启动完成或达到时间上限时释放图层；显示脚本额外设置了进程超时。
   - 如需仅关闭开机显示而保留数据采集，创建 `/data/adb/bootscope/disable_display`（或模块目录下的 `disable_display`）即可，重启后自动跳过显示探针。

每次开机的输出统一保存在设备 `/data/adb/bootscope/<boot-id>/`（目录权限 0700，文件默认 0600），通过独立的 `.probe.lock` / `.display.lock` 防止同一开机周期重复运行：
- 采集结果：`metadata.txt`、`samples.txt`、`boot-events.tsv`、`logcat-exit.txt`、`logcat-error.txt`
- 显示诊断：`display-status.txt`、`display-renderer.txt`、`display-compositor.txt`

日志和属性可能包含敏感的系统信息；不会自动上传，也不会自动删除旧记录。在目标设备确认 `ksud module --help` 支持后，可使用 `ksud module disable bootscope` 禁用模块，重启后验证其不再运行。不要将不同 KernelSU 分支的恢复行为视为相同；安装和重启前确认设备的实际恢复方式。

## 分析与回放

使用电脑端工具分析样本或在 Windows Terminal 中按实际采样间隔回放（兼容旧版样本）：

```powershell
python .\analyze_probe.py .\results\pixel-probe-v02\samples.txt
python .\replay.py .\results\pixel-probe-v02\samples.txt --speed 2
```

回放按核心组件与观测窗口内发生状态变化的服务分组展示，框架事件在回放结束后列出并附相邻事件差值。启动事件选择参考 [AOSP perfboot](https://android.googlesource.com/platform/system/core/+/android16-release/init/perfboot.py)。

## 实时 ADB 监控

需要 Python 3 和 Android platform-tools。连接设备并确认 ADB 授权后：

```powershell
python .\bootscope.py -s <设备序列号>
python .\bootscope.py -s <设备序列号> --watch
python .\bootscope.py -s <设备序列号> --json
```

使用 `adb devices -l` 查看序列号。`--watch` 按秒轮询并输出观测到的变化；ADB 在重启中断后需要重新运行。此工具不会保存系统属性或日志到文件。

## 数据含义与边界

- `init.svc.<name>` 是 init 的服务进程状态；`running` 不代表内部初始化完成。
- `ro.boottime.<name>` 是系统记录的服务首次启动时间，单位是从 `CLOCK_BOOTTIME` 起算的纳秒，不是耗时；未提供时显示未知。
- `sys.boot_completed` 是系统属性，不代表用户桌面已经可交互。
- 快照仅包含可读属性；轮询只能发现两次采样之间的净变化，无法可靠捕获短暂状态；时间为观察时间，不是事件发生的精确时间。
- 状态名称的排列是字典序，不是依赖树；当前不推断依赖、等待原因或初始化耗时。

## 单元测试

```powershell
python -m unittest discover -s tests -v
```

## 计划

1. 补充安全脱敏的 `dmesg` (`init:` 事件) 与 `ro.boottime.init.*` 阶段耗时采集，将分析与回放覆盖至 `0s–24s` 全程启动。
2. 加入来源标记和启动后报告，视设备能力研究 Bootchart 和 Perfetto。
3. 逐步研究更准确的依赖、等待和耗时，需要时增加系统侧追踪支持。
