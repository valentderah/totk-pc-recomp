"""The production custom cursor must not depend on uninitialized stack colours.

    python tests/cursor_art_check.py --cc path/to/clang
"""
import argparse
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif
#include "input_wsys.h"
int main(int argc, char **argv) {
    assert(argc == 2);
    int size = atoi(argv[1]), hot;
    assert(size > 0 && size <= 128);
    uint32_t *pixels = malloc((size_t)size * size * sizeof *pixels);
    assert(pixels);
    cursor_art(pixels, size, &hot);
#ifdef _WIN32
    assert(_setmode(_fileno(stdout), _O_BINARY) != -1);
#endif
    assert(fwrite(&hot, sizeof hot, 1, stdout) == 1);
    assert(fwrite(pixels, sizeof *pixels, (size_t)size * size, stdout) == (size_t)size * size);
    free(pixels);
    return 0;
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--cc', default=shutil.which('clang') or r'C:\msys64\ucrt64\bin\clang.exe')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='cursor-art-') as temp:
        work = Path(temp)
        source = work / 'probe.c'
        source.write_text(HARNESS, encoding='utf-8')
        outputs = {}
        for mode in ('zero', 'pattern'):
            exe = work / f'{mode}.exe'
            subprocess.run([args.cc, '-O2', '-std=gnu11', f'-ftrivial-auto-var-init={mode}',
                            '-I', str(ROOT / 'runtime'), str(source), str(ROOT / 'runtime/cursor_art.c'),
                            '-lm', '-o', str(exe)], check=True)
            outputs[mode] = {}
            for size in (16, 24, 32, 48, 64, 96, 128):
                data = subprocess.run([str(exe), str(size)], check=True, capture_output=True).stdout
                assert len(data) == 4 + size * size * 4
                pixels = struct.unpack(f'<{size * size}I', data[4:])
                assert any(p == 0 for p in pixels), 'the cursor must have transparent pixels'
                assert any(p >> 24 and p & 0xffffff for p in pixels), 'the cursor must contain visible colour'
                outputs[mode][size] = data
        differences = []
        for size in outputs['zero']:
            a, b = outputs['zero'][size], outputs['pattern'][size]
            different = sum(a[i:i + 4] != b[i:i + 4] for i in range(4, len(a), 4))
            if different or a[:4] != b[:4]:
                differences.append((size, different))
            print(f'cursor {size}x{size}: {different} pixels differ between zero and pattern stack initialization')
        assert not differences, f'cursor pixels depend on uninitialized automatic variables: {differences}'


if __name__ == '__main__':
    main()
