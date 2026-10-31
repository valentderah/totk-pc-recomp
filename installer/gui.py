"""The installer window: python -m installer.gui

A front end over the CLI (docs/installer.md): it collects the game files, the keys file path, the game language and the
install folder, runs `python -m installer ...` as a subprocess and shows its JSON events. Decisions live in guilogic.py;
this file only draws. The window text is in installer/text/<en|ru>.json under "ui.*" and "launcher.*" ids.

    --selftest [--shots DIR]   build every screen in both languages and close by itself (PNG per screen with --shots)
    --lang en|ru               window language (default: the Windows display language)
"""
import argparse
import json
import os
import subprocess
import sys
import tkinter as tk
from pathlib import Path
from tkinter import filedialog, messagebox, ttk

try:
    from PIL import ImageGrab   # only the screenshots of --selftest --shots need it; the release has no PIL
except ImportError:
    ImageGrab = None

from . import ini
from . import layout as layouts
from .guilogic import (DONE, FAILED, PENDING, RUNNING, SKIPPED, UI_LANGUAGES, Catalog, Form, Model, Process,
                       amount, default_game_language, describe_event, parse_line, windows_ui_language)

POLL_MS = 100
GLYPH = {PENDING: '\u25cb', RUNNING: '\u25b6', DONE: '\u2714', SKIPPED: '\u2013', FAILED: '\u2716'}
COLOR = {PENDING: '#808080', RUNNING: '#1a5fb4', DONE: '#26a269', SKIPPED: '#808080', FAILED: '#c01c28'}
PAD = 14


def fix_tcl_paths():
    """The embeddable Python keeps Tcl/Tk next to python.exe (python/tcl); point Tcl at it when it is not found."""
    tcl = Path(sys.executable).resolve().parent / 'tcl'
    if tcl.is_dir() and 'TCL_LIBRARY' not in os.environ:
        for lib, var in (('tcl9.0', 'TCL_LIBRARY'), ('tk9.0', 'TK_LIBRARY')):
            if (tcl / lib).is_dir():
                os.environ[var] = str(tcl / lib)


class App:
    def __init__(self, root, ui_lang, layout, run_args=None):
        self.root, self.layout = root, layout
        self.cat = Catalog(ui_lang)
        self.model = Model()
        self.process = None
        self.closing = False
        self.run_args = run_args or {}   # extra CLI options for tests: temp, verify
        self.log_entries = []            # events and raw lines of the run, rendered by render_log
        self.bound = []                  # (widget, option, id, params) retranslated on a language switch
        self.vars = {n: tk.StringVar() for n in ('base', 'update', 'keys', 'dest')}
        self.vars['lang'] = tk.StringVar()
        self.lang_code = default_game_language(ui_lang)
        self.screen = None
        self.fitted = None               # the (width, height) fit last gave the window

        root.title(self.cat('ui.title'))
        icon = layout.runtime / 'totk.ico'
        png = layout.runtime / 'totk.png'   # Tk loads no .ico off Windows; the Linux release converts it
        if os.name == 'nt' and icon.is_file():
            root.iconbitmap(default=str(icon))
        elif png.is_file():
            self.icon = tk.PhotoImage(file=str(png))
            root.iconphoto(True, self.icon)
        root.protocol('WM_DELETE_WINDOW', self.on_close)
        self.ui_font = 'Segoe UI' if os.name == 'nt' else 'DejaVu Sans'
        self.mono_font = 'Consolas' if os.name == 'nt' else 'DejaVu Sans Mono'
        style = ttk.Style(root)
        style.configure('Title.TLabel', font=(self.ui_font, 15, 'bold'))
        style.configure('Notice.TLabel', background='#fff3cd', foreground='#664d03', padding=8)
        style.configure('Err.TLabel', foreground='#c01c28')
        style.configure('Version.TLabel', foreground='#808080')

        top = ttk.Frame(root, padding=(PAD, PAD, PAD, 0))
        top.pack(fill='x')
        self.title = self.label(top, 'ui.title', style='Title.TLabel')
        self.title.pack(side='left')
        version = ttk.Label(top, style='Version.TLabel')
        self.bind_text(version, 'text', 'ui.version', version=layout.release)
        version.pack(side='left', padx=(10, 0), pady=(6, 0))
        sw = ttk.Frame(top)
        sw.pack(side='right')
        self.ui_var = tk.StringVar(value=self.cat.lang)
        for code in UI_LANGUAGES:
            ttk.Radiobutton(sw, text=code.upper(), value=code, variable=self.ui_var, style='Toolbutton',
                            command=self.switch_language).pack(side='left', padx=1)
        self.body = ttk.Frame(root, padding=PAD)
        self.body.pack(fill='both', expand=True)
        self.frames = {'form': self.build_form(), 'run': self.build_run(), 'finish': self.build_finish()}
        self.vars['lang'].set(self.lang_label(self.lang_code))
        self.vars['dest'].trace_add('write', lambda *_: self.refresh_form())
        self.show('form')

    # ---- text -----------------------------------------------------------------------------------------------------

    def bind_text(self, widget, option, id, **params):
        self.bound.append((widget, option, id, params))
        widget.configure(**{option: self.cat(id, **params)})

    def label(self, parent, id, **kw):
        w = ttk.Label(parent, **kw)
        self.bind_text(w, 'text', id)
        return w

    def button(self, parent, id, command, **kw):
        w = ttk.Button(parent, command=command, **kw)
        self.bind_text(w, 'text', id)
        return w

    def retext(self):
        self.root.title(self.cat('ui.title'))
        for widget, option, id, params in self.bound:
            widget.configure(**{option: self.cat(id, **params)})
        self.fill_languages()
        self.refresh_form()
        self.refresh_run()
        self.refresh_finish()
        self.render_log()

    def switch_language(self):
        self.cat.set_language(self.ui_var.get())
        code = self.selected_lang()
        self.retext()
        self.vars['lang'].set(self.lang_label(code))
        self.fit()

    def lang_label(self, code):
        return self.cat('lang.' + code)

    def fill_languages(self):
        code = self.selected_lang()
        self.lang_box.configure(values=[self.lang_label(c) for c in ini.LANGUAGES])
        self.vars['lang'].set(self.lang_label(code))

    def selected_lang(self):
        """The game language code behind the combobox text (kept when it is not one of the labels)."""
        shown = self.vars['lang'].get()
        for code in ini.LANGUAGES:
            for lang in UI_LANGUAGES:
                if shown == Catalog(lang)('lang.' + code):
                    return code
        return self.lang_code

    # ---- screens --------------------------------------------------------------------------------------------------

    def show(self, name):
        for f in self.frames.values():
            f.pack_forget()
        self.frames[name].pack(fill='both', expand=True)
        self.screen = name
        self.fit()

    def fit(self):
        """Size the window to its largest screen (an unpacked frame still reports the size its children ask for), so
        a screen or language switch neither cuts text nor moves the buttons. The window only grows: opening the log
        makes it taller, and a size the user dragged it to stays. Every text change that can add lines calls it again,
        or pack takes the missing height from the button bars."""
        self.root.update_idletasks()
        sizes = [(f.winfo_reqwidth(), f.winfo_reqheight()) for f in self.frames.values()]
        chrome = self.root.winfo_reqheight() - self.body.winfo_reqheight()   # the title row above the body
        w = max(self.root.winfo_reqwidth(), max(s[0] for s in sizes) + 2 * PAD)
        h = chrome + max(s[1] for s in sizes) + 2 * PAD
        if (w, h) == self.fitted:   # refresh_run calls it on every poll: leave the window alone while nothing grew
            return
        self.fitted = (w, h)
        self.root.minsize(w, h)
        self.root.geometry(f'{max(w, self.root.winfo_width())}x{max(h, self.root.winfo_height())}')

    def notice(self, parent):
        w = ttk.Label(parent, style='Notice.TLabel', wraplength=620, justify='left')
        self.bind_text(w, 'text', 'ui.notice')
        return w

    def build_form(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)   # packed first so pack shrinks the text above it, never the buttons
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.start_button = self.button(bar, 'ui.start', self.start)
        self.start_button.pack(side='right')
        self.form_intro = self.label(f, 'ui.intro', wraplength=620, justify='left')
        self.form_intro.pack(anchor='w', pady=(0, 8))
        self.notice(f).pack(fill='x', pady=(0, 10))
        grid = ttk.Frame(f)
        grid.pack(fill='x')
        grid.columnconfigure(1, weight=1)
        rows = (('base', 'ui.base', 'ui.pick_game'), ('update', 'ui.update', 'ui.pick_update'),
                ('keys', 'ui.keys', 'ui.pick_keys'))
        self.form_rows = {}
        for i, (name, label_id, pick_id) in enumerate(rows):
            lab = self.label(grid, label_id)
            lab.grid(row=i, column=0, sticky='w', pady=4, padx=(0, 10))
            ttk.Entry(grid, textvariable=self.vars[name]).grid(row=i, column=1, sticky='ew', pady=4)
            b = self.button(grid, 'ui.browse', lambda n=name, p=pick_id: self.pick_file(n, p))
            b.grid(row=i, column=2, padx=(8, 0))
            self.form_rows[name] = lab
        i = len(rows)
        self.label(grid, 'ui.game_language').grid(row=i, column=0, sticky='w', pady=4)
        self.lang_box = ttk.Combobox(grid, textvariable=self.vars['lang'], state='readonly', width=30)
        self.lang_box.grid(row=i, column=1, sticky='w', pady=4)
        self.lang_touched = False
        self.lang_box.bind('<<ComboboxSelected>>', lambda _e: setattr(self, 'lang_touched', True))
        self.fill_languages()
        self.label(grid, 'ui.dest').grid(row=i + 1, column=0, sticky='w', pady=4)
        ttk.Entry(grid, textvariable=self.vars['dest']).grid(row=i + 1, column=1, sticky='ew', pady=4)
        self.button(grid, 'ui.browse', self.pick_dest).grid(row=i + 1, column=2, padx=(8, 0))
        self.update_hint = ttk.Label(f, wraplength=620, justify='left')
        self.update_hint.pack(anchor='w', pady=(8, 0))
        self.form_error = ttk.Label(f, style='Err.TLabel', wraplength=620, justify='left')
        self.form_error.pack(anchor='w', pady=(8, 0))
        self.label(f, 'ui.keys_private', wraplength=620, justify='left').pack(anchor='w', pady=(8, 0))
        return f

    def build_run(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(8, 0))
        self.cancel_button = self.button(bar, 'ui.cancel', self.cancel)
        self.cancel_button.pack(side='right')
        self.log_toggle = self.button(bar, 'ui.show_log', self.toggle_log)
        self.log_toggle.pack(side='left')
        self.run_header = ttk.Label(f, font=(self.ui_font, 11, 'bold'))
        self.run_header.pack(anchor='w')
        self.step_rows = {}
        self.steps_box = ttk.Frame(f)
        self.steps_box.pack(fill='x', pady=(8, 8))
        self.phase_label = ttk.Label(f, wraplength=620, justify='left')
        self.phase_label.pack(anchor='w')
        self.bar = ttk.Progressbar(f, mode='determinate', maximum=1000)
        self.bar.pack(fill='x', pady=(4, 6))
        self.space_label = ttk.Label(f, wraplength=620, justify='left')
        self.space_label.pack(anchor='w')
        self.warn_label = ttk.Label(f, wraplength=620, justify='left', foreground='#9a6700')
        self.warn_label.pack(anchor='w')
        self.log_frame = ttk.Frame(f)
        self.log = tk.Text(self.log_frame, height=10, wrap='word', state='disabled', font=(self.mono_font, 9))
        scroll = ttk.Scrollbar(self.log_frame, command=self.log.yview)
        self.log.configure(yscrollcommand=scroll.set)
        scroll.pack(side='right', fill='y')
        self.log.pack(side='left', fill='both', expand=True)
        self.log_open = False
        return f

    def build_finish(self):
        f = ttk.Frame(self.body)
        bar = ttk.Frame(f)
        bar.pack(side='bottom', fill='x', pady=(12, 0))
        self.button(bar, 'ui.close', self.on_close).pack(side='right')
        self.open_button = self.button(bar, 'ui.open_folder', self.open_folder)
        self.open_button.pack(side='right', padx=(0, 8))
        self.back_button = self.button(bar, 'ui.back', lambda: self.show('form'))
        self.back_button.pack(side='left')
        self.finish_title = ttk.Label(f, style='Title.TLabel', wraplength=620, justify='left')
        self.finish_title.pack(anchor='w')
        self.finish_text = ttk.Label(f, wraplength=620, justify='left')
        self.finish_text.pack(anchor='w', pady=(8, 8))
        self.finish_steps = ttk.Frame(f)
        self.finish_steps.pack(fill='x', pady=(0, 8))
        self.finish_notice = self.notice(f)
        self.finish_notice.pack(fill='x', pady=(8, 0))
        return f

    # ---- form -----------------------------------------------------------------------------------------------------

    def form(self):
        return Form(base=self.vars['base'].get(), update=self.vars['update'].get(), keys=self.vars['keys'].get(),
                    lang=self.selected_lang(), dest=self.vars['dest'].get())

    def refresh_form(self):
        form = self.form()
        state = form.installed_state()
        if state is not None:
            self.update_hint.configure(text=self.cat('ui.update_mode'))
            recorded = ini.canonical_language(state.get('lang'))
            if recorded in ini.LANGUAGES and not self.lang_touched:
                self.vars['lang'].set(self.lang_label(recorded))
        else:
            self.update_hint.configure(text='')
        self.fit()

    def pick_file(self, name, pick_id):
        types = [(self.cat('ui.type_game'), '*.nsp *.xci'), (self.cat('ui.type_all'), '*.*')]
        if name == 'update':
            types = [(self.cat('ui.type_nsp'), '*.nsp'), (self.cat('ui.type_all'), '*.*')]
        if name == 'keys':
            types = [(self.cat('ui.type_keys'), '*.keys'), (self.cat('ui.type_all'), '*.*')]
        path = filedialog.askopenfilename(title=self.cat(pick_id), filetypes=types)
        if path:
            self.vars[name].set(os.path.normpath(path))

    def pick_dest(self):
        path = filedialog.askdirectory(title=self.cat('ui.pick_dest'), mustexist=False)
        if path:
            self.vars['dest'].set(os.path.normpath(path))

    # ---- run ------------------------------------------------------------------------------------------------------

    def start(self):
        form = self.form()
        problems = form.problems()
        if problems:
            self.form_error.configure(text=' '.join(self.cat(p) for p in problems))
            self.fit()
            return
        self.form_error.configure(text='')
        command = form.command(self.layout.python, root=self.layout.root if self.layout.dev else None,
                               temp=self.run_args.get('temp'), verify=self.run_args.get('verify', False))
        self.begin(command, self.layout.root)

    def begin(self, command, cwd):
        self.model = Model()
        self.model.dest = self.vars['dest'].get().strip()
        self.clear_log()
        self.build_step_rows(self.model.steps)
        self.cancel_button.configure(state='normal')
        self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
        self.bind_text(self.cancel_button, 'text', 'ui.cancel')
        self.show('run')
        self.refresh_run()
        self.process = Process(command, cwd=cwd, helper_python=self.layout.python)
        self.root.after(POLL_MS, self.poll)

    def build_step_rows(self, steps):
        for w in self.steps_box.winfo_children():
            w.destroy()
        self.step_rows = {}
        for s in steps:
            row = ttk.Frame(self.steps_box)
            row.pack(fill='x', pady=1)
            glyph = ttk.Label(row, width=3)
            glyph.pack(side='left')
            name = ttk.Label(row)
            name.pack(side='left')
            self.step_rows[s] = (glyph, name)

    def poll(self):
        if self.process is None:
            return
        try:
            while True:
                line = self.process.lines.get_nowait()
                if line is not None:
                    self.feed(line)
        except Exception:
            pass
        code = self.process.poll()
        self.refresh_run()
        if code is None:
            self.root.after(POLL_MS, self.poll)
            return
        self.model.finish(code)
        self.process = None
        self.refresh_run()
        self.refresh_finish()
        self.show('finish')
        if self.closing:
            self.root.destroy()

    def feed(self, line):
        """One stdout line of the CLI: an event updates the model and the log, anything else goes to the log as is."""
        event = parse_line(line)
        if event is None:
            if line.strip():
                self.log_entries.append(line.rstrip())
                self.append_log(line.rstrip())
            return
        before = list(self.model.steps)
        self.model.apply(event)
        if self.model.steps != before:
            self.build_step_rows(self.model.steps)
        self.log_entries.append(event)
        self.append_log(describe_event(self.cat, event))

    def refresh_run(self):
        m = self.model
        for s, (glyph, name) in self.step_rows.items():
            state = m.states.get(s, PENDING)
            glyph.configure(text=GLYPH[state], foreground=COLOR[state])
            name.configure(text=self.cat('step.' + s) + (f'  ({self.cat("ui.state_" + state)})'
                                                          if state in (SKIPPED, FAILED) else ''),
                           font=(self.ui_font, 10, 'bold') if state == RUNNING else (self.ui_font, 10))
        mode = m.mode or 'install'
        self.run_header.configure(text=self.cat('ui.running_' + mode if mode in ('install', 'resume', 'update')
                                                else 'ui.running_install'))
        if m.current and m.phase:
            text = self.cat(f'phase.{m.current}.{m.phase}')
            if m.total:
                text += '  ' + amount(self.cat, m.done, m.total, m.unit)
        elif m.current:
            text = self.cat('step.' + m.current)
        else:
            text = ''
        self.phase_label.configure(text=text)
        frac = m.fraction()
        if frac is None:
            if str(self.bar.cget('mode')) != 'indeterminate':
                self.bar.configure(mode='indeterminate')
                self.bar.start(15)
        else:
            if str(self.bar.cget('mode')) == 'indeterminate':
                self.bar.stop()
                self.bar.configure(mode='determinate')
            self.bar.configure(value=frac * 1000)
        self.space_label.configure(text=m.need_text(self.cat))
        self.warn_label.configure(text='\n'.join(self.cat(i, **p) for i, p in m.warnings))
        self.fit()   # new step rows and warnings make the screen taller

    def toggle_log(self):
        self.log_open = not self.log_open
        if self.log_open:
            self.log_frame.pack(fill='both', expand=True, pady=(8, 0))
        else:
            self.log_frame.pack_forget()
        self.bound = [b for b in self.bound if b[0] is not self.log_toggle]
        self.bind_text(self.log_toggle, 'text', 'ui.hide_log' if self.log_open else 'ui.show_log')
        self.fit()

    def clear_log(self):
        self.log_entries = []
        self.render_log()

    def render_log(self):
        """The whole log from the kept events in the current language (raw CLI lines stay as they are)."""
        self.log.configure(state='normal')
        self.log.delete('1.0', 'end')
        for entry in self.log_entries:
            text = entry if isinstance(entry, str) else describe_event(self.cat, entry)
            if text:
                self.log.insert('end', text + '\n')
        self.log.see('end')
        self.log.configure(state='disabled')

    def append_log(self, text):
        if text:
            self.log.configure(state='normal')
            self.log.insert('end', text + '\n')
            self.log.see('end')
            self.log.configure(state='disabled')

    def cancel(self):
        if self.process:
            self.cancel_button.configure(state='disabled')
            self.bound = [b for b in self.bound if b[0] is not self.cancel_button]
            self.bind_text(self.cancel_button, 'text', 'ui.cancelling')
            self.process.cancel()

    def on_close(self):
        if self.process:
            if not messagebox.askyesno(self.cat('ui.title'), self.cat('ui.confirm_cancel')):
                return
            self.closing = True
            self.cancel()
            return
        self.root.destroy()

    # ---- finish ---------------------------------------------------------------------------------------------------

    def refresh_finish(self):
        m = self.model
        result = m.result or 'ok'
        self.finish_title.configure(text=self.cat('ui.result_' + result))
        if result == 'ok':
            text = self.cat('ui.done_text', dest=m.dest)
        elif result == 'cancelled':
            text = self.cat('ui.cancelled_text')
        else:
            err = m.error or ('ui.process_died', {'code': m.exit_code}, None)
            text = self.cat(err[0], **err[1])
            if m.log_path:
                text += '\n\n' + self.cat('ui.log_file', path=m.log_path)
        self.finish_text.configure(text=text)
        for w in self.finish_steps.winfo_children():
            w.destroy()
        for s in m.steps:
            state = m.states.get(s, PENDING)
            ttk.Label(self.finish_steps, text=f'{GLYPH[state]}  {self.cat("step." + s)}',
                      foreground=COLOR[state]).pack(anchor='w')
        if result == 'ok':
            self.open_button.state(['!disabled'])
        else:
            self.open_button.state(['disabled' if not Path(m.dest or '.').is_dir() else '!disabled'])

    def open_folder(self):
        dest = self.model.dest
        if dest and Path(dest).is_dir():
            if os.name == 'nt':
                os.startfile(dest)
            else:
                subprocess.Popen(['xdg-open', dest], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                                 start_new_session=True)


# ---- self test ---------------------------------------------------------------------------------------------------

SAMPLE_START = {'type': 'start', 'release': 'selftest', 'mode': 'install', 'dest': 'Z:/selftest/TotK', 'log': 'D:\\log.txt',
                'steps': ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']}
SAMPLE_RUN = [
    SAMPLE_START,
    {'type': 'plan', 'run': ['check', 'extract'], 'skip': {}},
    {'type': 'step', 'step': 'check', 'state': 'started'},
    {'type': 'progress', 'step': 'check', 'phase': 'inputs'},
    {'type': 'space', 'dest': 'Z:/selftest/TotK', 'need': 36_400_000_000, 'free': 210_000_000_000, 'temp': 'C:\\t',
     'temp_need': 0, 'temp_free': 0, 'same_disk': True},
    {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}},
    {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 12.0},
    {'type': 'step', 'step': 'extract', 'state': 'started'},
    {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 6_000_000_000, 'total': 16_000_000_000,
     'unit': 'bytes'},
]


SAMPLE_OK = [SAMPLE_START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current',
                                                                       'prepare': 'current', 'cleanup': 'not_needed'}},
             {'type': 'step', 'step': 'check', 'state': 'started'},
             {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.0},
             {'type': 'step', 'step': 'extract', 'state': 'started'},
             {'type': 'step', 'step': 'extract', 'state': 'done', 'seconds': 1.0},
             {'type': 'step', 'step': 'lift', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'compile', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'prepare', 'state': 'skipped', 'reason': 'current'},
             {'type': 'step', 'step': 'cleanup', 'state': 'done', 'seconds': 0.1},
             {'type': 'end', 'result': 'ok', 'seconds': 3}]


def grab(root, path):
    if ImageGrab is None:
        raise SystemExit('--shots needs Pillow')
    root.attributes('-topmost', True)
    root.update()
    x, y, w, h = root.winfo_rootx(), root.winfo_rooty(), root.winfo_width(), root.winfo_height()
    ImageGrab.grab(bbox=(x, y, x + w, y + h), all_screens=True).save(path)


def selftest(layout, shots=None):
    """Every screen in both languages; the window closes itself. Returns 0 when nothing raised."""
    fix_tcl_paths()
    root = tk.Tk()
    root.geometry('+60+40')
    app = App(root, 'en', layout)
    steps, mixed = [], []

    def shot(name):
        root.update()
        if shots:
            Path(shots).mkdir(parents=True, exist_ok=True)
            grab(root, Path(shots) / f'{name}.png')

    def run_all():
        for lang in UI_LANGUAGES:
            app.ui_var.set(lang)
            app.switch_language()
            tag = lang
            app.vars['dest'].set('Z:/selftest/TotK')
            app.vars['base'].set('Z:/selftest/totk.xci')
            app.vars['update'].set('Z:/selftest/totk-1.2.1.nsp')
            app.vars['keys'].set('Z:/selftest/prod.keys')
            app.show('form')
            app.start()   # missing files: shows the problems
            steps.append(app.form_error.cget('text'))
            shot(f'form-{tag}')
            app.model = Model()
            app.clear_log()
            app.build_step_rows(app.model.steps)
            app.show('run')
            for e in SAMPLE_RUN:
                app.feed(json.dumps(e))
            app.log_open = False
            app.toggle_log()
            app.refresh_run()
            shot(f'run-{tag}')
            if lang == 'en':   # the log follows the window language: no English line may remain after the switch
                app.ui_var.set('ru')
                app.switch_language()
                text = app.log.get('1.0', 'end')
                mixed.append('Checking' in text or 'Extracting' not in text and 'Распаковка' not in text)
                app.ui_var.set('en')
                app.switch_language()
            app.model = Model()
            for e in SAMPLE_OK:
                app.model.apply(e)
            app.model.dest = 'Z:/selftest/TotK'
            app.model.finish(0)
            app.refresh_finish()
            app.show('finish')
            shot(f'finish-ok-{tag}')
            app.model = Model()
            for e in SAMPLE_RUN[:3] + [{'type': 'error', 'id': 'check.disk_space', 'step': 'check',
                                        'params': {'path': 'Z:/selftest/TotK', 'need_gb': 36.4, 'free_gb': 20.1}},
                                       {'type': 'end', 'result': 'failed', 'seconds': 3}]:
                app.model.apply(e)
            app.model.finish(1)
            app.refresh_finish()
            shot(f'finish-failed-{tag}')
            app.model = Model()
            app.model.finish(3)
            app.refresh_finish()
            shot(f'finish-cancelled-{tag}')
            app.toggle_log()
        root.destroy()

    root.after(200, run_all)
    root.after(60000, root.destroy)   # never leaves a window open
    root.mainloop()
    return 0 if len(steps) == len(UI_LANGUAGES) and all(steps) and mixed == [False] else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--selftest', action='store_true')
    ap.add_argument('--shots', type=Path)
    ap.add_argument('--lang', choices=UI_LANGUAGES)
    ap.add_argument('--root', type=Path)
    args = ap.parse_args(argv)
    layout = layouts.find(args.root)
    if args.selftest:
        return selftest(layout, args.shots)
    fix_tcl_paths()
    root = tk.Tk()
    App(root, args.lang or windows_ui_language(), layout)
    root.mainloop()
    return 0


if __name__ == '__main__':
    sys.exit(main())
