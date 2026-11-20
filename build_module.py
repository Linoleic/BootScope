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


def build_native_core(root: Path, sdk: Path, explicit_ndk: Path | None) -> Path:
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
    subprocess.run(
        [str(clang), '-std=c++17', '-O2', '-Wall', '-Wextra', '-static-libstdc++',
         f'-I{core_dir}', '-lnativewindow', '-ldl', '-lm', '-o', str(binary),
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
        subprocess.run([npm, 'install'], cwd=str(webui_dir), check=True)
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
    version = read_module_version(module_dir / 'module.prop')

    sdk = resolve_sdk_path(args.sdk)
    native_bin = build_native_core(root, sdk, args.ndk)
    print(native_bin)
    webroot_dir = build_webui(root, skip=args.skip_webui)
    print(webroot_dir)

    output = root / 'dist' / f'bootscope-{version}.zip'
    output.parent.mkdir(parents=True, exist_ok=True)

    text_entries = [
        ('module.prop', 0o100644),
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
        for rel_path, mode in text_entries:
            write_text_file(archive, rel_path, module_dir / rel_path, mode)
        write_zip_entry(archive, 'bootscope', native_bin.read_bytes(), 0o100755)
        for path in sorted(webroot_dir.rglob('*')):
            if path.is_file():
                rel = path.relative_to(webroot_dir).as_posix()
                write_zip_entry(archive, f'webroot/{rel}', path.read_bytes(), 0o100644)
    print(output)


if __name__ == '__main__':
    main()
