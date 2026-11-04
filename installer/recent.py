"""Last file and folder selections of the setup window, kept per user."""
import json
import os
from pathlib import Path

NAME = 'installer-paths.json'

PATH_KINDS = {
    'base': 'file',
    'update': 'file',
    'keys': 'file',
    'firmware': 'directory',
    'dest': 'directory',
}


def settings_path():
    """Store settings next to the installer."""
    return Path(__file__).resolve().parent.parent / NAME


def _read(path):
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        return {}
    return data if isinstance(data, dict) else {}


def load(settings_file=None):
    """Return only previously selected paths that still exist with the right type."""
    path = Path(settings_file) if settings_file is not None else settings_path()
    saved = _read(path)
    result = {}

    for name, kind in PATH_KINDS.items():
        value = saved.get(name)
        if not isinstance(value, str) or not value.strip():
            continue

        candidate = Path(value)
        try:
            exists = candidate.is_file() if kind == 'file' else candidate.is_dir()
        except (OSError, ValueError):
            continue

        if exists:
            result[name] = os.path.normpath(value)

    return result


def save(name, value, settings_file=None):
    """Save one selection without disturbing the other remembered paths."""
    if name not in PATH_KINDS:
        return

    value = os.fspath(value)
    if not isinstance(value, str) or not value:
        return

    path = Path(settings_file) if settings_file is not None else settings_path()
    data = {
        key: item
        for key, item in _read(path).items()
        if key in PATH_KINDS and isinstance(item, str)
    }
    data[name] = os.path.normpath(value)

    temporary = path.with_name(path.name + '.tmp')
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary.write_text(
            json.dumps(data, indent=1, ensure_ascii=False) + '\n',
            encoding='utf-8',
        )
        os.replace(temporary, path)
    except (OSError, ValueError):
        try:
            temporary.unlink(missing_ok=True)
        except OSError:
            pass