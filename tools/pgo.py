"""Merge the raw profiles of an instrumented build into the .profdata that build_game.py --pgo FILE reads.

    python tools/pgo.py merge build/v143-pgogen --version v143 [--out build/pgo/pgo-v143.profdata]

An instrumented build (build_game.py --pgo gen) writes <build>/pgo/default_<id>.profraw while totk.exe runs (every
minute and at exit); successive runs of the same exe add to that file. llvm-profdata comes from the llvm-mingw toolchain
the instrumented build used: its raw format is tied to the compiler version.

Beside the profile goes <out>.json, the training record: the game version and the git tree of recomp/ at the merge.
The profile matches the generated C of that generator; tools/make_release.py ships it only while recomp/ is unchanged,
since llvm-profdata matches functions by name and control-flow hash and a regenerated function silently loses its
counts. Merge right after the training runs, from the commit they were built at.
"""
import argparse
import datetime
import json
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
EXE = '.exe' if sys.platform == 'win32' else ''
DEFAULT_DIR = ROOT / 'build' / 'pgo'   # tools/make_release.py takes pgo-<version>.profdata from here


def llvm_profdata(toolchain):
    if toolchain:
        tool = toolchain / 'bin' / f'llvm-profdata{EXE}'
    else:
        found = sorted((ROOT / 'toolchain').glob(f'llvm-mingw-*/llvm-mingw-*/bin/llvm-profdata{EXE}'))
        tool = found[-1] if found else Path(f'llvm-profdata{EXE}')
    if not tool.is_file():
        sys.exit(f'{tool} not found: pass --toolchain')
    return tool


def recomp_tree():
    """The git tree id of recomp/ at HEAD; None when recomp/ has uncommitted changes."""
    dirty = subprocess.run(['git', '-C', str(ROOT), 'status', '--porcelain', '--', 'recomp'], capture_output=True,
                           text=True).stdout.strip()
    if dirty:
        return None
    return subprocess.run(['git', '-C', str(ROOT), 'rev-parse', 'HEAD:recomp'], capture_output=True,
                          text=True).stdout.strip() or None


def merge(build, out, version, toolchain):
    raw = sorted((build / 'pgo').glob('*.profraw'))
    if not raw:
        sys.exit(f'no .profraw in {build / "pgo"}: run the instrumented totk.exe first')
    tree = recomp_tree()
    if not tree:
        sys.exit('recomp/ has uncommitted changes: the record could not name the generator the profile matches')
    out.parent.mkdir(parents=True, exist_ok=True)
    tmp = out.with_suffix('.profdata.tmp')
    cmd = [str(llvm_profdata(toolchain)), 'merge', '--failure-mode=all', '-o', str(tmp), *map(str, raw)]
    p = subprocess.run(cmd, capture_output=True, text=True)
    if p.returncode:
        tmp.unlink(missing_ok=True)
        sys.exit(f'llvm-profdata merge failed: {(p.stdout + p.stderr).strip()[-2000:]}')
    tmp.replace(out)
    record = {'version': version, 'recomp_tree': tree, 'merged': datetime.date.today().isoformat(),
              'raw_files': len(raw)}
    out.with_suffix('.json').write_text(json.dumps(record, indent=1) + '\n', encoding='utf-8')
    size = sum(f.stat().st_size for f in raw)
    print(f'{out}: {len(raw)} raw files ({size / 2**20:.1f} MiB) -> {out.stat().st_size / 2**20:.1f} MiB; '
          f'recomp tree {tree[:12]}')


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    m = sub.add_parser('merge', help='merge <build>/pgo/*.profraw into one .profdata and its training record')
    m.add_argument('build', type=Path, help='the instrumented build directory')
    m.add_argument('--version', required=True, help='game version the build was made for (v121, v143)')
    m.add_argument('--out', type=Path, help=f'default {DEFAULT_DIR.relative_to(ROOT).as_posix()}/pgo-<version>.profdata')
    m.add_argument('--toolchain', type=Path, help='llvm-mingw directory (default: the newest under toolchain/)')
    a = ap.parse_args()
    out = (a.out or DEFAULT_DIR / f'pgo-{a.version}.profdata').resolve()
    merge(a.build.resolve(), out, a.version, a.toolchain)


if __name__ == '__main__':
    main()
