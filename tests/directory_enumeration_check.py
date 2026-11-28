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
        failed = subprocess.run([str(exe), str(data)], timeout=60).returncode != 0
        recovery = folder / 'recovery'
        recovery.mkdir()
        result = subprocess.run([str(exe), str(recovery), 'recovery'], timeout=60)
        expected = {
            'committed/a': b'old-a', 'committed/b': b'old-b',
            'tx/ready-0000000001/a': b'ready-one', 'tx/ready-0000000002/a': b'ready-two',
            'source/a': b'new-a', 'source/b': b'new-b',
        }
        actual = {p.relative_to(recovery).as_posix(): p.read_bytes() for p in recovery.rglob('*') if p.is_file()}
        recovered_safely = result.returncode == 76 and actual == expected
        print(f'incomplete recovery scan stops before changing files: {recovered_safely}', flush=True)
        backups = folder / 'backups'
        backups.mkdir()
        failed |= subprocess.run([str(exe), str(backups), 'backups'], timeout=60).returncode != 0
        if failed or not recovered_safely:
            raise SystemExit(1)


if __name__ == '__main__':
    main()