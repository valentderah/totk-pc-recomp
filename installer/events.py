"""Progress events: one JSON object per line on stdout for the window and tests, the same in English to a log file.

Every event has "type" and "t" (seconds since the start). Types:

    start     release, mode (install, resume, update), dest, log, steps (the step ids in order)
    step      step, state: started | done (seconds) | skipped (reason: current, not_needed)
    progress  step, phase, and done/total/unit (bytes, chunks, objects, shaders) when the amount is known
    space     dest, need, free (bytes on the install disk; with the temp folder on the same disk need includes it),
              temp, temp_need, temp_free, same_disk
    warning   id, params
    error     id, params, step (absent outside a step)
    end       result: ok | failed | cancelled, seconds

Text is never the payload: ids name a step ("step.<id>"), a phase ("phase.<step>.<phase>") or a message ("<id>"),
and installer/text/en.json holds their English text with {param} placeholders. The log renders events with it.
"""
import json
import os
import sys
import threading
import time
from pathlib import Path

CATALOG = Path(__file__).resolve().parent / 'text' / 'en.json'
PROGRESS_EVERY = 0.5   # seconds between two progress events of one phase


def load_catalog(path=CATALOG):
    return json.loads(path.read_text(encoding='utf-8'))


GAME_EXE = 'totk.exe' if os.name == 'nt' else 'totk'   # {game_exe} in the catalogs


def render(catalog, id, params=None):
    """The English text of id, or the id itself when the catalog lacks it."""
    text = catalog.get(id, id)
    try:
        return text.format(**{'game_exe': GAME_EXE, **(params or {})})
    except (KeyError, IndexError, ValueError):
        return f'{text} {params}'


class Reporter:
    def __init__(self, out=None, log_path=None):
        self.out = out or sys.stdout
        self.t0 = time.monotonic()
        self.catalog = load_catalog()
        self.lock = threading.Lock()
        self.log_file = None
        self.last_progress = {}
        if log_path:
            Path(log_path).parent.mkdir(parents=True, exist_ok=True)
            self.log_file = open(log_path, 'a', encoding='utf-8', buffering=1)
        self.log_path = log_path

    def elapsed(self):
        return time.monotonic() - self.t0

    def log(self, text):
        """A line for the log file only."""
        if self.log_file:
            with self.lock:
                self.log_file.write(f'[{self.elapsed():8.1f}] {text}\n')

    def emit(self, type, **fields):
        event = {'type': type, 't': round(self.elapsed(), 2), **fields}
        with self.lock:
            self.out.write(json.dumps(event, ensure_ascii=False) + '\n')
            self.out.flush()
        self.log(self.describe(event))
        return event

    def describe(self, e):
        c = self.catalog
        kind = e['type']
        if kind == 'step':
            name = render(c, 'step.' + e['step'])
            extra = f' ({e["seconds"]:.1f} s)' if 'seconds' in e else f' ({e["reason"]})' if 'reason' in e else ''
            return f'{name}: {e["state"]}{extra}'
        if kind == 'progress':
            text = render(c, f'phase.{e["step"]}.{e["phase"]}')
            if e.get('total'):
                text += f' {e["done"]}/{e["total"]} {e.get("unit", "")}'.rstrip()
            return text
        if kind in ('warning', 'error'):
            return f'{kind}: {render(c, e["id"], e.get("params"))} [{e["id"]}]'
        return json.dumps(e, ensure_ascii=False)

    def step(self, step, state, **fields):
        return self.emit('step', step=step, state=state, **fields)

    def progress(self, step, phase, done=None, total=None, unit=None, force=False):
        """A progress event, at most one per PROGRESS_EVERY per phase unless it is the first or the last."""
        now = time.monotonic()
        key = (step, phase)
        last = self.last_progress.get(key)
        final = total is not None and done is not None and done >= total
        if last is not None and not final and not force and now - last < PROGRESS_EVERY:
            return None
        self.last_progress[key] = now
        fields = {'step': step, 'phase': phase}
        if done is not None:
            fields['done'] = done
        if total is not None:
            fields['total'] = total
        if unit:
            fields['unit'] = unit
        return self.emit('progress', **fields)

    def warning(self, id, **params):
        return self.emit('warning', id=id, params=params)

    def error(self, id, params, step=None):
        fields = {'id': id, 'params': params}
        if step:
            fields['step'] = step
        return self.emit('error', **fields)

    def close(self):
        if self.log_file:
            self.log_file.close()
            self.log_file = None
