"""Installer migration and packaged-runtime requirements for ordinary independent-controls builds."""
from pathlib import Path
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock
ROOT=Path(__file__).resolve().parents[1];sys.path.insert(0,str(ROOT))
from installer.layout import Layout
from installer import steps

class Setup(unittest.TestCase):
    def setUp(self): (ROOT/'build/input-logger/tests').mkdir(parents=True,exist_ok=True)
    def test_old_cached_lift_requires_regeneration(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);gen=p/'gen';gen.mkdir()
            (gen/'layout.json').write_text('{}');(gen/steps.STAMP).write_text('same-key\n')
            ctx=SimpleNamespace(temp=p,keys={'lift':'same-key'})
            self.assertFalse(steps.Lift().current(ctx,{}))
            (gen/'input_actions_game_config.h').write_text('/* synthetic generated configuration */')
            self.assertTrue(steps.Lift().current(ctx,{}))
    def test_release_sources_include_generator_and_license(self):
        sys.path.insert(0,str(ROOT/'tools'))
        import make_release
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            out=Path(td)
            with mock.patch.object(make_release.shader_list,'history',return_value='synthetic shader list\n'):
                make_release.build_sources(out,{})
            self.assertTrue((out/'recomp/input_actions.py').is_file())
            self.assertTrue((out/'tools/data/expected-v1.4.3.json').is_file())
            self.assertTrue((out/'tools/licenses/weeping-player-state.txt').is_file())
            self.assertFalse((out/'tools/build_input_logger.py').exists())
            self.assertIn('input_actions_game.c',make_release.build_game.INSTALL_SIDE)

    def test_release_runtime_layout_needs_no_extracted_game(self):
        sys.path.insert(0,str(ROOT/'tools'))
        import make_release
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            with mock.patch.object(make_release,'MAIN_ROOT',Path(td)):
                gen=make_release.runtime_layout()
            import json
            rows=json.loads((gen/'layout.json').read_text())
            self.assertEqual([row['name'] for row in rows],['main','subsdk0','sdk'])
            self.assertTrue(all(int(row['size'],16)==0 for row in rows))
            self.assertIn('#define MOD_COUNT 3',(gen/'layout.h').read_text())
            self.assertFalse((Path(td)/'generated').exists())

    def test_packaged_runtime_includes_action_definitions(self):
        sys.path.insert(0,str(ROOT/'tools'))
        import make_release
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);out=p/'package';(out/'runtime').mkdir(parents=True)
            def fake_compile(cmd, **kwargs):
                build=Path(cmd[cmd.index('--out')+1]);build.mkdir(parents=True,exist_ok=True)
                (build/'libtotkrt.a').write_bytes(b'synthetic test archive')
            with mock.patch.object(make_release,'MAIN_ROOT',p), mock.patch.object(make_release,'run',side_effect=fake_compile):
                make_release.runtime_lib(out,p/'llvm',{})
            self.assertEqual((out/'runtime/input_actions.def').read_bytes(),(ROOT/'runtime/input_actions.def').read_bytes())

    def test_package_requires_adapter_sources_and_headers(self):
        with tempfile.TemporaryDirectory(dir=ROOT/'build/input-logger/tests') as td:
            p=Path(td);runtime=p/'runtime';runtime.mkdir()
            layout=Layout(root=p,dev=False,release='test',python=p/'python',tools=p/'tools',recomp=p/'recomp',
                          llvm=p/'llvm',deps=None,runtime=runtime,runtime_lib=runtime/'libtotkrt.a',
                          ninja=p/'ninja',hactool=p/'hactool',xlate=p/'xlate')
            for value in layout.missing():
                f=Path(value);f.parent.mkdir(parents=True,exist_ok=True);f.write_text('fixture')
            self.assertEqual(layout.missing(),[])
            for f in (runtime/'input_actions_game.c',runtime/'input_actions_backend.h',runtime/'input_player_state.h',runtime/'input_actions.def',p/'recomp/input_actions.py'):
                with self.subTest(path=f.name):
                    f.unlink();self.assertIn(str(f),layout.missing());f.write_text('fixture')
if __name__=='__main__':unittest.main()
