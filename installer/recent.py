"""The last install folder and source paths, in a per-user file, so an update, a reinstall or a switch to another game
version starts with the form filled in.

    {"format": 1, "dest": ..., "base": ..., "update": ..., "keys": ..., "firmware": ...}

Absolute paths only. The keys entry is the path of the keys file, never its contents.

Windows: %LOCALAPPDATA%\TotK Recomp\setup.json; elsewhere $XDG_STATE_HOME/totk-recomp/setup.json (default
~/.local/state/totk-recomp/setup.json). TOTK_SETUP_RECENT names another file (tests, --selftest). Nothing else is
stored: the language comes from the install's own totk-install.json. A file of format 0 (only "dest") still loads.
"""
import json
import os
import sys
from pathlib import Path

from . import state

FORMAT = 1
ENV = 'TOTK_SETUP_RECENT'
FILES = ('base', 'update', 'keys')   # recorded when they are files
SOURCES = FILES + ('firmware',)
FIELDS = ('dest',) + SOURCES


def path():
    override = os.environ.get(ENV)
    if override:
        return Path(override)
    if sys.platform == 'win32':
        base = os.environ.get('LOCALAPPDATA') or str(Path.home() / 'AppData' / 'Local')
        return Path(base) / 'TotK Recomp' / 'setup.json'
    base = os.environ.get('XDG_STATE_HOME') or str(Path.home() / '.local' / 'state')
    return Path(base) / 'totk-recomp' / 'setup.json'


def usable(name, value):
    """value when it is a non-empty absolute path of the right kind for the field, else None. One stat call per
    path, no file is opened or hashed; a path the OS rejects (embedded NUL, unreachable share) counts as missing."""
    if not isinstance(value, str) or not value or not os.path.isabs(value):
        return None
    try:
        p = Path(value)
        if name == 'dest':
            ok = (p / state.NAME).is_file()
        elif name == 'firmware':
            ok = p.is_dir()
        else:
            ok = p.is_file()
    except (OSError, ValueError):
        return None
    return value if ok else None


def read():
    try:
        data = json.loads(path().read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def load():
    """The recorded entries that still exist, by field name. Never raises."""
    data = read()
    return {name: data[name] for name in FIELDS if usable(name, data.get(name))}


def save(dest, sources=None):
    """Records dest and the non-empty, existing entries of sources (base, update, keys, firmware); an entry left out
    or empty keeps its saved value. Atomic; a failure to write is ignored, the window works without it."""
    target = path()
    tmp = target.with_name(target.name + '.tmp')
    data = {name: v for name, v in read().items() if name in FIELDS and isinstance(v, str)}
    data['dest'] = os.path.abspath(dest)
    for name, value in (sources or {}).items():
        if name in SOURCES and isinstance(value, str) and value.strip():
            value = os.path.abspath(value.strip())
            if usable(name, value):
                data[name] = value
    try:
        target.parent.mkdir(parents=True, exist_ok=True)
        tmp.write_text(json.dumps({'format': FORMAT, **data}), encoding='utf-8')
        os.replace(tmp, target)
    except OSError:
        pass
