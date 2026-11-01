"""tools/cheats.py without game files: how cheat files are found and named, and the symbolic run that tells where a
cheat writes. A write through a register lands where the same write as opcode 0 does, the registers of a setup cheat
reach the next file, and a value only a run can tell stays unknown.

Usage: tests/cheats_check.py
"""
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import cheats  # noqa: E402

BID = '0123456789ABCDEF'


class Main:
    """A main of 0x1000 bytes of .text, its padding up to 0x2000, and one pointer to its own start at 0x3000."""
    text = (0, 0x1000)
    text_end, code_end, ro_end = 0x1000, 0x2000, 0x3000

    def load(self, off, width):
        if off == 0x3000 and width == 8:
            return ('m', 0)
        return None


def run(text, regs=None):
    """The (address, width, value) of the writes of the first cheat of text."""
    c = cheats.parse(text, 'mod')[0]
    r = cheats.Run(Main(), cheats.ops(c.words), regs or [('k', 0)] * 16)
    return [w[:3] for w in r.writes], r.regs


def check(ok, what):
    if not ok:
        raise SystemExit(f'cheats_check: {what}')


def main():
    # the setting's name carries the mod, a repeated name gets its number
    a, b = cheats.parse('[x]\n04000000 00000010 D503201F\n[x]\n', 'one')
    check(a.key == cheats.fnv(b'one/x') and b.key == cheats.fnv(b'one/x#2'), 'name hash')
    check(a.words == [0x04000000, 0x10, 0xD503201F], 'words')
    check(cheats.parse('[x]\n', 'two')[0].key != a.key, 'two mods keep their own switch')

    # opcode lengths, the extended ones included
    check([len(w) for _, w in cheats.ops([0xC0400400, 1, 0xC1000000, 0xFF000000])] == [2, 1, 1], 'opcode lengths')
    check(cheats.vm_ok((0xC0, [0xC0400400, 1])) is None, 'the VM runs C0')
    check(cheats.vm_ok((0xB, [0xB0000000])) is not None, 'the VM does not run B')

    # opcode 0 and the same write through registers
    direct, _ = run('[x]\n04000000 00000010 D503201F\n')
    check(direct == [(('m', 0x10), 4, ('k', 0xD503201F))], f'opcode 0: {direct}')
    regs, left = run('[x]\n580F0000 00003000\n400B0000 00000000 00000010\n640F01B0 00000000 D503201F\n')
    check(regs == [(('m', 0x10), 4, ('k', 0xD503201F))], f'through registers: {regs}')
    check(left[15] == ('m', 0) and left[11] == ('k', 0x10), 'the registers it leaves')

    # a cheat that only uses a setup cheat's register
    later, _ = run('[x]\n400B0000 00000000 00000010\n640F01B0 00000000 D503201F\n', left)
    check(later == regs, f'a setup register: {later}')

    # without the setup, or loaded from memory the game writes, the address is not in main
    alone, _ = run('[x]\n400B0000 00000000 00000010\n640F01B0 00000000 D503201F\n')
    check(alone[0][0] == ('k', 0x10), f'no setup: {alone}')
    unknown, _ = run('[x]\n580F0000 00004000\n640F0100 00000000 D503201F\n')
    check(unknown[0][0] is None, f'read from memory: {unknown}')

    # a file in each place a mod may hold it, the name in any case
    with tempfile.TemporaryDirectory() as tmp:
        mods = Path(tmp)
        for mod, where in (('b', 'cheats'), ('a', f'atmosphere/contents/{cheats.TITLE}/cheats')):
            (mods / mod / where).mkdir(parents=True)
            (mods / mod / where / f'{BID.lower()}.txt').write_text('[x]\n')
        (mods / 'c').mkdir()
        check([m for m, _ in cheats.cheat_files(mods, BID)] == ['a', 'b'], 'mods by name')
        (mods / 'd.txt').write_text('')
        check(len(cheats.cheat_files(mods, BID)) == 2, 'files in the mods folder are no mods')
        check(cheats.cheat_files(mods / 'none', BID) == [], 'no mods folder')

    # GBK files read as runtime/cheats.c reads them
    with tempfile.TemporaryDirectory() as tmp:
        f = Path(tmp) / 'x.txt'
        f.write_bytes('[无敌]\n'.encode('cp936'))
        check(cheats.parse(cheats.read_cheats(f), 'm')[0].name == '无敌', 'GBK')
    print('cheats_check: ok')


if __name__ == '__main__':
    main()
