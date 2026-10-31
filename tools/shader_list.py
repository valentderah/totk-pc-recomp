"""The MacNativeGames shared shader list (refs/MacNativeGames games/totk/assets/shader_list.tsv, MIT; docs/shader-list.md
there) as our tools read it: every row of one revision, plus the V rows of every older revision that it dropped.

MacNativeGames prunes V rows its own runtime no longer needs: default variants, which its seed translates for every
shader (1.4.3 on 2026-10-22, 1.0.0 to 1.2.1 on 2026-10-25), and variants that differ only in vertex input or colour
output types (2026-10-26). Our answers (runtime/gfx_shader.c) still tell those apart, so a dropped row names a variant
our play meets: at 1340bee the list alone has 134 rows of 1.4.3 shaders, its history 24,839, and of 1.2.1 6,771 and
25,238. A row no draw asks for costs one translation. No copy of the list goes into the repository.
"""
import subprocess
import sys

PATH = 'games/totk/assets/shader_list.tsv'


def git(repo, *args):
    return subprocess.run(['git', '-C', str(repo), *args], check=True, capture_output=True, encoding='utf-8').stdout


def history(repo, rev='origin/HEAD'):
    """rev's list, then each V row of an older revision that no newer one has, newest first."""
    if git(repo, 'rev-parse', '--is-shallow-repository').strip() == 'true':
        # a shallow clone has rev's list only: preparing from it skips most variants and shows only as stutter in play
        sys.exit(f'{repo} is a shallow clone without the shader list\'s history; run tools/fetch_refs.sh')
    lines = git(repo, 'show', f'{rev}:{PATH}').splitlines()
    seen = set(lines)
    for h in git(repo, 'log', '--format=%H', rev, '--', PATH).split()[1:]:
        for line in git(repo, 'show', f'{h}:{PATH}').splitlines():
            if line.startswith('V\t') and line not in seen:
                seen.add(line)
                lines.append(line)
    return '\n'.join(lines) + '\n'
