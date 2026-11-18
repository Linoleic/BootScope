# BootScope

BootScope 是 Android 启动状态可视化及分析项目。当前包含 KernelSU 采集探针、电脑端的启动回放，以及用于 Pixel 7 Pro 的实验性开机显示探针。

## KernelSU 采集探针

`module/` 是采集探针，`python build_module.py` 生成 `dist/bootscope-probe-0.2.0.zip`。已在 Pixel 7 Pro Android 17 上验证。

模块从非阻塞的 `service.sh` 启动后台脚本。每轮采集 init 服务状态、首次启动时间及开机动画和启动完成属性，最多 60 次。连续 3 次检测到 `sys.boot_completed=1` 后，限时读取当前 `events` 环形缓冲，仅保存允许的启动事件标签和毫秒数值。快照记录观测时刻，事件字段使用各写入方定义的时钟。

分析最近一次的样本或在 Windows Terminal 中按实际采样间隔回放。工具兼容旧版样本。

```powershell
python .\analyze_probe.py .\results\pixel-probe-v02\samples.txt
python .\replay.py .\results\pixel-probe-v02\samples.txt --speed 2
```

回放按组件分组，展示服务进程状态、系统记录的首次启动时间和观察到的变化。框架事件单独显示在回放结束后，保留原始毫秒字段。启动事件选择参考 [AOSP perfboot](https://android.googlesource.com/platform/system/core/+/android16-release/init/perfboot.py)。

Pixel 7 Pro 首次实测的 v0.1 从 11.78 秒开始，在 26.09 秒结束，共采样 12 次，init 日志文件为空。v0.2 从 11.66 秒开始，在 24.01 秒结束，共采样 11 次。开机动画在 11.71 秒首次采样时仍在运行，21.37 秒首次观察到动画已停止、退出属性为 1、`sys.boot_completed=1`。新版保存了 13 条 Android 框架启动事件。测试结束后已禁用采集探针。

结果保存在设备 `/data/adb/bootscope-probe/<boot-id>/`，目录权限为 0700，文件默认 0600。日志和属性可能包含敏感的系统信息；不会自动上传，也不会自动删除旧记录。禁用模块可停止后续开机采集，已产生的数据需要另行处理。

在目标设备确认 `ksud module --help` 支持后，可使用 `ksud module disable bootscope_probe` 禁用模块，重启后验证其不再运行。不要将不同 KernelSU 分支的恢复行为视为相同；安装和重启前确认设备的实际恢复方式。

## 开机显示探针

`python build_display.py` 使用 JDK、Android SDK Platform 36 和 Build Tools 36.0.0 构建 `dist/bootscope-display-probe-0.1.0.zip`。显示探针是独立模块（`bootscope_display_probe`），从 KernelSU 的 `service.sh` 运行，在系统动画期间通过 SurfaceFlinger 提交一块小面板。面板显示设备开机时间与三个真实服务状态。动画退出、系统启动完成或达到时间上限时释放图层。显示脚本额外设置了进程超时。

测试记录位于设备的 `/data/adb/bootscope-display/<boot-id>/`，包含首次提交时间、图层状态和退出原因。当前绘制区域固定，按 Pixel 7 Pro 验证，厂商 ROM 和其他屏幕配置需要实测。

Pixel Android 17 实测在开机后 11.77 秒进入模块脚本，12.328 秒提交第一帧。SurfaceFlinger 报告图层可见且已有缓冲，探针在动画退出时于 21.316 秒释放图层，以状态 0 退出。屏幕唤醒后的独立预览也匹配了面板的像素颜色。开机那一轮的证据来自系统图层诊断，实际手机上的视觉效果仍需现场确认。验证结束后，两个模块都已禁用。

## 运行

需要 Python 3 和 Android platform-tools。连接设备并确认 ADB 授权后：

```powershell
python .\bootscope.py -s <设备序列号>
python .\bootscope.py -s <设备序列号> --watch
python .\bootscope.py -s <设备序列号> --json
```

使用 `adb devices -l` 查看序列号。`--watch` 按秒轮询并输出观测到的变化；ADB 在重启中断后需要重新运行。此原型不会保存系统属性或日志到文件。

## 数据含义与边界

- `init.svc.<name>` 是 init 的服务进程状态；`running` 不代表内部初始化完成。
- `ro.boottime.<name>` 是系统记录的服务首次启动时间，单位是从 CLOCK_BOOTTIME 起算的纳秒，不是耗时；未提供时显示未知。
- `sys.boot_completed` 是系统属性，不代表用户桌面已经可交互。
- 快照仅包含 ADB 用户能读取到的属性；不代表所有 Android 服务或启动事件。
- 轮询只能发现两次采样之间的净变化，无法可靠捕获短暂状态；时间为观察时间，不是事件发生的精确时间。
- 状态名称的排列是字典序，不是依赖树；当前不推断依赖、等待原因或初始化耗时。

## 计划

1. 用 Pixel 7 Pro 的实机观察确认开机面板的位置、尺寸和退出效果，继续完善真实事件的显示。
2. 加入来源标记和启动后报告，视设备能力研究 Bootchart 和 Perfetto。
3. 逐步研究更准确的依赖、等待和耗时，需要时增加系统侧追踪支持。
