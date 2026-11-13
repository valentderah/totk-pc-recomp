"""The blueprint picker's layout, link-tested against the real runtime/blueprint_picker.c.

This compiles the actual C file (not a copy of its rules) together with a small main that stubs the platform and UI
surface the picker touches, then asserts the geometry every band depends on: the three bands never overlap, the panel
stays inside the screen, the list has room, the scrollbar sits clear of the rows, and a scroll turn moves the thumb.

Run: .venv\\Scripts\\python tests\\picker_layout_test.py
"""
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')
MAIN = ROOT / 'tests' / 'picker_layout_main.c'
OUT = ROOT / 'build' / 'picker_layout_test.exe'

# The picker includes these headers; the platform and UI ones only need to resolve, their symbols are stubbed.
INCLUDES = ['-I', str(RUNTIME), '-I', str(ROOT / 'generated' / 'v143')]


def main():
    if not CLANG.exists():
        print('picker_layout_test: clang not found at %s' % CLANG)
        return 1
    out = OUT
    out.parent.mkdir(parents=True, exist_ok=True)
    sources = [str(MAIN),
               str(RUNTIME / 'blueprint_picker.c'),
               str(RUNTIME / 'blueprint_file.c'),
               str(RUNTIME / 'blueprint_cai.c'),
               str(RUNTIME / 'blueprint_txt.c'),
               str(RUNTIME / 'blueprint_inflate.c'),
]
    # Compile each source to an object, then link, because clang in this environment cannot make the temporary files a
    # single multi-input invocation needs.
    objs = []
    for i, src in enumerate(sources):
        obj = str(out.parent / ('picker_%d.o' % i))
        r = subprocess.run([str(CLANG), '-std=c11', '-O1', *INCLUDES, '-c', src, '-o', obj],
                           capture_output=True, text=True)
        if r.returncode != 0:
            print('picker_layout_test: compile failed for %s' % src)
            print(r.stdout)
            print(r.stderr)
            return 1
        objs.append(obj)
    r = subprocess.run([str(CLANG), *objs, '-o', str(out), '-lz', '-lzstd', '-lm'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print('picker_layout_test: link failed')
        print(r.stdout)
        print(r.stderr)
        return 1
    # The test links libzstd and the C++ runtime, whose DLLs live in MSYS2's ucrt64\bin; prepend it so the child finds
    # them (a missing DLL ends the child with STATUS_DLL_NOT_FOUND before it prints anything).
    env = dict(os.environ)
    env['PATH'] = str(CLANG.parent) + os.pathsep + env.get('PATH', '')
    run = subprocess.run([str(out)], capture_output=True, text=True, env=env)
    sys.stdout.write(run.stdout)
    if run.stderr:
        sys.stderr.write(run.stderr)
    return run.returncode


if __name__ == '__main__':
    sys.exit(main())
