"""Check the actual manual-rebuild predicate with controlled mod/plugin states and synthetic files.

python tests/removed_plugin_rebuild_check.py --cc clang [--source /path/to/baseline]
No game files, plugin binaries, rebuild processes or external services are used.
"""
import argparse
import hashlib
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def complete_function(source, signature):
    if source.count(signature) != 1:
        raise RuntimeError(f'Production function boundary changed: {signature}')
    start = source.index(signature)
    opening = source.index('{', start)
    depth = 1
    index = opening + 1
    quote = None
    while index < len(source):
        char = source[index]
        if quote:
            if char == '\\':
                index += 2
                continue
            if char == quote:
                quote = None
        elif source.startswith('//', index):
            newline = source.find('\n', index)
            index = len(source) if newline < 0 else newline + 1
            continue
        elif source.startswith('/*', index):
            closing = source.find('*/', index + 2)
            if closing < 0:
                raise RuntimeError('Unterminated production source comment')
            index = closing + 2
            continue
        elif char in ('"', "'"):
            quote = char
        elif char == '{':
            depth += 1
        elif char == '}':
            depth -= 1
            if not depth:
                return source[start:index + 1]
        index += 1
    raise RuntimeError(f'Incomplete production function: {signature}')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--source', type=Path, default=ROOT, help='repository source to test (default: this checkout)')
    args = parser.parse_args()
    root = args.source.resolve()
    path = root / 'runtime/cheats.c'
    source = path.read_text(encoding='utf-8')
    extracted = '\n\n'.join(complete_function(source, signature) for signature in
                            ('int cheats_rebuild_pending(void)', 'void cheats_rebuild_allow(void)'))
    print('Cheat source SHA256:', hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='totk-removed-plugin-') as temp:
        folder = Path(temp)
        (folder / 'rebuild_functions.c').write_text(extracted, encoding='utf-8')
        executable = folder / ('check.exe' if os.name == 'nt' else 'check')
        command = [args.cc, '-O2', '-march=x86-64-v3', '-I', str(root / 'runtime'), '-I', str(folder),
                   str(Path(__file__).with_suffix('.c')), '-o', str(executable)]
        subprocess.run(command, check=True, timeout=120)
        data = folder / 'data'
        (data / 'build-cache').mkdir(parents=True)
        result = subprocess.run([str(executable), str(data)], timeout=30)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()