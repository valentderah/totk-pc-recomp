"""installer/ bookkeeping with fake steps: fresh install, resume after a failure or cancel, transient steps, update
with a newer release, totk.ini and the event stream. No game files, no tools; stdlib only.

Run: python tests/installer_test.py
"""
import io
import json
import re
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
from installer import ini, runner, state, steps  # noqa: E402
from installer.events import Reporter  # noqa: E402
from installer.layout import Layout  # noqa: E402


class Fake(runner.Step):
    """A step whose output is <dir>/<id>.out holding its key; dir is the temp folder for a transient step."""

    def __init__(self, id, transient=False, always=False, previous=None):
        self.id, self.transient, self.always, self.previous = id, transient, always, previous

    def out(self, ctx):
        return (ctx.temp if self.transient else ctx.dest) / f'{self.id}.out'

    def key(self, ctx):
        upstream = ctx.keys[self.previous] if self.previous else ''
        return f'{self.id}:{ctx.options.inputs.get(self.id, "v1")}:{upstream}'

    def current(self, ctx, record):
        p = self.out(ctx)
        return p.is_file() and p.read_text() == ctx.keys[self.id]

    def verify(self, ctx, record):
        ctx.options.verified.append(self.id)
        return ctx.options.verify_ok.get(self.id, True)

    def run(self, ctx):
        o = ctx.options
        o.ran.append(self.id)
        if self.id in ctx.forced:
            o.forced.append(self.id)
        if self.id == 'check':
            ctx.verify_kept(self.id)
        failure = o.fail.pop(self.id, None)
        if failure == 'error':
            raise runner.InstallError('fake.failed', step=self.id)
        if failure == 'cancel':
            raise KeyboardInterrupt
        if self.id == 'cleanup':
            for p in ctx.temp.glob('*.out'):
                p.unlink()
            return {}
        p = self.out(ctx)
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(ctx.keys[self.id])
        return {'note': self.id}


def fake_steps():
    return [Fake('check', always=True), Fake('extract'), Fake('lift', transient=True, previous='extract'),
            Fake('compile', previous='lift'), Fake('prepare', previous='compile'), Fake('cleanup', always=True)]


class Bed:
    """A dest and temp folder plus one run of the fake steps per call."""

    def __init__(self, base):
        self.dest, self.temp = base / 'dest', base / 'temp'
        self.temp.mkdir()

    def run(self, release='1.0', inputs=None, fail=None, verify_ok=None, verify=False):
        layout = Layout(root=Path('.'), dev=True, release=release, python=Path('python'), tools=Path('tools'),
                        recomp=Path('recomp'), llvm=Path('llvm'),
                        deps=Path('deps'), runtime=Path('rt'), runtime_lib=Path('lib'), ninja=Path('ninja'),
                        hactool=Path('hactool'), xlate=Path('xlate'))
        options = SimpleNamespace(inputs=inputs or {}, fail=dict(fail or {}), verify_ok=verify_ok or {}, ran=[],
                                  verified=[], forced=[])
        out = io.StringIO()
        ctx = runner.Context(layout, self.dest, self.temp, Reporter(out=out), options, state=state.load(self.dest),
                             verify=verify)
        mode = ctx.mode
        result = runner.run_steps(fake_steps(), ctx)
        events = [json.loads(line) for line in out.getvalue().splitlines()]
        return SimpleNamespace(result=result, ran=options.ran, verified=options.verified, forced=options.forced,
                               events=events, mode=mode,
                               state=state.load(self.dest))

    @staticmethod
    def skipped(r):
        return {e['step']: e['reason'] for e in r.events if e['type'] == 'step' and e['state'] == 'skipped'}


ALL = ['check', 'extract', 'lift', 'compile', 'prepare', 'cleanup']


class RunnerTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bed = Bed(Path(self.tmp.name))

    def tearDown(self):
        self.tmp.cleanup()

    def test_fresh_install_runs_everything_and_records_it(self):
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'install', ALL))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift', 'compile', 'prepare'})
        self.assertEqual(r.state['steps']['compile']['note'], 'compile')
        self.assertEqual(r.state['release'], '1.0')
        started = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'started']
        done = [e['step'] for e in r.events if e['type'] == 'step' and e['state'] == 'done']
        self.assertEqual((started, done), (ALL, ALL))

    def test_rerun_skips_finished_steps_and_unneeded_transient(self):
        self.bed.run()
        r = self.bed.run()
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'cleanup']))
        self.assertEqual(self.bed.skipped(r), {'extract': 'current', 'lift': 'not_needed', 'compile': 'current',
                                               'prepare': 'current'})
        self.assertEqual(r.verified, [])   # same release: no full checks

    def test_resume_after_failure(self):
        r = self.bed.run(fail={'compile': 'error'})
        self.assertEqual((r.result, r.ran), ('failed', ['check', 'extract', 'lift', 'compile']))
        error = [e for e in r.events if e['type'] == 'error'][0]
        self.assertEqual((error['id'], error['step'], error['params']), ('fake.failed', 'compile', {'step': 'compile'}))
        self.assertEqual(set(r.state['steps']), {'extract', 'lift'})
        r = self.bed.run()
        # lift's output is still in the temp folder (no cleanup ran), so only compile and later run.
        self.assertEqual((r.result, r.mode, r.ran), ('ok', 'resume', ['check', 'compile', 'prepare', 'cleanup']))

    def test_resume_after_cancel_with_transient_output_gone(self):
        r = self.bed.run(fail={'prepare': 'cancel'})
        self.assertEqual(r.result, 'cancelled')
        self.assertNotIn('prepare', r.state['steps'])
        for p in self.bed.temp.glob('*.out'):
            p.unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'prepare', 'cleanup'])
        self.assertEqual(self.bed.skipped(r)['lift'], 'not_needed')

    def test_failed_check_writes_nothing(self):
        r = self.bed.run(fail={'check': 'error'})
        self.assertEqual(r.result, 'failed')
        self.assertFalse(self.bed.dest.exists())

    def test_removed_output_runs_again(self):
        self.bed.run()
        (self.bed.dest / 'compile.out').unlink()
        r = self.bed.run()
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'cleanup'])

    def test_update_with_new_lifter_keeps_extraction(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', inputs={'lift': 'v2'})
        self.assertEqual((r.result, r.mode), ('ok', 'update'))
        self.assertEqual(r.ran, ['check', 'lift', 'compile', 'prepare', 'cleanup'])
        self.assertEqual(r.verified, ['extract'])   # kept from another release: full check
        self.assertEqual(r.state['steps']['extract']['release'], '1.0')
        self.assertEqual(r.state['steps']['compile']['release'], '1.1')
        self.assertEqual(r.state['release'], '1.1')

    def test_update_that_changes_nothing_skips_the_rebuild(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1')
        self.assertEqual(r.ran, ['check', 'cleanup'])
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_update_with_changed_kept_files_extracts_again(self):
        self.bed.run(release='1.0')
        r = self.bed.run(release='1.1', verify_ok={'extract': False})
        self.assertEqual(r.ran, ['check', 'extract', 'cleanup'])
        self.assertEqual(r.forced, ['extract'])   # the step knows not to trust what it left behind
        warnings = [e for e in r.events if e['type'] == 'warning']
        self.assertEqual([(w['id'], w['params']) for w in warnings], [('state.output_changed', {'step': 'extract'})])

    def test_verify_option_checks_same_release(self):
        self.bed.run()
        r = self.bed.run(verify=True)
        self.assertEqual(r.verified, ['extract', 'compile', 'prepare'])

    def test_state_errors(self):
        self.bed.dest.mkdir()
        (self.bed.dest / state.NAME).write_text('{not json')
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.unreadable')
        (self.bed.dest / state.NAME).write_text(json.dumps({'format': 99, 'steps': {}}))
        with self.assertRaises(state.StateError) as e:
            state.load(self.bed.dest)
        self.assertEqual(e.exception.id, 'state.newer_format')


class ReleaseFilesTest(unittest.TestCase):
    def test_release_ships_every_tool_the_steps_run(self):
        sys.path.insert(0, str(ROOT / 'tools'))
        import make_release
        text = (ROOT / 'installer' / 'steps.py').read_text(encoding='utf-8')
        used = set(re.findall(r"layout\.tools / '(\w+\.py)'", text))
        for names in re.findall(r"layout\.tools / n for n in \((.*?)\)", text, re.S):
            used |= set(re.findall(r"'(\w+\.py)'", names))
        self.assertIn('cheats.py', used)
        self.assertEqual(used - set(make_release.TOOLS_FILES), set())


class LiftKeyTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.dest = Path(tmp.name)
        layout = SimpleNamespace(recomp=self.dest / 'no-recomp', tools=self.dest / 'no-tools',
                                 patches=lambda version: self.dest / 'no-patches')
        self.ctx = SimpleNamespace(dest=self.dest, layout=layout, keys={'extract': 'x'}, state={'version': '1.4.3'})

    def key(self):
        return steps.Lift().key(self.ctx)

    def add_cheats(self, name, text, mod='Cheats', where='cheats'):
        folder = self.dest / 'mods' / mod / where
        folder.mkdir(parents=True, exist_ok=True)
        (folder / name).write_text(text)

    def test_extracting_main_keeps_the_key(self):
        before = self.key()
        (self.dest / 'exefs').mkdir()
        (self.dest / 'exefs' / 'main').write_bytes(bytes(0x48))
        self.assertEqual(self.key(), before)

    def test_cheat_files_change_the_key(self):
        before = self.key()
        self.add_cheats('277178B7DBA1B6D4.txt', '[a]')
        added = self.key()
        self.assertNotEqual(added, before)
        self.add_cheats('277178B7DBA1B6D4.txt', '[b]')
        edited = self.key()
        self.assertNotEqual(edited, added)
        self.add_cheats('277178B7DBA1B6D4.txt', '[c]', mod='Other', where='atmosphere/contents/0100F2C0115B6000/cheats')
        self.assertNotEqual(self.key(), edited)

    def test_exefs_patch_files_change_the_key(self):
        before = self.key()
        self.add_cheats('dynamic-fps.pchtxt', '@nsobid-277178B7DBA1B6D4', mod='Fps', where='exefs')
        added = self.key()
        self.assertNotEqual(added, before)
        self.add_cheats('277178B7DBA1B6D4.ips', 'PATCHEOF', mod='Ips', where='atmosphere/exefs_patches/x')
        both = self.key()
        self.assertNotEqual(both, added)
        self.add_cheats('main.npdm', 'not a patch', mod='Fps', where='exefs')
        self.assertEqual(self.key(), both)

    def test_status_and_other_files_keep_the_key(self):
        self.add_cheats('277178B7DBA1B6D4.txt', '[a]')
        before = self.key()
        (self.dest / 'build-cache').mkdir()
        (self.dest / 'build-cache' / 'cheats.status').write_text('written by lift')
        self.add_cheats('readme.md', 'not a cheat file')
        (self.dest / 'mods' / 'Cheats' / 'romfs').mkdir()
        (self.dest / 'mods' / 'Cheats' / 'romfs' / 'a.txt').write_text('a RomFS file')
        self.assertEqual(self.key(), before)


class IniTest(unittest.TestCase):
    EXAMPLE = '[display]\n# vk_present = mailbox\n\n[language]\n# locale = en-US\n# lang = en-US\n# x = 1\n'

    def test_example_line_is_activated(self):
        out = ini.set_language(self.EXAMPLE, 'ru')
        self.assertIn('\nlang = ru\n', out)
        self.assertNotIn('# lang', out)
        self.assertIn('# locale = en-US', out)

    def test_active_line_is_rewritten_and_the_rest_kept(self):
        text = '[display]\nvk_present = fifo\n[language]\n  LANG =  ja  \nfps60 = 0\n'
        out = ini.set_language(text, 'de')
        self.assertEqual(out, '[display]\nvk_present = fifo\n[language]\n  lang =  de\nfps60 = 0\n')
        self.assertEqual(ini.get_language(out), 'de')

    def test_missing_section_is_added(self):
        self.assertEqual(ini.set_language('fps60 = 0\n', 'fr'), 'fps60 = 0\n\n[language]\nlang = fr\n')
        self.assertEqual(ini.set_language('[language]\nlocale = de-DE\n', 'fr'),
                         '[language]\nlang = fr\nlocale = de-DE\n')

    def test_old_chinese_codes_are_written_as_the_canonical_ones(self):
        self.assertEqual(ini.canonical_language('zh-CN'), 'zh-Hans')
        self.assertEqual(ini.canonical_language('zh-TW'), 'zh-Hant')
        self.assertEqual(ini.canonical_language('ja'), 'ja')
        self.assertNotIn('zh-CN', ini.LANGUAGES)
        self.assertNotIn('zh-TW', ini.LANGUAGES)
        out = ini.set_language(self.EXAMPLE, 'zh-CN')
        self.assertEqual(ini.get_language(out), 'zh-Hans')
        self.assertEqual(ini.get_language(ini.set_language(out, 'zh-TW')), 'zh-Hant')

    def test_write_keeps_user_lines(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            example = d / 'totk.ini.example'
            example.write_text(self.EXAMPLE)
            path = ini.write(d / '', 'en-GB', example)
            self.assertEqual(ini.get_language(path.read_text()), 'en-GB')
            path.write_text(path.read_text().replace('# vk_present = mailbox', 'vk_present = fifo'))
            ini.write(d, 'ja', example)
            text = path.read_text()
            self.assertIn('vk_present = fifo', text)
            self.assertEqual(ini.get_language(text), 'ja')


class CleanupTest(unittest.TestCase):
    """The real Cleanup step on a finished install whose romfs/ is intact, emptied or gone (#107)."""

    def setUp(self):
        self.steps = steps
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp = base / 'dest', base / 'temp'
        (self.dest / 'romfs').mkdir(parents=True)
        temp.mkdir()
        image = b'romfs image'
        (self.dest / 'romfs' / 'romfs.bin').write_bytes(image)
        digest = steps.extract.sha256(self.dest / 'romfs' / 'romfs.bin')
        (self.dest / 'romfs' / 'index.json').write_text(json.dumps({'romfs_size': len(image), 'romfs_sha256': digest}))
        self.expected = {'romfs': {'size': len(image), 'sha256': digest}}
        self.ctx = SimpleNamespace(reporter=Reporter(out=io.StringIO(), log_path=base / 'x.log'), dest=self.dest,
                                   temp=temp, lang='en-US', state={},
                                   layout=SimpleNamespace(ini_example=ROOT / 'totk.ini.example', dev=False,
                                                          python=base / 'python' / 'python.exe', root=base / 'release'))
        self.saved = steps.expected_update
        steps.expected_update = lambda ctx: self.expected

    def tearDown(self):
        self.steps.expected_update = self.saved
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def run_cleanup(self):
        try:
            self.steps.Cleanup().run(self.ctx)
            return 'ok'
        except self.steps.InstallError as e:
            return e.id

    def test_intact_romfs_passes(self):
        self.assertEqual(self.run_cleanup(), 'ok')

    def test_keeps_build_and_writes_rebuild_command(self):
        for name in ('gen', 'build', 'check'):
            (self.ctx.temp / name).mkdir()
        self.assertEqual(self.run_cleanup(), 'ok')
        self.assertTrue((self.ctx.temp / 'gen').is_dir() and (self.ctx.temp / 'build').is_dir())
        self.assertFalse((self.ctx.temp / 'check').exists())
        lines = (self.dest / 'build-cache' / 'rebuild.txt').read_text(encoding='utf-8').splitlines()
        self.assertEqual(lines[1:], ['-c', steps.REBUILD_BOOT, str(self.ctx.layout.root), '--root',
                                     str(self.ctx.layout.root), '--rebuild', str(self.dest)])

    def test_emptied_romfs_fails(self):
        for p in (self.dest / 'romfs').iterdir():
            p.unlink()
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')

    def test_missing_romfs_fails(self):
        self.steps.remove(self.dest / 'romfs')
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')

    def test_romfs_of_another_size_fails(self):
        (self.dest / 'romfs' / 'romfs.bin').write_bytes(b'short')
        self.assertEqual(self.run_cleanup(), 'cleanup.romfs_unreadable')


class BaseGameCheckTest(unittest.TestCase):
    """Issue #261: an update NSP chosen as the base game was reported as "not Tears of the Kingdom"."""
    BASE, UPDATE = '0100F2C0115B6000', '0100F2C0115B6800'

    @staticmethod
    def nca(content_type, title_id):
        return ({'name': 'x.nca', 'nca_offset': 0, 'size': 0xc00, 'content_type': content_type, 'title_id': title_id}, b'')

    def test_update_package_names_the_update(self):
        # An update NSP keeps the base title id on its Program NCA; only its Meta NCA carries the update id.
        records = [self.nca(0, self.BASE), self.nca(1, self.UPDATE)]
        with self.assertRaises(ValueError):
            steps.program_nca(records)
        self.assertEqual(steps.base_misuse(records), 'check.base_is_update')

    def test_combined_package_is_named(self):
        records = [self.nca(0, self.BASE), self.nca(1, self.BASE), self.nca(1, self.UPDATE)]
        self.assertEqual(steps.base_misuse(records), 'check.base_combined')

    def test_plain_base_game_passes(self):
        records = [self.nca(0, self.BASE), self.nca(1, self.BASE), self.nca(3, self.BASE)]
        self.assertIsNone(steps.base_misuse(records))
        self.assertEqual(steps.program_nca(records)[0]['title_id'], self.BASE)


class FontsTest(unittest.TestCase):
    """The fonts step: keyed by language and firmware, a no-op without firmware, writes dest/shared-fonts."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        base = Path(self.tmp.name)
        self.dest, temp, self.firmware = base / 'dest', base / 'temp', base / 'firmware'
        temp.mkdir()
        self.firmware.mkdir()
        (self.firmware / 'a.nca').write_bytes(b'nca')
        self.keys = base / 'prod.keys'
        self.keys.write_text('header_key = 00\n')
        self.ctx = SimpleNamespace(dest=self.dest, temp=temp, lang='zh-Hans', state={},
                                   layout=SimpleNamespace(tools=ROOT / 'tools', root=ROOT),
                                   reporter=Reporter(out=io.StringIO()),
                                   options=SimpleNamespace(lang='zh-Hans', firmware=self.firmware, keys=str(self.keys)))
        self.saved_run_tool = steps.run_tool

    def tearDown(self):
        steps.run_tool = self.saved_run_tool
        self.ctx.reporter.close()
        self.tmp.cleanup()

    def fake_tool(self, files=None, code=0):
        """Stand-in for extract_shared_fonts.py: writes the requested files where --output points."""
        def run(ctx, step, script, args):
            out = Path(args[args.index('--output') + 1])
            out.mkdir(parents=True)
            for name in steps.SHARED_FONT_FILES if files is None else files:
                (out / name).write_bytes(b'bfttf')
            (out / 'manifest.json').write_text('{}')
            log = ctx.temp / 'fonts.log'
            log.write_text('error line\n')
            return code, log
        steps.run_tool = run

    def test_all_steps_order(self):
        self.assertEqual([s.id for s in steps.all_steps()],
                         ['check', 'fonts', 'extract', 'lift', 'compile', 'prepare', 'cleanup'])

    def test_extracts_into_the_install_folder(self):
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {'files': 6})
        self.assertTrue(steps.shared_fonts_ready(self.dest))
        self.assertEqual(sorted(p.name for p in (self.dest / 'shared-fonts').iterdir()),
                         sorted((*steps.SHARED_FONT_FILES, 'manifest.json')))

    def test_noop_without_firmware(self):
        self.ctx.options.firmware = None
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {})
        self.assertFalse((self.dest / 'shared-fonts').exists())

    def test_noop_for_other_languages(self):
        self.ctx.lang, self.ctx.options.lang = 'en-US', 'en-US'
        self.fake_tool()
        self.assertEqual(steps.Fonts().run(self.ctx), {})
        self.assertFalse((self.dest / 'shared-fonts').exists())

    def test_failed_extraction_keeps_the_old_folder(self):
        self.fake_tool()
        steps.Fonts().run(self.ctx)
        self.fake_tool(code=1)
        with self.assertRaises(steps.InstallError) as e:
            steps.Fonts().run(self.ctx)
        self.assertEqual(e.exception.id, 'fonts.failed')
        self.assertTrue(steps.shared_fonts_ready(self.dest))

    def test_incomplete_output_is_an_error(self):
        self.fake_tool(files=steps.SHARED_FONT_FILES[:1])
        with self.assertRaises(steps.InstallError) as e:
            steps.Fonts().run(self.ctx)
        self.assertEqual(e.exception.id, 'fonts.incomplete')

    def test_current_tracks_language_and_output(self):
        fonts = steps.Fonts()
        self.assertFalse(fonts.current(self.ctx, {}))
        self.fake_tool()
        fonts.run(self.ctx)
        self.assertTrue(fonts.current(self.ctx, {}))
        self.ctx.options.lang = 'en-US'
        self.assertTrue(fonts.current(self.ctx, {}))
        self.ctx.options.lang = 'zh-Hans'
        (self.dest / 'shared-fonts' / '0-standard.bfttf').unlink()
        self.assertFalse(fonts.current(self.ctx, {}))

    def test_key_changes_with_language_and_firmware(self):
        fonts = steps.Fonts()
        zh = fonts.key(self.ctx)
        self.ctx.options.lang = 'en-US'
        self.assertNotEqual(zh, fonts.key(self.ctx))
        self.ctx.options.lang = 'zh-Hans'
        (self.firmware / 'b.nca').write_bytes(b'other')
        self.assertNotEqual(zh, fonts.key(self.ctx))

    def test_registered_child_is_used(self):
        nested = self.firmware / 'registered'
        nested.mkdir()
        (nested / 'c.nca').write_bytes(b'nca')
        self.assertEqual(steps.firmware_dir(self.firmware), nested)


class EventsTest(unittest.TestCase):
    def test_lines_are_json_and_the_log_is_english(self):
        with tempfile.TemporaryDirectory() as d:
            out = io.StringIO()
            rep = Reporter(out=out, log_path=Path(d) / 'x.log')
            rep.step('check', 'started')
            rep.progress('extract', 'copy', 0, 10, 'bytes')
            rep.progress('extract', 'copy', 5, 10, 'bytes')    # throttled
            rep.progress('extract', 'copy', 10, 10, 'bytes')   # last one always goes out
            rep.error('check.disk_space', {'path': 'D:\\x', 'need_gb': 40, 'free_gb': 3}, step='check')
            rep.close()
            events = [json.loads(line) for line in out.getvalue().splitlines()]
            self.assertEqual([e['type'] for e in events], ['step', 'progress', 'progress', 'error'])
            self.assertEqual([e.get('done') for e in events[1:3]], [0, 10])
            self.assertTrue(all('t' in e for e in events))
            log = (Path(d) / 'x.log').read_text(encoding='utf-8')
            self.assertIn('40 GB needed, 3 GB free', log)
            self.assertIn('[check.disk_space]', log)

    def test_catalog_has_every_id_the_code_uses(self):
        catalog = json.loads((ROOT / 'installer' / 'text' / 'en.json').read_text(encoding='utf-8'))
        used = set()
        for path in (ROOT / 'installer').glob('*.py'):
            text = path.read_text(encoding='utf-8')
            used |= set(re.findall(r"InstallError\('([a-z_.]+)'", text))
            used |= set(re.findall(r"warning\('([a-z_.]+)'", text))
            used |= {f'phase.{s}.{p}' for s, p in re.findall(r"progress\('(\w+)', '(\w+)'[,)]", text)}
        self.assertEqual(sorted(i for i in used if i not in catalog), [])


if __name__ == '__main__':
    unittest.main()
