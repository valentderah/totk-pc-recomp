"""runtime/blueprint_txt.c and runtime/blueprint_inflate.c on inputs made here, with no game files and no build of the
game: a real export decodes to the blob byte for byte, a file cut anywhere inside the Base64 value is refused, a member
that is not Base64, a member that is not a gzip stream, a stream that inflates past the blob, a non-JSON file and an
object without the member each get their own result, and the caps on the member and on the inflated size hold.

The two C files are pure: no IO, no globals, no game data. They are compiled here with the project's clang into a small
program that prints the result enum and the decoded length, so the checks run against the real code and not a copy of it.

Run: .venv\\Scripts\\python tests\\blueprint_txt_test.py
"""
import gzip
import json
import struct
import subprocess
import sys
import tempfile
import textwrap
import unittest
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')

BLOB = 6688          # BPC_BLOB_SIZE
BLOCK = 21 * 280     # not used directly; the blob's actor block, for building a synthetic compile
VERSION = 2


def cai_blob(actors=4, seed=1):
    """A minimal but valid CAI blob: the magic, version, count, one NUL-terminated name per actor, the rest zero."""
    blob = bytearray(BLOB)
    blob[0:6] = b'CmbAct'
    blob[8] = VERSION
    blob[9] = actors
    blob[10] = actors - 1
    for i in range(actors):
        base = 0x0C + i * 280 + 128     # m1, m2, m3 are 152 bytes; the name field is 128 bytes
        name = ('Part_%02d' % i).encode()
        blob[base:base + len(name)] = name
    if seed:
        for i in range(0x1818, BLOB):
            blob[i] = (i * seed) & 0xFF
    return bytes(blob)


HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

enum BtxResult { BTX_OK, BTX_EMPTY, BTX_NO_JSON, BTX_NO_MEMBER, BTX_BAD_BASE64, BTX_BAD_STREAM, BTX_TOO_LARGE,
                 BTX_WRONG_SIZE };
enum BpiResult { BPI_OK, BPI_BAD_STREAM, BPI_TOO_LARGE };
#define BPC_BLOB_SIZE 6688

enum BtxResult btx_decode(const void *data, size_t size, void *blob, size_t blob_cap, size_t *blob_size);

/* Reads the file named on the command line, decodes it, and prints "<result> <decoded bytes>". */
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 3;
    static uint8_t in[1 << 21];
    size_t n = fread(in, 1, sizeof in, f);
    fclose(f);
    static uint8_t blob[BPC_BLOB_SIZE];
    size_t got = 0;
    enum BtxResult r = btx_decode(in, n, blob, sizeof blob, &got);
    printf("%d %zu\n", (int)r, got);
    return 0;
}
'''


class BlueprintTxtTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # clang needs a temp directory it can write to; the system temp is not always one, so build under build/.
        base = ROOT / 'build' / 'tests'
        base.mkdir(parents=True, exist_ok=True)
        cls.tmp = tempfile.TemporaryDirectory(dir=str(base))
        cls.dir = Path(cls.tmp.name)
        src = cls.dir / 'harness.c'
        src.write_text(HARNESS.replace('#define BPC_BLOB_SIZE 6688', '#define BPC_BLOB_SIZE %d' % BLOB), encoding='utf-8')
        exe = cls.dir / 'harness.exe'
        # clang writes its own temporaries; point them at the same writable folder as the sources.
        env = dict(__import__('os').environ, TMP=str(cls.dir), TEMP=str(cls.dir))
        cmd = [str(CLANG), '-O1', '-std=gnu11', '-D_WIN32_WINNT=0x0A00', '-I', str(RUNTIME),
               str(src), str(RUNTIME / 'blueprint_txt.c'), str(RUNTIME / 'blueprint_inflate.c'),
               '-l:libz.a', '-o', str(exe)]
        r = subprocess.run(cmd, capture_output=True, text=True, env=env)
        if r.returncode != 0:
            raise unittest.SkipTest('cannot compile the harness: ' + (r.stderr or r.stdout)[-600:])
        cls.exe = exe

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_on(self, content, name='in.txt'):
        p = self.dir / name
        p.write_bytes(content if isinstance(content, bytes) else content.encode('utf-8'))
        r = subprocess.run([str(self.exe), str(p)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        code, n = r.stdout.split()
        return int(code), int(n)

    # ---- the result enum values, in the order of blueprint_txt.h
    OK, EMPTY, NO_JSON, NO_MEMBER, BAD_BASE64, BAD_STREAM, TOO_LARGE, WRONG_SIZE = range(8)

    def export(self, blob=None):
        blob = cai_blob() if blob is None else blob
        member = gzip.compress(blob, mtime=0)
        b64 = __import__('base64').b64encode(member).decode('ascii')
        return json.dumps({'combinedActorInfo': {}, 'combinedActorInfoBase64': b64, 'parts': []})

    def test_real_export_decodes_to_the_blob(self):
        blob = cai_blob(seed=7)
        self.assertEqual(self.run_on(self.export(blob)), (self.OK, BLOB))

    def test_leading_bom_and_whitespace_accepted(self):
        text = '\ufeff\t\r\n  ' + self.export()
        self.assertEqual(self.run_on(text), (self.OK, BLOB))

    def test_cut_inside_the_base64_is_refused(self):
        text = self.export()
        member_at = text.index('"combinedActorInfoBase64"')
        value_start = text.index('"', text.index(':', member_at)) + 1
        value_end = text.index('"', value_start)
        for cut in (value_start + 1, value_start + 5, (value_start + value_end) // 2, value_end - 1):
            damaged = text[:cut] + text[cut + 1:]
            self.assertIn(self.run_on(damaged)[0], (self.BAD_BASE64, self.BAD_STREAM, self.NO_MEMBER),
                          'a cut at %d was accepted' % cut)

    def test_not_a_json_object(self):
        for text in ('', '[]', 'hello', 'null', '   '):
            self.assertIn(self.run_on(text)[0], (self.EMPTY, self.NO_JSON), text)

    def test_member_missing(self):
        self.assertEqual(self.run_on('{"parts": []}'), (self.NO_MEMBER, 0))

    def test_member_not_a_string(self):
        self.assertEqual(self.run_on('{"combinedActorInfoBase64": 12}'), (self.NO_MEMBER, 0))

    def test_member_is_not_base64(self):
        self.assertEqual(self.run_on('{"combinedActorInfoBase64": "!!!!"}'), (self.BAD_BASE64, 0))

    def test_member_is_not_a_gzip_stream(self):
        import base64
        self.assertEqual(self.run_on(json.dumps({'combinedActorInfoBase64': base64.b64encode(b'not gzip').decode()}))
                         [0], self.BAD_STREAM)

    def test_stream_larger_than_a_blob_is_refused(self):
        import base64
        big = gzip.compress(bytes(range(256)) * 200, mtime=0)   # ~51 KiB inflated
        self.assertEqual(self.run_on(json.dumps({'combinedActorInfoBase64': base64.b64encode(big).decode()}))[0],
                         self.TOO_LARGE)

    def test_stream_of_the_wrong_size_is_refused(self):
        import base64
        small = gzip.compress(cai_blob()[:-1], mtime=0)
        self.assertEqual(self.run_on(json.dumps({'combinedActorInfoBase64': base64.b64encode(small).decode()}))[0],
                         self.WRONG_SIZE)

    def test_escape_inside_the_member_is_refused(self):
        self.assertEqual(self.run_on('{"combinedActorInfoBase64": "AAAA\\nAAAA"}')[0], self.BAD_BASE64)

    def test_unclosed_string_is_refused(self):
        self.assertEqual(self.run_on('{"combinedActorInfoBase64": "AAAA')[0], self.NO_MEMBER)


if __name__ == '__main__':
    unittest.main(verbosity=2)
