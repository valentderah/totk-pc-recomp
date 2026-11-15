"""tools/worktree.py against a scratch repo whose "main checkout" holds dummy ignored data. No game files needed.

Proves that marker files in the linked directories and in the linked file survive:
  1. tools/worktree.py remove,
  2. git worktree remove --force with the junctions still in place,
  3. git worktree prune after the worktree directory is deleted with its junctions in place.
Also checks that git status in a worktree shows no untracked or typechanged entry for the links.
Run with the repo's Python: tests/worktree_test.py
"""
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import worktree  # noqa: E402

IGNORED = ''.join(f'/{p}\n' for p in [d + '/' for d in worktree.LINK_DIRS] + worktree.LINK_FILES + ['.worktrees/'])


def git(cwd, *a):
    r = subprocess.run(['git', '-C', str(cwd), *a], capture_output=True, text=True)
    assert r.returncode == 0, f'git {a}: {r.stderr}'
    return r.stdout


def tool(scratch, *a):
    r = subprocess.run([sys.executable, str(scratch / 'tools' / 'worktree.py'), *a], capture_output=True, text=True)
    assert r.returncode == 0, f'worktree.py {a}: {r.stdout}{r.stderr}'
    return r.stdout


def markers(main):
    return [main / d / 'deep' / 'marker.txt' for d in worktree.LINK_DIRS if (main / d).is_dir()] + \
           [main / f for f in worktree.LINK_FILES]


def check(main, what):
    for m in markers(main):
        assert m.is_file() and m.read_text() == 'keep', f'{what}: {m} lost'
    print(f'ok: {what}: {len(markers(main))} markers intact')


def make_repo(base):
    main = base / 'main'
    (main / 'tools').mkdir(parents=True)
    shutil.copy(ROOT / 'tools' / 'worktree.py', main / 'tools' / 'worktree.py')
    (main / '.gitignore').write_text(IGNORED)
    (main / 'README.md').write_text('x\n')
    git(main, 'init', '-b', 'main')
    git(main, 'config', 'user.email', 't@t')
    git(main, 'config', 'user.name', 't')
    git(main, 'add', '.')
    git(main, 'commit', '-m', 'init')
    for d in worktree.LINK_DIRS:
        if d == 'game':
            continue    # absent in a real checkout too: link only what exists
        (main / d / 'deep').mkdir(parents=True)
        (main / d / 'deep' / 'marker.txt').write_text('keep')
    for f in worktree.LINK_FILES:
        (main / f).write_text('keep')
    return main


def main():
    base = Path(tempfile.mkdtemp(prefix='wt-test-')).resolve()
    try:
        main_dir = make_repo(base)
        check(main_dir, 'setup')

        # tools/worktree.py new / status / remove
        wt = Path(tool(main_dir, 'new', 'a').strip())
        assert wt.resolve() == (main_dir / '.worktrees' / 'a').resolve(), wt
        for d in worktree.LINK_DIRS:
            if (main_dir / d).is_dir():
                assert os.path.isjunction(wt / d), d
        assert (wt / 'build' / 'deep' / 'marker.txt').read_text() == 'keep'
        assert os.path.samefile(wt / 'config.local.toml', main_dir / 'config.local.toml')
        status = git(wt, 'status', '--porcelain', '--ignored=no')
        assert status == '', f'links show in git status: {status!r}'
        assert git(wt, 'ls-files', '--others', '--exclude-standard') == ''
        print('ok: git status in the worktree is clean, links are ignored')
        assert '.worktrees/a' in tool(main_dir, 'list').replace(os.sep, '/')
        (wt / 'dirty.txt').write_text('x')
        r = subprocess.run([sys.executable, str(main_dir / 'tools' / 'worktree.py'), 'remove', 'a'],
                           capture_output=True, text=True)
        assert r.returncode != 0 and (wt / 'build').is_dir(), 'remove must refuse a dirty worktree'
        check(main_dir, 'dirty remove refused')
        (wt / 'dirty.txt').unlink()
        tool(main_dir, 'remove', 'a')
        assert not wt.exists()
        check(main_dir, 'tools/worktree.py remove')

        # unmerged branch: --delete-branch refuses, --force deletes
        wt = Path(tool(main_dir, 'new', 'b').strip())
        (wt / 'f.txt').write_text('x')
        git(wt, 'add', 'f.txt')
        git(wt, 'commit', '-m', 'work')
        r = subprocess.run([sys.executable, str(main_dir / 'tools' / 'worktree.py'), 'remove', 'b', '--delete-branch'],
                           capture_output=True, text=True)
        assert r.returncode != 0 and wt.is_dir(), 'unmerged branch must block --delete-branch'
        git(main_dir, 'merge', '--ff-only', 'task/b')
        tool(main_dir, 'remove', 'b', '--delete-branch')
        assert 'task/b' not in git(main_dir, 'branch', '--list')
        check(main_dir, 'remove --delete-branch after merge')

        # raw git: worktree remove --force with the links in place
        wt = Path(tool(main_dir, 'new', 'c').strip())
        git(main_dir, 'worktree', 'remove', '--force', str(wt))
        check(main_dir, 'raw git worktree remove --force')

        # raw: delete the worktree directory with the junctions in place, then prune
        wt = Path(tool(main_dir, 'new', 'e').strip())
        shutil.rmtree(wt)
        git(main_dir, 'worktree', 'prune')
        check(main_dir, 'rmtree of a linked worktree, then git worktree prune')
        print('PASS')
    finally:
        # never follow a junction while cleaning up
        for w in (base / 'main' / '.worktrees').glob('*'):
            for d in worktree.LINK_DIRS:
                worktree.drop_link(w / d)
        shutil.rmtree(base, ignore_errors=True)


if __name__ == '__main__':
    main()
