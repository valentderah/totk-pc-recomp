"""Local development RSF package integrity and recoverable installation, without downloads."""
import hashlib
import json
import os
import re
import shutil
import struct
import uuid
from pathlib import Path, PurePosixPath

NAME = 'ReScaleFrame'
# Producer source roles map to package-relative names; consumers use only the destinations.
SOURCES = {'ReScaleFrame.Runtime.dll': ('runtime', '')}
for name in ('sl.interposer.dll', 'sl.common.dll', 'sl.pcl.dll', 'sl.reflex.dll',
             'sl.dlss.dll', 'sl.dlss_g.dll', 'nvngx_dlss.dll', 'nvngx_dlssg.dll',
             'nvngx_dlss.license.txt', 'reflex.license.txt', 'streamline-sdk-build.json'):
    SOURCES['streamline/' + name] = ('streamline', name)
for name in ('amd_fidelityfx_vk.dll', 'fsr3-sdk-build.json'):
    SOURCES['fidelityfx/' + name] = ('fidelityfx', name)
for name in ('amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
             'amd_fidelityfx_framegeneration_dx12.dll'):
    SOURCES['fidelityfx/' + name] = ('source', 'vendor/fidelityfx/Kits/FidelityFX/signedbin/' + name)
for name in ('libxess.dll', 'libxess_fg.dll', 'libxell.dll'):
    SOURCES['xess/' + name] = ('source', 'vendor/xess/bin/' + name)
SOURCES.update({
    'licenses/ReScaleFrame.txt': ('source', 'LICENSE'),
    'licenses/Streamline.txt': ('streamline_source', 'license.txt'),
    'licenses/Streamline-third-party.md': ('streamline_source', '3rd-party-licenses.md'),
    'licenses/FidelityFX-Vulkan.txt': ('source', 'references/godot/fidelityfx-vulkan/LICENSE.txt'),
    'licenses/FidelityFX-third-party.md': ('source', 'vendor/fidelityfx/3rdpartynotice.md'),
    'licenses/XeSS.txt': ('source', 'vendor/xess/LICENSE.txt'),
    'licenses/XeSS-third-party.txt': ('source', 'vendor/xess/third-party-programs.txt'),
})
REQUIRED = set(SOURCES)
RESERVED_NAMES = {'CON', 'PRN', 'AUX', 'NUL'} | {f'{kind}{i}' for kind in ('COM', 'LPT') for i in range(1, 10)}

def file_sha256(path, progress=None):
    """SHA-256 hex of a file; progress(done, total) after each block."""
    h = hashlib.sha256()
    total = path.stat().st_size
    done = 0
    with open(path, 'rb', buffering=0) as f:
        buf = bytearray(8 << 20)
        view = memoryview(buf)
        while n := f.readinto(buf):
            h.update(view[:n])
            done += n
            if progress:
                progress(done, total)
    return h.hexdigest()


digest = file_sha256


def ordinary(path):
    info = path.lstat()
    if path.is_symlink() or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise ValueError(f'links/reparse points are not package inputs: {path}')


def safe_package_name(name):
    if not name or '\\' in name or ':' in name or PurePosixPath(name).is_absolute():
        return False
    for part in name.split('/'):
        if (not part or part in ('.', '..') or part.rstrip(' .') != part or
                any(c in part for c in '\x00<>"|?*') or part.split('.')[0].upper() in RESERVED_NAMES):
            return False
    return True


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
        if not safe_package_name(name) or name.casefold() == 'manifest.json' or name.casefold() in folded:
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
    validate_receipts(package, files)
    return files


def preflight(source, dest):
    source = Path(source).absolute()
    files = validate(source)
    target = Path(dest).absolute() / NAME
    same = source.resolve() == target.resolve()
    if (target.exists() or target.is_symlink()) and not same:
        validate(target)
    return files, target, same


def install(source, dest):
    """Validate everything first; keep old owned package recoverable on every replacement failure."""
    source, dest = Path(source).absolute(), Path(dest).absolute()
    files, target, same = preflight(source, dest)
    identity = digest(source / 'manifest.json')
    if same:
        return identity
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


def validate_receipts(package, files, *, streamline=None, fidelityfx=None):
    sl = streamline if streamline is not None else json.loads((package / 'streamline/streamline-sdk-build.json').read_text(encoding='utf-8-sig'))
    ffx = fidelityfx if fidelityfx is not None else json.loads((package / 'fidelityfx/fsr3-sdk-build.json').read_text(encoding='utf-8-sig'))
    if not isinstance(sl, dict) or not isinstance(sl.get('binaries'), dict) or not isinstance(ffx, dict):
        raise ValueError('invalid provider receipt object')
    if any(not isinstance(sl['binaries'].get(n), str) or sl['binaries'][n].lower() != files['streamline/' + n] for n in ('sl.common.dll', 'sl.interposer.dll')):
        raise ValueError('Streamline receipt does not match package')
    if ffx.get('schema') != 2 or not isinstance(ffx.get('runtimeSha256'), str) or ffx['runtimeSha256'].lower() != files['fidelityfx/amd_fidelityfx_vk.dll']:
        raise ValueError('FidelityFX receipt does not match package')


def select_source(*, explicit=None, opt_out=False, bundled=None, installed=None, recorded=None, prefill=False):
    """Choose a runtime; GUI prefill prefers an existing installed manifest to the bundle."""
    if opt_out and not (prefill and explicit is not None):
        return None
    if explicit is not None:
        return explicit if prefill else Path(explicit).absolute()
    candidates = ((installed, True), (bundled, False)) if prefill else ((bundled, False), (installed, False))
    for candidate, need_manifest in candidates:
        if candidate is None:
            continue
        candidate = Path(candidate)
        if prefill:
            exists = (candidate / 'manifest.json').is_file() if need_manifest else candidate.is_dir()
        else:
            exists = candidate.exists() or candidate.is_symlink()
        if exists:
            return candidate
    return Path(recorded).absolute() if recorded and Path(recorded).is_dir() else None


def replace_file(src, dst, *, verify=None, suffix='.tmp', preserve_metadata=True):
    """Copy via a temporary sibling and verify it before atomically replacing the destination."""
    temporary = dst.with_name(dst.name + suffix)
    (shutil.copy2 if preserve_metadata else shutil.copyfile)(src, temporary)
    if verify is not None:
        verify(temporary)
    os.replace(temporary, dst)
