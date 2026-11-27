"""Machine-wide locks for checks, builds and game runs, so heavy work does not overlap.

    python tools/factory/lock.py <class> -- <command> [args...]    run the command holding the class's locks
    python tools/factory/lock.py status                            who holds what

Classes (README.md, "Locks", says which check belongs to which):

  cc       compiles small things beside other work (CPU-only exe probes, xlate smoke)
  tests    the run_all quick set and the single tests that build into build/test/: one at a time, because every
           worktree shares the main checkout's build/ (two quick sets at once broke each other's harness builds)
  build    lift, compile, shader translator, shader preparation: one at a time on the machine
  game     starts totk.exe on the GPU without timing it (GPU probes, screenshots, validation runs)
  measure  timed runs (run_game.py --quiet, TOTK_AB runs)

cc, tests and build run beside each other. game and measure run alone: each waits until no other work holds a lock and
no totk.exe started outside the factory runs, and nothing else starts until it ends. Work without a class
(check_content, Python-only tests, reading logs) needs no lock. Every lock is an OS file lock, so a dead holder
never blocks anyone. The files live in the main checkout's build/ (worktrees link it), next to build/.gpu-run.lock,
which run_game.py --quiet holds: a timed run started outside the factory blocks the factory's work too.
Standard library only.
"""
import os
import shutil
import subprocess
import sys
import time
from contextlib import contextmanager
from pathlib import Path

if os.name == 'nt':
    import msvcrt
else:
    import fcntl

MAIN = Path(__file__).resolve().parents[2]
DIR = MAIN / 'build' / 'factory' / 'locks'
GPU_LOCK = MAIN / 'build' / '.gpu-run.lock'   # tools/run_game.py take_gpu_lock
LOCK_OFFSET = 1 << 16                         # as in tools/run_game.py: the holder text stays readable
CLASSES = ('cc', 'tests', 'build', 'game', 'measure')
POLL = 5.0


def try_lock(f):
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_NBLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
    except OSError:
        return False
    return True


def unlock(f):
    try:
        if os.name == 'nt':
            f.seek(LOCK_OFFSET)
            msvcrt.locking(f.fileno(), msvcrt.LK_UNLCK, 1)
        else:
            fcntl.flock(f.fileno(), fcntl.LOCK_UN)
    finally:
        f.close()


def held(path):
    """True while another process holds the lock on `path`."""
    if not path.exists():
        return False
    f = open(path, 'a+b')
    if try_lock(f):
        unlock(f)
        return False
    f.close()
    return True


def holder(path):
    try:
        return path.read_bytes()[:200].decode('utf-8', 'replace').strip() or 'unknown'
    except OSError:
        return 'unknown'


def write_holder(f, text):
    f.seek(0)
    f.truncate()
    f.write(text.encode())
    f.flush()


def wait_for(cond, what):
    told = False
    while cond():
        if not told:
            print(f'lock: waiting for {what()}', file=sys.stderr, flush=True)
            told = True
        time.sleep(POLL)


def take(path, text):
    """Block until this process holds the lock on `path`; return the open file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    f = open(path, 'a+b')
    wait_for(lambda: not try_lock(f), lambda: f'{path.name}, held by {holder(path)}')
    write_holder(f, text)
    return f


GAME_EXE = 'totk.exe' if os.name == 'nt' else 'totk'   # tools/build_game.py's output


def games_running():
    """How many game processes run on this machine, the factory's own included."""
    if os.name != 'nt':
        try:
            out = subprocess.run(['pgrep', '-x', GAME_EXE], capture_output=True, text=True).stdout
        except OSError:   # no pgrep: nothing to see
            return 0
        return len(out.split())
    # the "no tasks" line is in the system's language and OEM code page: only the image names are read
    out = subprocess.run(['tasklist', '/FI', f'IMAGENAME eq {GAME_EXE}', '/NH'], capture_output=True, text=True,
                         errors='replace').stdout
    return sum(line.lower().startswith(GAME_EXE) for line in out.splitlines())


def exclusive_run():
    """The holder of a game or timed run in progress, or None."""
    for p in (DIR / 'exclusive.lock', GPU_LOCK):
        if held(p):
            return f'the run of {holder(p)}'
    return None


def heavy_holders():
    """Live cc, tests and build holders; stale files are removed."""
    live = []
    for p in sorted((DIR / 'heavy').glob('*.lock')):
        if held(p):
            live.append(holder(p))
        else:
            try:
                p.unlink()
            except OSError:
                pass
    return live


@contextmanager
def hold(cls, what=''):
    if cls not in CLASSES:
        sys.exit(f'lock: unknown class {cls!r}; one of {", ".join(CLASSES)}')
    text = f'pid {os.getpid()}, {cls}: {what}'.strip()
    files = []
    try:
        if cls in ('game', 'measure'):
            files.append(take(DIR / 'exclusive.lock', text))
            wait_for(heavy_holders, lambda: 'cc, tests or build work: ' + '; '.join(heavy_holders()))
            # A timed run started outside the factory holds build/.gpu-run.lock. lock.py only looks at that file and
            # never takes it, so a measure command that is run_game.py --quiet still takes it itself once this ends.
            wait_for(lambda: held(GPU_LOCK), lambda: f'the timed run of {holder(GPU_LOCK)}')
            wait_for(games_running, lambda: f'{games_running()} {GAME_EXE} started outside the factory')
        else:
            if cls in ('build', 'tests'):   # one of each at a time on the machine
                files.append(take(DIR / f'{cls}.lock', text))
            mine = DIR / 'heavy' / f'{os.getpid()}-{cls}.lock'
            while True:
                wait_for(exclusive_run, exclusive_run)
                f = take(mine, text)
                if not exclusive_run():   # a run that started meanwhile waits for this file, or this one backs off
                    files.append(f)
                    break
                unlock(f)
        yield
    finally:
        for f in reversed(files):
            unlock(f)


def status():
    for p in [DIR / 'exclusive.lock', GPU_LOCK, DIR / 'build.lock', DIR / 'tests.lock']:
        print(f'{p.relative_to(MAIN)}: {"held by " + holder(p) if held(p) else "free"}')
    for h in heavy_holders():
        print(f'running: {h}')
    print(f'{GAME_EXE} processes: {games_running()}')


def main(argv):
    if argv[:1] in (['-h'], ['--help']):
        print(__doc__.strip())
        return 0
    if argv == ['status']:
        return status()
    if len(argv) < 3 or argv[1] != '--':
        sys.exit(__doc__.strip())
    cmd = argv[2:]
    # CreateProcess does not resolve a relative path written with forward slashes (.venv/Scripts/python.exe): name the
    # program by its full path when it is a file here, else look it up on PATH as the shell would
    if Path(cmd[0]).is_file():
        cmd[0] = str(Path(cmd[0]).resolve())
    else:
        cmd[0] = shutil.which(cmd[0]) or cmd[0]
    with hold(argv[0], ' '.join(argv[2:])[:120]):
        return subprocess.run(cmd).returncode


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
