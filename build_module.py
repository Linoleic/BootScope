"""Build and package the unified BootScope KernelSU module."""
import argparse
import os
from pathlib import Path
import re
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

from package_utils import read_module_version, write_text_file, write_zip_entry

PREFERRED_PLATFORM = 'android-36'
PREFERRED_BUILD_TOOLS = '36.0.0'
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


def resolve_android_jar(sdk: Path, platform: str | None) -> Path:
    """Pick requested or preferred android.jar (API >= 33)."""
    platforms_dir = sdk / 'platforms'
    if platform:
        candidate = platforms_dir / platform / 'android.jar'
        if not candidate.is_file():
            raise FileNotFoundError(f'Missing android.jar at {candidate}')
        return candidate
    preferred = platforms_dir / PREFERRED_PLATFORM / 'android.jar'
    if preferred.is_file():
        return preferred
    candidates = []
    if platforms_dir.is_dir():
        for entry in platforms_dir.iterdir():
            jar = entry / 'android.jar'
            match = re.fullmatch(r'android-(\d+)(?:\.\d+)?', entry.name)
            if jar.is_file() and match and int(match.group(1)) >= MIN_PLATFORM_API:
                candidates.append((_version_key(entry.name), jar))
    if not candidates:
        raise FileNotFoundError(
            f'No Android SDK platform >= {MIN_PLATFORM_API} found under {platforms_dir}'
        )
    return max(candidates, key=lambda item: item[0])[1]


def resolve_d8(sdk: Path, build_tools: str | None) -> Path:
    """Pick requested, preferred, or latest installed d8 executable."""
    exe = 'd8.bat' if os.name == 'nt' else 'd8'
    bt_dir = sdk / 'build-tools'
    if build_tools:
        candidate = bt_dir / build_tools / exe
        if not candidate.is_file():
            raise FileNotFoundError(f'Missing d8 at {candidate}')
        return candidate
    preferred = bt_dir / PREFERRED_BUILD_TOOLS / exe
    if preferred.is_file():
        return preferred
    candidates = []
    if bt_dir.is_dir():
        for entry in bt_dir.iterdir():
            candidate = entry / exe
            if candidate.is_file():
                candidates.append((_version_key(entry.name), candidate))
    if not candidates:
        raise FileNotFoundError(f'No d8 found under {bt_dir}')
    return max(candidates, key=lambda item: item[0])[1]


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


def build_display_dex(root: Path, sdk: Path, platform: str | None, build_tools: str | None) -> Path:
    """Compile display/*.java into dist/bootscope-display/classes.dex."""
    android_jar = resolve_android_jar(sdk, platform)
    d8 = resolve_d8(sdk, build_tools)
    output = root / 'dist' / 'bootscope-display'
    output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as temp:
        sources = sorted((root / 'display').glob('*.java'))
        subprocess.run(
            ['javac', '--release', '8', '-Xlint:-options', '-classpath', str(android_jar),
             '-d', temp, *map(str, sources)],
            check=True,
        )
        classes = sorted((Path(temp) / 'bootscope').glob('*.class'))
        subprocess.run(
            [str(d8), '--min-api', str(MIN_PLATFORM_API), '--lib', str(android_jar),
             '--output', str(output), *map(str, classes)],
            check=True,
        )
    return output / 'classes.dex'


def build_early_native(root: Path, sdk: Path, explicit_ndk: Path | None) -> Path:
    """Compile display/bootscope_early.cpp and display/early/*.cpp into dist/bootscope-display/bootscope_early."""
    ndk = resolve_ndk(sdk, explicit_ndk)
    clang, strip = resolve_ndk_clang(ndk)
    output = root / 'dist' / 'bootscope-display'
    output.mkdir(parents=True, exist_ok=True)
    binary = output / 'bootscope_early'
    display_dir = root / 'display'
    sources = [display_dir / 'bootscope_early.cpp'] + sorted((display_dir / 'early').glob('*.cpp'))
    subprocess.run(
        [str(clang), '-O2', '-Wall', '-Wextra', '-static-libstdc++',
         f'-I{display_dir}', '-lnativewindow', '-ldl', '-lm', '-o', str(binary),
         *[str(s) for s in sources]],
        check=True,
    )
    if strip.is_file():
        subprocess.run([str(strip), str(binary)], check=True)
    return binary


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--no-display', action='store_true',
                        help='Package shell probe only without compiling display components')
    parser.add_argument('--no-early', action='store_true',
                        help='Skip compiling the native early-boot display binary')
    parser.add_argument('--sdk', type=Path, help='Android SDK root directory')
    parser.add_argument('--ndk', type=Path, help='Android NDK root directory')
    parser.add_argument('--platform', help=f'Platform folder (default: {PREFERRED_PLATFORM} or latest)')
    parser.add_argument('--build-tools', help=f'Build-tools version (default: {PREFERRED_BUILD_TOOLS} or latest)')
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    module_dir = root / 'module'
    version = read_module_version(module_dir / 'module.prop')

    dex_path = None
    early_path = None
    if not args.no_display:
        sdk = resolve_sdk_path(args.sdk)
        dex_path = build_display_dex(root, sdk, args.platform, args.build_tools)
        print(dex_path)
        if not args.no_early:
            early_path = build_early_native(root, sdk, args.ndk)
            print(early_path)

    suffix = '-headless' if args.no_display else ''
    output = root / 'dist' / f'bootscope-{version}{suffix}.zip'
    output.parent.mkdir(parents=True, exist_ok=True)

    text_entries = [
        ('module.prop', 0o100644),
        ('action.sh', 0o100755),
        ('service.sh', 0o100755),
        ('probe.sh', 0o100755),
    ]
    if not args.no_display:
        text_entries.extend([
            ('display.sh', 0o100755),
            ('rules.conf', 0o100644),
        ])
        if early_path is not None:
            text_entries.extend([
                ('customize.sh', 0o100755),
                ('uninstall.sh', 0o100755),
                ('initrc/bootscope.rc', 0o100644),
            ])

    with ZipFile(output, 'w', ZIP_DEFLATED) as archive:
        for rel_path, mode in text_entries:
            write_text_file(archive, rel_path, module_dir / rel_path, mode)
        if dex_path is not None:
            write_zip_entry(archive, 'classes.dex', dex_path.read_bytes(), 0o100444)
        if early_path is not None:
            write_zip_entry(archive, 'bootscope_early', early_path.read_bytes(), 0o100755)
    print(output)


if __name__ == '__main__':
    main()
