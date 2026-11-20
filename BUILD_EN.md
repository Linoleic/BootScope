# Building & Releasing BootScope

[中文](BUILD.md) | **English** | [Back to README](README_EN.md)

This guide explains how to build BootScope from source, run the automated test suite, and use GitHub Actions for self-builds or tagged releases. See [README_EN.md](README_EN.md) for usage and [docs/ARCHITECTURE_EN.md](docs/ARCHITECTURE_EN.md) for architecture details.

---

## Prerequisites

1. **Operating System**: Windows, Linux, or macOS (a single cross-platform Python build entry serves local devs and Linux CI alike)
2. **Android NDK**: API 33+ (automatically resolved via `ANDROID_NDK_HOME`, `ANDROID_NDK_ROOT`, or the newest version under `ANDROID_HOME/ndk/`)
3. **Node.js & npm**: Node 18+ / 22 to build `webui/` (`tsc --noEmit && vite build`)
4. **Python**: Python 3.10+ / 3.12 (standard library only; no `pip install` needed)
5. **Git**: Used during packaging to compute `-r<commit>` labels and embed the commit revision into the native binary

---

## Versioning Rules

Release versions and package build identities are maintained separately:

- **Release Metadata**: [module/module.prop](module/module.prop) explicitly defines `version=vMAJOR.MINOR.PATCH` and a positive integer `versionCode` (recommended mapping: `MAJOR * 10000 + MINOR * 100 + PATCH`, e.g., `v0.5.0` -> `500`). **Commit counts never drive `versionCode`**, preventing shallow clones or history rewrites from downgrading upgrade codes.
- **Packaged Display Version**: `build_module.py` queries Git history and stages `version=vMAJOR.MINOR.PATCH-r<commit_count>` (`-local` appended when the working tree has uncommitted edits) inside the generated ZIP's `module.prop`, leaving tracked [module/module.prop](module/module.prop) untouched.
- **Embedded Build Identity**: Compilation generates `dist/build-info/bootscope_build_info.h` and embeds the package version plus 40-character Git revision inside the stripped `bootscope` ELF binary. Run `bootscope version` (or `--version`) on-device to inspect exact build provenance.
- **License Bundling**: Per GPL-3.0 binary distribution requirements, [LICENSE](LICENSE) is automatically packaged into the root of `dist/bootscope-v*.zip`.

---

## Common Build & Verification Commands

```sh
# 1. Full build (C++17 binary + Preact WebUI + packaged dist/bootscope-v*.zip)
python build_module.py

# 2. Reuse existing WebUI assets when iterating purely on core/*.cpp
python build_module.py --skip-webui

# 3. Run the full unittest suite
python -m unittest discover -s tests -v

# 4. Run the same strict package integrity check used by Release CI
python tools/ci_release.py --tag v0.5.0 --package dist/bootscope-v0.5.0.zip --revision <40-char-sha>
```

### Host Offline Analysis CLI

Exported device run directories can also be analyzed offline on PC:

```sh
python analyze_probe.py results/bootA/samples.txt --compare results/bootB/samples.txt --html results/report.html
```

---

## GitHub Actions

### Building via Your Own Fork

1. Fork this repository, open **Actions** on your fork, and enable workflows.
2. Select **Build → Run workflow**, choose a branch, and start the run.
3. Once build and tests pass, download the `bootscope-module` artifact from the workflow summary.
4. Extract the inner `bootscope-v*.zip` and install that ZIP in KernelSU.

### Hardened CI & Release Workflows

- [.github/workflows/build.yml](.github/workflows/build.yml): Runs on `ubuntu-24.04` with `contents: read` permissions and SHA-pinned external actions; validates release metadata, compiles & packages `build_module.py`, executes `unittest`, runs `tools/ci_release.py` (rejecting dirty `-local` builds), and verifies an untouched source tree (`git status --porcelain`).
- [.github/workflows/release.yml](.github/workflows/release.yml): Triggered when pushing `v*` tags matching `module/module.prop`, `update.json`, and `CHANGELOG.md`. After `build.yml` succeeds, verifies inputs again and publishes the exact `CHANGELOG.md` section as a GitHub Release without overwriting existing releases.
- Passing CI verifies build reproducibility and package integrity; always perform on-device boot verification before tagging a public release.
