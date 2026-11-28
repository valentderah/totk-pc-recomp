"""Check the Amiibo directory functions against real C++ iterator errors on Linux.

python3 tests/amiibo_directory_check.py [--cc clang++] [--source /path/to/baseline]
Uses synthetic filenames and bytes; no amiibo dump or game data is loaded.
"""
import argparse
import hashlib
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source', type=Path, default=ROOT, help='repository source to test (default: this checkout)')
    parser.add_argument('--cc', default='clang++')
    args = parser.parse_args()
    if not sys.platform.startswith('linux'):
        print('SKIP: directory-error injection uses Linux /proc and readdir interposition')
        return
    root = args.source.resolve()
    path = root / 'runtime/ui/amiibo_page.cpp'
    source = path.read_text(encoding='utf-8')
    tab = 'void tab_amiibo(float scale) {'
    tail = '// Collect amiibo filenames for font glyph building'
    if source.count(tab) != 1 or source.count(tail) != 1 or source.index(tail) <= source.index(tab):
        raise RuntimeError('Amiibo function source boundaries changed')
    # Compile the complete browser/collector implementations, omitting only the unrelated ImGui drawing function.
    extracted = source[:source.index(tab)] + source[source.index(tail):]
    for signature in ('static void refresh_dir(void)', 'void amiibo_collect_filenames('):
        if extracted.count(signature) != 1:
            raise RuntimeError('Complete production directory function missing')
    print('Amiibo source SHA256:', hashlib.sha256(path.read_bytes()).hexdigest(), flush=True)
    with tempfile.TemporaryDirectory(prefix='totk-amiibo-directory-') as temp:
        folder = Path(temp)
        (folder / 'amiibo_functions.cpp').write_text(extracted, encoding='utf-8')
        executable = folder / 'check'
        command = [args.cc, '-std=c++17', '-O2', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-Wl,--export-dynamic',
                   '-I', str(root / 'runtime'), '-I', str(root / 'runtime/third_party/imgui'),
                   '-I', str(folder), str(Path(__file__).with_suffix('.cpp')), '-ldl', '-o', str(executable)]
        subprocess.run(command, check=True, timeout=120)
        result = subprocess.run([str(executable), str(folder / 'files')], timeout=30)
        raise SystemExit(result.returncode)


if __name__ == '__main__':
    main()