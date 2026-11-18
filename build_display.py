"""Compile the experimental screen probe to a standalone app_process DEX."""
import os
from pathlib import Path
import subprocess
import tempfile
from zipfile import ZIP_DEFLATED, ZipFile

root = Path(__file__).resolve().parent
default_sdk = Path(os.environ.get('LOCALAPPDATA', '.')) / 'Android' / 'Sdk'
sdk = Path(os.environ.get('ANDROID_HOME') or os.environ.get('ANDROID_SDK_ROOT') or default_sdk)
android_jar = sdk / 'platforms' / 'android-36' / 'android.jar'
d8 = sdk / 'build-tools' / '36.0.0' / ('d8.bat' if os.name == 'nt' else 'd8')
output = root / 'dist' / 'bootscope-display'
output.mkdir(parents=True, exist_ok=True)
with tempfile.TemporaryDirectory() as temp:
    sources = sorted((root / 'display').glob('*.java'))
    subprocess.run(['javac', '--release', '8', '-Xlint:-options', '-classpath', str(android_jar),
                    '-d', temp, *map(str, sources)], check=True)
    classes = sorted((Path(temp) / 'bootscope').glob('*.class'))
    subprocess.run([str(d8), '--min-api', '33', '--lib', str(android_jar),
                    '--output', str(output), *map(str, classes)],
                   check=True)
print(output / 'classes.dex')
source = root / 'display'
version = next(line.removeprefix('version=') for line in
               (source / 'module.prop').read_text(encoding='utf-8').splitlines()
               if line.startswith('version='))
module = root / 'dist' / f'bootscope-display-probe-{version}.zip'
with ZipFile(module, 'w', ZIP_DEFLATED) as archive:
    for name in ('module.prop', 'service.sh'):
        archive.writestr(name, (source / name).read_bytes().replace(b'\r\n', b'\n'))
    archive.write(output / 'classes.dex', 'classes.dex')
print(module)
