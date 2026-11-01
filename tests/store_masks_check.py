"""Conservative SPIR-V store-lane inference and mixed CPU/GPU writeback regressions.

Build store_masks_probe.c with gfx_store_mask.c and vk_buffer_ranges.c, then pass that executable here.
Fixtures contain synthetic SPIR-V only and are generated in a temporary directory.
"""
from pathlib import Path
import json
import random
import struct
import subprocess
import sys
import tempfile


class Module:
    def __init__(self, stride=4, offset=0, descriptor_set=0):
        self.next_id = 1
        self.decls = []
        self.imports = []
        self.annotations = []
        self.names = []
        self.body = []
        self.locals = []
        self.void = self.decl(19)
        self.function_type = self.decl(33, self.void)
        self.uint = self.decl(21, 32, 0)
        self.float = self.decl(22, 32)
        self.constants = {}
        self.zero = self.constant(0)
        array = self.decl(29, self.uint)
        block = self.decl(30, array)
        pointer = self.decl(32, 2, block)
        self.buffer = self.result(self.decls, 59, pointer, 2)
        self.pointer = self.decl(32, 2, self.uint)
        self.function_pointer = self.decl(32, 7, self.uint)
        self.annotations += [self.op(71, array, 6, stride), self.op(71, block, 3), self.op(72, block, 0, 35, offset),
                             self.op(71, self.buffer, 34, descriptor_set), self.op(71, self.buffer, 33, 1)]
        self.entry = self.id()
        self.label = self.id()

    def id(self):
        result = self.next_id
        self.next_id += 1
        return result

    @staticmethod
    def op(opcode, *args):
        return [(len(args) + 1) << 16 | opcode, *args]

    def decl(self, opcode, *args):
        result = self.id()
        self.decls.append(self.op(opcode, result, *args))
        return result

    def result(self, dest, opcode, type_id, *args):
        result = self.id()
        dest.append(self.op(opcode, type_id, result, *args))
        return result

    def constant(self, value):
        value &= 0xffffffff
        if value not in self.constants:
            self.constants[value] = self.result(self.decls, 43, self.uint, value)
        return self.constants[value]

    def undef(self):
        return self.result(self.decls, 45, self.uint)

    def calc(self, opcode, *args):
        return self.result(self.body, opcode, self.uint, *args)

    def variable(self, *sources):
        var = self.result(self.locals, 59, self.function_pointer, 7)
        for source in sources:
            self.body.append(self.op(62, var, source))
        return self.result(self.body, 61, self.uint, var)

    def store(self, index, copied=False):
        ptr = self.result(self.body, 65, self.pointer, self.buffer, self.zero, index)
        if copied:
            ptr = self.result(self.body, 83, self.pointer, ptr)
        self.body.append(self.op(62, ptr, self.zero))

    def c0_address(self, row=40, component=0):
        vector = self.decl(23, self.float, 4)
        count = self.constant(4096)
        array = self.decl(28, vector, count)
        block = self.decl(30, array)
        ptr = self.decl(32, 2, block)
        c0 = self.result(self.decls, 59, ptr, 2)
        float_ptr = self.decl(32, 2, self.float)
        name = b'cp_c0\0'
        name += b'\0' * (-len(name) % 4)
        self.names.append(self.op(5, c0, *struct.unpack('<%dI' % (len(name)//4), name)))
        self.annotations += [self.op(71, array, 6, 16), self.op(71, block, 2), self.op(72, block, 0, 35, 0),
                             self.op(71, c0, 34, 0), self.op(71, c0, 33, 7)]
        address = self.result(self.body, 65, float_ptr, c0, self.zero, self.constant(row), self.constant(component))
        value = self.result(self.body, 61, self.float, address)
        return self.calc(124, value)

    def words(self):
        instructions = [self.op(17, 1)] + self.imports + [self.op(14, 0, 1),
                        self.op(15, 5, self.entry, 0x6e69616d, 0), self.op(16, self.entry, 17, 1, 1, 1)]
        instructions += self.names + self.annotations + self.decls
        instructions += [self.op(54, self.void, self.entry, 0, self.function_type), self.op(248, self.label)]
        instructions += self.locals + self.body + [self.op(253), self.op(56)]
        return [0x07230203, 0x00010300, 0, self.next_id, 0] + [x for inst in instructions for x in inst]


def check(exe, folder, label, module, expected=None):
    words = module.words()
    path = folder / (label + '.spv')
    path.write_bytes(struct.pack('<%dI' % len(words), *words))
    output = subprocess.check_output([str(exe), str(path)], text=True)
    actual = dict(line.split(':') for line in output.splitlines())
    result = int(actual['1'])
    if expected is not None:
        assert result == expected, (label, expected, result)
    return result


def run(exe):
    subprocess.run([str(exe)], check=True)
    cases = 0
    with tempfile.TemporaryDirectory(prefix='store-masks-') as temp:
        folder = Path(temp)
        for lane in range(4):
            m = Module()
            index = m.calc(128, m.calc(196, m.undef(), m.constant(2)), m.constant(lane))
            m.store(index)
            check(exe, folder, 'lane%d' % lane, m, 1 << lane)
            cases += 1
        m = Module()
        base = m.calc(196, m.undef(), m.constant(2))
        for lane in [0, 1, 2]:
            m.store(m.calc(128, base, m.constant(lane)))
        check(exe, folder, 'xyz', m, 7)
        cases += 1
        for label, stride, offset, copied in [('unknown', 4, 0, False), ('stride', 8, 0, False),
                                               ('member', 4, 16, False), ('alias', 4, 0, True)]:
            m = Module(stride, offset)
            m.store(m.undef() if label == 'unknown' else m.constant(0), copied=copied)
            check(exe, folder, label, m, 0)
            cases += 1
        m = Module()
        index = m.variable(m.constant(0), m.constant(4))
        m.store(index)
        check(exe, folder, 'variable_join', m, 1)
        cases += 1
        m = Module()
        index = m.variable(m.constant(0), m.constant(1))
        m.store(index)
        check(exe, folder, 'variable_unknown', m, 0)
        cases += 1
        m = Module()
        var = m.result(m.locals, 59, m.function_pointer, 7, m.constant(1))
        m.body.append(m.op(62, var, m.zero))
        m.store(m.result(m.body, 61, m.uint, var))
        check(exe, folder, 'variable_initializer', m, 0)
        cases += 1
        m = Module()
        var = m.result(m.locals, 59, m.function_pointer, 7)
        m.body.append(m.op(62, var, m.zero))
        alias = m.result(m.body, 83, m.function_pointer, var)
        m.body.append(m.op(62, alias, m.constant(1)))
        m.store(m.result(m.body, 61, m.uint, var))
        check(exe, folder, 'variable_alias_store', m, 0)
        cases += 1
        for row, component, expected in [(40, 0, 2), (32, 0, 0), (40, 1, 0)]:
            m = Module()
            address = m.c0_address(row, component)
            ptr = m.calc(128, m.calc(196, m.undef(), m.constant(4)), address)
            ptr = m.calc(128, ptr, m.constant(4))
            relative = m.calc(130, ptr, m.calc(199, address, m.constant(-16)))
            m.store(m.calc(194, relative, m.constant(2)))
            check(exe, folder, 'c0_%d_%d' % (row, component), m, expected)
            cases += 1
        m = Module()
        ptr = m.result(m.body, 65, m.pointer, m.buffer, m.zero, m.zero)
        m.body.append(m.op(228, ptr, m.constant(1), m.zero, m.zero))
        m.store(m.zero)
        check(exe, folder, 'atomic', m, 0)
        cases += 1
        m = Module(descriptor_set=1)
        m.store(m.zero)
        check(exe, folder, 'other_descriptor_set', m, 0)
        cases += 1
        m = Module()
        m.store(m.calc(196, m.constant(5), m.zero))
        check(exe, folder, 'shift_zero', m, 2)
        cases += 1
        for pointer_write in [False, True]:
            m = Module()
            import_id = m.id()
            name = b'GLSL.std.450\0'
            name += b'\0' * (-len(name) % 4)
            m.imports.append(m.op(11, import_id, *struct.unpack('<%dI' % (len(name)//4), name)))
            one = m.result(m.decls, 43, m.float, 0x3f800000)
            if pointer_write:
                fp = m.decl(32, 7, m.float)
                dest = m.result(m.locals, 59, fp, 7)
                m.result(m.body, 12, m.float, import_id, 35, one, dest)
            else:
                m.result(m.body, 12, m.float, import_id, 50, one, one, one)
            m.store(m.zero)
            check(exe, folder, 'ext_pointer' if pointer_write else 'ext_pure', m, 0 if pointer_write else 1)
            cases += 1
        # Differential soundness check: inferred masks must contain every concrete store lane sampled.
        rng = random.Random(0x51437)
        narrowed = 0
        for case in range(300):
            m = Module()
            value = m.undef()
            operations = []
            for _ in range(rng.randrange(2, 9)):
                opcode = rng.choice([126, 128, 130, 132, 194, 195, 196, 197, 198, 199, 200])
                constant = rng.randrange(32) if opcode in [194, 195, 196] else rng.choice(
                    [0, 1, 2, 3, 4, 7, 15, 16, 31, 32, 63, 255, 65535, 0xfffffff0, 0xffffffff])
                value = m.calc(opcode, value) if opcode in [126, 200] else m.calc(opcode, value, m.constant(constant))
                operations.append((opcode, constant))
            m.store(value)
            inferred = check(exe, folder, 'differential%d' % case, m) or 15
            narrowed += inferred != 15
            concrete = 0
            for initial in list(range(64)) + [rng.getrandbits(32) for _ in range(64)] + [1 << x for x in range(32)]:
                x = initial
                for op, c in operations:
                    if op == 126: x = -x
                    elif op == 128: x += c
                    elif op == 130: x -= c
                    elif op == 132: x *= c
                    elif op == 194: x >>= c
                    elif op == 195: x = ((x if x < 0x80000000 else x - 0x100000000) >> c)
                    elif op == 196: x <<= c
                    elif op == 197: x |= c
                    elif op == 198: x ^= c
                    elif op == 199: x &= c
                    elif op == 200: x = ~x
                    x &= 0xffffffff
                concrete |= 1 << (x & 3)
            assert inferred & concrete == concrete, (case, operations, inferred, concrete)
    print(json.dumps({'shader_cases': cases, 'differential_cases': 300, 'narrowed': narrowed, 'buffer_ranges': 'passed'}))


if __name__ == '__main__':
    run(Path(sys.argv[1]).resolve())
