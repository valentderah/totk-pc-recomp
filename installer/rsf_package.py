"""Local development RSF package integrity and recoverable installation, without downloads."""
import hashlib
import json
import os
import re
import shutil
import struct
import uuid
from pathlib import Path, PurePosixPath

REQUIRED = {'ReScaleFrame.Runtime.dll'}
REQUIRED.update('streamline/' + n for n in ('sl.interposer.dll', 'sl.common.dll', 'sl.pcl.dll', 'sl.reflex.dll',
    'sl.dlss.dll', 'sl.dlss_g.dll', 'nvngx_dlss.dll', 'nvngx_dlssg.dll', 'nvngx_dlss.license.txt',
    'reflex.license.txt', 'streamline-sdk-build.json'))
REQUIRED.update('fidelityfx/' + n for n in ('amd_fidelityfx_vk.dll', 'fsr3-sdk-build.json',
    'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll', 'amd_fidelityfx_framegeneration_dx12.dll'))
REQUIRED.update('xess/' + n for n in ('libxess.dll', 'libxess_fg.dll', 'libxell.dll'))
REQUIRED.update('licenses/' + n for n in ('ReScaleFrame.txt', 'Streamline.txt', 'Streamline-third-party.md',
    'FidelityFX-Vulkan.txt', 'FidelityFX-third-party.md', 'XeSS.txt', 'XeSS-third-party.txt'))


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def ordinary(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError(f'links/reparse points are not package inputs: {path}')


def manifest(package):
    ordinary(package)
    path = package / 'manifest.json'
    ordinary(path)
    if path.stat().st_size > 2 << 20:
        raise ValueError('RSF manifest is too large')
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError(f'duplicate manifest key: {key}')
            result[key] = value
        return result
    data = json.loads(path.read_text(encoding='utf-8-sig'), object_pairs_hook=unique)
    if not isinstance(data, dict) or data.get('schema') != 1 or data.get('purpose') != 'development validation' or not isinstance(data.get('files'), dict):
        raise ValueError('expected schema1 development validation manifest')
    files, folded = data['files'], set()
    if not REQUIRED <= files.keys():
        raise ValueError('incomplete RSF runtime/provider/license package: ' + ', '.join(sorted(REQUIRED - files.keys())))
    for name, hash_value in files.items():
        parts = name.split('/')
        if (not name or '\\' in name or ':' in name or PurePosixPath(name).is_absolute() or
            any(not p or p in ('.', '..') or p.rstrip(' .') != p or any(c in p for c in '\x00<>\"|?*') or
                p.split('.')[0].upper() in {'CON', 'PRN', 'AUX', 'NUL', *(f'COM{i}' for i in range(1, 10)),
                                           *(f'LPT{i}' for i in range(1, 10))} for p in parts) or
            name.casefold() == 'manifest.json' or name.casefold() in folded):
            raise ValueError(f'unsafe/colliding package path: {name}')
        folded.add(name.casefold())
        if not isinstance(hash_value, str) or not re.fullmatch('[0-9a-f]{64}', hash_value):
            raise ValueError(f'invalid SHA256 for {name}')
    return files


def check_dll(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError(f'not a native DLL: {path.name}')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        pe = stream.read(26)
        if (len(pe) != 26 or pe[:4] != b'PE\0\0' or struct.unpack_from('<H', pe, 4)[0] != 0x8664 or
            not struct.unpack_from('<H', pe, 22)[0] & 0x2000 or struct.unpack_from('<H', pe, 24)[0] != 0x20b):
            raise ValueError(f'expected Windows AMD64 PE32+ DLL: {path.name}')


def validate(package):
    package = Path(package).absolute()
    files = manifest(package)
    expected = set(files) | {'manifest.json'}
    parents = {str(a) for n in files for a in PurePosixPath(n).parents if str(a) != '.'}
    for path in package.rglob('*'):
        ordinary(path)
        relative = path.relative_to(package).as_posix()
        if path.is_dir():
            if relative not in parents:
                raise ValueError(f'unknown package directory retained: {relative}')
        elif not path.is_file() or relative not in expected:
            raise ValueError(f'unknown package file retained: {relative}')
    for name, expected_hash in files.items():
        path = package / name
        for parent in path.relative_to(package).parents:
            ordinary(package / parent)
        ordinary(path)
        if not path.is_file() or digest(path) != expected_hash:
            raise ValueError(f'missing or changed package input: {name}')
        if name.lower().endswith('.dll'):
            check_dll(path)
        elif path.stat().st_size == 0:
            raise ValueError(f'empty package input: {name}')
    sl = json.loads((package / 'streamline/streamline-sdk-build.json').read_text(encoding='utf-8-sig'))
    ffx = json.loads((package / 'fidelityfx/fsr3-sdk-build.json').read_text(encoding='utf-8-sig'))
    if not isinstance(sl, dict) or not isinstance(sl.get('binaries'), dict) or not isinstance(ffx, dict):
        raise ValueError('invalid provider receipt object')
    if any(not isinstance(sl['binaries'].get(n), str) or sl['binaries'][n].lower() != files['streamline/' + n] for n in ('sl.common.dll', 'sl.interposer.dll')):
        raise ValueError('Streamline receipt does not match package')
    if ffx.get('schema') != 2 or not isinstance(ffx.get('runtimeSha256'), str) or ffx['runtimeSha256'].lower() != files['fidelityfx/amd_fidelityfx_vk.dll']:
        raise ValueError('FidelityFX receipt does not match package')
    return files


def install(source, dest):
    """Validate everything first; keep old owned package recoverable on every replacement failure."""
    source, dest = Path(source).absolute(), Path(dest).absolute()
    files = validate(source)
    identity = digest(source / 'manifest.json')
    target = dest / 'ReScaleFrame'
    if target.exists() or target.is_symlink():
        validate(target)  # Refuse unknown/player-modified files rather than replacing them.
        if source.resolve() == target.resolve():
            return digest(target / 'manifest.json')
    dest.mkdir(parents=True, exist_ok=True)
    stage = dest / ('.ReScaleFrame.stage-' + uuid.uuid4().hex)
    backup = dest / ('.ReScaleFrame.backup-' + uuid.uuid4().hex)
    stage.mkdir()
    try:
        for name in [*files, 'manifest.json']:
            output = stage / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source / name, output)
        if validate(stage) != files or digest(stage / 'manifest.json') != identity:
            raise ValueError('package changed during staging')
        old = target.exists()
        if old:
            os.replace(target, backup)
        try:
            os.replace(stage, target)
        except OSError as promotion:
            if old and not target.exists():
                try:
                    os.replace(backup, target)
                except OSError as restore:
                    raise OSError(f'package promotion failed; old package retained at {backup}; restore failed: {restore}') from promotion
            elif old:
                raise OSError(f'package promotion failed; old package retained at {backup}: {promotion}') from promotion
            raise
        return digest(target / 'manifest.json')
    finally:
        if stage.exists():
            ordinary(stage)
            if not stage.resolve().is_relative_to(dest.resolve()):
                raise OSError('staging directory escaped installation folder; retained for inspection')
            shutil.rmtree(stage)  # Only this invocation's unique staging directory.
