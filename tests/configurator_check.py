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
3. On Windows: the updates (--check-update, --download-update, the Updates page) against a fake GitHub on 127.0.0.1.
No game files, no network.
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


def updates(exe):
    """The Updates tab's work (runtime/configurator/cfgr_update.c) against a fake GitHub on this machine: the check,
    the download with its size and SHA-256, the unpacking, and the refusals (a wrong digest, a pre-release, a download
    from another host, a zip entry outside its folder, a release whose VERSION differs)."""
    if not WIN:
        print('skip  updates: Windows only (the releases are Windows setups)')
        return
    import hashlib
    import http.server
    import io
    import json
    import threading
    import zipfile

    def make_zip(version='9.8.7+abc1234', extra=None):
        b = io.BytesIO()
        with zipfile.ZipFile(b, 'w') as z:
            z.writestr('TotK-Setup/VERSION', version + '\n')
            z.writestr('TotK-Setup/python/pythonw.exe', b'MZ')
            z.writestr('TotK-Setup/installer/gui.py', "ap.add_argument('--rebuild', type=Path)\n")
            for name, data in (extra or {}).items():
                z.writestr(name, data)
        return b.getvalue()

    routes = {}

    class Handler(http.server.BaseHTTPRequestHandler):
        def do_GET(self):
            body = routes.get(self.path)
            self.send_response(200 if body is not None else 404)
            self.send_header('Content-Length', str(len(body or b'')))
            self.end_headers()
            self.wfile.write(body or b'')

        def log_message(self, *a):
            pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    threading.Thread(target=server.serve_forever, daemon=True).start()
    base = f'http://127.0.0.1:{server.server_address[1]}'
    latest = '/repos/valentderah/totk-pc-recomp/releases/latest'

    def serve(data, digest=None, url=None, **over):
        rel = {'tag_name': 'v9.8.7', 'name': 'TotK Recomp 9.8.7', 'draft': False, 'prerelease': False,
               'html_url': base + '/r', 'published_at': '2026-11-23T00:00:00Z',
               'body': '## What changed\r\n- **Faster** loading\r\n- `totk-config` updates\r\n> [!NOTE]\r\n> Plain text',
               'assets': [{'name': 'TotK-Setup-9.8.7-x86_64.AppImage', 'size': 1, 'digest': 'sha256:00',
                           'browser_download_url': base + '/x'},
                          {'name': 'TotK-Setup-9.8.7-windows.zip', 'size': len(data),
                           'digest': digest or 'sha256:' + hashlib.sha256(data).hexdigest(),
                           'browser_download_url': url or base + '/dl/TotK-Setup-9.8.7-windows.zip'}]}
        rel.update(over)
        routes[latest] = json.dumps(rel).encode()
        routes['/dl/TotK-Setup-9.8.7-windows.zip'] = data

    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        game = tmp / 'game'
        game.mkdir()

        def upd(*args, folder='u'):
            env = {**os.environ, 'TOTK_MENU_LANG': 'en', 'TOTK_CONFIG_UPDATE_API': base,
                   'TOTK_CONFIG_UPDATES_DIR': str(tmp / folder)}
            r = subprocess.run([str(exe), '--dir', str(game), *args], capture_output=True, text=True, timeout=120,
                               env=env)
            return r.returncode, r.stdout + r.stderr

        good = make_zip()
        serve(good)
        code, out = upd('--check-update')
        check(code == 0 and 'latest: v9.8.7' in out and ', newer' in out, '--check-update: the fake latest release')
        code, out = upd('--download-update')
        rel = tmp / 'u' / 'TotK-Setup-9.8.7'
        check(code == 0 and (rel / 'VERSION').is_file() and (rel / 'python' / 'pythonw.exe').is_file() and
              not list((tmp / 'u').glob('*.zip*')),
              '--download-update: downloaded, size and SHA-256 checked, unpacked, the zip gone' +
              ('' if code == 0 else f'\n{out}'))
        serve(good, digest='sha256:' + '0' * 64)
        code, out = upd('--download-update', folder='u2')
        left = list((tmp / 'u2').iterdir()) if (tmp / 'u2').is_dir() else []
        check(code == 1 and 'SHA-256' in out and not left, 'a wrong SHA-256: refused, nothing left behind')
        serve(good, prerelease=True)
        code, out = upd('--check-update')
        check(code == 1 and 'pre-release' in out, 'a pre-release: refused')
        serve(good, url='https://example.com/TotK-Setup-9.8.7-windows.zip')
        code, out = upd('--check-update')
        check(code == 1 and 'not from this project' in out, 'a download from another host: refused')
        serve(make_zip(extra={'../evil.txt': 'x'}))
        code, out = upd('--download-update', folder='u3')
        check(code == 1 and 'outside its folder' in out and not (tmp / 'evil.txt').exists(),
              'a zip entry outside its folder: refused, nothing unpacked')
        serve(make_zip(version='1.0.0'))
        code, out = upd('--download-update', folder='u4')
        check(code == 1 and 'says 1.0.0' in out and not (tmp / 'u4' / 'TotK-Setup-9.8.7').exists(),
              'a release whose VERSION names another version: refused')
        # the version installed: the setup's record and the log of the last run
        (game / 'logs').mkdir()
        (game / 'totk-install.json').write_text('{"format": 1, "release": "9.8.7+abc1234"}')
        (game / 'logs' / 'totk.log').write_text('[host] version: TotK Recomp 9.8.7+abc1234, game 1.4.3\n')
        serve(good)
        code, out = upd('--check-update')
        check(code == 0 and 'installed: 9.8.7+abc1234\n' in out and 'not newer' in out,
              'installed: the release the setup made and the game ran, not newer')
        (game / 'logs' / 'totk.log').write_text('[host] version: TotK Recomp 9.8.6+dead, game 1.4.3\n')
        code, out = upd('--check-update')
        check(code == 0 and '9.8.6+dead (own build)' in out and ', newer' in out,
              'installed: a build of its own, and the release is newer')
        # the check when the window opens: on without a settings file or without the key, off when turned off
        state = tmp / 'state'
        (state / 'TotK Recomp').mkdir(parents=True)
        prefs = state / 'TotK Recomp' / 'configurator.json'
        for text, want in ((None, 'on'), ('{}', 'on'), ('{"check_updates": false}', 'off'),
                           ('{"check_updates": true}', 'on')):
            if text is None:
                prefs.unlink(missing_ok=True)
            else:
                prefs.write_text(text)
            env = {**os.environ, 'TOTK_CONFIG_UPDATE_API': base, 'LOCALAPPDATA': str(state)}
            r = subprocess.run([str(exe), '--dir', str(game), '--check-update'], capture_output=True, text=True,
                               timeout=120, env=env)
            check(f'check when the window opens: {want}\n' in r.stdout,
                  f'the check when the window opens with {text or "no settings file"}: {want}')
        # the page with the fake release, drawn, and the footer button on another page
        pics = []
        for asks in (False, True):
            shot = tmp / f'banner-{asks}.bmp'
            env = {**os.environ, 'TOTK_MENU_LANG': 'en', 'TOTK_CONFIG_UPDATE_API': base}
            if asks:
                env['TOTK_CONFIG_SHOT_CHECK'] = '1'
            r = subprocess.run([str(exe), '--dir', str(game), '--screenshot', str(shot), '--tab', '0'],
                               capture_output=True, text=True, timeout=120, env=env)
            pics.append(shot.read_bytes() if r.returncode == 0 and shot.is_file() else b'')
        check(all(pics) and pics[0] != pics[1], 'a newer release puts a button in the footer of the Screen tab')
        shot = tmp / 'updates.bmp'
        env = {**os.environ, 'TOTK_MENU_LANG': 'en', 'TOTK_CONFIG_UPDATE_API': base, 'TOTK_CONFIG_SHOT_CHECK': '1'}
        r = subprocess.run([str(exe), '--dir', str(game), '--screenshot', str(shot), '--tab', '10'],
                           capture_output=True, text=True, timeout=120, env=env)
        check(r.returncode == 0 and shot.is_file() and shot.stat().st_size > 100000, 'the Updates page draws a release')
    server.shutdown()


def screenshots(exe):
    if not WIN and not (os.environ.get('DISPLAY') or os.environ.get('WAYLAND_DISPLAY')):
        print('skip  the window: no display')
        return
    with tempfile.TemporaryDirectory() as tmp:
        game = Path(tmp)
        (game / 'totk.ini').write_text('fullscreen = 1\n')
        for mod, part in (('Alpha', 'romfs'), ('Beta', 'exefs')):   # the Mods tab's list draws
            (game / 'mods' / mod / part).mkdir(parents=True)
        for lang in ('en', 'ru', 'pt-BR', 'zh-Hans'):
            for tab in range(11 if WIN else 10):   # tab 10: Updates (Windows), not checked
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
                updates(exe)
                screenshots(exe)
    print(f'\n{len(FAILS)} failed' if FAILS else '\nall passed')
    return 1 if FAILS else 0


if __name__ == '__main__':
    sys.exit(main())
