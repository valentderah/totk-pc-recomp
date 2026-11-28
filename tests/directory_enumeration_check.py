"""Check real platform directory enumeration and save commit failure handling without game data.

python tests/directory_enumeration_check.py --cc clang [--deps /path/to/deps]
"""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cc', required=True)
    parser.add_argument('--deps', type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='totk-dir-list-') as temp:
        folder = Path(temp)
        exe = folder / ('check.exe' if os.name == 'nt' else 'check')
        # The save transaction implementation is an exact prefix of fs_host.c. Its later guest overrides and
        # full-game probes are unrelated to this fixture and keep unresolved game dependencies alive on COFF.
        source = (ROOT / 'runtime/fs_host.c').read_text(encoding='utf-8')
        marker = '// ---- quick save (quicksave.c):'
        if source.count(marker) != 1:
            raise RuntimeError('save transaction source boundary changed')
        prefix = source[:source.index(marker)]
        if 'static uint32_t save_commit_locked(void)' not in prefix:
            raise RuntimeError('save transaction implementation missing')
        (folder / 'save_prefix.c').write_text(prefix, encoding='utf-8')
        command = [args.cc, '-O2', '-flto', '-march=x86-64-v3', '-ffunction-sections', '-fdata-sections',
                   '-Wl,--gc-sections', '-I', str(ROOT / 'runtime'), '-I', str(ROOT / 'runtime/platform'),
                   '-I', str(folder), '-DDIRECTORY_FS_SOURCE="save_prefix.c"']
        if args.deps:
            command += ['-I', str(args.deps / 'include'), '-L', str(args.deps / 'lib')]
        if os.name == 'nt':
            command += ['-D_WIN32_WINNT=0x0A00', '-Wl,--exclude-all-symbols']
        command += [str(ROOT / 'tests/directory_enumeration_check.c'), '-o', str(exe)]
        if os.name == 'nt':
            command += ['-lbcrypt', '-lpsapi', '-luser32', '-lwinmm', '-lole32', '-lshell32', '-luuid']
        else:
            command += ['-lpthread', '-ldl', '-lm']
        subprocess.run(command, check=True)
        data = folder / 'data'
        data.mkdir()
        subprocess.run([str(exe), str(data)], check=True, timeout=60)


if __name__ == '__main__':
    main()