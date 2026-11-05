"""The last install folder, in a per-user file, so an update starts with the folder already filled in.

    {"format": 1, "dest": "<absolute path>"}

Windows: %LOCALAPPDATA%\TotK Recomp\setup.json; elsewhere $XDG_STATE_HOME/totk-recomp/setup.json (default
~/.local/state/totk-recomp/setup.json). TOTK_SETUP_RECENT names another file (tests, --selftest). Nothing else is
stored: the language comes from the install's own totk-install.json, and no key or key file path is kept.
"""
import json
import os
import sys
from pathlib import Path

from . import state

FORMAT = 1
ENV = 'TOTK_SETUP_RECENT'


def path():
    override = os.environ.get(ENV)
    if override:
        return Path(override)
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
        return Path(base) / 'TotK Recomp' / 'setup.json'
    base = os.environ.get('XDG_STATE_HOME') or str(Path.home() / '.local' / 'state')
    return Path(base) / 'totk-recomp' / 'setup.json'


def load():
    """The recorded install folder when it still holds totk-install.json, else None. Never raises."""
    try:
        data = json.loads(path().read_text(encoding='utf-8'))
        dest = data['dest']
        if not isinstance(dest, str) or not dest:
            return None
        return dest if (Path(dest) / state.NAME).is_file() else None
    except (OSError, ValueError, KeyError, TypeError):
        return None


def save(dest):
    """Records dest atomically; a failure to write is ignored, the window works without it."""
    target = path()
    tmp = target.with_name(target.name + '.tmp')
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps({'format': FORMAT, 'dest': os.path.abspath(dest)}), encoding='utf-8')
        os.replace(tmp, target)
    except OSError:
        pass
