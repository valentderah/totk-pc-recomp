"""The installer window: event model, form, catalogs, cancel, DPI awareness, a self-closing Tk run and one end-to-end
run.

    .venv-py314\\Scripts\\python tests\\installer_gui_test.py

The end-to-end test runs the real CLI in --verify mode on a finished install (every step skips) and folds its events
through the same code the window uses (the fixture's step keys are recomputed for the checkout under test, because the keys fingerprint recomp/, tools/ and runtime/). The install is build/inst-gui/e2e-install: hard links to build/install-test, so
nothing there is written, except the state file and totk.ini, which a run rewrites and which are copied. The run hashes
the 17 GB RomFS; it is skipped when the hash differs from the expected one in all three tries. On the developer machine
(RAM with a bad bit) a buffered read of a file sometimes returns flipped bits from the page cache while the file on
disk is intact (an unbuffered read of install-test's romfs.bin gives the expected hash), so such a skip is the machine,
not the installer. It is also skipped when build/install-test is absent.
"""
import io
import json
import os
import queue
import re
import shutil
import subprocess
import sys
import tempfile
import time
import unittest
import unittest.mock
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import layout as layouts  # noqa: E402
from installer import dpi  # noqa: E402
from installer import recent  # noqa: E402
try:
    from installer import gui  # noqa: E402
except ImportError:   # no tkinter
    gui = None
from installer import state as statefile  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.runner import Context  # noqa: E402
from installer.steps import Fonts, all_steps  # noqa: E402
from installer.guilogic import (DONE, FAILED, PENDING, RUNNING, SKIPPED, UI_LANGUAGE_LABELS,  # noqa: E402
                                UI_LANGUAGES, Catalog, Form, Model, Process, amount, default_game_language,
                                describe_event, parse_line, ui_language_for_langid, windows_ui_language)

INSTALLED = ROOT / 'build' / 'install-test'
FIXTURE = ROOT / 'build' / 'inst-gui' / 'e2e-install'
COPIED = ('totk.ini', statefile.NAME)   # written by a run, so never linked
START = {'type': 'start', 'release': 'r1', 'mode': 'install', 'dest': 'D:\\T', 'log': 'D:\\T.log',
         'steps': ['check', 'fonts', 'extract', 'lift', 'compile', 'prepare', 'cleanup']}


def fold(*events):
    m = Model()
    for e in events:
        m.apply(e)
    return m


class ParseTest(unittest.TestCase):
    def test_event_line(self):
        self.assertEqual(parse_line('{"type": "step", "step": "check", "state": "started"}\n')['step'], 'check')

    def test_noise_is_not_an_event(self):
        for line in ('', 'Traceback (most recent call last):', '{broken', '[1, 2]', '{"no_type": 1}', '{"type": 3}'):
            self.assertIsNone(parse_line(line), line)

    def test_unicode_survives(self):
        e = parse_line(json.dumps({'type': 'warning', 'id': 'x', 'params': {'p': 'Привет'}}, ensure_ascii=False))
        self.assertEqual(e['params']['p'], 'Привет')


class ModelTest(unittest.TestCase):
    def test_steps_follow_the_events(self):
        m = fold(START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'lift': 'current', 'compile': 'current'}},
                 {'type': 'step', 'step': 'check', 'state': 'started'})
        self.assertEqual([m.states[s] for s in m.steps],
                         [RUNNING, PENDING, PENDING, SKIPPED, SKIPPED, PENDING, PENDING])
        self.assertEqual(m.current, 'check')
        m.apply({'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.0})
        self.assertEqual(m.states['check'], DONE)
        self.assertIsNone(m.current)
        m.apply({'type': 'step', 'step': 'extract', 'state': 'skipped', 'reason': 'current'})
        self.assertEqual(m.states['extract'], SKIPPED)

    def test_progress_fraction(self):
        m = fold(START, {'type': 'step', 'step': 'extract', 'state': 'started'},
                 {'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 5, 'total': 20, 'unit': 'bytes'})
        self.assertEqual((m.current, m.phase, m.fraction()), ('extract', 'copy', 0.25))
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'merge'})
        self.assertIsNone(m.fraction())   # a phase without size is indeterminate
        m.apply({'type': 'progress', 'step': 'extract', 'phase': 'copy', 'done': 30, 'total': 20})
        self.assertEqual(m.fraction(), 1.0)

    def test_a_new_step_resets_progress(self):
        m = fold(START, {'type': 'progress', 'step': 'check', 'phase': 'inputs', 'done': 1, 'total': 2},
                 {'type': 'step', 'step': 'extract', 'state': 'started'})
        self.assertIsNone(m.phase)
        self.assertIsNone(m.fraction())

    def test_error_marks_the_step(self):
        m = fold(START, {'type': 'step', 'step': 'check', 'state': 'started'},
                 {'type': 'error', 'id': 'check.disk_space', 'step': 'check', 'params': {'path': 'D:\\T'}},
                 {'type': 'end', 'result': 'failed', 'seconds': 2})
        self.assertEqual(m.states['check'], FAILED)
        self.assertEqual(m.error[0], 'check.disk_space')
        self.assertEqual(m.result, 'failed')

    def test_space_text(self):
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 36_400_000_000, 'free': 100_000_000_000, 'temp': 'C:\\t',
                  'temp_need': 0, 'temp_free': 5, 'same_disk': True})
        text = m.need_text(Catalog('en'))
        self.assertIn('36.4', text)
        self.assertIn('100.0', text)
        self.assertEqual(Model().need_text(Catalog('en')), '')
        m = fold({'type': 'space', 'dest': 'D:\\T', 'need': 20_000_000_000, 'free': 1, 'temp': 'C:\\t',
                  'temp_need': 2_000_000_000, 'temp_free': 3, 'same_disk': False})
        self.assertIn('C:\\t', m.need_text(Catalog('en')))

    def test_warnings_collect(self):
        m = fold(START, {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}})
        self.assertEqual(m.warnings, [('warn.lang_needs_fonts', {'lang': 'ko'})])

    def test_finish_without_end_event(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'})
        m.finish(-1073741510)
        self.assertEqual(m.result, 'failed')
        self.assertEqual(m.states['lift'], FAILED)
        self.assertEqual(m.error[0], 'ui.process_died')

    def test_finish_after_cancel(self):
        m = fold(START, {'type': 'step', 'step': 'lift', 'state': 'started'}, {'type': 'end', 'result': 'cancelled'})
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')
        self.assertIsNone(m.error)
        self.assertEqual(m.states['lift'], PENDING)   # not finished; the next run resumes here

    def test_exit_code_3_alone_is_cancelled(self):
        m = Model()
        m.finish(3)
        self.assertEqual(m.result, 'cancelled')

    def test_unknown_event_is_ignored(self):
        m = fold(START, {'type': 'future_thing', 'x': 1})
        self.assertEqual(m.states['check'], PENDING)


class RunStatesTest(unittest.TestCase):
    OK = [START, {'type': 'plan', 'run': ['check', 'extract'], 'skip': {'fonts': 'current', 'lift': 'current',
                                                                     'compile': 'current', 'prepare': 'current',
                                                                     'cleanup': 'not_needed'}},
          {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
          {'type': 'step', 'step': 'extract', 'state': 'started'}, {'type': 'step', 'step': 'extract', 'state': 'done'},
          {'type': 'step', 'step': 'fonts', 'state': 'skipped'},
          {'type': 'step', 'step': 'lift', 'state': 'skipped'}, {'type': 'step', 'step': 'compile', 'state': 'skipped'},
          {'type': 'step', 'step': 'prepare', 'state': 'skipped'}, {'type': 'step', 'step': 'cleanup', 'state': 'done'},
          {'type': 'end', 'result': 'ok', 'seconds': 1}]
    FAILED = [START, {'type': 'step', 'step': 'check', 'state': 'started'}, {'type': 'step', 'step': 'check', 'state': 'done'},
              {'type': 'step', 'step': 'extract', 'state': 'started'},
              {'type': 'error', 'id': 'extract.romfs_mismatch', 'step': 'extract', 'params': {}},
              {'type': 'warning', 'id': 'warn.lang_needs_fonts', 'params': {'lang': 'ko'}},
              {'type': 'end', 'result': 'failed', 'seconds': 1}]

    def test_ok_run_has_only_done_and_skipped_steps(self):
        m = fold(*self.OK)
        m.finish(0)
        self.assertEqual(m.result, 'ok')
        self.assertIsNone(m.error)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})

    def test_a_new_run_keeps_nothing_of_the_failed_one(self):
        m = fold(*self.FAILED)
        m.finish(1)
        self.assertEqual(m.states['extract'], FAILED)
        for e in self.OK:   # the same model object fed a second run (the window makes a new one, but even then)
            m.apply(e)
        m.finish(0)
        self.assertEqual({m.states[s] for s in m.steps}, {DONE, SKIPPED})
        self.assertIsNone(m.error)
        self.assertEqual(m.warnings, [])
        self.assertEqual(m.result, 'ok')


class FormTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        for name in ('base.nsp', 'upd.nsp', 'prod.keys'):
            (self.dir / name).write_bytes(b'x')

    def tearDown(self):
        self.tmp.cleanup()

    def form(self, **kw):
        values = dict(base=str(self.dir / 'base.nsp'), update=str(self.dir / 'upd.nsp'), keys=str(self.dir / 'prod.keys'),
                      lang='en-US', dest=str(self.dir / 'out'))
        values.update(kw)
        return Form(**values)

    def test_complete_form(self):
        self.assertEqual(self.form().problems(), [])

    def test_empty_form(self):
        self.assertEqual(Form().problems(), ['ui.need_dest', 'ui.need_base', 'ui.need_update', 'ui.need_keys',
                                             'ui.need_lang'])

    def test_missing_files(self):
        self.assertEqual(self.form(base=str(self.dir / 'no.nsp'), keys=str(self.dir / 'no.keys')).problems(),
                         ['ui.missing_base', 'ui.missing_keys'])

    def test_unknown_language(self):
        self.assertEqual(self.form(lang='xx').problems(), ['ui.need_lang'])

    def test_firmware_folder(self):
        firmware = self.dir / 'firmware'
        firmware.mkdir()
        self.assertEqual(self.form(firmware=str(firmware)).problems(), [])
        self.assertEqual(self.form(firmware=str(self.dir / 'no-firmware')).problems(), ['ui.missing_firmware'])

    def test_command(self):
        firmware = self.dir / 'firmware'
        firmware.mkdir()
        cmd = self.form(firmware=str(firmware)).command('py.exe', root=ROOT, temp=self.dir / 't', verify_hashes=True)
        self.assertEqual(cmd[:3], ['py.exe', '-m', 'installer'])
        for flag in ('--base', '--update', '--keys', '--firmware', '--lang', '--dest', '--root', '--temp', '--verify'):
            self.assertIn(flag, cmd)
        self.assertEqual(cmd[cmd.index('--firmware') + 1], str(firmware))
        self.assertEqual(cmd[cmd.index('--lang') + 1], 'en-US')

    def test_installed_folder_needs_only_itself(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0') | {'lang': 'ru'})
        form = Form(dest=str(out))
        self.assertTrue(form.update_mode())
        self.assertEqual(form.problems(), [])
        cmd = form.command('py.exe')
        self.assertEqual(cmd, ['py.exe', '-m', 'installer', '--dest', str(out)])

    def test_installed_folder_still_checks_given_files(self):
        out = self.dir / 'out'
        statefile.save(self._mkdir(out), statefile.empty('r0'))
        self.assertEqual(Form(dest=str(out), base=str(self.dir / 'no.nsp')).problems(), ['ui.missing_base'])

    def test_broken_state_is_a_new_install(self):
        out = self._mkdir(self.dir / 'out')
        (out / statefile.NAME).write_text('{nope', encoding='utf-8')
        self.assertFalse(Form(dest=str(out)).update_mode())

    @staticmethod
    def _mkdir(path):
        path.mkdir(parents=True, exist_ok=True)
        return path


class CatalogTest(unittest.TestCase):
    def test_every_language_covers_english_with_the_same_placeholders(self):
        en = Catalog('en').english
        for lang in UI_LANGUAGES[1:]:
            with self.subTest(lang=lang):
                other = Catalog(lang).texts
                self.assertEqual(sorted(set(en) - set(other)), [])
                self.assertEqual(sorted(set(other) - set(en)), [])
                holes = [k for k in en
                         if sorted(re.findall(r'\{\w+\}', en[k])) != sorted(re.findall(r'\{\w+\}', other[k]))]
                self.assertEqual(holes, [])

    def test_language_names_tell_the_codes_apart(self):
        # gui.App.selected_lang maps the shown name back to its code across every window language
        names = {}
        for lang in UI_LANGUAGES:
            cat = Catalog(lang)
            for id in cat.english:
                if id.startswith('lang.'):
                    names.setdefault(cat(id), set()).add(id)
        self.assertEqual({n: ids for n, ids in names.items() if len(ids) > 1}, {})
        self.assertEqual(sorted(UI_LANGUAGE_LABELS), sorted(UI_LANGUAGES))

    def test_every_id_the_window_uses_exists(self):
        cat = Catalog('en')
        ids = ['ui.' + n for n in ('title', 'start', 'cancel', 'open_folder', 'notice', 'space_one', 'space_two',
                                   'process_died', 'result_ok', 'result_failed', 'result_cancelled')]
        ids += [f'step.{s}' for s in ('check', 'fonts', 'extract', 'lift', 'compile', 'prepare', 'cleanup')]
        ids += ['ui.state_' + n for n in ('started', 'done', 'skipped', 'pending', 'running', 'failed')]
        for lang in UI_LANGUAGES:
            cat.set_language(lang)
            self.assertEqual(cat.lang, lang)
            self.assertEqual([i for i in ids if not cat.has(i)], [])

    def test_codes_become_names(self):
        for lang, korean in (('en', 'Korean'), ('ru', 'Корейский'), ('pt-BR', 'Coreano'), ('zh-Hans', '韩语')):
            cat = Catalog(lang)
            self.assertIn(korean, cat('warn.lang_needs_fonts', lang='ko'))
            self.assertNotIn(' ko ', cat('warn.lang_needs_fonts', lang='ko'))
            self.assertNotIn('extract', cat('state.output_changed', step='extract'))
            for word in ('base', 'update', 'keys'):
                self.assertIn(cat('param.input.' + word), cat('check.keys_wrong', input=word))
            self.assertNotIn('en-US', cat('check.lang_unknown', lang='xx', known='ja, en-US'))
        self.assertEqual(Catalog('en')('check.lang_unknown', lang='xx', known='ja, en-US'),
                         "Unknown language 'xx'. Known: Japanese, English (US).")

    def test_fallback_and_params(self):
        cat = Catalog('ru')
        self.assertIn('36.4', cat('ui.log_space', need='36.4', free='1.0'))
        self.assertEqual(cat('no.such.id'), 'no.such.id')
        self.assertIn('a, b', cat('check.release_incomplete', paths=['a', 'b']))
        cat.set_language('de')   # unknown language falls back to English
        self.assertEqual(cat.lang, 'en')

    def test_describe_and_amount(self):
        cat = Catalog('en')
        self.assertEqual(amount(cat, 6e9, 16e9, 'bytes'), '6.0 / 16.0 GB')
        self.assertEqual(amount(cat, 1, 9, 'shaders'), '1 / 9 shaders')
        self.assertEqual(amount(cat, 1, 9, 'widgets'), '1 / 9 widgets')
        line = describe_event(cat, {'type': 'step', 'step': 'check', 'state': 'done', 'seconds': 1.5})
        self.assertIn('1.5 s', line)
        self.assertEqual(describe_event(cat, {'type': 'start'}), '')

    def test_windows_language_is_a_known_value(self):
        self.assertIn(windows_ui_language(), UI_LANGUAGES)

    def test_windows_langid_picks_the_window_language(self):
        for langid, lang in ((0x0419, 'ru'), (0x0819, 'ru'), (0x0416, 'pt-BR'), (0x0816, 'en'),
                             (0x0804, 'zh-Hans'), (0x1004, 'zh-Hans'),
                             (0x0404, 'en'), (0x0c04, 'en'), (0x1404, 'en'), (0x0409, 'en'), (0x0411, 'en')):
            self.assertEqual(ui_language_for_langid(langid), lang, hex(langid))

    def test_chinese_window_starts_with_a_game_language_that_needs_no_shared_fonts(self):
        self.assertEqual(default_game_language('zh-Hans'), 'en-US')
        self.assertEqual(default_game_language('ru'), 'ru')


BREAK_CHILD = 'import time\nprint("ready", flush=True)\ntime.sleep(60)\n'


class CancelTest(unittest.TestCase):
    def test_ctrl_break_ends_the_process_group(self):
        p = Process([sys.executable, '-c', BREAK_CHILD])
        try:
            self.assertEqual(p.lines.get(timeout=20).strip(), 'ready')
            t0 = time.monotonic()
            p.cancel()
            code = None
            while code is None and time.monotonic() - t0 < 20:
                code = p.poll()
                time.sleep(0.05)
            self.assertIsNotNone(code, 'the child ignored Ctrl+Break')
            self.assertNotEqual(code, 0)
        finally:
            p.kill()


class DpiTest(unittest.TestCase):
    def test_px_follows_the_display_scale(self):
        scale = dpi.scale()
        self.assertGreaterEqual(scale, 1.0)
        self.assertEqual(dpi.px(0), 0)
        self.assertEqual(dpi.px(14), round(14 * scale))

    def test_px_at_the_scales_of_other_displays(self):
        """A 96 DPI runner cannot show the scaling: give px() the scales Windows reports at 125, 150 and 200%."""
        saved = dpi._scale
        try:
            for scale, pad, wrap in ((1.0, 14, 620), (1.25, 18, 775), (1.5, 21, 930), (2.0, 28, 1240)):
                dpi._scale = scale
                self.assertEqual((dpi.px(14), dpi.px(620)), (pad, wrap), scale)
        finally:
            dpi._scale = saved

    @unittest.skipUnless(os.name == 'nt', 'the awareness and the DPI Tk reads are Windows settings')
    def test_the_window_draws_at_the_display_scale(self):
        """A fresh process imports the window (which makes itself DPI aware) and starts Tk: the fonts must be sized for
        the display, not for 96 DPI, which is what Windows then stretched (the blurry window)."""
        try:
            import tkinter  # noqa: F401
        except ImportError:
            self.skipTest('no tkinter')
        code = ('import tkinter\n'
                'from installer import dpi, gui\n'
                'root = tkinter.Tk()\n'
                'print(dpi.awareness(), dpi.scale(), root.tk.call("tk", "scaling"), gui.PAD, gui.WRAP)\n'
                'root.destroy()\n')
        r = subprocess.run([sys.executable, '-c', code], cwd=ROOT, capture_output=True, text=True, timeout=120)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])
        awareness, scale, scaling, pad, wrap = r.stdout.split()
        self.assertGreaterEqual(int(awareness), 1)   # 1 system aware, 2 per-monitor aware
        self.assertAlmostEqual(float(scaling), float(scale) * 96 / 72, places=6)
        self.assertEqual((int(pad), int(wrap)), (round(14 * float(scale)), round(620 * float(scale))))


class RecentTest(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.file = self.tmp / 'sub' / 'setup.json'
        old = os.environ.get(recent.ENV)
        os.environ[recent.ENV] = str(self.file)
        self.addCleanup(lambda: os.environ.pop(recent.ENV, None) if old is None else os.environ.update({recent.ENV: old}))
        self.dest = self.tmp / 'TotK'
        self.dest.mkdir()

    def test_override_is_honoured(self):
        self.assertEqual(recent.path(), self.file)

    def test_missing_file(self):
        self.assertEqual(recent.load(), {})

    def installed(self):
        (self.dest / statefile.NAME).write_text('{}', encoding='utf-8')

    def files(self):
        out = {}
        for name in ('base', 'update', 'keys'):
            f = self.tmp / (name + '.bin')
            f.write_text('x', encoding='utf-8')
            out[name] = str(f)
        fw = self.tmp / 'fw'
        fw.mkdir()
        out['firmware'] = str(fw)
        return out

    def write(self, **fields):
        self.file.parent.mkdir(parents=True, exist_ok=True)
        self.file.write_text(json.dumps(fields), encoding='utf-8')

    def test_folder_without_state(self):
        recent.save(self.dest)
        self.assertEqual(recent.load(), {})

    def test_round_trip(self):
        self.installed()
        src = self.files()
        recent.save(self.dest, src)
        want = {name: os.path.abspath(v) for name, v in src.items()}
        want['dest'] = os.path.abspath(self.dest)
        self.assertEqual(json.loads(self.file.read_text(encoding='utf-8')), {'format': 1, **want})
        self.assertEqual(recent.load(), want)
        self.assertFalse(self.file.with_name('setup.json.tmp').exists())

    def test_empty_field_keeps_saved_value(self):
        self.installed()
        src = self.files()
        recent.save(self.dest, src)
        recent.save(self.dest, {'base': '', 'update': '  ', 'keys': src['keys']})
        self.assertEqual(recent.load()['base'], os.path.abspath(src['base']))
        self.assertEqual(recent.load()['firmware'], os.path.abspath(src['firmware']))

    def test_missing_path_not_saved(self):
        self.installed()
        recent.save(self.dest, {'base': str(self.tmp / 'gone.xci'), 'firmware': str(self.tmp / 'nofw')})
        self.assertEqual(recent.load(), {'dest': os.path.abspath(self.dest)})
        self.assertNotIn('base', json.loads(self.file.read_text(encoding='utf-8')))

    def test_vanished_path_dropped_on_load(self):
        self.installed()
        src = self.files()
        recent.save(self.dest, src)
        os.remove(src['base'])
        self.assertNotIn('base', recent.load())
        self.assertIn('update', recent.load())

    def test_directory_as_file_dropped(self):
        self.installed()
        self.write(format=1, dest=str(self.dest), base=str(self.tmp), update=str(self.tmp), keys=str(self.tmp))
        self.assertEqual(recent.load(), {'dest': str(self.dest)})

    def test_file_as_firmware_dropped(self):
        self.installed()
        src = self.files()
        self.write(format=1, dest=str(self.dest), firmware=src['base'])
        self.assertEqual(recent.load(), {'dest': str(self.dest)})

    def test_dest_file_dropped(self):
        f = self.tmp / 'afile'
        f.write_text('x', encoding='utf-8')
        self.write(format=1, dest=str(f))
        self.assertEqual(recent.load(), {})

    def test_bad_values_dropped(self):
        self.installed()
        src = self.files()
        self.write(format=1, dest=str(self.dest), base=5, update='', keys='rel/prod.keys', firmware=['x'])
        self.assertEqual(recent.load(), {'dest': str(self.dest)})
        self.write(format=1, dest=str(self.dest), base=src['base'] + '\0x', update=None, keys=True)
        self.assertEqual(recent.load(), {'dest': str(self.dest)})
        self.write(format=1, dest='relative/dir')
        self.assertEqual(recent.load(), {})

    def test_stat_errors_dropped(self):
        self.installed()
        src = self.files()
        self.write(format=1, dest=str(self.dest), base=src['base'])
        for exc in (OSError('share gone'), ValueError('embedded null byte')):
            with unittest.mock.patch.object(Path, 'is_file', side_effect=exc):
                self.assertEqual(recent.load(), {})

    def test_old_dest_only_format(self):
        self.installed()
        self.write(format=0, dest=str(self.dest))
        self.assertEqual(recent.load(), {'dest': str(self.dest)})

    def test_garbage_file(self):
        self.file.parent.mkdir()
        for text in ('not json', '[]', '{"dest": 5}', '{"format": 1}'):
            self.file.write_text(text, encoding='utf-8')
            self.assertEqual(recent.load(), {}, text)

    @unittest.skipIf(gui is None, 'no tkinter')
    def test_blocking_load_leaves_fields_empty(self):
        with unittest.mock.patch.object(recent, 'load', side_effect=lambda: time.sleep(5) or {'dest': 'x'}):
            start = time.monotonic()
            self.assertEqual(gui.load_recent(0.3), {})
            self.assertLess(time.monotonic() - start, 2)

    @unittest.skipIf(gui is None, 'no tkinter')
    def test_fast_load_is_returned(self):
        with unittest.mock.patch.object(recent, 'load', return_value={'dest': 'x'}):
            self.assertEqual(gui.load_recent(), {'dest': 'x'})


class FakeVar:
    def __init__(self, value=''):
        self.value = value

    def get(self):
        return self.value

    def set(self, value):
        self.value = value


@unittest.skipIf(gui is None, 'no tkinter')
class PrefillTest(unittest.TestCase):
    """App.match_prefill on a stand-in object: no window needed."""

    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.update = self.tmp / 'totk-1.4.3.nsp'
        self.update.write_bytes(b'x' * 10)
        self.base = self.tmp / 'totk.xci'
        self.base.write_bytes(b'y' * 20)
        self.app = SimpleNamespace(vars={n: FakeVar() for n in ('base', 'update')}, touched=set(),
                                   recorded={'base': str(self.base), 'update': str(self.update)})
        self.app.set_prefill = lambda name, value: self.app.vars[name].set(value)
        for name, value in self.app.recorded.items():
            self.app.vars[name].set(value)

    def state(self, update_name, update_size, base_size=20):
        return {'inputs': {'base': {'name': 'totk.xci', 'size': base_size},
                           'update': {'name': update_name, 'size': update_size}}}

    def shown(self):
        return {n: v.get() for n, v in self.app.vars.items()}

    def test_same_install_keeps(self):
        gui.App.match_prefill(self.app, self.state('totk-1.4.3.nsp', 10))
        self.assertEqual(self.shown(), {'base': str(self.base), 'update': str(self.update)})

    def test_other_install_clears_and_restores(self):
        gui.App.match_prefill(self.app, self.state('totk-1.2.1.nsp', 10))
        self.assertEqual(self.shown(), {'base': str(self.base), 'update': ''})
        gui.App.match_prefill(self.app, self.state('totk-1.4.3.nsp', 99))   # same name, other size
        self.assertEqual(self.shown()['update'], '')
        gui.App.match_prefill(self.app, self.state('totk-1.4.3.nsp', 10))
        self.assertEqual(self.shown()['update'], str(self.update))

    def test_install_without_recorded_inputs_clears(self):
        gui.App.match_prefill(self.app, {'inputs': {}})
        self.assertEqual(self.shown(), {'base': '', 'update': ''})

    def test_user_picked_field_survives(self):
        self.app.vars['update'].set('C:/mine/other.nsp')
        self.app.touched.add('update')
        gui.App.match_prefill(self.app, self.state('totk-1.2.1.nsp', 10, base_size=1))
        self.assertEqual(self.shown(), {'base': '', 'update': 'C:/mine/other.nsp'})

    def test_fresh_install_keeps(self):
        gui.App.match_prefill(self.app, None)
        self.assertEqual(self.shown(), {'base': str(self.base), 'update': str(self.update)})

    def test_vanished_file_clears(self):
        self.update.unlink()
        gui.App.match_prefill(self.app, self.state('totk-1.4.3.nsp', 10))
        self.assertEqual(self.shown()['update'], '')


class SmokeTest(unittest.TestCase):
    def test_every_screen_in_both_languages(self):
        try:
            import tkinter  # noqa: F401
        except ImportError:
            self.skipTest('no tkinter')
        r = subprocess.run([sys.executable, '-m', 'installer.gui', '--selftest'], cwd=ROOT, timeout=120,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr[-2000:])


def make_fixture():
    """build/inst-gui/e2e-install: links to install-test (see the module docstring), with the step keys of the state
    set to the ones this checkout computes, so the install counts as current for it."""
    if not (FIXTURE / statefile.NAME).is_file():
        shutil.rmtree(FIXTURE, ignore_errors=True)
        for folder, _dirs, files in os.walk(INSTALLED):
            target = FIXTURE / Path(folder).relative_to(INSTALLED)
            target.mkdir(parents=True, exist_ok=True)
            for name in files:
                src, dst = Path(folder) / name, target / name
                if name in COPIED and target == FIXTURE:
                    shutil.copy2(src, dst)
                else:
                    os.link(src, dst)
    layout = layouts.find(ROOT)
    state = statefile.load(FIXTURE)
    options = SimpleNamespace(base=None, update=None, keys=None, lang=None)
    ctx = Context(layout, FIXTURE, ROOT / 'build' / 'inst-gui' / 'e2e-temp', Reporter(out=io.StringIO()), options, state=state)
    ctx.steps = all_steps()
    ctx.plan()
    # An install made before the fonts step has no record for it: record it as current when its output is (the
    # languages that need the fonts have them in shared-fonts/).
    if Fonts().current(ctx, {}):
        state['steps'].setdefault('fonts', {'release': state.get('release', ''), 'key': ctx.keys['fonts']})
    for step_id, record in state['steps'].items():
        record['key'] = ctx.keys[step_id]
    statefile.save(FIXTURE, state)
    return FIXTURE


@unittest.skipUnless((INSTALLED / statefile.NAME).is_file(), 'build/install-test is absent')
class EndToEndTest(unittest.TestCase):
    @staticmethod
    def run_cli(form, temp, tries=1):
        """Runs the CLI in --verify mode through Process, up to `tries` times while the RomFS hash reads wrong."""
        for attempt in range(tries):
            p = Process(form.command(sys.executable, root=ROOT, temp=temp, verify_hashes=True), cwd=ROOT)
            model, stray = Model(), []
            t0, code = time.monotonic(), None
            while code is None and time.monotonic() - t0 < 600:
                try:
                    line = p.lines.get(timeout=0.2)
                except queue.Empty:
                    line = ''
                if line:
                    event = parse_line(line)
                    if event:
                        model.apply(event)
                    elif line.strip():
                        stray.append(line)
                code = p.poll()
            p.kill()
            model.finish(code)
            if not any(w[0] == 'state.output_changed' for w in model.warnings):
                break
        return code, model, stray, time.monotonic() - t0

    def test_verify_run_skips_every_step(self):
        form = Form(dest=str(make_fixture()))
        self.assertTrue(form.update_mode())
        code, model, stray, elapsed = self.run_cli(form, ROOT / 'build' / 'inst-gui' / 'e2e-temp', tries=3)
        print(f'\nend-to-end: exit {code}, result {model.result}, {elapsed:.1f} s, states {model.states}')
        if any(w[0] == 'state.output_changed' for w in model.warnings):
            self.skipTest('the RomFS hash read wrong three times (page cache of the faulty RAM, see the module docstring)')
        self.assertEqual(code, 0, stray[-5:] or model.error)
        self.assertEqual(model.result, 'ok')
        self.assertIn(model.mode, ('resume', 'update'))
        self.assertEqual({s: model.states[s] for s in model.steps if s not in ('check', 'cleanup')},
                         {s: SKIPPED for s in model.steps if s not in ('check', 'cleanup')})
        self.assertIsNotNone(model.space)
        self.assertEqual(stray, [])

    def test_cancel_during_the_check(self):
        cmd = Form(dest=str(make_fixture())).command(sys.executable, root=ROOT, temp=ROOT / 'build' / 'inst-gui' / 'e2e-temp',
                                                     verify_hashes=True)
        p = Process(cmd, cwd=ROOT)
        model, t0, code, cancelled = Model(), time.monotonic(), None, False
        while code is None and time.monotonic() - t0 < 120:
            try:
                event = parse_line(p.lines.get(timeout=0.2))
            except queue.Empty:
                event = None
            if event:
                model.apply(event)
                if event.get('phase') == 'verify_romfs' and not cancelled:
                    cancelled = True
                    p.cancel()
            code = p.poll()
        p.kill()
        model.finish(code)
        self.assertTrue(cancelled)
        self.assertEqual((code, model.result), (3, 'cancelled'))
        self.assertIsNone(model.error)


if __name__ == '__main__':
    unittest.main(verbosity=2)
