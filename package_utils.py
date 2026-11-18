"""Shared helpers for building KernelSU module ZIP archives with Unix modes."""
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile, ZipInfo


def read_module_version(prop_path: Path) -> str:
    """Return the version field from a KernelSU module.prop file."""
    metadata = prop_path.read_text(encoding='utf-8')
    for line in metadata.splitlines():
        if line.startswith('version='):
            return line.removeprefix('version=').strip()
    raise ValueError(f'Missing version= in {prop_path}')


def write_zip_entry(archive: ZipFile, name: str, data: bytes, mode: int) -> None:
    """Write bytes to a ZipFile entry with explicit Unix file permissions."""
    info = ZipInfo(name)
    info.create_system = 3
    info.external_attr = mode << 16
    info.compress_type = ZIP_DEFLATED
    archive.writestr(info, data)


def write_text_file(archive: ZipFile, name: str, source: Path, mode: int) -> None:
    """Write a text file to the archive with LF line endings and Unix permissions."""
    write_zip_entry(archive, name, source.read_bytes().replace(b'\r\n', b'\n'), mode)
