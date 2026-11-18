"""Package the test module with predictable paths and Unix executable modes."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo

root = Path(__file__).resolve().parent
metadata = (root / 'module' / 'module.prop').read_text(encoding='utf-8')
version = next(line.removeprefix('version=') for line in metadata.splitlines()
               if line.startswith('version='))
output = root / 'dist' / f'bootscope-probe-{version}.zip'
output.parent.mkdir(exist_ok=True)
with ZipFile(output, 'w', ZIP_DEFLATED) as archive:
    for name in ('module.prop', 'service.sh', 'probe.sh'):
        info = ZipInfo(name)
        info.create_system = 3
        info.external_attr = ((0o100755 if name.endswith('.sh') else 0o100644) << 16)
        info.compress_type = ZIP_DEFLATED
        archive.writestr(info, (root / 'module' / name).read_bytes().replace(b'\r\n', b'\n'))
print(output)
