"""totk-install.json in the install folder: what built it and which steps finished.

    {"format": 1, "release": "...", "lang": "en-US",
     "inputs": {"base": {"name", "size", "sha256"}, "update": {...}},
     "steps": {"<step>": {"key": "<input fingerprint>", "release": "...", "finished": "<UTC time>", "seconds": 12.3,
                          ...the step's own fields}}}

A step's key fingerprints everything its output depends on (runner.py), so a step whose recorded key equals the
current one and whose output still checks out is not run again. The file is replaced atomically after each step.
No key value or keys file content is ever stored.
"""
import json
import os
from pathlib import Path

NAME = 'totk-install.json'
FORMAT = 1


class StateError(Exception):
    def __init__(self, id, path):
        super().__init__(id)
        self.id, self.path = id, path


def empty(release):
    return {'format': FORMAT, 'release': release, 'inputs': {}, 'steps': {}}


def load(dest):
    """The state of dest, or None when the folder has none. StateError for an unreadable or newer file."""
    path = Path(dest) / NAME
    if not path.is_file():
        return None
    try:
        data = json.loads(path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        raise StateError('state.unreadable', path) from None
    if not isinstance(data, dict):
        raise StateError('state.unreadable', path)
    version = data.get('format', 0)
    if type(version) is not int or version < 0:
        raise StateError('state.unreadable', path)
    if version > FORMAT:
        raise StateError('state.newer_format', path)
    # The GUI and resume planner read these fields before running a step. A JSON value of the wrong type must take
    # the same error path as unreadable JSON, rather than raising from their comparisons or dict lookups.
    for name in ('release', 'lang', 'version'):
        if name in data and not isinstance(data[name], str):
            raise StateError('state.unreadable', path)
    for name in ('steps', 'inputs'):
        records = data.get(name, {} if name == 'inputs' else None)
        if not isinstance(records, dict) or any(not isinstance(record, dict) for record in records.values()):
            raise StateError('state.unreadable', path)
    data.setdefault('inputs', {})   # older records may omit it; the extraction step expects a dictionary
    return data


def save(dest, data):
    path = Path(dest) / NAME
    tmp = path.with_name(NAME + '.tmp')
    with open(tmp, 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, indent=1, ensure_ascii=False)
        f.write('\n')
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, path)
