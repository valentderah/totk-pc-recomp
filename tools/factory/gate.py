"""The factory's gate: is a job branch's committed head fit for review?

    python tools/factory/gate.py <worktree> <gate.md>

Runs from the main checkout's copy, so a branch cannot change what judges it. Checks, in order:

1. the worktree has no uncommitted changes (the gate judges the head commit);
2. the branch modifies or deletes nothing under PROTECTED (adding files is fine);
3. tools/check_content.py of the main checkout passes on the worktree's tracked files, as CI runs it;
4. tests/run_all.py (the quick set) is green: every failing test fails the gate. A red test is either the branch's or
   one that is red on dev too; the gate says which, from the branch's merge base with origin/dev, so the builder knows
   whether to fix its change or to stop and report dev. Base results are cached per base commit and interpreter under
   build/factory/baseline/.

The tests run with the worktree's .venv (docs/building.md; tools/worktree.py links the main checkout's), else with the
Python that runs the gate. That interpreter must import unicorn, which the quick set's harnesses need: without it every
such test fails on the branch and on the base alike, and the gate would pass without having tested anything.

Writes <gate.md> (first line GATE: PASS or GATE: FAIL, then COMMIT: <sha>) and the full output next to it as
gate.log. Exit 0 on PASS, 1 on FAIL, 2 when the gate itself cannot run. Standard library only.
"""
import fnmatch
import hashlib
import json
import subprocess
import sys
import time
from pathlib import Path

from lock import hold

MAIN = Path(__file__).resolve().parents[2]
BASE_REF = 'origin/dev'
PROTECTED = ['tools/factory/*', 'tools/check_content.py', '.github/workflows/*']
TAIL = 60


def die(msg):
    print(f'gate: {msg}', file=sys.stderr)
    sys.exit(2)


def run(cmd, cwd):
    t0 = time.monotonic()
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, r.stdout + r.stderr, time.monotonic() - t0


def git(cwd, *args):
    code, out, _ = run(['git', *args], cwd)
    if code:
        die(f'git {" ".join(args)} failed: {out.strip()}')
    return out.strip()


def protected_changes(wt, base):
    hits = []
    for line in git(wt, 'diff', '--name-status', f'{base}...HEAD').splitlines():
        status, path = line.split('\t')[:2]  # for a rename, the old path
        if not status.startswith('A') and any(fnmatch.fnmatch(path, p) for p in PROTECTED):
            hits.append(f'{status[0]} {path}')
    return hits


def python_of(wt):
    """The interpreter for the tests: <wt>/.venv, else this one. Exits 2 when it cannot import unicorn."""
    for rel in ('.venv/Scripts/python.exe', '.venv/bin/python'):
        if (wt / rel).exists():
            py = str(wt / rel)
            break
    else:
        py = sys.executable
    code, out, _ = run([py, '-c', 'import unicorn'], wt)
    if code:
        die(f'{py} cannot import unicorn, which the run_all quick set needs; make the .venv of docs/building.md '
            f'(a junction to an existing one is fine) or run the gate with that Python')
    return py


def test_results(out):
    """{test: PASS | FAIL | SKIP} from the per-test lines of run_all.py."""
    res = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2 and line.startswith('  ') and parts[0] in ('PASS', 'FAIL', 'SKIP'):
            res[parts[1]] = parts[0]
    return res


def base_results(base, names, py):
    """run_all results of `names` at commit `base` with interpreter `py`, from the cache or a throwaway worktree."""
    tag_py = hashlib.sha256(str(Path(py).resolve()).lower().encode()).hexdigest()[:8]
    cache = MAIN / 'build' / 'factory' / 'baseline' / f'{base}-{tag_py}.json'
    known = json.loads(cache.read_text()) if cache.exists() else {}
    missing = [n for n in names if n not in known]
    if missing:
        tag = f'base-{base[:12]}'
        code, out, _ = run([sys.executable, 'tools/worktree.py', 'new', tag, '--base', base], MAIN)
        if code:
            die(f'cannot make the base worktree: {out.strip()}')
        try:
            _, out, _ = run([py, 'tests/run_all.py', '--only', ','.join(missing)], out.splitlines()[0])
            got = test_results(out)
            known.update({n: got.get(n, 'FAIL') for n in missing})
        finally:
            run([sys.executable, 'tools/worktree.py', 'remove', tag, '--delete-branch', '--force'], MAIN)
        cache.parent.mkdir(parents=True, exist_ok=True)
        cache.write_text(json.dumps(known, indent=1, sort_keys=True))
    return {n: known[n] for n in names}


def main(argv):
    if argv[:1] in (['-h'], ['--help']):
        print(__doc__.strip())
        return 0
    if len(argv) != 2:
        die(__doc__.strip())
    wt, md = Path(argv[0]).resolve(), Path(argv[1]).resolve()
    if not (wt / '.git').exists():
        die(f'{wt} is not a worktree')
    commit = git(wt, 'rev-parse', 'HEAD')
    base = git(wt, 'merge-base', BASE_REF, 'HEAD')
    problems, log, facts = [], [], []

    dirty = git(wt, 'status', '--porcelain')
    if dirty:
        problems.append('uncommitted changes in the worktree; commit them and run the gate again')
        log.append('$ git status --porcelain\n' + dirty)

    hits = protected_changes(wt, base)
    if hits:
        problems.append('protected paths changed: ' + ', '.join(hits))

    code, out, dt = run([sys.executable, str(MAIN / 'tools' / 'check_content.py')], wt)
    log.append(f'$ python tools/check_content.py  (exit {code}, {dt:.0f}s)\n{out}')
    facts.append(f'check_content.py: {"passed" if code == 0 else f"failed, exit {code}"}')
    if code:
        problems.append('tools/check_content.py failed: game content or generated code is tracked')

    py = python_of(wt)
    facts.append(f'tests run with {py}')
    with hold('tests', f'gate {wt.name}'):   # the quick set builds harnesses into the shared build/test/
        code, out, dt = run([py, 'tests/run_all.py'], wt)
        log.append(f'$ python tests/run_all.py  (exit {code}, {dt:.0f}s)\n{out}')
        results = test_results(out)
        if code and not results:
            problems.append(f'tests/run_all.py failed without test results, exit {code}')
        failed = sorted(n for n, s in results.items() if s == 'FAIL')
        new, known = [], []
        if failed:
            at_base = base_results(base, failed, py)
            new = [n for n in failed if at_base[n] != 'FAIL']
            known = [n for n in failed if at_base[n] == 'FAIL']
    counts = {s: sum(1 for v in results.values() if v == s) for s in ('PASS', 'FAIL', 'SKIP')}
    facts.append(f'run_all quick set: {counts["PASS"]} passed, {counts["FAIL"]} failed, {counts["SKIP"]} skipped')
    if new:
        problems.append(f'new test failures (pass on the base {base[:7]}): {", ".join(new)}')
    if known:
        problems.append(f'red on the base {base[:7]} too, so dev is red: {", ".join(known)}; the gate asks for a green '
                        f'quick set, so report it as BLOCKED unless fixing it is part of this issue')

    verdict = 'FAIL' if problems else 'PASS'
    full = '\n\n'.join(log)
    md.parent.mkdir(parents=True, exist_ok=True)
    md.with_name('gate.log').write_text(full, encoding='utf-8')
    tail = '\n'.join(full.rstrip().splitlines()[-TAIL:])
    md.write_text(
        f'GATE: {verdict}\nCOMMIT: {commit}\nBASE: {base}\n\n'
        '## Summary\n' + '\n'.join(f'- {f}' for f in facts) + '\n\n'
        '## Problems\n' + ('\n'.join(f'- {p}' for p in problems) or 'None.') + '\n\n'
        f'## Last {TAIL} lines of gate.log\n\n```\n{tail}\n```\n', encoding='utf-8')
    print(f'GATE: {verdict} ({commit[:7]}' + (f'; {"; ".join(problems)}' if problems else '') + ')')
    return 0 if verdict == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
