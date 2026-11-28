"""Tests for shader_list supplement merging, mvprev seed generation, and harvest_shader_list round-trips.

No game files or compiled totk.exe; runs on Python 3.14 (tools/shader_census uses compression.zstd).

Run: .venv-py314\\Scripts\\python tests\\shader_list_prepare_test.py
"""
import hashlib
import json
import struct
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))
import harvest_shader_list as hsl  # noqa: E402
import prepare_shaders as ps  # noqa: E402
import shader_list as sl  # noqa: E402


def fake_program(stage=0, code_body=b'\x01\x02\x03\x04' * 16, c1=b'\xaa\xbb'):
    """Synthetic NVN control section (0x880 bytes) + code + c1 trailer."""
    ctrl = bytearray(ps.CONTROL)
    ctrl[0:4] = b'NVN\0'
    c1_off = len(code_body)
    length = c1_off + len(c1)
    struct.pack_into('<II', ctrl, 0x700, c1_off, length)
    struct.pack_into('<I', ctrl, 0x714, stage)
    struct.pack_into('<III', ctrl, 0x748, 8, 4, 1)
    code = code_body + c1
    census_key = hashlib.sha256(bytes(ctrl) + code).hexdigest()
    ch = ps.code_hash(stage, bytes(ctrl), code, length, c1)
    return census_key, ch, bytes(ctrl), code, length, c1


class ShaderListPrepareTest(unittest.TestCase):
    def test_failed_replace_preserves_existing_supplement(self):
        _, ch, ctrl, code, length, c1 = fake_program()
        program = struct.pack('<III', 0, length, len(c1)) + ch + ctrl + code + c1
        record = struct.pack('<I', 0) + ch + bytes(ps.ANS_REC - 36)
        headers = {name: struct.pack('<8sII', magic, 1, size) for name, magic, size in (
            ('programs', hsl.PRG_MAGIC, 0), ('answers', hsl.ANS_MAGIC, ps.ANS_REC), ('prepared', hsl.PRP_MAGIC, 36))}
        valid = dict(headers, programs=headers['programs'] + program, answers=headers['answers'] + record)
        cases = [(name, buf[:8] + struct.pack('<I', 0) + buf[12:], 'unsupported cache version')
                 for name, buf in valid.items()]
        cases += [('answers', headers['answers'], 'no recorded shader answers'),
                  ('answers', valid['answers'] + bytes(ps.ANS_REC), '1 of 2'),
                  ('answers', valid['answers'] + b'x', 'incomplete final record'),
                  ('prepared', valid['prepared'] + b'x', 'incomplete final record'),
                  ('programs', valid['programs'][:-1], 'invalid program length')]
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            base, output = d / 'base.tsv', d / 'supplement.tsv'
            base.write_bytes(b'')
            before = b'# Keep original bytes and line endings\r\nV\texisting\t{}\r\n'
            output.write_bytes(before)
            for name, bad, message in cases:
                with self.subTest(cache=name, error=message):
                    for kind, contents in valid.items():
                        (d / f'shader-{kind}.bin').write_bytes(bad if name == kind else contents)
                    run = subprocess.run([sys.executable, hsl.__file__, d, '--base', base, '--replace', '--out', output],
                                         capture_output=True, text=True)
                    self.assertNotEqual(run.returncode, 0)
                    self.assertIn(message, run.stderr)
                    self.assertEqual(output.read_bytes(), before)

    def test_refs_and_release_keep_a_base_only_list(self):
        import make_release

        base = 'V\t' + '0' * 64 + '\t{}\n'
        with tempfile.TemporaryDirectory() as tmp, \
             mock.patch.object(sl, 'git', side_effect=lambda repo, cmd, *_: {
                 'rev-parse': 'false', 'show': base, 'log': 'upstream'}[cmd]):
            release = Path(tmp)
            # No bundle: read upstream history from refs. Only 1.1.0 adds the repository rows.
            with mock.patch.object(ps, 'BUNDLED_LIST', release / 'missing.tsv'):
                for version in ('v121', 'v143', None):
                    self.assertEqual(ps.read_list(None, version=version), base)
                self.assertEqual(ps.read_list(None, supplement=False, version='v110'), base)
                merged = ps.read_list(None, version='v110')
                self.assertNotEqual(merged, base)

            # Assemble the release sources; the base and supplement must also be separable without refs.
            with mock.patch.object(make_release, 'version', return_value='test'):
                make_release.build_sources(release, {})
            bundled = release / 'data' / 'shader_list.tsv'
            self.assertEqual((release / 'tools' / 'data' / sl.SUPPLEMENT.name).read_bytes(), sl.SUPPLEMENT.read_bytes())
            with mock.patch.object(ps, 'BUNDLED_LIST', bundled):
                for version in ('v121', 'v143', None):
                    self.assertEqual(ps.read_list(None, version=version), base)
                self.assertEqual(ps.read_list(None, supplement=False, version='v110'), base)
                self.assertEqual(ps.read_list(None, version='v110'), merged)

    def test_unspecified_storage_image_format_roundtrips(self):
        _, _, ctrl, _, _, _ = fake_program(4)
        packed = bytearray(147)
        packed[32 + 3] = ps.TEX.index('2d')
        deps = hsl.unpack_deps(packed, 4)
        self.assertEqual(deps['img']['3'], {'type': '2d'})
        self.assertEqual(ps.answers(deps, 4, ctrl), packed)

    def test_merge_supplement_appends_unique_non_comment_rows(self):
        with tempfile.TemporaryDirectory() as tmp:
            extra = Path(tmp) / 'supp.tsv'
            extra.write_text('# comment\n\nV\tabc\t{"mvprev":true}\nV\tdef\t{}\n', encoding='utf-8')
            base = 'V\tabc\t{"mvprev":true}\nS\t123\t0\tt\n'
            merged = sl.merge_supplement(base, extra=extra)
            self.assertEqual(merged.splitlines(), [
                'V\tabc\t{"mvprev":true}',
                'S\t123\t0\tt',
                'V\tdef\t{}',
            ])
            # Merging twice is idempotent.
            self.assertEqual(sl.merge_supplement(merged, extra=extra), merged)

    def test_answers_packs_mvprev_only_for_vertex_stage(self):
        ctrl = bytearray(ps.CONTROL)
        struct.pack_into('<III', ctrl, 0x748, 16, 1, 1)
        vs_mv = ps.answers({'attr': {'6': 'uint'}, 'mvprev': True}, 0, bytes(ctrl))
        vs_plain = ps.answers({'attr': {'6': 'uint'}}, 0, bytes(ctrl))
        ps_mv = ps.answers({'fout': {'0': 'uint'}, 'mvprev': True}, 4, bytes(ctrl))
        self.assertEqual(vs_mv[96 + 6], 2)
        self.assertEqual(vs_mv[146], 0x80)
        self.assertEqual(vs_plain[146], 0)
        self.assertEqual(ps_mv[146], 0)

    def test_harvest_roundtrip_recovers_misses_and_warm_rows(self):
        vs_key, vs_ch, vs_ctrl, vs_code, vs_len, vs_c1 = fake_program(0, b'\x11' * 64, b'\x22' * 4)
        cs_key, cs_ch, cs_ctrl, cs_code, cs_len, cs_c1 = fake_program(5, b'\x33' * 64, b'')

        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp)
            # Write TOTKSPRG with both programs.
            with open(d / 'shader-programs.bin', 'wb') as f:
                f.write(b'TOTKSPRG' + struct.pack('<II', 1, 0))
                for st, length, c1, ch, ctrl, code in (
                    (0, vs_len, vs_c1, vs_ch, vs_ctrl, vs_code),
                    (5, cs_len, cs_c1, cs_ch, cs_ctrl, cs_code),
                ):
                    f.write(struct.pack('<III', st, length, len(c1)) + ch + ctrl + code + c1)

            # Empty shader-prepared.bin -> both met variants count as misses.
            (d / 'shader-prepared.bin').write_bytes(b'TOTKSPRP' + struct.pack('<II', 1, 36))

            packed_vs = ps.answers({'attr': {'6': 'uint'}, 'tex': {'12': '2darray'}, 'mvprev': True}, 0, vs_ctrl)
            packed_cs = ps.answers({'img': {'0': {'type': '2d', 'fmt': 'R16Float'}}}, 5, cs_ctrl)
            iid_vs = hashlib.sha256(b'TKSI' + struct.pack('<I', 1) + vs_ch + packed_vs).digest()
            iid_cs = hashlib.sha256(b'TKSI' + struct.pack('<I', 1) + cs_ch + packed_cs).digest()

            with open(d / 'shader-answers.bin', 'wb') as f:
                f.write(b'TOTKSANS' + struct.pack('<II', 1, ps.ANS_REC))
                f.write(struct.pack('<I', 0) + vs_ch + iid_vs + packed_vs + b'\x00')
                f.write(struct.pack('<I', 5) + cs_ch + iid_cs + packed_cs + b'\x00')

            base_tsv = d / 'base.tsv'
            base_tsv.write_text('', encoding='utf-8')
            lines, stats = hsl.harvest([d], base_path=base_tsv, include_warm=True)
            self.assertEqual(stats['unmapped'], 0)
            self.assertEqual(stats['v_rows'], 2)
            self.assertEqual(stats['warm_rows'], 2)

            supp = d / 'supp.tsv'
            hsl.write_output(supp, lines)
            text = sl.merge_supplement('', extra=supp)
            variants, prio = ps.parse_list(text)
            self.assertEqual(prio[vs_key], 1)
            self.assertEqual(prio[cs_key], 1)
            by_k = {k: deps for k, deps in variants}
            self.assertEqual(ps.answers(by_k[vs_key], 0, vs_ctrl), packed_vs)
            self.assertEqual(ps.answers(by_k[cs_key], 5, cs_ctrl), packed_cs)

            # A direct answers-file argument finds the sibling program map AND the prepared hit.
            (d / 'shader-prepared.bin').write_bytes(b'TOTKSPRP' + struct.pack('<II', 1, 36) + bytes(4) + iid_cs)
            for source in (d, d / 'shader-answers.bin'):
                rows, counts = hsl.harvest([source], base_path=base_tsv)
                self.assertEqual((counts['unmapped'], counts['prepared_hits']), (0, 1))
                self.assertEqual([key for key, _ in ps.parse_list('\n'.join(rows))[0]], [vs_key])

            # Replacing an existing supplement from the same captures and raw upstream history is repeatable.
            (d / 'shader-prepared.bin').unlink()
            (d / 'refs' / 'MacNativeGames').mkdir(parents=True)
            merge = sl.merge_supplement
            with mock.patch.object(hsl, 'ROOT', d), mock.patch.object(ps, 'BUNDLED_LIST', d / 'missing.tsv'), \
                 mock.patch.object(sl, 'git', side_effect=['false', '', 'upstream']), \
                 mock.patch.object(sl, 'merge_supplement', side_effect=lambda text: merge(text, supp)), \
                 mock.patch.object(sys, 'argv', ['harvest', str(d), '--warm', '--replace', '--out', str(supp)]):
                before = supp.read_bytes()
                self.assertEqual(hsl.main(), 0)
                self.assertEqual(supp.read_bytes(), before)

    def test_cli_supplement_only_changes_110_seed(self):
        key, _, ctrl, code, _, _ = fake_program()
        inventory = {key: {'stage': 'vs', 'control': ctrl, 'code': code}}
        with tempfile.TemporaryDirectory() as tmp:
            out = Path(tmp)
            base = out / 'list.tsv'
            base.write_bytes(b'')
            extra = out / 'extra.tsv'
            extra.write_text(f'V\t{key}\t{{"mvprev":true}}\nS\t{key}\t0\tt\n', encoding='utf-8')
            argv = ['prepare_shaders.py', str(out), '--list', str(base), '--no-run']
            merge = sl.merge_supplement
            for args, expected in (([], [(0, 2)]), (['--version', 'v143'], [(0, 2)]),
                                   (['--version', 'v110'], [(0x80, 0), (0, 0)]),
                                   (['--version', 'v110', '--no-supplement'], [(0, 2)])):
                with self.subTest(args=args), mock.patch.object(sys, 'argv', argv + args), \
                     mock.patch.object(sl, 'merge_supplement', side_effect=lambda text: merge(text, extra)), \
                     mock.patch.object(ps, 'inventory', return_value=(inventory, 0)):
                    self.assertEqual(ps.main(), 0)
                    records = (out / 'seed-answers.bin').read_bytes()[16:]
                    ends = range(ps.ANS_REC, len(records) + 1, ps.ANS_REC)
                    self.assertEqual([tuple(records[end - 2:end]) for end in ends], expected)

    def test_prepare_shaders_cli_with_mvprev_and_supplement(self):
        vs_key, _, vs_ctrl, vs_code, _, _ = fake_program(0, b'\x44' * 64, b'\x55' * 4)
        fake_inv = {vs_key: {'stage': 'vs', 'control': vs_ctrl, 'code': vs_code,
                             'source': 'Shader/test', 'origin': 'bnsh', 'startup': True}}
        with tempfile.TemporaryDirectory() as tmp:
            t = Path(tmp)
            out = t / 'build'
            romfs, exefs = t / 'romfs', t / 'exefs'
            out.mkdir(), romfs.mkdir(), exefs.mkdir()
            base_list = t / 'list.tsv'
            base_list.write_text(f'V\t{vs_key}\t{{"tex":{{"0":"2d"}}}}\nS\t{vs_key}-x_b\t0\tt\n', encoding='utf-8')
            argv = ['prepare_shaders.py', str(out), '--romfs', str(romfs), '--exefs', str(exefs),
                    '--list', str(base_list), '--no-supplement', '--mvprev', '--no-run']
            with mock.patch.object(ps, 'inventory', return_value=(fake_inv, 0)), \
                 mock.patch.object(sys, 'argv', argv):
                self.assertEqual(ps.main(), 0)
            ans_body = (out / 'seed-answers.bin').read_bytes()[16:]
            flags = [ans_body[i + 214] for i in range(0, len(ans_body), ps.ANS_REC)]
            prios = [ans_body[i + 215] for i in range(0, len(ans_body), ps.ANS_REC)]
            # Both normal and mvprev variants emitted at priority 0 (title t).
            self.assertIn(0, flags)
            self.assertIn(0x80, flags)
            self.assertEqual(set(prios), {0})

    def test_repo_supplement_parses_cleanly(self):
        text = sl.merge_supplement('')
        variants, prio = ps.parse_list(text)
        self.assertGreater(len(variants), 0)
        self.assertGreater(len(prio), 0)
        for key, deps in variants:
            self.assertEqual(len(key), 64)
            packed = ps.answers(deps, 0 if deps.get('mvprev') or deps.get('attr') else 4, bytes(ps.CONTROL))
            self.assertEqual(len(packed), 147)


if __name__ == '__main__':
    unittest.main()
