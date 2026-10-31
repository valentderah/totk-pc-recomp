"""The installer window without Tk: text catalogs, the form, the event model and the CLI subprocess.

gui.py draws; everything it decides lives here so tests/installer_gui_test.py runs it headless.

    Catalog      text of an id in the window language (installer/text/<lang>.json), English when that lacks it
    Form         what the player entered; command() and problems() build and check the CLI call
    Model        folds the CLI's JSON events (events.py) into step states, progress, space, errors and the result
    Process      runs `python -m installer ...`, cancels it with Ctrl+Break and feeds lines to a queue
"""
import ctypes
import json
import os
import queue
import signal
import subprocess
import sys
import threading
import time
from dataclasses import dataclass
from pathlib import Path

from . import ini
from . import state as statefile
from .events import render

TEXT = Path(__file__).resolve().parent / 'text'
UI_LANGUAGES = ('en', 'ru')
STEP_ORDER = ('check', 'extract', 'lift', 'compile', 'prepare', 'cleanup')
# message parameter -> catalog id prefix of the names that replace its raw values
PARAM_NAMES = {'lang': 'lang.', 'known': 'lang.', 'step': 'step.', 'input': 'param.input.'}
LANG_RU = 0x19   # primary language id of Russian in a Windows LANGID


def load_texts(lang):
    path = TEXT / f'{lang}.json'
    return json.loads(path.read_text(encoding='utf-8')) if path.is_file() else {}


class Catalog:
    """Window text: ids from text/<lang>.json, then text/en.json, then the id itself."""

    def __init__(self, lang='en'):
        self.english = load_texts('en')
        self.set_language(lang)

    def set_language(self, lang):
        self.lang = lang if lang in UI_LANGUAGES else 'en'
        self.texts = load_texts(self.lang)

    def has(self, id):
        return id in self.texts or id in self.english

    def __call__(self, id, **params):
        text = self.texts.get(id) or self.english.get(id)
        if text is None:
            return id
        return render({id: text}, id, {k: self.param(k, v) for k, v in params.items()})

    def param(self, name, value):
        """A message parameter as the player should read it: language codes, step ids and input names become the
        catalog's names (the ones the pickers and the step list show); the rest stays as it is."""
        prefix = PARAM_NAMES.get(name)
        if prefix:
            if isinstance(value, (list, tuple)):
                return ', '.join(self.param(name, v) for v in value)
            ident = prefix + str(value)
            if self.has(ident):
                return self(ident)
            if name == 'known':
                return ', '.join(self(prefix + c) if self.has(prefix + c) else c for c in str(value).split(', '))
        return flat(value)


def flat(value):
    return ', '.join(str(v) for v in value) if isinstance(value, (list, tuple)) else value


def windows_ui_language():
    """'ru' when the display language (Windows; elsewhere LC_ALL, LC_MESSAGES or LANG) is Russian, else 'en'."""
    if os.name != 'nt':
        loc = os.environ.get('LC_ALL') or os.environ.get('LC_MESSAGES') or os.environ.get('LANG') or ''
        return 'ru' if loc.lower().startswith('ru') else 'en'
    try:
        langid = ctypes.windll.kernel32.GetUserDefaultUILanguage()
    except (AttributeError, OSError):
        return 'en'
    return 'ru' if langid & 0x3ff == LANG_RU else 'en'


def default_game_language(ui):
    return 'ru' if ui == 'ru' else 'en-US'


def gib(n):
    """Bytes as decimal gigabytes with one digit, the unit the check.disk_space message uses."""
    return f'{n / 1e9:.1f}'


def amount(cat, done, total, unit):
    """'6.0 / 16.0 GB', '120 / 4000 files'..., the progress of a phase whose size is known."""
    if unit == 'bytes':
        return cat('ui.amount_bytes', done=gib(done or 0), total=gib(total))
    id = f'ui.amount_{unit}'
    return cat(id if cat.has(id) else 'ui.amount_other', done=done or 0, total=total, unit=unit or '')


# ---- the form ----------------------------------------------------------------------------------------------------

@dataclass
class Form:
    base: str = ''
    update: str = ''
    keys: str = ''
    lang: str = ''
    dest: str = ''

    def installed_state(self):
        """The recorded state of the install folder (totk-install.json), or None. Never raises."""
        if not self.dest.strip():
            return None
        try:
            return statefile.load(Path(self.dest.strip()))
        except statefile.StateError:
            return None

    def update_mode(self):
        return self.installed_state() is not None

    def problems(self):
        """Ids of the entries that are missing or wrong, in screen order. A folder with totk-install.json needs only
        itself; the CLI asks for the game files when it must extract again."""
        found = []
        update = self.update_mode()
        if not self.dest.strip():
            found.append('ui.need_dest')
        if not update:
            for name in ('base', 'update', 'keys'):
                value = getattr(self, name).strip()
                if not value:
                    found.append(f'ui.need_{name}')
                elif not Path(value).is_file():
                    found.append(f'ui.missing_{name}')
            if self.lang not in ini.LANGUAGES:
                found.append('ui.need_lang')
        else:
            for name in ('base', 'update', 'keys'):
                value = getattr(self, name).strip()
                if value and not Path(value).is_file():
                    found.append(f'ui.missing_{name}')
        return found

    def command(self, python, root=None, temp=None, verify=False):
        cmd = [str(python), '-m', 'installer']
        for flag, value in (('--base', self.base), ('--update', self.update), ('--keys', self.keys),
                            ('--lang', self.lang), ('--dest', self.dest)):
            if value.strip():
                cmd += [flag, value.strip()]
        if root:
            cmd += ['--root', str(root)]
        if temp:
            cmd += ['--temp', str(temp)]
        if verify:
            cmd.append('--verify')
        return cmd


# ---- events -> state ---------------------------------------------------------------------------------------------

PENDING, RUNNING, DONE, SKIPPED, FAILED = 'pending', 'running', 'done', 'skipped', 'failed'


def parse_line(line):
    """The event dict of a stdout line, or None when it is not a JSON event (a traceback line, noise)."""
    line = line.strip()
    if not line.startswith('{'):
        return None
    try:
        event = json.loads(line)
    except ValueError:
        return None
    return event if isinstance(event, dict) and isinstance(event.get('type'), str) else None


class Model:
    """What the window shows, folded from events. apply() takes one event; nothing else mutates it."""

    def __init__(self):
        self.steps = list(STEP_ORDER)
        self.states = {s: PENDING for s in self.steps}
        self.mode = ''
        self.release = ''
        self.log_path = ''
        self.dest = ''
        self.current = None          # step id running
        self.phase = None
        self.done = self.total = None
        self.unit = None
        self.space = None            # the space event
        self.warnings = []           # (id, params)
        self.error = None            # (id, params, step)
        self.result = None           # ok | failed | cancelled once the end event arrived
        self.seconds = None
        self.exit_code = None

    def apply(self, e):
        kind = e['type']
        if kind == 'start':
            self.__init__()   # a new run: nothing of an earlier one stays
            self.steps = list(e.get('steps') or STEP_ORDER)
            self.states = {s: PENDING for s in self.steps}
            self.mode, self.release = e.get('mode', ''), e.get('release', '')
            self.log_path, self.dest = e.get('log', ''), e.get('dest', '')
        elif kind == 'plan':
            for s, reason in (e.get('skip') or {}).items():
                if s in self.states and self.states[s] == PENDING:
                    self.states[s] = SKIPPED
        elif kind == 'step':
            s, state = e.get('step'), e.get('state')
            if s not in self.states:
                self.states[s] = PENDING
                self.steps.append(s)
            if state == 'started':
                self.states[s], self.current = RUNNING, s
                self.phase = self.done = self.total = self.unit = None
            elif state == 'done':
                self.states[s] = DONE
                self.current = None if self.current == s else self.current
            elif state == 'skipped':
                self.states[s] = SKIPPED
        elif kind == 'progress':
            if e.get('step') in self.states and self.states[e['step']] == PENDING:
                self.states[e['step']] = RUNNING
            self.current = e.get('step', self.current)
            self.phase = e.get('phase')
            self.done, self.total, self.unit = e.get('done'), e.get('total'), e.get('unit')
        elif kind == 'space':
            self.space = e
        elif kind == 'warning':
            self.warnings.append((e.get('id', ''), e.get('params') or {}))
        elif kind == 'error':
            self.error = (e.get('id', ''), e.get('params') or {}, e.get('step'))
            if e.get('step') in self.states:
                self.states[e['step']] = FAILED
        elif kind == 'end':
            self.result = e.get('result')
            self.seconds = e.get('seconds')
            self.current = None

    def fraction(self):
        """0..1 of the current phase, or None when its size is unknown."""
        if self.total:
            return max(0.0, min(1.0, (self.done or 0) / self.total))
        return None

    def finish(self, exit_code):
        """The process ended. A missing end event (the process died) counts as failed, or cancelled on exit code 3."""
        self.exit_code = exit_code
        if self.result is None:
            self.result = 'cancelled' if exit_code == 3 else 'failed'
        if self.result == 'failed' and self.error is None:
            self.error = ('ui.process_died', {'code': exit_code}, self.current)
        for s, state in self.states.items():
            if state == RUNNING:
                self.states[s] = FAILED if self.result == 'failed' else PENDING

    def need_text(self, cat):
        """The space line, or '' before the check step reported."""
        sp = self.space
        if not sp:
            return ''
        key = 'ui.space_one' if sp.get('same_disk') or not sp.get('temp_need') else 'ui.space_two'
        return cat(key, path=sp.get('dest', ''), need=gib(sp.get('need', 0)), free=gib(sp.get('free', 0)),
                   temp=sp.get('temp', ''), temp_need=gib(sp.get('temp_need', 0)),
                   temp_free=gib(sp.get('temp_free', 0)))


def describe_event(cat, e):
    """One log line for an event, in the window language."""
    kind = e['type']
    if kind == 'step':
        text = f'{cat("step." + e["step"])}: {cat("ui.state_" + e["state"])}'
        if 'seconds' in e:
            text += f' ({cat("ui.seconds", n=format(e["seconds"], ".1f"))})'
        elif e.get('reason'):
            text += f' ({e["reason"]})'
        return text
    if kind == 'progress':
        text = cat(f'phase.{e["step"]}.{e["phase"]}')
        if e.get('total'):
            text += '  ' + amount(cat, e.get('done'), e['total'], e.get('unit'))
        return text
    if kind in ('warning', 'error'):
        return f'{cat("ui.log_" + kind)}: {cat(e["id"], **(e.get("params") or {}))}'
    if kind == 'space':
        return cat('ui.log_space', need=gib(e.get('need', 0)), free=gib(e.get('free', 0)))
    if kind == 'end':
        return f'{cat("ui.log_end")}: {cat("ui.result_" + str(e.get("result")))}'
    return ''


# ---- the CLI subprocess ------------------------------------------------------------------------------------------

CREATE_NEW_PROCESS_GROUP = 0x200
CREATE_NO_WINDOW = 0x08000000

_BREAK = """
import ctypes, sys, time
k = ctypes.windll.kernel32
pid = int(sys.argv[1])
k.FreeConsole()
if k.AttachConsole(pid):
    k.SetConsoleCtrlHandler(None, True)
    k.GenerateConsoleCtrlEvent(1, pid)
    time.sleep(1)
    k.FreeConsole()
else:
    sys.exit(1)
"""


def send_break(proc, python=None):
    """Ctrl+Break to the process group of proc (started with CREATE_NEW_PROCESS_GROUP). The window itself may have no
    console (pythonw), so a short helper process attaches to the child's console and sends it. Elsewhere SIGTERM to
    proc's session (Process starts it in one of its own), which installer/cli.py handles as Ctrl+Break."""
    if os.name != 'nt':
        try:
            os.killpg(proc.pid, signal.SIGTERM)
            return True
        except OSError:
            return False
    helper = [str(python or sys.executable), '-c', _BREAK, str(proc.pid)]
    flags = CREATE_NO_WINDOW if os.name == 'nt' else 0
    r = subprocess.run(helper, creationflags=flags, capture_output=True)
    return r.returncode == 0


class Process:
    """`python -m installer ...` with its stdout lines going to self.lines (a queue).

    poll() returns the exit code once the process ended and its output is drained, else None."""

    def __init__(self, command, cwd=None, helper_python=None, creationflags=None):
        self.command = command
        self.helper_python = helper_python
        self.lines = queue.Queue()
        env = dict(os.environ, PYTHONIOENCODING='utf-8', PYTHONUTF8='1', PYTHONUNBUFFERED='1')
        if os.name == 'nt':
            flags = (CREATE_NEW_PROCESS_GROUP | CREATE_NO_WINDOW) if creationflags is None else creationflags
            group = {'creationflags': flags}
        else:
            group = {'start_new_session': True}
        self.proc = subprocess.Popen(command, cwd=cwd, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, **group)
        self.cancelled_at = None
        self.reader = threading.Thread(target=self.read, daemon=True)
        self.reader.start()

    def read(self):
        for raw in iter(self.proc.stdout.readline, b''):
            self.lines.put(raw.decode('utf-8', errors='replace'))
        self.proc.stdout.close()

    def cancel(self):
        if self.cancelled_at is None and self.proc.poll() is None:
            self.cancelled_at = time.monotonic()
            send_break(self.proc, self.helper_python)

    def poll(self):
        code = self.proc.poll()
        if code is not None and not self.reader.is_alive() and self.lines.empty():
            return code
        return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
