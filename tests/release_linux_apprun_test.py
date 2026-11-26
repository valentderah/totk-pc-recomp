"""The AppImage entry must ignore the user's site-packages (issue 985).

AppRun starts the bundled Python with `-m installer.gui`. Without PYTHONNOUSERSITE, a PyPI package named `installer`
in the user site shadows the project's own installer package and the window fails with "No module named installer.gui".

The test reads the export lines from AppRun, then runs Python with that environment against a fake user site holding
a shadowing `installer` package (no `gui`) and a fake bundle holding the real layout. The bundle is added the way
totk-release.pth adds it, after the user site, so the user site wins unless AppRun turns it off.
"""
import os
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import make_release_linux  # noqa: E402

line = 'export PYTHONNOUSERSITE=1'
lines = make_release_linux.APPRUN.splitlines()

# Environment from AppRun's export lines. Values that refer to $HERE point into the mount and are not needed here.
env = dict(os.environ)
env.pop('PYTHONNOUSERSITE', None)
for text in lines:
    if text.startswith('export ') and '=' in text and '$' not in text:
        name, value = text[len('export '):].split('=', 1)
        env[name] = value
env.pop('PYTHONPATH', None)

with tempfile.TemporaryDirectory() as tmp:
    tmp = Path(tmp)
    user_base = tmp / 'userbase'
    env['PYTHONUSERBASE'] = str(user_base)
    user_site = Path(subprocess.run(
        [sys.executable, '-c', 'import site; print(site.getusersitepackages())'],
        env=env, capture_output=True, text=True, check=True, cwd=tmp).stdout.strip())
    shadow = user_site / 'installer'
    shadow.mkdir(parents=True)
    (shadow / '__init__.py').write_text('# stands in for the PyPI "installer" package: no gui submodule\n')

    # The test only means something if this interpreter would put the user site on sys.path without AppRun's
    # setting. A venv has no user site, so run it with the system Python; otherwise the shadow is never tried.
    probe_env = dict(env)
    probe_env.pop('PYTHONNOUSERSITE', None)
    probe = subprocess.run([sys.executable, '-c', 'import sys; print(sys.argv[1] in sys.path)', str(user_site)],
                           env=probe_env, capture_output=True, text=True, cwd=tmp)
    assert probe.stdout.strip() == 'True', (
        f'user site {user_site} is not on sys.path for {sys.executable}; run this test with the system Python, '
        'not a venv, or it cannot catch the shadowing')

    bundle = tmp / 'bundle'
    (bundle / 'installer').mkdir(parents=True)
    (bundle / 'installer' / '__init__.py').write_text('')
    (bundle / 'installer' / 'gui.py').write_text('print("gui imported")\n')

    code = 'import site, sys; site.addsitedir(sys.argv[1]); import installer.gui'
    result = subprocess.run([sys.executable, '-c', code, str(bundle)],
                            env=env, capture_output=True, text=True, cwd=tmp)
    assert result.returncode == 0, f'installer.gui not importable with AppRun env: {result.stderr.strip()}'

# Checked last so the import behaviour is what fails on the base commit; this line guards the mechanism.
assert line in lines, f'AppRun does not set {line!r}; AppRun lines: {lines}'

print('AppRun sets PYTHONNOUSERSITE=1 and bundled installer.gui imports past a user-site shadow')
