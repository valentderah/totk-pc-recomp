"""recomp/gen.py's lift cache (<mod>/lift_cache.json) on a module it makes itself: a run with unchanged inputs reuses
every chunk, a changed patch site or spin wait lifts only the chunk that holds it, a chunk file or cache entry changed
behind the cache's back is not taken, a cache that cannot be written only warns, TOTK_LIFT_CACHE=0 and a change to
the generator's sources or options lift everything, and the output always equals a --no-cache lift's, folded.txt
included. No game files.

Run with .venv (lz4 and numpy, as recomp/): python tests/lift_cache_test.py
"""
import json
import os
import shutil
import stat
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tests'))
sys.path.insert(0, str(ROOT / 'tools'))
from check_content import GENERATED_MARKER  # noqa: E402
# The jump table module and the NSO writer of tests/exefs_patches_test.py; a change there changes this test's module,
# and test_reuse_and_invalidation checks that it still has its jump table (folded.txt 1020 1024).
from exefs_patches_test import JT_DATA, JT_RO, JT_TEXT, MOV_W8_100, MOV_W8_200, RET, nso  # noqa: E402

MOV_W8_300 = 0x52806008


def bl(pc, target):
    return 0x94000000 | (((target - pc) >> 2) & 0x3FFFFFF)


# The jump table module of tests/exefs_patches_test.py (fn at 0x8, its table in .rodata) and two more functions after
# it that call each other, so both are function starts: g: bl h; ret, h: mov w8, #0x100; bl g; ret. With --chunk-fns 1
# each of fn, g and h is a chunk of its own; h's mov is the patch site.
G = len(JT_TEXT)
H = G + 8
TEXT = JT_TEXT + struct.pack('<5I', bl(G, H), RET, MOV_W8_100, bl(H + 4, G), RET)
# fn's `b.hi Ldefault` at 0xc, as a spin wait back-edge to Ldefault (0x48): patches.check accepts it, the lift gates it
SPIN = 'spin main 48 c'


def patch_file(*replacements, first=None, spin=False):
    """A patch file with set a patching h's mov with each replacement (sets a0, a1, ...); first: a set defined before
    them, which moves their indices; spin: SPIN too."""
    lines = [f'set {first} TEST_{first.upper()} 0'] if first else []
    for i, w in enumerate(replacements):
        lines += [f'set a{i} TEST_A{i} 0', f'site a{i} main {H:x} {MOV_W8_100:08x} {w:08x}']
    if spin:
        lines.append(SPIN)
    return '\n'.join(lines) + '\n'


class LiftCache(unittest.TestCase):
    def setUp(self):
        self.d = Path(tempfile.mkdtemp())
        (self.d / 'exefs').mkdir()
        (self.d / 'exefs' / 'main').write_bytes(nso(TEXT, JT_RO, JT_DATA))

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def gen(self, patches, out='gen', *extra, env=None):
        p = self.d / 'patches.txt'
        p.write_text(patches)
        env = {**os.environ, 'TOTK_LIFT_CACHE': '', **(env or {})}
        r = subprocess.run([sys.executable, str(ROOT / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                            '--out', str(self.d / out), '--modules', 'main', '--jobs', '1', '--chunk-fns', '1',
                            '--patches', str(p), *extra], capture_output=True, text=True, env=env)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.stderr = r.stderr
        return json.loads((self.d / out / 'main' / 'manifest.json').read_text())['stats']

    def files(self, out='gen'):
        """{name: bytes} of everything gen.py wrote except the manifest (timings) and the cache."""
        root = self.d / out
        return {p.relative_to(root).as_posix(): p.read_bytes() for p in sorted(root.rglob('*'))
                if p.is_file() and p.name not in ('manifest.json', 'lift_cache.json')}

    def assert_like_fresh(self, patches):
        """The output under gen/ equals a --no-cache lift of the same inputs into an empty directory."""
        stats = self.gen(patches, 'fresh', '--no-cache')
        self.assertEqual(stats['chunks_reused'], 0)
        self.assertEqual(self.files(), self.files('fresh'))
        shutil.rmtree(self.d / 'fresh')

    def chunk(self, n):
        return self.d / 'gen' / 'main' / f'main_{n:04d}.c'

    def test_reuse_and_invalidation(self):
        stats = self.gen(patch_file(MOV_W8_200))
        self.assertEqual((stats['emitted_functions'], stats['chunks_reused']), (3, 0))
        cache = json.loads((self.d / 'gen' / 'main' / 'lift_cache.json').read_text())
        self.assertEqual(cache['marker'], GENERATED_MARKER)
        self.assertEqual(sorted(cache['chunks']), ['main_0000.c', 'main_0001.c', 'main_0002.c'])
        self.assertIn('rt_patch_on[0]', self.chunk(2).read_text())
        folded = (self.d / 'gen' / 'main' / 'folded.txt').read_text()
        self.assertIn('1020 1024', folded)

        # Same inputs: nothing lifted, nothing written, the jump table's bytes still in folded.txt (replayed from the
        # cache, not read again) and the statistics unchanged.
        mtimes = [self.chunk(n).stat().st_mtime_ns for n in range(3)]
        again = self.gen(patch_file(MOV_W8_200))
        self.assertEqual((again['chunks_reused'], again['chunks_written']), (3, 0))
        self.assertEqual({k: v for k, v in again.items() if not k.startswith('chunks_')},
                         {k: v for k, v in stats.items() if not k.startswith('chunks_')})
        self.assertEqual([self.chunk(n).stat().st_mtime_ns for n in range(3)], mtimes)
        self.assertEqual((self.d / 'gen' / 'main' / 'folded.txt').read_text(), folded)
        self.assert_like_fresh(patch_file(MOV_W8_200))

        # Another replacement word: only h's chunk is lifted.
        self.assertEqual(self.gen(patch_file(MOV_W8_300))['chunks_reused'], 2)
        self.assert_like_fresh(patch_file(MOV_W8_300))

        # A set defined before the site's moves its index (rt_patch_on[1]): h's chunk again.
        self.assertEqual(self.gen(patch_file(MOV_W8_300, first='z'))['chunks_reused'], 2)
        self.assertIn('rt_patch_on[1]', self.chunk(2).read_text())
        self.assert_like_fresh(patch_file(MOV_W8_300, first='z'))

        # A second alternative at the site.
        self.assertEqual(self.gen(patch_file(MOV_W8_300, MOV_W8_200))['chunks_reused'], 2)
        self.assert_like_fresh(patch_file(MOV_W8_300, MOV_W8_200))

    def test_changed_file_is_lifted_again(self):
        """A chunk whose bytes changed while its size and mtime stayed (a flipped bit) is lifted and rewritten."""
        self.gen(patch_file(MOV_W8_200))
        p = self.chunk(0)
        st = p.stat()
        data = bytearray(p.read_bytes())
        data[-2] ^= 0x01
        p.write_bytes(bytes(data))
        os.utime(p, ns=(st.st_atime_ns, st.st_mtime_ns))
        stats = self.gen(patch_file(MOV_W8_200))
        self.assertEqual((stats['chunks_reused'], stats['chunks_written']), (2, 1))
        self.assert_like_fresh(patch_file(MOV_W8_200))

    def test_deleted_file_and_cache(self):
        self.gen(patch_file(MOV_W8_200))
        self.chunk(1).unlink()
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 2)
        (self.d / 'gen' / 'main' / 'lift_cache.json').write_text('{not json')
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 0)
        self.assertEqual(self.gen(patch_file(MOV_W8_200), 'gen', '--no-cache')['chunks_reused'], 0)
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 3)
        self.assert_like_fresh(patch_file(MOV_W8_200))

    def test_spin_wait_is_in_the_key(self):
        self.gen(patch_file(MOV_W8_200))
        self.assertEqual(self.gen(patch_file(MOV_W8_200, spin=True))['chunks_reused'], 2)
        self.assertIn('rt_spin_gate', self.chunk(0).read_text())
        self.assert_like_fresh(patch_file(MOV_W8_200, spin=True))
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 2)
        self.assertNotIn('rt_spin_gate', self.chunk(0).read_text())

    def test_hook_is_in_the_key(self):
        """A host hook at a site (recomp/patches.py `@hook:` words) is a ('hook', index) word of the site: another hook
        there lifts h's chunk again and nothing else."""
        def hooked(name):
            return ('set a0 TEST_A0 0\nhook h0 probe_a\nhook h1 probe_b\n'
                    f'site a0 main {H:x} {MOV_W8_100:08x} @hook:{name}\n')
        self.gen(hooked('h0'))
        self.assertIn('rt_hook(c, 0,', self.chunk(2).read_text())
        self.assertEqual(self.gen(hooked('h1'))['chunks_reused'], 2)
        self.assertIn('rt_hook(c, 1,', self.chunk(2).read_text())
        self.assert_like_fresh(hooked('h1'))

    def test_changed_cache_entry_is_not_taken(self):
        """A changed result in lift_cache.json (a folded range, here) fails the entry's digest: the chunk is lifted
        and folded.txt keeps the jump table's real bytes."""
        self.gen(patch_file(MOV_W8_200))
        p = self.d / 'gen' / 'main' / 'lift_cache.json'
        data = json.loads(p.read_text())
        self.assertEqual(data['chunks']['main_0000.c']['folded'], [[0x1020, 0x1024]])
        data['chunks']['main_0000.c']['folded'] = [[0x1060, 0x1064]]
        p.write_text(json.dumps(data))
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 2)
        self.assertIn('1020 1024', (self.d / 'gen' / 'main' / 'folded.txt').read_text())
        self.assert_like_fresh(patch_file(MOV_W8_200))

    def test_unwritable_cache_only_warns(self):
        """A cache that cannot be replaced leaves the lift successful, its manifest written; the old cache's entry for
        the chunk the run rewrote no longer matches it."""
        self.gen(patch_file(MOV_W8_200))
        p = self.d / 'gen' / 'main' / 'lift_cache.json'
        os.chmod(p, stat.S_IREAD)
        try:
            self.assertEqual(self.gen(patch_file(MOV_W8_300))['chunks_reused'], 2)
            self.assertIn('lift_cache.json not written', self.stderr)
        finally:
            os.chmod(p, stat.S_IREAD | stat.S_IWRITE)
        self.assertEqual(self.gen(patch_file(MOV_W8_300))['chunks_reused'], 2)
        self.assert_like_fresh(patch_file(MOV_W8_300))

    def test_environment_switch(self):
        self.gen(patch_file(MOV_W8_200))
        self.assertEqual(self.gen(patch_file(MOV_W8_200), env={'TOTK_LIFT_CACHE': '0'})['chunks_reused'], 0)
        self.assertIn('lifting every chunk (TOTK_LIFT_CACHE=0)', self.stderr)
        self.assertEqual(self.gen(patch_file(MOV_W8_200))['chunks_reused'], 3)

    def test_emitter_options_are_in_the_key(self):
        self.gen(patch_file(MOV_W8_200))
        self.assertEqual(self.gen(patch_file(MOV_W8_200), 'gen', '--reg-locals', '0')['chunks_reused'], 0)

    def test_generator_sources_are_in_the_key(self):
        """A copy of the generator lifts everything again after a comment in its lifter.py changes, and nothing
        after a patch table beside it (recomp/patches-*.txt) changes."""
        copy = self.d / 'repo'
        shutil.copytree(ROOT / 'recomp', copy / 'recomp', ignore=shutil.ignore_patterns('__pycache__'))
        (copy / 'tools').mkdir()
        for name in ('check_content.py', 'hostmem.py'):
            shutil.copy2(ROOT / 'tools' / name, copy / 'tools' / name)

        def run():
            r = subprocess.run([sys.executable, str(copy / 'recomp' / 'gen.py'), '--exefs', str(self.d / 'exefs'),
                                '--out', str(self.d / 'gen'), '--modules', 'main', '--jobs', '1', '--chunk-fns', '1'],
                               capture_output=True, text=True, env={**os.environ, 'TOTK_LIFT_CACHE': ''})
            self.assertEqual(r.returncode, 0, r.stderr)
            return json.loads((self.d / 'gen' / 'main' / 'manifest.json').read_text())['stats']['chunks_reused']

        self.assertEqual(run(), 0)
        self.assertEqual(run(), 3)
        table = next((copy / 'recomp').glob('patches-*.txt'))
        table.write_text(table.read_text() + '# another line\n')
        self.assertEqual(run(), 3)
        lifter = copy / 'recomp' / 'lifter.py'
        lifter.write_text(lifter.read_text() + '# another comment\n')
        self.assertEqual(run(), 0)


if __name__ == '__main__':
    unittest.main()
