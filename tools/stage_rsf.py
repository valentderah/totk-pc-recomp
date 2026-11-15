"""Stage a complete local RSF vendor runtime beside a development game build.

This stages binaries/licenses and records hashes; it does not claim provider or
game acceptance and is not a release/source-distribution packager.
"""
import argparse
import hashlib
import json
import shutil
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def check_dll(path):
    with path.open('rb') as stream:
        header = stream.read(64)
        if len(header) != 64 or header[:2] != b'MZ':
            raise ValueError(f'{path}: not a native DLL (possibly an unresolved LFS pointer)')
        stream.seek(struct.unpack_from('<I', header, 60)[0])
        pe = stream.read(26)
        if len(pe) != 26 or pe[:4] != b'PE\0\0' or struct.unpack_from('<H', pe, 4)[0] != 0x8664 or not (
                struct.unpack_from('<H', pe, 22)[0] & 0x2000) or struct.unpack_from('<H', pe, 24)[0] != 0x20b:
            raise ValueError(f'{path}: expected a Windows x86-64 DLL')


def stage(args):
    out = args.out.resolve()
    if not out.is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('staging output must remain under this workspace build directory')
    source = args.source.resolve()
    files = [(args.runtime, Path('ReScaleFrame.Runtime.dll'))]
    streamline = ('sl.interposer.dll', 'sl.common.dll', 'sl.pcl.dll', 'sl.reflex.dll', 'sl.dlss.dll',
                  'sl.dlss_g.dll', 'nvngx_dlss.dll', 'nvngx_dlssg.dll', 'nvngx_dlss.license.txt',
                  'reflex.license.txt', 'streamline-sdk-build.json')
    files += [(args.streamline / name, Path('streamline') / name) for name in streamline]
    files += [(args.fidelityfx / name, Path('fidelityfx') / name)
              for name in ('amd_fidelityfx_vk.dll', 'fsr3-sdk-build.json')]
    signed = source / 'vendor/fidelityfx/Kits/FidelityFX/signedbin'
    files += [(signed / name, Path('fidelityfx') / name) for name in (
        'amd_fidelityfx_loader_dx12.dll', 'amd_fidelityfx_upscaler_dx12.dll',
        'amd_fidelityfx_framegeneration_dx12.dll')]
    files += [(source / 'vendor/xess/bin' / name, Path('xess') / name)
              for name in ('libxess.dll', 'libxess_fg.dll', 'libxell.dll')]
    files += [
        (source / 'LICENSE', Path('licenses/ReScaleFrame.txt')),
        (args.streamline_source / 'license.txt', Path('licenses/Streamline.txt')),
        (args.streamline_source / '3rd-party-licenses.md', Path('licenses/Streamline-third-party.md')),
        (source / 'references/godot/fidelityfx-vulkan/LICENSE.txt', Path('licenses/FidelityFX-Vulkan.txt')),
        (source / 'vendor/fidelityfx/3rdpartynotice.md', Path('licenses/FidelityFX-third-party.md')),
        (source / 'vendor/xess/LICENSE.txt', Path('licenses/XeSS.txt')),
        (source / 'vendor/xess/third-party-programs.txt', Path('licenses/XeSS-third-party.txt')),
    ]
    # Preflight every input before changing the package. Do not stage pointer files.
    hashes = {}
    for src, relative in files:
        if not src.is_file():
            raise ValueError(f'missing runtime input: {src}')
        if src.suffix.lower() == '.dll':
            check_dll(src)
        hashes[relative.as_posix()] = digest(src)
    sl = json.loads((args.streamline / 'streamline-sdk-build.json').read_text(encoding='utf-8-sig'))
    if sl['sdkCommit'] != '2122257e0fce486f91b385aa63b9a09b0a34b363':
        raise ValueError('unexpected corrected Streamline source revision')
    for name in ('sl.common.dll', 'sl.interposer.dll'):
        if hashes[f'streamline/{name}'] != sl['binaries'][name].lower():
            raise ValueError(f'corrected Streamline receipt differs from {name}')
    ffx = json.loads((args.fidelityfx / 'fsr3-sdk-build.json').read_text(encoding='utf-8-sig'))
    if ffx['schema'] != 2 or ffx['sdkCommit'] != 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55' or (
            hashes['fidelityfx/amd_fidelityfx_vk.dll'] != ffx['runtimeSha256'].lower()):
        raise ValueError('corrected FidelityFX runtime does not match its receipt')
    for existing in out.rglob('*.dll'):
        if existing.relative_to(out).as_posix() not in hashes:
            raise ValueError(f'unexpected stale DLL in staging directory: {existing}')
    for src, relative in files:
        dest = out / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        temporary = dest.with_name(dest.name + '.staging')
        shutil.copyfile(src, temporary)
        if digest(temporary) != hashes[relative.as_posix()]:
            raise ValueError(f'input changed during staging: {src}')
        temporary.replace(dest)
    manifest = {'schema': 1, 'purpose': 'development validation', 'files': hashes}
    (out / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n', encoding='utf-8')
    print(f'Staged {len(files)} RSF runtime/license files at {out}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT / 'external' / 'ReScaleFrame')
    parser.add_argument('--runtime', type=Path, required=True)
    parser.add_argument('--streamline', type=Path, required=True)
    parser.add_argument('--streamline-source', type=Path, required=True)
    parser.add_argument('--fidelityfx', type=Path, required=True)
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    try:
        stage(args)
    except (OSError, ValueError, KeyError) as error:
        parser.exit(1, f'stage_rsf: {error}\n')


if __name__ == '__main__':
    main()
