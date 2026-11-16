"""Stage a complete local RSF vendor runtime beside a development game build.

This stages binaries/licenses and records hashes; it does not claim provider or
game acceptance and is not a release/source-distribution packager.
"""
import argparse
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


sys.path.insert(0, str(ROOT))
from installer.rsf_package import SOURCES, check_dll, digest, validate_receipts, replace_file


def stage(args):
    out = args.out.resolve()
    if not out.is_relative_to((ROOT / 'build').resolve()):
        raise ValueError('staging output must remain under this workspace build directory')
    files = [(getattr(args, role).resolve() / name if name else getattr(args, role).resolve(), Path(relative))
             for relative, (role, name) in SOURCES.items()]
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
    ffx = json.loads((args.fidelityfx / 'fsr3-sdk-build.json').read_text(encoding='utf-8-sig'))
    if ffx['sdkCommit'] != 'c6efa6bf7f2027b3ec94f28578bb5965eabb9e55':
        raise ValueError('corrected FidelityFX runtime does not match its receipt')
    validate_receipts(None, hashes, streamline=sl, fidelityfx=ffx)
    for existing in out.rglob('*.dll'):
        if existing.relative_to(out).as_posix() not in hashes:
            raise ValueError(f'unexpected stale DLL in staging directory: {existing}')
    for src, relative in files:
        dest = out / relative
        dest.parent.mkdir(parents=True, exist_ok=True)
        def verify(temporary):
            if digest(temporary) != hashes[relative.as_posix()]:
                raise ValueError(f'input changed during staging: {src}')
        replace_file(src, dest, verify=verify, suffix='.staging', preserve_metadata=False)
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
