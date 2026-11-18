"""Package the test module with predictable paths and Unix executable modes."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile

from package_utils import read_module_version, write_text_file


def main() -> None:
    root = Path(__file__).resolve().parent
    version = read_module_version(root / 'module' / 'module.prop')
    output = root / 'dist' / f'bootscope-probe-{version}.zip'
    output.parent.mkdir(exist_ok=True)
    with ZipFile(output, 'w', ZIP_DEFLATED) as archive:
        for name in ('module.prop', 'service.sh', 'probe.sh'):
            mode = 0o100755 if name.endswith('.sh') else 0o100644
            write_text_file(archive, name, root / 'module' / name, mode)
    print(output)


if __name__ == '__main__':
    main()
