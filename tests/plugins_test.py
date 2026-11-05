"""Host hooks of the code patch table (recomp/patches.py `hook` lines and `@hook:` words, recomp/gen.py) and the code
modules of mods that become hook sites (tools/plugins.py, tools/exefs_patches.py), on a module the test makes itself
(tests/exefs_patches_test.py's module with a jump table) and a plugin build it registers for the run. No game files.

Run with .venv (lz4 and numpy, as recomp/): python tests/plugins_test.py
"""
import argparse
import contextlib
import hashlib
import io
import re
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'recomp'))
import exefs_patches_test as ept  # noqa: E402
import patches  # noqa: E402

ep = ept.ep   # tools/exefs_patches.py
plugins = ep.plugins

MAIN = 0x7000000000   # main's base in the layout of an exefs folder with main alone
# The jump-table module's words: cmp w0, #3 at 0x8, b.hi Ldefault (0x48) at 0xc, ldrb w10, [x8, w0, uxtw] at 0x1c
BHI, LDRB = 0x540001E8, 0x3860490A


class HookSites(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / 'exefs').mkdir()
        (self.d / 'exefs' / 'main').write_bytes(ept.nso(ept.JT_TEXT, ept.JT_RO, ept.JT_DATA))

    def tearDown(self):
        self.tmp.cleanup()

    def gen(self, text):
        """(exit code, stderr, the chunk's C, patches.h) of gen.py with the patch file text."""
        (self.d / 'patches.txt').write_text(text)
        out = self.d / 'gen'
        r = subprocess.run([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                            '--out', str(out), '--modules', 'main', '--jobs', '1', '--patches',
                            str(self.d / 'patches.txt')], capture_output=True, text=True)
        chunk = out / 'main' / 'main_0000.c'
        return (r.returncode, r.stderr, chunk.read_text() if chunk.is_file() else '',
                (out / 'patches.h').read_text() if (out / 'patches.h').is_file() else '')

    def test_hook_site(self):
        """The hook runs before the original instruction with the Cpu current; the flags of the cmp before it reach the
        b.hi after it; a fn: argument is a function start; patches.h lists the hook."""
        code, err, c, h = self.gen(f'set probe TOTK_PROBE 1\nhook h1 probe_handler fn:main+48 2a\n'
                                   f'site probe main c {BHI:08x} @hook:h1 {BHI:08x}\n')
        self.assertEqual(code, 0, err)
        self.assertIn('void rt_hook(Cpu *c, int hook, uint64_t site);', c)
        site = next(line for line in c.splitlines() if 'rt_hook(c, 0' in line)
        hooked, plain = site.split('} else {')
        self.assertRegex(hooked, rf'rt_hook\(c, 0, {MAIN + 0xc:#x}ull\); (x\d+ = c->x\[\d+\]; )+'
                                 rf'if \(fc_sub32\(&F, CC_HI\)\)')
        self.assertIn('if (fc_sub32(&F, CC_HI))', plain)
        self.assertNotIn('F =', site)   # the hook neither reads nor writes the flags
        self.assertIn(f'void f_{MAIN + 0x48:x}(Cpu *c) {{', c)
        self.assertIn(f'X(0, "h1", "probe_handler", 2, {MAIN + 0x48:#x}ull, 0x2aull, 0x0ull, 0x0ull)', h)
        self.assertIn('#define RT_HOOK_COUNT 1', h)

    def test_registers_stored_before_the_hook(self):
        """Registers changed since the last sync go to the Cpu before the hook and every local comes back after it."""
        code, err, c, _ = self.gen(f'set probe TOTK_PROBE 1\nhook h2 other\n'
                                   f'site probe main 1c {LDRB:08x} @hook:h2 {LDRB:08x}\n')
        self.assertEqual(code, 0, err)
        site = next(line for line in c.splitlines() if 'rt_hook(c, 0' in line)
        before, after = site.split(f'rt_hook(c, 0, {MAIN + 0x1c:#x}ull);')
        self.assertIn('c->x[8] = x8;', before)   # x8 and x9 were written by the adrp, add and adr before
        self.assertIn('c->x[9] = x9;', before)
        locals_ = re.search(r'uint64_t (.*);', c).group(1)
        for reg in re.findall(r'(x\d+) = c->x\[\d+\]', locals_):
            self.assertIn(f'{reg} = c->x[{reg[1:]}];', after.split('{ uint64_t ea')[0])

    def test_without_hooks(self):
        code, err, c, h = self.gen(f'set probe TOTK_PROBE 1\nsite probe main c {BHI:08x} {BHI:08x}\n')
        self.assertEqual(code, 0, err)
        self.assertNotIn('rt_hook', c)
        self.assertIn('#define RT_HOOK_COUNT 0', h)

    def test_function_outside_text(self):
        code, err, _, _ = self.gen('hook h1 x fn:main+1000\n')   # .rodata
        self.assertNotEqual(code, 0)
        self.assertIn('outside .text', err)

    def test_refused_lines(self):
        mods = [('main', MAIN, 0x4000)]
        for text, why in (('set s E 1\nsite s main c 0 @hook:h 0\n', 'unknown hook h'),
                          ('hook h x fn:main+4000\n', 'outside its module'),
                          ('hook h x fn:main+6\n', 'misaligned'),
                          ('hook h x fn:sdk+0\n', 'not fn:<module'),
                          ('hook h x 1 2 3 4 5\n', 'more than 4 arguments'),
                          ('hook h-1 x\n', 'other characters'),
                          ('hook h x\nhook h y\n', 'defined twice')):
            path = self.d / 'bad.txt'
            path.write_text(text)
            with self.assertRaises(patches.PatchError, msg=text) as e:
                patches.load(path, mods)
            self.assertIn(why, str(e.exception))


PLUGIN = b'a plugin build of the test'
OTHER = b'a code module nobody knows'


class CodeModules(unittest.TestCase):
    """tools/exefs_patches.py on a mods folder with code modules, a plugin build registered for the test."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.d = Path(self.tmp.name)
        (self.d / 'exefs').mkdir()
        (self.d / 'exefs' / 'main').write_bytes(ept.nso(ept.JT_TEXT, ept.JT_RO, ept.JT_DATA))
        (self.d / 'patches.txt').write_text(f'set port TOTK_PORT 1\nsite port main 8 {0x71000C1F:08x} '
                                            f'{0x71000C1F:08x}\n')   # one site of the port's own: cmp w0, #3 at 0x8

    def tearDown(self):
        self.tmp.cleanup()

    def known(self, sites, version='9.9.9'):
        hooks = plugins.Hooks(('fn:main+48', '2a'), sites)
        return mock.patch.dict(plugins.KNOWN, {hashlib.sha256(PLUGIN).hexdigest():
                                               plugins.Plugin('Probe plugin', 'probe_', {version: hooks})})

    def put(self, rel, data):
        f = self.d / 'mods' / rel
        f.parent.mkdir(parents=True, exist_ok=True)
        f.write_bytes(data)

    def build(self, version='9.9.9'):
        """(status lines by mod folder, the patch file written, the tool's output)."""
        args = argparse.Namespace(exefs=self.d / 'exefs', mods=self.d / 'mods', patches=self.d / 'patches.txt',
                                  out=self.d / 'out.txt', status=self.d / 'exefs.status', data=self.d / 'exefs.data',
                                  game_version=version)
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            ep.build(args)
        keys = {ep.fnv(f'{m}/exefs/subsdk9'.encode()): m for m in ('TSQ', 'Other', 'Old')}
        status = {keys.get(f[0], f[0]): f for f in (line.split('\t') for line in
                                                    (self.d / 'exefs.status').read_text().splitlines())}
        return status, (self.d / 'out.txt').read_text(), out.getvalue()

    def test_known_build(self):
        """A known build becomes a set of hook sites: one hook line per callback, the hooks of one instruction in one
        site before the original word; the port's own set at a hooked site is switched off with the mod; gen.py lifts
        the result."""
        self.put('TSQ/exefs/subsdk9', PLUGIN)
        self.put('TSQ/romfs/x.bin', b'x')
        self.put('Other/exefs/subsdk9', OTHER)
        with self.known([(0xC, 'x'), (0x8, 'y'), (0xC, 'y')]):
            status, text, out = self.build()
        key = ep.fnv(b'TSQ/exefs/subsdk9')
        self.assertEqual(status['TSQ'][2:], ['plugin', 'port', 'hooks 3 Probe plugin'])
        self.assertEqual(status['Other'][2:4], ['unsupported', '-'])
        self.assertIn('does not know', status['Other'][4])
        name = f'mod_{key.lower()}'
        self.assertIn(f'set {name} TOTK_MODPATCH_{key} 0   # TSQ/exefs/subsdk9: Probe plugin', text)
        self.assertIn(f'hook {name}_x probe_x fn:main+48 2a\nhook {name}_y probe_y fn:main+48 2a\n', text)
        self.assertIn(f'site {name} main 8 71000c1f @hook:{name}_y 71000c1f', text)
        self.assertIn(f'site {name} main c {ept_bhi()} @hook:{name}_x @hook:{name}_y {ept_bhi()}', text)
        self.assertIn('3 hooks, over port', out)
        r = subprocess.run([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                            '--out', str(self.d / 'gen'), '--modules', 'main', '--jobs', '1', '--patches',
                            str(self.d / 'out.txt')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        c = (self.d / 'gen' / 'main' / 'main_0000.c').read_text()
        self.assertRegex(c, r'rt_hook\(c, 0, 0x700000000cull\);.*rt_hook\(c, 1, 0x700000000cull\);')
        self.assertIn('X(0, "%s_x", "probe_x", 2, 0x7000000048ull, 0x2aull' % name,
                      (self.d / 'gen' / 'patches.h').read_text())

    def test_no_hooks_for_the_version(self):
        """A known build without hooks for the game version installs none, as on the Switch: no set."""
        self.put('Old/exefs/subsdk9', PLUGIN)
        with self.known([(0xC, 'x')], version='1.0.0'):
            status, text, _ = self.build()
        self.assertEqual(status['Old'][2:], ['plugin', '-', 'hooks 0 Probe plugin'])
        self.assertNotIn('hook ', text)

    def test_unhookable(self):
        """A hook on a br, or on an instruction a mod's patch file already patches, makes the build unsupported."""
        self.put('TSQ/exefs/subsdk9', PLUGIN)
        with self.known([(0x24, 'x')]):   # br x9
            status, text, _ = self.build()
        self.assertEqual(status['TSQ'][2], 'unsupported')
        self.assertIn('a br instruction', status['TSQ'][4])
        self.assertNotIn('hook ', text)
        self.put('Patch/exefs/' + ept.BID_NAME + '.ips', ept.ips32({0x28: (0x52800160).to_bytes(4, 'little')}))
        with self.known([(0x28, 'x')]):
            status, _, _ = self.build()
        self.assertEqual(status['TSQ'][2], 'unsupported')
        self.assertIn('patches too', status['TSQ'][4])

    def test_game_version_from_main(self):
        """Without --game-version, a main that is none of the game's versions has no hooks of any build."""
        self.put('TSQ/exefs/subsdk9', PLUGIN)
        with self.known([(0xC, 'x')]):
            status, _, _ = self.build(version=None)
        self.assertEqual(status['TSQ'][2:], ['plugin', '-', 'hooks 0 Probe plugin'])


def ept_bhi():
    return f'{BHI:08x}'


class KnownBuilds(unittest.TestCase):
    """tools/plugins.py's table: every hook a handler of runtime/plugins.c, arguments recomp/patches.py takes, the same
    builds (hash and name) as runtime/plugins.c's, which tells a known build the game lacks from an unknown module."""

    def test_table(self):
        source = (ROOT / 'runtime' / 'plugins.c').read_text()
        handlers = set(re.findall(r'\{"(\w+)", \w+, EXL_', source))
        runtime_builds = dict(re.findall(r'\{"([0-9a-f]{64})",\s*"([^"]+)"\}', source))
        self.assertEqual(runtime_builds, {sha: p.name for sha, p in plugins.KNOWN.items()})
        mods = [('main', MAIN, 0x4000000)]
        for sha, p in plugins.KNOWN.items():
            self.assertRegex(sha, r'^[0-9a-f]{64}$')
            for version, hooks in p.versions.items():
                self.assertEqual(len(hooks.arguments), 3, p.name)
                for _, cb in hooks.sites:
                    self.assertIn(p.prefix + cb, handlers, f'{p.name} {version}')
                path = Path(tempfile.mkdtemp()) / 'p.txt'
                path.write_text(f'hook h {p.prefix}a {" ".join(hooks.arguments)}\n')
                table = patches.load(path, mods)
                self.assertEqual(len(table.hooks[0][2]), 3)


if __name__ == '__main__':
    unittest.main()
