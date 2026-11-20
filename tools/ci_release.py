"""Validate release inputs and packages; keep Release notes identical to CHANGELOG."""
import argparse
import json
from pathlib import Path, PurePosixPath
import re
import struct
import zipfile


def properties(text: str) -> dict[str, str]:
    return dict(
        line.split('=', 1)
        for line in text.splitlines()
        if '=' in line and not line.startswith('#')
    )


def release_notes(changelog: str, version: str) -> str:
    headings = list(re.finditer(r'^## \[([^\]]+)\][^\n]*$', changelog, re.M))
    matches = [i for i, h in enumerate(headings) if h[1] == version]
    if len(matches) != 1:
        raise ValueError(f'Expected exactly one CHANGELOG section for {version}')
    i = matches[0]
    heading = headings[i]
    if heading[0] != f'## [{version}]':
        raise ValueError('Release heading must not contain a date or suffix')
    end = headings[i + 1].start() if i + 1 < len(headings) else len(changelog)
    section = changelog[heading.start():end]
    section = re.split(r'^\[[^\]]+\]: ', section, maxsplit=1, flags=re.M)[0]
    categories = re.findall(r'^### (.+)$', section, re.M)
    allowed = {'新增', '变更', '弃用', '移除', '修复', '安全'}
    if not categories or any(cat not in allowed for cat in categories):
        raise ValueError('Use Chinese Keep a Changelog categories')
    if not re.search(r'^- \S', section, re.M):
        raise ValueError('Release section is empty')
    return section.rstrip() + '\n'


def prepare(root: Path, tag: str, output: Path) -> dict[str, str]:
    prop = properties((root / 'module/module.prop').read_text(encoding='utf-8'))
    version = prop.get('version', '')
    version_code = prop.get('versionCode', '')
    if not re.fullmatch(r'v\d+\.\d+\.\d+', version) or not re.fullmatch(r'[1-9]\d*', version_code):
        raise ValueError('Invalid module version or versionCode')
    if prop.get('author') != 'Linoleic' or not prop.get('updateJson'):
        raise ValueError('module.prop must define author=Linoleic and updateJson')

    upd = json.loads((root / 'update.json').read_text(encoding='utf-8'))
    if upd.get('version') != version or str(upd.get('versionCode')) != version_code:
        raise ValueError('update.json version/versionCode mismatch with module.prop')
    if not str(upd.get('zipUrl', '')).endswith(f'/bootscope-{version}.zip'):
        raise ValueError('update.json zipUrl mismatch')
    if not str(upd.get('changelog', '')).endswith('/CHANGELOG.md'):
        raise ValueError('update.json changelog URL must point to CHANGELOG.md')

    if tag and tag != version:
        raise ValueError(f'Tag {tag} does not match module version {version}')

    notes = release_notes((root / 'CHANGELOG.md').read_text(encoding='utf-8'), version)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(notes.encode('utf-8'))
    return prop


def verify_package(package: Path, prop: dict[str, str], revision: str) -> None:
    with zipfile.ZipFile(package) as z:
        names = z.namelist()
        if len({name.casefold() for name in names}) != len(names):
            raise ValueError('Duplicate ZIP entries')
        for name in names:
            path = PurePosixPath(name)
            if path.is_absolute() or '..' in path.parts or '\\' in name or ':' in name:
                raise ValueError(f'Unsafe ZIP path: {name}')
        if z.testzip():
            raise ValueError('ZIP CRC failure')

        packed = properties(z.read('module.prop').decode('utf-8'))
        if (
            packed.get('versionCode') != prop['versionCode']
            or not re.fullmatch(re.escape(prop['version']) + r'-r[1-9]\d*', packed.get('version', ''))
        ):
            raise ValueError('Package version mismatch or dirty build')
        if package.name != f"bootscope-{prop['version']}.zip":
            raise ValueError('Unexpected package filename')

        binary = z.read('bootscope')
        if binary[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', binary, 18)[0] != 183:
            raise ValueError('Expected arm64 ELF: bootscope')
        if revision.encode('ascii') not in binary:
            raise ValueError('Missing build revision in bootscope')
        if (z.getinfo('bootscope').external_attr >> 16) != 0o100755:
            raise ValueError('bootscope must have mode 0755')

        for req in ('LICENSE', 'webroot/index.html', 'initrc/bootscope.rc', 'rules.conf', 'module.prop'):
            if not z.read(req):
                raise ValueError(f'Empty required file: {req}')
        for name in names:
            if name.endswith('.sh'):
                if b'\r' in z.read(name):
                    raise ValueError(f'Shell script must use LF: {name}')
                if (z.getinfo(name).external_attr >> 16) != 0o100755:
                    raise ValueError(f'Shell script must have mode 0755: {name}')


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', default='')
    parser.add_argument('--package', type=Path)
    parser.add_argument('--revision', default='')
    parser.add_argument('--output', type=Path, default=Path('dist/release-notes.md'))
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    prop = prepare(root, args.tag, args.output)
    if args.package:
        if not re.fullmatch(r'[a-f0-9]{40}', args.revision):
            parser.error('--package requires a full --revision')
        verify_package(args.package, prop, args.revision)
    print(f"Validated {prop['version']} ({prop['versionCode']})")


if __name__ == '__main__':
    main()
