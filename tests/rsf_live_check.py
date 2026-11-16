"""Build and run SR/FG live-selection and startup contracts without game content or SDK binaries."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--cc', default=shutil.which('clang') or 'C:/msys64/ucrt64/bin/clang.exe')
    ap.add_argument('--deps', type=Path, help='optional dependency root containing Vulkan include/ and lib/')
    args = ap.parse_args()
    out = ROOT / 'build/checks/rsf-live'
    out.mkdir(parents=True, exist_ok=True)
    includes = [ROOT / 'runtime'] + [ROOT / 'external/ReScaleFrame' / d for d in
        ('runtime/orchestrator/include', 'runtime/contract/include', 'sdk/game/include')]
    if args.deps:
        includes.append(args.deps / 'include')
    flags = ['-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror']
    for directory in includes:
        flags += ['-I', str(directory)]
    for name, source in [('config', 'rsf_runtime'), ('startup', 'rsf_vulkan')]:
        exe = out / (name + ('.exe' if os.name == 'nt' else ''))
        command = [args.cc, *flags, str(ROOT / f'tests/rsf_live_{name}_check.c'),
                   str(ROOT / f'runtime/{source}.c'), '-o', str(exe)]
        if name == 'startup':
            if args.deps:
                command += ['-L', str(args.deps / 'lib')]
            command += ['-lvulkan-1' if os.name == 'nt' else '-lvulkan']
        subprocess.run(command, check=True)
        subprocess.run([str(exe)], check=True)
    subprocess.run([sys.executable, str(ROOT / 'tests/rsf_live_frame_check.py'), '--cc', args.cc], check=True)


if __name__ == '__main__':
    main()
