"""Guest yields must preserve virtual-core handoffs without yielding to macOS."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
from macos.build import stage_sources


def main():
    runtime = stage_sources() / 'runtime'
    with tempfile.TemporaryDirectory(prefix='native-threads-', dir=ROOT / 'build') as temporary:
        binary = Path(temporary) / 'threads'
        subprocess.run(['xcrun', 'clang', '-std=gnu11', '-O2', '-g',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-Wno-format', '-Wno-unused-parameter', '-Wno-unused-function',
                        '-I', str(runtime),
                        '-I', str(runtime.parent / 'third_party/d2r_gfx/gfx/nvn/include'),
                        str(ROOT / 'tests/macos_thread_test.c'),
                        '-o', str(binary)], check=True)
        env = dict(os.environ)
        for key in ('TOTK_LISTCHECK', 'TOTK_DUMP_AFTER'):
            env.pop(key, None)
        for args in ([], ['--backoff']):
            subprocess.run([binary, *args], env=env, check=True, timeout=30)


if __name__ == '__main__':
    main()
