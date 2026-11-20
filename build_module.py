"""Build and package the unified BootScope KernelSU module (native C++ core + Preact WebUI)."""
import argparse
import os
from pathlib import Path
import re
import subprocess
from zipfile import ZIP_DEFLATED, ZipFile

from package_utils import read_module_version, write_text_file, write_zip_entry

MIN_PLATFORM_API = 33


def _version_key(text: str) -> tuple[int, ...]:
    return tuple(int(part) for part in re.findall(r'\d+', text))


def resolve_sdk_path(explicit: Path | None) -> Path:
    """Resolve Android SDK directory from argument, environment, or OS default."""
    if explicit is not None:
        return explicit
    env = os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT')
    if env:
        return Path(env)
    if os.name == 'nt':
        return Path(os.environ.get('LOCALAPPDATA', '.')) / 'Android' / 'Sdk'
    return Path.home() / 'Android' / 'Sdk'


def resolve_ndk(sdk: Path, explicit: Path | None) -> Path:
    """Resolve Android NDK directory from argument, environment, or SDK ndk/."""
    if explicit is not None:
        if not explicit.is_dir():
            raise FileNotFoundError(f'Missing NDK directory at {explicit}')
        return explicit
    for var in ('ANDROID_NDK_HOME', 'ANDROID_NDK_ROOT'):
        env = os.environ.get(var)
        if env and Path(env).is_dir():
            return Path(env)
    ndk_dir = sdk / 'ndk'
    candidates = []
    if ndk_dir.is_dir():
        for entry in ndk_dir.iterdir():
            if entry.is_dir() and (entry / 'toolchains').is_dir():
                candidates.append((_version_key(entry.name), entry))
    if not candidates:
        raise FileNotFoundError(f'No Android NDK found under {ndk_dir}')
    return max(candidates, key=lambda item: item[0])[1]


def resolve_ndk_clang(ndk: Path) -> tuple[Path, Path]:
    """Return (aarch64-linux-android33-clang++, llvm-strip) paths inside an NDK."""
    prebuilt = ndk / 'toolchains' / 'llvm' / 'prebuilt'
    clang_name = f'aarch64-linux-android{MIN_PLATFORM_API}-clang++' + ('.cmd' if os.name == 'nt' else '')
    strip_name = 'llvm-strip' + ('.exe' if os.name == 'nt' else '')
    if prebuilt.is_dir():
        for host_dir in prebuilt.iterdir():
            clang = host_dir / 'bin' / clang_name
            strip = host_dir / 'bin' / strip_name
            if clang.is_file():
                return clang, strip
    raise FileNotFoundError(f'Missing {clang_name} under {prebuilt}')


def _run_git(root: Path, *args: str) -> str | None:
    res = subprocess.run(
        ['git', '-C', str(root), *args],
        capture_output=True, text=True, encoding='utf-8', errors='replace',
    )
    return res.stdout.strip() if res.returncode == 0 else None


def resolve_build_info(root: Path, prop_path: Path) -> dict:
    """Resolve release/package version, versionCode, and Git revision without mutating module.prop."""
    props: dict[str, str] = {}
    for line in prop_path.read_text(encoding='utf-8').splitlines():
        if '=' in line and not line.startswith('#'):
            k, v = line.split('=', 1)
            props[k.strip()] = v.strip()

    release_version = props.get('version', '')
    if not re.fullmatch(r'v\d+\.\d+\.\d+', release_version):
        raise ValueError(f'module.prop version must match vMAJOR.MINOR.PATCH: {release_version!r}')
    raw_code = props.get('versionCode', '')
    if not re.fullmatch(r'[1-9]\d*', raw_code):
        raise ValueError(f'module.prop versionCode must be a positive integer: {raw_code!r}')
    version_code = int(raw_code)

    revision = _run_git(root, 'rev-parse', 'HEAD')
    commit_count = _run_git(root, 'rev-list', '--count', 'HEAD')
    porcelain = _run_git(root, 'status', '--porcelain', '--untracked-files=normal')

    if (revision and re.fullmatch(r'[a-f0-9]{40}', revision)
            and commit_count and re.fullmatch(r'[1-9]\d*', commit_count)
            and porcelain is not None):
        dirty = bool(porcelain)
        package_version = f"{release_version}-r{commit_count}{'-local' if dirty else ''}"
    else:
        revision = 'unknown'
        dirty = False
        package_version = release_version

    info_dir = root / 'dist' / 'build-info'
    info_dir.mkdir(parents=True, exist_ok=True)
    header = '\n'.join([
        '#pragma once',
        f'#define BOOTSCOPE_RELEASE_VERSION "{release_version}"',
        f'#define BOOTSCOPE_PACKAGE_VERSION "{package_version}"',
        f'#define BOOTSCOPE_VERSION_CODE {version_code}',
        f'#define BOOTSCOPE_GIT_REVISION "{revision}"',
        f'#define BOOTSCOPE_BUILD_DIRTY {1 if dirty else 0}',
        '',
    ])
    header_path = info_dir / 'bootscope_build_info.h'
    if not header_path.is_file() or header_path.read_text(encoding='utf-8') != header:
        header_path.write_bytes(header.encode('utf-8'))

    return {
        'release_version': release_version,
        'package_version': package_version,
        'version_code': version_code,
        'revision': revision,
        'dirty': dirty,
        'include_dir': info_dir,
    }


def render_staged_module_prop(prop_path: Path, package_version: str) -> bytes:
    """Render module.prop bytes for ZIP packaging with LF newlines and computed package version."""
    lines = prop_path.read_text(encoding='utf-8').splitlines()
    rendered = []
    replaced = False
    for line in lines:
        if line.startswith('version='):
            rendered.append(f'version={package_version}')
            replaced = True
        else:
            rendered.append(line)
    if not replaced:
        rendered.append(f'version={package_version}')
    return ('\n'.join(rendered) + '\n').encode('utf-8')


def build_native_core(
    root: Path,
    sdk: Path,
    explicit_ndk: Path | None,
    build_info_dir: Path | None = None,
) -> Path:
    """Compile core/*.cpp into dist/bootscope-bin/bootscope."""
    ndk = resolve_ndk(sdk, explicit_ndk)
    clang, strip = resolve_ndk_clang(ndk)
    output = root / 'dist' / 'bootscope-bin'
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'bootscope'
    core_dir = root / 'core'
    sources = sorted(core_dir.glob('*.cpp'))
    if not sources:
        raise FileNotFoundError(f'No C++ sources found under {core_dir}')
    includes = [f'-I{core_dir}']
    if build_info_dir is not None:
        includes.append(f'-I{build_info_dir}')
    subprocess.run(
        [str(clang), '-std=c++17', '-O2', '-Wall', '-Wextra', '-static-libstdc++',
         *includes, '-lnativewindow', '-ldl', '-lm', '-o', str(binary),
         *[str(s) for s in sources]],
        check=True,
    )
    if strip.is_file():
        subprocess.run([str(strip), str(binary)], check=True)
    return binary


def build_webui(root: Path, skip: bool = False) -> Path:
    """Build Vite + TypeScript + Preact WebUI into dist/webroot/."""
    webroot = root / 'dist' / 'webroot'
    if skip and (webroot / 'index.html').is_file():
        return webroot
    webui_dir = root / 'webui'
    npm = 'npm.cmd' if os.name == 'nt' else 'npm'
    if not (webui_dir / 'node_modules').is_dir():
        install_cmd = 'ci' if (webui_dir / 'package-lock.json').is_file() else 'install'
        subprocess.run([npm, install_cmd], cwd=str(webui_dir), check=True)
    subprocess.run([npm, 'run', 'build'], cwd=str(webui_dir), check=True)
    if not (webroot / 'index.html').is_file():
        raise FileNotFoundError(f'Expected WebUI build output missing at {webroot / "index.html"}')
    return webroot


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, help='Android SDK root directory')
    parser.add_argument('--ndk', type=Path, help='Android NDK root directory')
    parser.add_argument('--skip-webui', action='store_true',
                        help='Reuse existing dist/webroot build instead of running npm')
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    module_dir = root / 'module'
    prop_path = module_dir / 'module.prop'
    build_info = resolve_build_info(root, prop_path)
    read_module_version(prop_path)

    sdk = resolve_sdk_path(args.sdk)
    native_bin = build_native_core(root, sdk, args.ndk, build_info['include_dir'])
    print(native_bin)
    webroot_dir = build_webui(root, skip=args.skip_webui)
    print(webroot_dir)

    dist_dir = root / 'dist'
    dist_dir.mkdir(parents=True, exist_ok=True)
    for stale_zip in dist_dir.glob('bootscope-*.zip'):
        stale_zip.unlink()
    output = dist_dir / f"bootscope-{build_info['package_version']}.zip"

    text_entries = [
        ('action.sh', 0o100755),
        ('service.sh', 0o100755),
        ('probe.sh', 0o100755),
        ('display.sh', 0o100755),
        ('customize.sh', 0o100755),
        ('uninstall.sh', 0o100755),
        ('rules.conf', 0o100644),
        ('initrc/bootscope.rc', 0o100644),
    ]

    with ZipFile(output, 'w', ZIP_DEFLATED) as archive:
        write_zip_entry(
            archive,
            'module.prop',
            render_staged_module_prop(prop_path, build_info['package_version']),
            0o100644,
        )
        for rel_path, mode in text_entries:
            write_text_file(archive, rel_path, module_dir / rel_path, mode)
        license_path = root / 'LICENSE'
        if license_path.is_file():
            write_text_file(archive, 'LICENSE', license_path, 0o100644)
        write_zip_entry(archive, 'bootscope', native_bin.read_bytes(), 0o100755)
        for path in sorted(webroot_dir.rglob('*')):
            if path.is_file():
                rel = path.relative_to(webroot_dir).as_posix()
                write_zip_entry(archive, f'webroot/{rel}', path.read_bytes(), 0o100644)
    print(output)


if __name__ == '__main__':
    main()
