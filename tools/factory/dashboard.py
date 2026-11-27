"""One page with all of the factory's work on this machine: jobs, their steps and verdicts, what blocks them, their
pull requests, the locks, running games and the worktrees.

    python tools/factory/dashboard.py                 write build/factory/dashboard.html and print its path
    python tools/factory/dashboard.py --json          print the same data as JSON (for an agent asked "how is it going")
    python tools/factory/dashboard.py --serve [PORT]  serve it on http://127.0.0.1:PORT/ (default 8765), rebuilt on
                                                      every request; the page reloads itself every 30 s; the JSON is at
                                                      /status.json

Reads only: the job folders under build/factory/, the lock files, `git worktree list`, and `gh pr view` for each job's
branch (skipped when gh is missing or slow). Issue titles and agent files are data: the page escapes them.
Standard library only.
"""
import html
import http.server
import json
import re
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import lock  # noqa: E402

MAIN = lock.MAIN
JOBS = MAIN / 'build' / 'factory'
OUT = JOBS / 'dashboard.html'
REFRESH = 30
REPO = 'valentderah/totk-pc-recomp'   # SKILL.md, "Names": <repo>

# first-line verdicts that need a person, and the ones that are fine
BAD = ('ASK', 'SPLIT', 'SKIP', 'BLOCKED', 'FAIL', 'CHANGES', 'NEEDS HUMAN', 'DROPPED')
GOOD = ('GO', 'READY', 'PASS', 'READY FOR MERGE')


def first_line(p):
    try:
        with open(p, encoding='utf-8', errors='replace') as f:
            return f.readline().strip()
    except OSError:
        return ''


def value(line):
    return line.split(':', 1)[1].strip() if ':' in line else line


def run(cmd, timeout=15):
    try:
        r = subprocess.run(cmd, cwd=MAIN, capture_output=True, text=True, encoding='utf-8', errors='replace',
                           timeout=timeout)
        return r.returncode, r.stdout
    except (OSError, subprocess.TimeoutExpired):
        return 1, ''


def repo_url():
    """https://github.com/<repo>: the repository whose issues and pull requests the factory works on."""
    return f'https://github.com/{REPO}'


def worktrees():
    """{branch: path} of every worktree."""
    _, out = run(['git', 'worktree', 'list', '--porcelain'])
    res, path = {}, None
    for line in out.splitlines():
        if line.startswith('worktree '):
            path = line[9:]
        elif line.startswith('branch ') and path:
            res[line[7:].removeprefix('refs/heads/')] = path
    return res


def pr_of(branch, repo):
    """The pull request of `branch` in <repo>; --repo because a clone with a second remote makes gh guess."""
    if not repo:
        return None
    code, out = run(['gh', 'pr', 'view', branch, '--repo', repo.removeprefix('https://github.com/'),
                     '--json', 'number,state,isDraft,url,title'])
    if code:
        return None
    try:
        return json.loads(out)
    except ValueError:
        return None


def issue_title(job):
    for line in (job / 'issue.md').read_text(encoding='utf-8', errors='replace').splitlines()[:6] \
            if (job / 'issue.md').exists() else []:
        m = re.match(r'#(\d+) (.*) \[(\w+)\]$', line)
        if m:
            return m.group(2)
    return ''


def step_of(job):
    """(the step the job is at or ended in, its first line, rounds) from the files, as SKILL.md's resume table."""
    rounds = sorted((p for p in job.glob('round-*') if p.is_dir()), key=lambda p: int(p.name.split('-')[1]))
    steps = [('issue', first_line(job / 'issue.md') and 'quoted')] if (job / 'issue.md').exists() else []
    if (job / 'intake.md').exists():
        steps.append(('intake', value(first_line(job / 'intake.md'))))
    for r in rounds:
        k = r.name.split('-')[1]
        for name in ('build', 'gate', 'review'):
            f = r / f'{name}.md'
            if f.exists():
                steps.append((f'round {k} {name}', value(first_line(f))))
    if (job / 'done.md').exists():
        steps.append(('done', value(first_line(job / 'done.md'))))
    return steps, len(rounds)


def next_step(job, steps):
    names = [s[0] for s in steps]
    if 'done' in names:
        return 'finished'
    if not names:
        return 'start'
    last, verdict = steps[-1]
    if last == 'issue':
        return 'intake (agent running or due)'
    if last == 'intake':
        if not verdict.startswith('GO'):
            return 'comment on the issue, then done'
        return 'build round 1 (builder working)' if (job / 'round-1').is_dir() else 'worktree and build round 1'
    if last.endswith('build'):
        return 'gate and review' if verdict.startswith('READY') else 'finish (blocked)'
    if last.endswith('gate'):
        return 'review (reviewer working)' if verdict.startswith('PASS') else 'builder fixes the gate'
    if last.endswith('review'):
        return 'finish' if verdict.startswith('PASS') else 'next build round'
    return '?'


def tone(v):
    u = v.upper()
    if any(u.startswith(b) for b in BAD):
        return 'bad'
    if any(u.startswith(g) for g in GOOD):
        return 'good'
    return 'neutral'


def age(p):
    newest = max((f.stat().st_mtime for f in p.rglob('*') if f.is_file()), default=p.stat().st_mtime)
    s = time.time() - newest
    return f'{s / 60:.0f} min ago' if s < 3600 else f'{s / 3600:.1f} h ago' if s < 86400 else f'{s / 86400:.0f} d ago'


def jobs(trees, repo):
    active = first_line(JOBS / 'active')
    rows = []
    found = JOBS.iterdir() if JOBS.is_dir() else []
    for job in sorted((p for p in found if p.is_dir() and p.name.isdigit()), key=lambda p: -int(p.name)):
        steps, nrounds = step_of(job)
        branch = next((b for b in trees if b.startswith(f'task/{job.name}-')), None)
        pr = pr_of(branch, repo) if branch else None
        if not pr and (job / 'done.md').exists():   # a finished job's branch is gone: its PR link is in done.md
            m = re.search(r'/pull/(\d+)', (job / 'done.md').read_text(encoding='utf-8', errors='replace'))
            pr = pr_of(m.group(1), repo) if m else None
        rows.append({'n': job.name, 'title': issue_title(job), 'active': job.name == active, 'steps': steps,
                     'rounds': nrounds, 'next': next_step(job, steps), 'age': age(job), 'branch': branch,
                     'pr': pr})
    return active, rows


def locks():
    res = []
    for p in (lock.DIR / 'exclusive.lock', lock.GPU_LOCK, lock.DIR / 'build.lock', lock.DIR / 'tests.lock'):
        res.append((str(p.relative_to(MAIN)), lock.holder(p) if lock.held(p) else ''))
    return res, lock.heavy_holders(), lock.games_running()


def esc(s):
    return html.escape(str(s))


def chip(v):
    return f'<span class="chip {tone(v)}">{esc(v or "-")}</span>'


def snapshot():
    """The page's data as one JSON-ready dict, for an agent that answers questions about the factory."""
    trees = worktrees()
    repo = repo_url()
    active, rows = jobs(trees, repo)
    held, heavy, games = locks()
    return {
        'built': datetime.now().isoformat(timespec='seconds'),
        'checkout': str(MAIN),
        'active_job': active or None,
        'needs_person': [{'issue': int(r['n']), 'title': r['title'], 'step': r['steps'][-1][0],
                          'verdict': r['steps'][-1][1], 'next': r['next']}
                         for r in rows if r['steps'] and r['next'] != 'finished' and tone(r['steps'][-1][1]) == 'bad'],
        'jobs': [{'issue': int(r['n']), 'title': r['title'], 'active': r['active'], 'rounds': r['rounds'],
                  'steps': [{'step': s, 'verdict': v, 'tone': tone(v)} for s, v in r['steps']],
                  'next': r['next'], 'last_change': r['age'], 'branch': r['branch'],
                  'pr': ({k: r['pr'][k] for k in ('number', 'state', 'isDraft', 'url', 'title')} if r['pr'] else None)}
                 for r in rows],
        'locks': {name: (h or None) for name, h in held},
        'cc_build_work': heavy,
        'games_running': games,
        'worktrees': trees,
        'links': {'repo': repo or None},
    }


def page():
    trees = worktrees()
    repo = repo_url()
    active, rows = jobs(trees, repo)
    held, heavy, games = locks()
    blockers = [(r, r['steps'][-1]) for r in rows if r['steps'] and r['next'] != 'finished' and tone(r['steps'][-1][1]) == 'bad']
    out = [f'''<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1"><meta http-equiv="refresh" content="{REFRESH}">
<title>totk-factory</title><style>
:root{{--bg:#f6f7f9;--card:#fff;--fg:#1d232b;--mute:#667085;--line:#e3e6ea;--good:#1a7f37;--bad:#c4321f;--neu:#6e7781}}
@media (prefers-color-scheme:dark){{:root{{--bg:#111418;--card:#1a1f25;--fg:#e6e9ed;--mute:#98a2b3;--line:#2b323b;
--good:#56d364;--bad:#ff7b72;--neu:#9aa4af}}}}
body{{margin:0;background:var(--bg);color:var(--fg);font:14px/1.45 system-ui,Segoe UI,sans-serif}}
main{{max-width:1100px;margin:0 auto;padding:16px}}h1{{font-size:20px;margin:4px 0 2px}}h2{{font-size:15px;margin:22px 0 8px}}
.mute{{color:var(--mute)}}.card{{background:var(--card);border:1px solid var(--line);border-radius:8px;padding:12px;margin:8px 0}}
table{{border-collapse:collapse;width:100%}}td,th{{text-align:left;padding:5px 8px;border-bottom:1px solid var(--line);vertical-align:top}}
.chip{{display:inline-block;padding:1px 7px;border-radius:10px;font-size:12px;border:1px solid currentColor}}
.good{{color:var(--good)}}.bad{{color:var(--bad)}}.neutral{{color:var(--neu)}}.steps span{{margin-right:6px;white-space:nowrap}}
a{{color:inherit}}.wrap{{overflow-x:auto}}code{{font-size:12px}}
</style></head><body><main>
<h1>totk-factory</h1><div class="mute">{esc(MAIN)} &middot; built {datetime.now():%Y-%m-%d %H:%M:%S} &middot;
reloads every {REFRESH} s</div>''']
    links = [f'<a href="{esc(repo)}/pulls">pull requests</a>', f'<a href="{esc(repo)}/issues">issues</a>'] if repo else []
    if links:
        out.append('<div class="mute">' + ' &middot; '.join(links) + '</div>')

    out.append('<h2>Needs a person</h2><div class="card">')
    if blockers:
        out.append('<ul>' + ''.join(f'<li>#{esc(r["n"])} {esc(r["title"])}: {esc(s[0])} {chip(s[1])} &rarr; '
                                    f'{esc(r["next"])}</li>' for r, s in blockers) + '</ul>')
    else:
        out.append('<span class="mute">Nothing is waiting for a decision.</span>')
    out.append('</div>')

    out.append('<h2>Jobs</h2><div class="card wrap"><table><tr><th>Issue</th><th>Steps</th><th>Next</th>'
               '<th>Pull request</th><th>Last change</th></tr>')
    for r in rows:
        pr = r['pr']
        prc = (f'<a href="{esc(pr["url"])}">#{pr["number"]}</a> {chip(("draft " if pr["isDraft"] else "") + pr["state"].lower())}'
               if pr else '<span class="mute">none</span>')
        steps = ' '.join(f'<span>{esc(s)} {chip(v)}</span>' for s, v in r['steps'])
        star = ' <b>(active)</b>' if r['active'] else ''
        num = f'<a href="{esc(repo)}/issues/{esc(r["n"])}">#{esc(r["n"])}</a>' if repo else f'#{esc(r["n"])}'
        out.append(f'<tr><td>{num}{star}<br><span class="mute">{esc(r["title"])}</span></td>'
                   f'<td class="steps">{steps}</td><td>{esc(r["next"])}</td><td>{prc}</td><td>{esc(r["age"])}</td></tr>')
    if not rows:
        out.append('<tr><td colspan="5" class="mute">No jobs yet.</td></tr>')
    out.append('</table></div>')

    out.append('<h2>Machine</h2><div class="card"><table>')
    for name, h in held:
        out.append(f'<tr><td><code>{esc(name)}</code></td><td>{chip("held") if h else chip("free")} {esc(h)}</td></tr>')
    out.append(f'<tr><td>cc / build work</td><td>{esc("; ".join(heavy)) if heavy else chip("none")}</td></tr>')
    out.append(f'<tr><td>{esc(lock.GAME_EXE)} running</td><td>{chip(str(games))}</td></tr></table></div>')

    out.append('<h2>Worktrees</h2><div class="card wrap"><table><tr><th>Branch</th><th>Path</th></tr>')
    out += [f'<tr><td><code>{esc(b)}</code></td><td><code>{esc(p)}</code></td></tr>' for b, p in sorted(trees.items())]
    out.append('</table></div></main></body></html>')
    return '\n'.join(out)


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        as_json = self.path.split('?')[0] == '/status.json'
        body = (json.dumps(snapshot(), indent=1) if as_json else page()).encode('utf-8')
        self.send_response(200)
        self.send_header('Content-Type', 'application/json' if as_json else 'text/html; charset=utf-8')
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *a):
        pass


def main(argv):
    if argv[:1] in (['-h'], ['--help']):
        print(__doc__.strip())
        return 0
    if argv[:1] == ['--serve']:
        port = int(argv[1]) if len(argv) > 1 else 8765
        print(f'dashboard: http://127.0.0.1:{port}/', flush=True)
        http.server.ThreadingHTTPServer(('127.0.0.1', port), Handler).serve_forever()
        return 0
    if argv == ['--json']:
        print(json.dumps(snapshot(), indent=1))
        return 0
    if argv:
        sys.exit(__doc__.strip())
    JOBS.mkdir(parents=True, exist_ok=True)
    OUT.write_text(page(), encoding='utf-8')
    print(OUT)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
