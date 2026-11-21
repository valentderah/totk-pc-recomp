"""totk-config, the settings window (runtime/configurator, docs/configurator.md): its table and its commands.

    python tests/configurator_check.py [--no-build]

1. tools/gen_configurator.py reads every setting row of runtime/ui/menu.cpp: a row written another way than the ones
   docs/configurator.md ("Adding a setting") lists fails here, as the window would miss it. Every label and description
   id of the table is a UiTextId, and every key of known[] is on a tab.
2. Unless --no-build: tools/build_configurator.py builds the window with clang, and its commands run against a folder
   in a temporary directory: --set changes only the lines of the keys it sets (cfg.c cfg_save_to, CRLF and comments
   kept), an empty value removes the key, --new-profile writes each preset's keys, --profile swaps totk.ini, and
   --list names every key of the table. Then the window draws each tab once in each menu language and saves the picture
   (--screenshot), where a display is there (Windows; Linux with DISPLAY or WAYLAND_DISPLAY).
No game files.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import gen_configurator   # noqa: E402

WIN = os.name == 'nt'
FAILS = []


def check(ok, what):
    print(('ok    ' if ok else 'FAIL  ') + what)
    if not ok:
        FAILS.append(what)


def table():
    rows, errors = gen_configurator.build()
    check(not errors, 'the menu rows all read: ' + ('; '.join(errors) or 'no unknown row'))
    ids = set(re.findall(r'\b(UI_\w+)\b', (ROOT / 'runtime' / 'ui_text.h').read_text(encoding='utf-8')))
    used = {r[k] for r in rows for k in ('label', 'desc', 'group') if r.get(k)}
    used |= {n[1] for r in rows for n in r.get('names') or [] if n and n[0] == 'id'}
    check(used <= ids, f'every text id of the table is a UiTextId ({sorted(used - ids)[:5] or "all"})')
    known, metas = gen_configurator.parse_cfg()
    keys = {r['key'] for r in rows}
    check(set(known) <= keys, f'every key of known[] is on a tab ({sorted(set(known) - keys)[:5] or "all"})')
    menu = sum(r['tab'] != 'EXPERT' for r in rows)
    check(menu > 100, f'{menu} menu rows, {len(rows) - menu} expert rows')
    return rows


def run(exe, *args, cwd=None):
    r = subprocess.run([str(exe), *args], capture_output=True, text=True, cwd=cwd, timeout=60,
                       env={**os.environ, 'TOTK_MENU_LANG': os.environ.get('TOTK_MENU_LANG', 'en')})
    return r.returncode, r.stdout + r.stderr


def commands(exe, rows):
    with tempfile.TemporaryDirectory() as tmp:
        game = Path(tmp)
        ini = game / 'totk.ini'
        original = ('# my settings\r\n[display]\r\nfullscreen = 1\r\nfov = 110\r\n\r\n'
                    '[frame rate]\r\nfps_limit = 60\r\nmystery_key = 42\r\n')
        ini.write_bytes(original.encode())
        code, out = run(exe, '--dir', game, '--set', 'fps_limit=120', '--set', 'fov=', '--set', 'hdr=1')
        want = ('# my settings\r\n[display]\r\nfullscreen = 1\r\nhdr = 1\r\n\r\n'
                '[frame rate]\r\nfps_limit = 120\r\nmystery_key = 42\r\n')
        got = ini.read_bytes().decode()
        check(code == 0 and got == want, '--set: one value changed, one key removed, one added under its section, '
                                         'CRLF and the unknown key kept' + ('' if got == want else f'\n{got!r}'))
        code, out = run(exe, '--dir', game, '--list')
        listed = {line.split('\t')[1] for line in out.splitlines() if line.count('\t') >= 3}
        check(code == 0 and {r['key'] for r in rows} <= listed and 'MYSTERY_KEY' in listed,
              f'--list: every key of the table and the unknown one ({len(listed)} lines)')
        before = ini.read_bytes()
        for preset in ('current', 'defaults', 'quality', 'performance', 'potato', 'deck'):
            code, out = run(exe, '--dir', game, '--new-profile', f'P-{preset}', '--from', preset)
            check(code == 0 and (game / 'profiles' / f'P-{preset}.ini').is_file(), f'--new-profile --from {preset}')
        check(ini.read_bytes() == before, 'a new profile leaves totk.ini as it was')
        potato = (game / 'profiles' / 'P-potato.ini').read_text()
        check('fps_limit = 30' in potato and 'render_scale = 67' in potato and 'fov' not in potato,
              'the potato profile: fps_limit 30, render_scale 67 over the current settings')
        deck = (game / 'profiles' / 'P-deck.ini').read_text()
        check('window_size = 1280x800' in deck and 'vk_present = fifo' in deck,
              'the Steam Deck profile: cfg.c DECK[] (cfg_deck_at)')
        defaults = (game / 'profiles' / 'P-defaults.ini').read_text()
        check(not re.search(r'^\s*[a-z_]+\s*=', defaults, re.M) or defaults.count('=') == 0,
              'the defaults profile sets no key')
        code, out = run(exe, '--dir', game, '--profile', 'P-potato')
        check(code == 0 and 'fps_limit = 30' in ini.read_text() and
              (game / 'profiles' / 'active.txt').read_text().strip() == 'P-potato', '--profile: totk.ini holds it')
        code, out = run(exe, '--dir', game, '--profile', '')
        check(code == 0 and (game / 'profiles' / 'P-potato.ini').read_text() == ini.read_text(),
              '--profile "": the active profile keeps what totk.ini held')
        code, out = run(exe, '--dir', game, '--set', 'no_such_key=1')
        check('no key this version' in out and 'no_such_key = 1' in ini.read_text(),
              '--set of an unknown key: written, with a note')


def screenshots(exe):
    if not WIN and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        print('skip  the window: no display')
        return
    with tempfile.TemporaryDirectory() as tmp:
        game = Path(tmp)
        (game / 'totk.ini').write_text('fullscreen = 1\n')
        for lang in ('en', 'ru', 'pt-BR', 'zh-Hans'):
            for tab in range(10):
                shot = game / f'{lang}-{tab}.bmp'
                env = {**os.environ, 'TOTK_MENU_LANG': lang}
                r = subprocess.run([str(exe), '--dir', str(game), '--screenshot', str(shot), '--tab', str(tab)],
                                   capture_output=True, text=True, timeout=60, env=env)
                if r.returncode or not shot.is_file() or shot.stat().st_size < 100000:
                    check(False, f'the window draws tab {tab} in {lang}: exit {r.returncode} {r.stderr[-300:]}')
                    return
        check(True, 'the window draws every tab in en, ru, pt-BR and zh-Hans')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--no-build', action='store_true', help='only check the table')
    args = ap.parse_args()
    rows = table()
    if not args.no_build:
        import build_configurator
        with tempfile.TemporaryDirectory() as tmp:
            try:
                exe = build_configurator.build(Path(tmp) / 'build')
            except build_configurator.BuildError as e:
                check(False, f'build: {e}')
            else:
                check(True, f'build: {exe.name}, {exe.stat().st_size // 1024} KiB')
                if WIN:   # the window finds SDL3.dll beside it
                    import shutil
                    shutil.copy2(build_configurator.UCRT / 'SDL3.dll', exe.parent / 'SDL3.dll')
                commands(exe, rows)
                screenshots(exe)
    print(f'\n{len(FAILS)} failed' if FAILS else '\nall passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
