"""The in-game update (runtime/update.c) against a release server on this machine, and the setup window's --upgrade
helpers (installer/gui.py).

    python tests\\update_test.py --exe <install folder>\\totk.exe

Each scenario runs `totk.exe --probe-update` with update_api pointing at a server in this process and LOCALAPPDATA at a
temp folder. The server answers like GitHub's releases/latest and serves a small zip shaped like a Windows setup (one
folder TotK-Setup-<version>-windows with VERSION, python\\pythonw.exe and installer\\gui.py), changed per scenario: a
wrong SHA-256, a size off by one byte, another version inside, an entry outside the folder, a download that breaks off.
The test reads the state and failure the probe prints and looks at what stays in the download folder. Windows only;
without --exe only the window helpers run. No game files.
"""
import argparse
import hashlib
import http.server
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import unittest
import unittest.mock
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
try:
    from installer import gui  # noqa: E402
except ImportError:   # no tkinter
    gui = None

EXE = None   # --exe
NEWER = '9.0.0'   # newer than any build of this repository
# runtime/update.h UpdateState and UpdateError
IDLE, CHECKING, CURRENT, NEWER_STATE, DOWNLOADING, READY = range(6)
OK, ERR_CHECK, ERR_VERSION, ERR_NO_SETUP, ERR_DOWNLOAD, ERR_VERIFY, ERR_DISK, ERR_START = range(8)


def setup_zip(version, inside_version=None, extra=None):
    """A zip shaped like TotK-Setup-<version>-windows.zip; inside_version: the VERSION it holds; extra: (name, bytes)."""
    top = f'TotK-Setup-{version}-windows'
    out = io.BytesIO()
    with zipfile.ZipFile(out, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr(f'{top}/VERSION', f'{inside_version or version}+test\n')
        z.writestr(f'{top}/python/pythonw.exe', b'MZ not a real program')
        z.writestr(f'{top}/installer/gui.py', '# not the real window\n')
        z.writestr(f'{top}/installer/text/en.json', '{}\n' * 2000)   # some bytes to download in more than one piece
        if extra:
            z.writestr(*extra)
    return out.getvalue()


class Release:
    """What one scenario serves: the release answer and the setup zip."""

    def __init__(self, version=NEWER, data=None, digest=None, size=None, asset=True, url=None, status=200,
                 cut=None):
        self.version, self.status, self.cut = version, status, cut
        self.data = data if data is not None else setup_zip(version)
        self.digest = digest or 'sha256:' + hashlib.sha256(self.data).hexdigest()
        self.size = len(self.data) if size is None else size
        self.asset, self.url = asset, url
        self.downloads = 0

    def answer(self, base, name):
        assets = [{'name': f'TotK-Setup-x86_64-{self.version}-linux.AppImage', 'size': 5, 'digest': 'sha256:' + '0' * 64,
                   'browser_download_url': f'{base}/{name}/download/linux.AppImage'}]
        if self.asset:
            assets.append({'name': f'TotK-Setup-{self.version}-windows.zip', 'size': self.size, 'digest': self.digest,
                           'label': None, 'uploader': {'login': 'someone', 'id': 1},
                           'browser_download_url': self.url or f'{base}/{name}/download/setup.zip'})
        return json.dumps({'tag_name': f'v{self.version}', 'name': f'v{self.version} \u2014 notes', 'draft': False,
                           'prerelease': False, 'html_url': f'https://github.com/x/y/releases/tag/v{self.version}',
                           'assets': assets, 'body': '## Changes\r\n- "quoted" \\ text'}).encode()


class Server:
    """A release server on 127.0.0.1: /<scenario>/releases/latest and /<scenario>/download/setup.zip."""

    def __init__(self):
        self.releases = {}
        owner = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def log_message(self, *args):
                pass

            def do_GET(self):
                parts = self.path.strip('/').split('/')
                rel = owner.releases.get(parts[0])
                if rel is None or rel.status != 200:
                    self.send_error(rel.status if rel else 404)
                    return
                if parts[1:] == ['releases', 'latest']:
                    body = rel.answer(owner.base, parts[0])
                elif parts[1:] == ['download', 'setup.zip']:
                    rel.downloads += 1
                    body = rel.data
                else:
                    self.send_error(404)
                    return
                self.send_response(200)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                if parts[1] == 'download' and rel.cut is not None:
                    self.wfile.write(body[:rel.cut])   # then the connection closes before the rest
                    self.wfile.flush()
                    self.close_connection = True
                    return
                self.wfile.write(body)

        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
        self.base = f'http://127.0.0.1:{self.httpd.server_address[1]}'
        threading.Thread(target=self.httpd.serve_forever, daemon=True).start()

    def close(self):
        self.httpd.shutdown()


def probe(exe, api, appdata):
    """Runs totk.exe --probe-update; (state, error, output)."""
    env = dict(os.environ, TOTK_UPDATE_API=api, LOCALAPPDATA=str(appdata), TOTK_UPDATE_CHECK='1')
    r = subprocess.run([str(exe), '--probe-update'], env=env, capture_output=True, text=True, timeout=300,
                       cwd=str(Path(exe).parent))
    out = r.stdout + r.stderr
    for line in out.splitlines():
        if 'update: probe state=' in line:
            fields = dict(f.split('=', 1) for f in line.split('probe ', 1)[1].split(' ') if '=' in f)
            if r.returncode:
                raise AssertionError(f'--probe-update failed its own checks:\n{out}')
            return int(fields['state']), int(fields['error']), out
    raise AssertionError(f'no probe result line (exit {r.returncode}):\n{out}')


@unittest.skipUnless(os.name == 'nt', 'the in-game update is Windows only')
class ProbeTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if EXE is None:
            raise unittest.SkipTest('no --exe')
        cls.server = Server()
        cls.tmp = Path(tempfile.mkdtemp(prefix='totk-update-'))

    @classmethod
    def tearDownClass(cls):
        cls.server.close()
        shutil.rmtree(cls.tmp, ignore_errors=True)

    def run_scenario(self, name, release, appdata=None):
        self.server.releases[name] = release
        appdata = appdata or self.tmp / name
        appdata.mkdir(exist_ok=True)
        state, error, out = probe(EXE, f'{self.server.base}/{name}/releases/latest', appdata)
        return state, error, appdata / 'TotK Recomp' / 'updates', out

    def assertNothingLeft(self, updates, out):
        left = sorted(p.name for p in updates.iterdir()) if updates.is_dir() else []
        self.assertEqual(left, [], out)

    def test_ready_then_reused(self):
        rel = Release()
        state, error, updates, out = self.run_scenario('ready', rel)
        self.assertEqual((state, error), (READY, OK), out)
        folder = updates / f'TotK-Setup-{NEWER}-windows'
        self.assertTrue((folder / '.totk-update-ok').is_file(), out)
        self.assertTrue((folder / 'installer' / 'gui.py').is_file(), out)
        self.assertEqual(sorted(p.name for p in updates.iterdir()), [folder.name], out)   # no zip, no .part folder
        self.assertEqual(rel.downloads, 1)
        # a later run finds the finished folder and does not download again
        state, error, _, out = self.run_scenario('ready', rel, appdata=self.tmp / 'ready')
        self.assertEqual((state, error, rel.downloads), (READY, OK, 1), out)

    def test_wrong_sha256(self):
        state, error, updates, out = self.run_scenario('sha', Release(digest='sha256:' + 'ab' * 32))
        self.assertEqual((state, error), (NEWER_STATE, ERR_VERIFY), out)
        self.assertNothingLeft(updates, out)

    def test_longer_than_listed(self):
        rel = Release()
        rel.size -= 1
        state, error, updates, out = self.run_scenario('long', rel)
        self.assertEqual((state, error), (NEWER_STATE, ERR_VERIFY), out)
        self.assertNothingLeft(updates, out)

    def test_shorter_than_listed(self):
        rel = Release()
        rel.size += 1
        state, error, updates, out = self.run_scenario('short', rel)
        self.assertEqual((state, error), (NEWER_STATE, ERR_VERIFY), out)
        self.assertNothingLeft(updates, out)

    def test_other_version_inside(self):
        state, error, updates, out = self.run_scenario('inside', Release(data=setup_zip(NEWER, inside_version='8.0.0')))
        self.assertEqual((state, error), (NEWER_STATE, ERR_VERIFY), out)
        self.assertNothingLeft(updates, out)

    def test_entry_outside_the_folder(self):
        data = setup_zip(NEWER, extra=('../escaped.txt', 'outside'))
        state, error, updates, out = self.run_scenario('escape', Release(data=data))
        self.assertEqual((state, error), (NEWER_STATE, ERR_DISK), out)
        self.assertFalse((updates / 'escaped.txt').exists(), out)
        self.assertFalse((updates.parent / 'escaped.txt').exists(), out)
        self.assertNothingLeft(updates, out)

    def test_download_breaks_off(self):
        rel = Release()
        rel.cut = len(rel.data) // 2
        state, error, updates, out = self.run_scenario('cut', rel)
        self.assertEqual((state, error), (NEWER_STATE, ERR_DOWNLOAD), out)
        self.assertNothingLeft(updates, out)

    def test_no_windows_setup(self):
        state, error, updates, out = self.run_scenario('nosetup', Release(asset=False))
        self.assertEqual((state, error), (NEWER_STATE, ERR_NO_SETUP), out)

    def test_download_elsewhere_refused(self):
        rel = Release(url='https://example.invalid/TotK-Setup-9.0.0-windows.zip')
        state, error, updates, out = self.run_scenario('elsewhere', rel)
        self.assertEqual((state, error, rel.downloads), (NEWER_STATE, ERR_NO_SETUP, 0), out)

    def test_up_to_date(self):
        rel = Release(version='0.0.1')
        state, error, updates, out = self.run_scenario('current', rel)
        self.assertEqual((state, error, rel.downloads), (CURRENT, OK, 0), out)

    def test_api_error(self):
        state, error, updates, out = self.run_scenario('missing', Release(status=404))
        self.assertEqual((state, error), (IDLE, ERR_CHECK), out)


@unittest.skipIf(gui is None, 'no tkinter')
class WindowTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='totk-upgrade-'))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def test_old_downloads_go(self):
        updates = self.tmp / 'TotK Recomp' / 'updates'
        for name in ('TotK-Setup-0.3.12-windows', 'TotK-Setup-0.3.13-windows', 'TotK-Setup-0.3.14-windows.part',
                     'other'):
            (updates / name / 'python').mkdir(parents=True)
        mine = self.tmp / 'Downloads' / 'TotK-Setup-0.3.11-windows'
        mine.mkdir(parents=True)
        with unittest.mock.patch.dict(os.environ, {'LOCALAPPDATA': str(self.tmp)}), \
                unittest.mock.patch.object(gui.os, 'name', 'nt'):
            gui.remove_old_downloads(mine)   # a release the player unpacked: nothing goes
            self.assertEqual(len(list(updates.iterdir())), 4)
            gui.remove_old_downloads(updates / 'TotK-Setup-0.3.13-windows')
        self.assertEqual(sorted(p.name for p in updates.iterdir()), ['TotK-Setup-0.3.13-windows', 'other'])
        self.assertTrue(mine.is_dir())

    @unittest.skipUnless(os.name == 'nt', 'a running program locks its file on Windows')
    def test_game_running(self):
        self.assertFalse(gui.game_running(self.tmp))   # no totk.exe
        exe = self.tmp / 'totk.exe'
        shutil.copy2(Path(os.environ.get('SystemRoot', r'C:\Windows')) / 'System32' / 'ping.exe', exe)
        self.assertFalse(gui.game_running(self.tmp))
        p = subprocess.Popen([str(exe), '-n', '30', '127.0.0.1'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        try:
            self.assertTrue(gui.game_running(self.tmp))
        finally:
            p.kill()
            p.wait()
        self.assertFalse(gui.game_running(self.tmp))


def main():
    global EXE
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--exe', type=Path, help='totk.exe of a build with runtime/update.c')
    args, rest = ap.parse_known_args()
    EXE = args.exe.resolve() if args.exe else None
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)


if __name__ == '__main__':
    main()
