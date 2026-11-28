"""Check production Discord frame parsing with in-memory IPC; no Discord client is contacted.

python tests/discord_frames_check.py --cc clang [--source /path/to/baseline]
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--source', type=Path, default=ROOT, help='repository source to test (default: this checkout)')
    args = parser.parse_args()
    root = args.source.resolve()
    path = root / 'runtime/discord_rpc.c'
    source = path.read_text(encoding='utf-8')
    marker = "// The first of Discord's pipes"
    if source.count(marker) != 1:
        raise RuntimeError('Discord frame source boundary changed')
    prefix = source[:source.index(marker)]
    for signature in ('static int read_one_frame(', 'static int read_frame('):
        if prefix.count(signature) != 1:
            raise RuntimeError('Complete production frame reader missing')
    print('Discord source SHA256:', hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='totk-discord-frames-') as temp:
        folder = Path(temp)
        (folder / 'discord_frames.c').write_text(prefix, encoding='utf-8')
        executable = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(root / 'runtime'), '-I', str(folder),
                   str(Path(__file__).with_suffix('.c')), '-o', str(executable)]
        if os.name == 'nt':
            command += ['-Wl,--exclude-all-symbols']
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable)], timeout=30)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()