"""The BYML reader, link-tested against the real runtime/byml.c.

The reader is the module every ".byml"/".bgyml" parameter table in the port will be read through, so the test pins down
what the format means instead of replaying a recorded file: it builds documents byte by byte (the header, the hash key
and string tables, every value type, arrays, hashes, the 0x20/0x21 hash tables) and damaged variants (truncations, bogus
offsets, wrong magic and version, cycles), writes them to a temporary directory, compiles the real reader with a small
walker (tests/byml_main.c) and compares what the walker prints with what Python built. A second pass truncates a valid
document at every byte and only requires that the walker survives.

Run: .venv\\Scripts\\python tests\\byml_test.py
"""
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
RUNTIME = ROOT / 'runtime'
CLANG = Path(r'C:\msys64\ucrt64\bin\clang.exe')
MAIN = ROOT / 'tests' / 'byml_main.c'
OUT = ROOT / 'build' / 'byml_test.exe'
INCLUDES = ['-I', str(RUNTIME)]

# The walker's recursion budget (tests/byml_main.c); the expected dump repeats the cut-off.
MAX_DEPTH = 32
MAX_NODES = 4096

# The tagged model the builder and the expected dump share. A node is a tuple whose first item names its type, so that a
# value node's type is stated rather than guessed from the Python value.
CONTAINERS = frozenset({'hash', 'array', 'hashed20', 'hashed21'})
TYPES = {
    'hash': 0xC1, 'array': 0xC0, 'hashed20': 0x20, 'hashed21': 0x21,
    'string': 0xA0, 'stringindex': 0xA0, 'binary': 0xA1, 'file': 0xA2,
    'bool': 0xD0, 'int': 0xD1, 'float': 0xD2, 'uint': 0xD3, 'int64': 0xD4, 'uint64': 0xD5, 'double': 0xD6,
    'null': 0xFF,
}
NUMERIC = frozenset({'bool', 'int', 'float', 'uint', 'int64', 'uint64', 'double'})


def u24(value):
    """The little-endian u24 the format stores in a container header."""
    return struct.pack('<I', value)[:3]


def node_type(node):
    """The one-byte type code a parent entry stores for this node; a brokenref states its own type."""
    tag = node[0]
    if tag == 'brokenref':
        return node[1]
    return TYPES[tag]


class Writer:
    """Writes a BYML document from a tagged model, Nintendo's layout: the header, the key and string tables, then the
    containers, each entry's absolute value offset patched once its node has been written."""

    def __init__(self):
        self.buf = bytearray(16)
        self.keys = []
        self.strings = []

    def build(self, root, version=7):
        self._scan(root)
        self.keys.sort()       # Nintendo stores both tables sorted, so the test files look like the game's
        self.strings.sort()
        self.buf[0:2] = b'YB'
        struct.pack_into('<H', self.buf, 2, version)
        if self.keys:
            self._align()                     # a container starts on a 4-byte boundary, its offset recorded after it
            struct.pack_into('<I', self.buf, 4, len(self.buf))
            self._table(self.keys)
        if self.strings:
            self._align()
            struct.pack_into('<I', self.buf, 8, len(self.buf))
            self._table(self.strings)
        struct.pack_into('<I', self.buf, 12, self._node(root))
        return bytes(self.buf)

    def _scan(self, node):
        """Collects every hash key and string value, the two tables being written before the nodes."""
        tag = node[0]
        if tag == 'hash':
            for key, value in node[1].items():
                if key not in self.keys:
                    self.keys.append(key)
                self._scan(value)
        elif tag == 'array':
            for value in node[1]:
                self._scan(value)
        elif tag == 'string':
            if node[1] not in self.strings:
                self.strings.append(node[1])

    def _align(self):
        while len(self.buf) % 4:
            self.buf.append(0)

    def _table(self, strings):
        """A 0xC2 table: header, count + 1 offsets relative to the table, then the NUL-terminated strings."""
        self._align()
        base = len(self.buf)
        count = len(strings)
        self.buf += bytes([0xC2]) + u24(count)
        offsets = []
        body = bytearray()
        for text in strings:
            offsets.append(4 + 4 * (count + 1) + len(body))
            body += text.encode('utf-8') + b'\0'
        offsets.append(4 + 4 * (count + 1) + len(body))
        for offset in offsets:
            self.buf += struct.pack('<I', offset)
        self.buf += body

    def _node(self, node):
        """Writes a container, appending it after everything written so far, and returns its offset."""
        if node[0] not in CONTAINERS:
            raise ValueError('not a container: %r' % (node[0],))
        self._align()
        base = len(self.buf)
        if node[0] == 'hash':
            items = sorted(node[1].items(), key=lambda item: self.keys.index(item[0]))
            self.buf += bytes([0xC1]) + u24(len(items)) + b'\0' * (8 * len(items))
            for i, (key, value) in enumerate(items):
                at = base + 4 + 8 * i
                self.buf[at:at + 4] = struct.pack('<I', self.keys.index(key))
                self.buf[at + 3] = node_type(value)
                word = self._node(value) if value[0] in CONTAINERS else self._inline(value)
                self.buf[at + 4:at + 8] = struct.pack('<I', word)
        elif node[0] == 'array':
            items = node[1]
            types = (len(items) + 3) & ~3   # the type bytes, padded to a multiple of 4
            self.buf += bytes([0xC0]) + u24(len(items)) + b'\0' * (types + 4 * len(items))
            for i, value in enumerate(items):
                self.buf[base + 4 + i] = node_type(value)
                word = self._node(value) if value[0] in CONTAINERS else self._inline(value)
                at = base + 4 + types + 4 * i
                self.buf[at:at + 4] = struct.pack('<I', word)
        else:
            kind = 0x20 if node[0] == 'hashed20' else 0x21
            pairs = node[1]
            self.buf += bytes([kind]) + u24(len(pairs)) + b'\0' * (8 * len(pairs))
            for i, (hash_value, word) in enumerate(pairs):
                at = base + 4 + 8 * i
                if kind == 0x20:
                    struct.pack_into('<II', self.buf, at, hash_value, word)
                else:
                    struct.pack_into('<II', self.buf, at, word, hash_value)
        return base

    def _inline(self, node):
        """The 4-byte word a parent entry stores for a value node, appending the value's payload when it is elsewhere."""
        tag = node[0]
        if tag == 'string':
            return self.strings.index(node[1])
        if tag == 'stringindex':       # a valid string node whose index the test breaks on purpose
            return node[1]
        if tag == 'int':
            return node[1] & 0xFFFFFFFF
        if tag == 'uint':
            return node[1] & 0xFFFFFFFF
        if tag == 'float':
            return struct.unpack('<I', struct.pack('<f', node[1]))[0]
        if tag == 'bool':
            return 1 if node[1] else 0
        if tag == 'null':
            return 0
        if tag == 'brokenref':         # a container-typed entry whose word deliberately points nowhere
            return node[2]
        if tag in ('int64', 'uint64', 'double', 'binary', 'file'):
            return self._payload(node)
        raise ValueError('not a value node: %r' % (tag,))

    def _payload(self, node):
        self._align()
        offset = len(self.buf)
        tag = node[0]
        if tag == 'int64':
            self.buf += struct.pack('<q', node[1])
        elif tag == 'uint64':
            self.buf += struct.pack('<Q', node[1])
        elif tag == 'double':
            self.buf += struct.pack('<d', node[1])
        elif tag == 'binary':
            self.buf += struct.pack('<I', len(node[1])) + node[1]
        elif tag == 'file':
            self.buf += struct.pack('<I', len(node[1])) + struct.pack('<I', 0x1000) + node[1]
        return offset


def number(value):
    """The walker prints numbers with printf("%.17g"); Python formats the same way for the expected output."""
    return '%.17g' % float(value)


# The type codes byml_number handles, for a brokenref's expected line.
NUMERIC_TYPES = frozenset(TYPES[tag] for tag in NUMERIC)

# The numeric types whose value word is inline in the parent (the others hold an offset to their 8 bytes). The walker
# prints the raw word for these, so the test checks byml_array_value on real data.
INLINE_NUMERIC = frozenset({'bool', 'int', 'float', 'uint'})


def inline_word(node):
    """The 4-byte word an inline value node stores, as the walker prints it for an array element."""
    tag = node[0]
    if tag == 'bool':
        return 1 if node[1] else 0
    if tag == 'int':
        return struct.unpack('<I', struct.pack('<i', node[1]))[0]
    if tag == 'uint':
        return node[1] & 0xFFFFFFFF
    if tag == 'float':
        return struct.unpack('<I', struct.pack('<f', node[1]))[0]
    raise ValueError('not an inline numeric value: %r' % (tag,))


def describe(node):
    """The one line the walker prints for a node, before its children (tests/byml_main.c)."""
    tag = node[0]
    if tag == 'hash':
        return '0xc1 hash %d' % len(node[1])
    if tag == 'array':
        return '0xc0 array %d' % len(node[1])
    if tag == 'hashed20':
        return '0x20 hashtable %d' % len(node[1])
    if tag == 'hashed21':
        return '0x21 hashtable %d' % len(node[1])
    if tag in ('string', 'stringindex'):
        return '0xa0 string "%s"' % node[1] if tag == 'string' else '0xa0 string NULL'
    if tag in ('binary', 'file'):
        return '0x%02x data' % TYPES[tag]
    if tag == 'null':
        return '0xff null'
    if tag == 'brokenref':
        # A container entry that does not resolve is an absent node; a numeric one is a value whose payload is missing.
        return '0x%02x number nan' % node[1] if node[1] in NUMERIC_TYPES else '0x00 unknown'
    return '0x%02x number %s' % (TYPES[tag], number(node[1]))


def array_value_desc(node):
    """The walker's line for a value-typed array element, whose type and value live in the array's own lists."""
    tag = node[0]
    if tag == 'string':
        return '0xa0 string "%s"' % node[1]
    if tag == 'stringindex':
        return '0xa0 string NULL'
    if tag in ('binary', 'file'):
        return '0x%02x data' % TYPES[tag]
    if tag == 'null':
        return '0xff null'
    if tag == 'brokenref':
        return '0x%02x number nan' % node[1] if node[1] in NUMERIC_TYPES else '0x00 unknown'
    if tag in INLINE_NUMERIC:
        return '0x%02x number %s word 0x%08x' % (TYPES[tag], number(node[1]), inline_word(node))
    return '0x%02x number %s' % (TYPES[tag], number(node[1]))


def dump(node, depth, label, keys, visited):
    """The walker's dump of a model tree, in the order tests/byml_main.c prints it."""
    indent = '  ' * depth
    if depth > MAX_DEPTH:
        return [indent + '...']
    visited[0] += 1
    if visited[0] > MAX_NODES:
        return [indent + '...']
    lines = [indent + label + describe(node)]
    if node[0] == 'hash':
        items = sorted(node[1].items(), key=lambda item: keys.index(item[0]))
        for i, (key, value) in enumerate(items):
            lines += dump(value, depth + 1, 'entry %d key "%s" ' % (i, key), keys, visited)
    elif node[0] == 'array':
        for i, value in enumerate(node[1]):
            if value[0] in CONTAINERS:
                lines += dump(value, depth + 1, 'elem %d ' % i, keys, visited)
            else:
                lines.append('  ' * (depth + 1) + 'elem %d ' % i + array_value_desc(value))
    elif node[0] in ('hashed20', 'hashed21'):
        # These tables carry no type per entry, so the API exposes only their count: there is nothing to walk.
        pass
    return lines


def model_case(name, model, version=7):
    """A case built from a model: the expected output is the same walk over the model Python just built."""
    writer = Writer()
    data = writer.build(model, version)
    lines = ['file %s.bgyml' % name, 'open 1']
    lines += dump(model, 0, '', writer.keys, [0])
    lines += ['probes ok']
    return Case('%s.bgyml' % name, data, lines)


def raw_case(name, data, expected):
    """A case whose bytes and expected output are hand-written; expected is the output after the file line."""
    return Case('%s.bgyml' % name, data, ['file %s.bgyml' % name] + expected + ['probes ok'])


class Case:
    def __init__(self, name, data, expected):
        self.name = name
        self.data = data
        self.expected = expected


def document(version=7, keys=0, strings=0, root=0):
    """A 16-byte header with the given table and root offsets."""
    return bytearray(b'YB' + struct.pack('<H', version) + struct.pack('<III', keys, strings, root))


def build_cases():
    cases = []

    # Every value type, arrays, hashes, the key and string tables, a nested container, and both opaque value types.
    values = ('hash', {
        'Array': ('array', [('int', 1), ('float', 2.5), ('string', 'two'), ('bool', True), ('null',),
                            ('int64', 3), ('uint64', 5), ('double', -0.25), ('hash', {'Inner': ('uint', 5)})]),
        'ArrayData': ('array', [('binary', b'\xde\xad'), ('file', b'\xbe\xef')]),
        'Binary': ('binary', b'\x01\x02\x03\x04'),
        'BoolFalse': ('bool', False),
        'BoolTrue': ('bool', True),
        'Double': ('double', 2.25),
        'EmptyArray': ('array', []),
        'EmptyHash': ('hash', {}),
        'File': ('file', b'\x10\x20'),
        'Float': ('float', 0.125),
        'Int': ('int', -123456),
        'Int64': ('int64', -1234567890123),
        'Null': ('null',),
        'String': ('string', 'hello, world'),
        'UInt': ('uint', 4294967295),
        'UInt64': ('uint64', 1234567890123),
    })
    cases.append(model_case('values', values))

    # A document whose root is an array, and the versions v4..v7 byml_open must accept.
    root_array = ('array', [('int', 42), ('string', 's'), ('hash', {'K': ('float', -1.5)}),
                            ('array', [('bool', False)])])
    cases.append(model_case('rootarray', root_array))
    for version in (4, 5, 6, 7):
        cases.append(model_case('version%d' % version, root_array, version))

    # The hash tables keyed by 32-bit hashes instead of names: counts and value words are readable, key names are not.
    # The value words are chosen outside this document so the reader reports them as values of unknown type.
    cases.append(model_case('hashtables', ('hash', {
        'Int': ('int', 5),
        'T20': ('hashed20', [(0x11111111, 0x5A5A5A0B), (0x22222222, 0x5A5A5A0C)]),
        'T21': ('hashed21', [(0x33333333, 0x5A5A5A0D), (0x44444444, 0x5A5A5A0E)]),
    })))

    # A string value whose index is out of range, and a document with no string table at all.
    cases.append(model_case('stringindex', ('hash', {'A': ('string', 'x'), 'B': ('stringindex', 9)})))
    cases.append(model_case('nostrings', ('hash', {'S': ('stringindex', 0)})))

    # A container entry whose offset points nowhere, and a numeric entry whose payload is missing.
    cases.append(model_case('dangling', ('hash', {'D': ('brokenref', 0xC1, 0xFFFFFFF0)})))
    cases.append(model_case('dangling64', ('hash', {'B': ('brokenref', 0xD4, 0xFFFFFFF0)})))

    # The header alone: valid, empty, no root.
    cases.append(raw_case('empty', bytes(document()), ['open 1', 'root none']))

    # Every way byml_open must refuse a buffer, and the versions it accepts (v2..v7, as resolution.c reads the HeapDef
    # table), which open an empty document.
    cases.append(raw_case('badmagic', b'XX' + b'\0' * 14, ['open 0']))
    cases.append(raw_case('bigendian', b'BY' + struct.pack('<H', 7) + b'\0' * 12, ['open 0']))
    cases.append(raw_case('version2', bytes(document(version=2)), ['open 1', 'root none']))
    cases.append(raw_case('version3', bytes(document(version=3)), ['open 1', 'root none']))
    cases.append(raw_case('version8', bytes(document(version=8)), ['open 0']))
    cases.append(raw_case('emptyfile', b'', ['open 0']))
    cases.append(raw_case('short', b'YB\x07\x00', ['open 0']))
    cases.append(raw_case('badkeys', bytes(document(keys=0x100)), ['open 0']))
    cases.append(raw_case('badstrings', bytes(document(strings=0x100)), ['open 0']))
    cases.append(raw_case('badkeytable', bytes(document(keys=16)) + b'\0' * 16, ['open 0']))
    cases.append(raw_case('badroot', bytes(document(root=0x100)), ['open 0']))
    cases.append(raw_case('rootscalar', bytes(document(root=16)) + b'\xd1\0\0\0' + b'\0' * 12, ['open 0']))
    cases.append(raw_case('garbage', bytes([(i * 7 + 3) % 256 for i in range(64)]), ['open 0']))

    # A root hash whose count does not fit the document: valid to open, and byml_count reports it as empty.
    shortentries = bytes(document(root=20)) + b'\0' * 4 + b'\xc1\x05\x00\x00'
    cases.append(raw_case('shortentries', shortentries, ['open 1', '0xc1 hash 0']))
    cases.append(raw_case('hugecount', bytes(document(root=16)) + b'\xc1\xff\xff\xff',
                          ['open 1', '0xc1 hash 0']))

    # A 0xC1 hash with no key table: byml_key never matches and the entry has no name, so the walker can print the entry
    # but not reach its value.
    nokeys = bytes(document(root=16)) + b'\xc1\x01\x00\x00' + b'\x00\x00\x00\xd1' + struct.pack('<I', 7)
    cases.append(raw_case('nokeys', nokeys, ['open 1', '0xc1 hash 1', '  entry 0 key NULL']))

    # A key index outside a valid key table: the entry has no name either.
    badkeyindex = (bytes(document(keys=16, root=32)) + b'\xc2\x01\x00\x00' +
                   struct.pack('<II', 12, 14) + b'A\0' + b'\0' * 2 +
                   b'\xc1\x01\x00\x00' + b'\x05\x00\x00\xd1' + struct.pack('<I', 8))
    cases.append(raw_case('badkeyindex', badkeyindex, ['open 1', '0xc1 hash 1', '  entry 0 key NULL']))

    # A string table whose string offsets run past the document, and one whose byte range holds no NUL: the entry is
    # reachable by name, the string behind it is not.
    for name, end in (('badstring', 0x100), ('nonul', 13)):
        data = bytearray(document(keys=16, strings=32, root=64))
        data += b'\xc2\x01\x00\x00' + struct.pack('<II', 12, 14) + b'S\0'
        data += b'\0' * (32 - len(data))
        data += b'\xc2\x01\x00\x00' + struct.pack('<II', 12, end) + b's\0'
        data += b'\0' * (64 - len(data))
        data += b'\xc1\x01\x00\x00' + b'\x00\x00\x00\xa0' + b'\0\0\0\0'
        cases.append(raw_case(name, bytes(data),
                              ['open 1', '0xc1 hash 1', '  entry 0 key "S" 0xa0 string NULL']))

    # A hash entry that points back at its own hash: the walker's depth budget has to stop it.
    loop = bytearray(document(keys=32, root=16))
    loop += b'\xc1\x01\x00\x00' + b'\x00\x00\x00\xc1' + struct.pack('<I', 16)
    loop += b'\0' * (32 - len(loop))
    loop += b'\xc2\x01\x00\x00' + struct.pack('<II', 12, 14) + b'L\0'
    lines = ['open 1']
    for depth in range(MAX_DEPTH + 1):
        lines.append('  ' * depth + ('' if depth == 0 else 'entry 0 key "L" ') + '0xc1 hash 1')
    lines.append('  ' * (MAX_DEPTH + 1) + '...')
    cases.append(raw_case('loop', bytes(loop), lines))

    return cases


def compile_reader():
    if not CLANG.exists():
        print('byml_test: clang not found at %s' % CLANG)
        return False
    OUT.parent.mkdir(parents=True, exist_ok=True)
    # Each source is compiled on its own and the objects linked: clang only writes temporary files when it compiles
    # several inputs in one go, and this test then writes nothing outside build/.
    objects = []
    for source in (MAIN, RUNTIME / 'byml.c'):
        obj = OUT.with_suffix('.' + source.stem + '.o')
        cmd = [str(CLANG), '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', *INCLUDES, '-c', str(source),
               '-o', str(obj)]
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode != 0:
            print('byml_test: compile failed for %s' % source)
            print(result.stdout)
            print(result.stderr)
            return False
        objects.append(str(obj))
    result = subprocess.run([str(CLANG), '-o', str(OUT), *objects], capture_output=True, text=True)
    if result.returncode != 0:
        print('byml_test: link failed')
        print(result.stdout)
        print(result.stderr)
        return False
    for obj in objects:
        Path(obj).unlink(missing_ok=True)   # a failed compile keeps its objects for inspection; a good one does not
    return True


def run_cases(tmp, cases):
    paths = []
    expected = []
    for case in cases:
        path = tmp / case.name
        path.write_bytes(case.data)
        paths.append(str(path))
        expected += case.expected
    result = subprocess.run([str(OUT), *paths], capture_output=True, text=True)
    if result.stderr:
        sys.stderr.write(result.stderr)
    if result.returncode != 0:
        print('byml_test: the walker exited %d (an accessor broke its promise)' % result.returncode)
        return False
    got = result.stdout.splitlines()
    if got == expected:
        return True
    print('byml_test: the walker output differs (%d expected lines, %d printed)' % (len(expected), len(got)))
    for i in range(max(len(expected), len(got))):
        got_line = got[i] if i < len(got) else '<no line>'
        want = expected[i] if i < len(expected) else '<no line>'
        if got_line != want:
            print('  first difference at line %d of the output:' % (i + 1))
            print('    expected: %r' % want)
            print('    printed:  %r' % got_line)
            break
    return False


def run_truncations(tmp):
    """Every prefix of a valid document: a truncated file may fail to open or open partly, but must never crash.
    Returns the number of prefixes checked, or None when one of them broke an accessor."""
    model = ('array', [('int', 1), ('float', 2.5), ('string', 'truncation'),
                       ('hash', {'K': ('uint', 7), 'Arr': ('array', [('bool', True), ('int64', -5)])})])
    data = Writer().build(model)
    paths = []
    for length in range(len(data) + 1):
        path = tmp / ('truncated_%04d.bgyml' % length)
        path.write_bytes(data[:length])
        paths.append(str(path))
    batch = 200
    for start in range(0, len(paths), batch):
        group = paths[start:start + batch]
        result = subprocess.run([str(OUT), *group], capture_output=True, text=True)
        if result.returncode != 0:
            print('byml_test: a truncated document (%d bytes) broke an accessor' % (start + 1))
            if result.stderr:
                sys.stderr.write(result.stderr)
            return None
        if result.stdout.count('file ') != len(group):
            print('byml_test: the walker stopped early on the truncated documents at %d' % start)
            return None
    return len(paths)


def main():
    if not CLANG.exists():
        print('byml_test: clang not found at %s' % CLANG)
        return 1
    if not compile_reader():
        return 1
    tmp = Path(tempfile.mkdtemp(prefix='byml_test_', dir=str(OUT.parent)))
    cases = []
    truncations = 0
    ok = False
    try:
        cases = build_cases()
        if run_cases(tmp, cases):
            truncations = run_truncations(tmp) or 0
            ok = bool(truncations)
    finally:
        if ok:
            shutil.rmtree(tmp, ignore_errors=True)
        else:
            print('byml_test: the documents are kept in %s' % tmp)
    if not ok:
        return 1
    print('byml_test: %d documents and %d truncated prefixes pass' % (len(cases), truncations))
    return 0


if __name__ == '__main__':
    sys.exit(main())