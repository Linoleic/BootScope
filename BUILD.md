# 构建与打包

**中文** | [English](BUILD_EN.md) | [返回 README](README.md)

本文档说明如何从源码构建 BootScope 模块、运行测试以及使用 GitHub Actions 发布版本。项目概览见 [README.md](README.md)，内部设计与数据契约详见 [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

---

## 环境准备

1. **操作系统**：Windows、Linux 或 macOS（构建脚本完全跨平台，与 CI 共用同一入口）
2. **Android NDK**：API 33+（脚本自动探测 `ANDROID_NDK_HOME`、`ANDROID_NDK_ROOT` 或 `ANDROID_HOME/ndk/` 下最高版本）
3. **Node.js 与 npm**：推荐 Node 18+ / 22，用于编译 `webui/`（`tsc --noEmit && vite build`）
4. **Python**：推荐 Python 3.10+ / 3.12（仅使用标准库，无需额外 `pip install`）
5. **Git**：用于在构建期自动生成包内 `-r<commit数>` 标签与提交校验标识

---

## 版本规则

发布版本与构建身份分开维护：

- **发布版本**：在 [module/module.prop](module/module.prop) 中显式维护 `version=vMAJOR.MINOR.PATCH` 与正整数 `versionCode`（推荐按 `MAJOR * 10000 + MINOR * 100 + PATCH` 递增，例如 `v0.5.0` 对应 `500`）。**提交次数不再用于计算 `versionCode`**，避免浅克隆或历史整理导致升级码倒退。
- **包内可读版本**：构建脚本通过 `resolve_build_info()` 读取 Git 提交数与工作区状态，生成 `vMAJOR.MINOR.PATCH-r<Git提交数>`（存在未提交修改时追加 `-local`）写入打包 ZIP 内的 `module.prop` 与输出文件名（如 `bootscope-v0.5.0-r33.zip`），同时严格保持仓库源码中的 [module/module.prop](module/module.prop) 不被修改。
- **二进制构建标识**：构建期在 `dist/build-info/bootscope_build_info.h` 生成提交哈希与包版本信息并内嵌至 `bootscope` 二进制；在设备上运行 `bootscope version`（或 `--version`）即可查看精确构建号与完整 Commit SHA。
- **许可证随包附带**：根据 GPL-3.0 要求，打包时会自动把仓库根目录 [LICENSE](LICENSE) 一并打入模块 ZIP。

---

## 常用构建与测试命令

```sh
# 1. 完整构建（编译 C++17 核心、构建 Preact WebUI、打包 dist/bootscope-v*.zip）
python build_module.py

# 2. 复用已有 WebUI 产物仅重编 C++ 并重新打包（调试核心代码时加速）
python build_module.py --skip-webui

# 3. 运行全量单元测试（覆盖解析器、回放、包结构、开源元数据与版本校验）
python -m unittest discover -s tests -v

# 4. 运行与 CI 相同的发布前完整性体检
python tools/ci_release.py --tag v0.5.0 --package dist/bootscope-v0.5.0-r<N>.zip --revision <40位完整CommitSHA>
```

### PC 离线分析工具

仓库根目录保留桌面端辅助分析工具，可直接分析从设备导出的采样目录：

```sh
python analyze_probe.py results/bootA/samples.txt --compare results/bootB/samples.txt --html results/report.html
```

---

## GitHub Actions

### 使用 GitHub 自行构建

1. Fork 本仓库，在自己的仓库中打开 **Actions** 并按提示启用工作流。
2. 选择 **Build → Run workflow**，选择目标分支后点击运行。
3. 等待构建与单元测试通过后，从运行页面的 **Artifacts** 下载 `bootscope-module`。
4. 解压外层归档得到 `bootscope-v*.zip`，再在 KernelSU 管理器中安装该模块压缩包。

手动触发构建不会创建 Release，也无需额外配置个人令牌，工作流仅请求 `contents: read` 权限，产物默认保留 14 天。

### 自动测试与发布工作流

- [.github/workflows/build.yml](.github/workflows/build.yml)：在 `ubuntu-24.04` 上运行；推送 `main`、发起 PR、手动运行或被 Release 工作流调用时执行元数据验证、完整 `python3 build_module.py`、全部 `unittest`、ZIP 体检（禁止脏构建 `-local` 与工作树修改），并锁定所有外部 Action 的完整 Commit SHA。
- [.github/workflows/release.yml](.github/workflows/release.yml)：推送 `v*` 标签且与 [module/module.prop](module/module.prop)、[update.json](update.json) 及 [CHANGELOG.md](CHANGELOG.md) 严格一致时触发；待构建校验通过后提取 `CHANGELOG.md` 对应章节原文，以草稿创建并公开发布 GitHub Release（拒绝覆盖已有 Release）。
- Actions 构建通过仅代表源码与包结构完整，正式公开版本仍需在实机上验证早期 HUD、WebUI 展示与升级行为。
