"""Compile the experimental screen probe to a standalone app_process DEX."""
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


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--sdk', type=Path, help='Android SDK root directory')
    parser.add_argument('--platform', help=f'Platform folder (default: {PREFERRED_PLATFORM} or latest)')
    parser.add_argument('--build-tools', help=f'Build-tools version (default: {PREFERRED_BUILD_TOOLS} or latest)')
    args = parser.parse_args()

    root = Path(__file__).resolve().parent
    sdk = resolve_sdk_path(args.sdk)
    android_jar = resolve_android_jar(sdk, args.platform)
    d8 = resolve_d8(sdk, args.build_tools)

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
    dex_path = output / 'classes.dex'
    print(dex_path)

    source = root / 'display'
    version = read_module_version(source / 'module.prop')
    module = root / 'dist' / f'bootscope-display-probe-{version}.zip'
    with ZipFile(module, 'w', ZIP_DEFLATED) as archive:
        for name in ('module.prop', 'service.sh', 'run.sh'):
            mode = 0o100755 if name.endswith('.sh') else 0o100644
            write_text_file(archive, name, source / name, mode)
        write_zip_entry(archive, 'classes.dex', dex_path.read_bytes(), 0o100444)
    print(module)


if __name__ == '__main__':
    main()
