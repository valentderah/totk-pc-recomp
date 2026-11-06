"""Map local input-related NSO imports, address references and direct caller graphs.

All addresses in the report are module offsets, not runtime guest addresses. Reports and disassembly are game-derived
and must remain under build/. An address-materialization match is evidence of a reference, not proof of its purpose.
"""
import argparse
import bisect
import hashlib
import json
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'recomp'))
sys.path.insert(0, str(ROOT / 'tools'))


def signed(value, bits):
    return (value ^ (1 << (bits - 1))) - (1 << (bits - 1))


def direct_branches(words, base=0):
    """Return instruction addresses, destinations and BL flags for B/BL; includes tail branches."""
    sources = np.flatnonzero((words & 0x7c000000) == 0x14000000)
    immediate = words[sources].astype(np.int64) & 0x3ffffff
    immediate = np.where(immediate & (1 << 25), immediate - (1 << 26), immediate)
    return sources * 4 + base, (sources + immediate) * 4 + base, (words[sources] >> 31).astype(bool)


def materializations(words, base=0, distance=4):
    """ADRs and bounded ADRP+ADD/LDR uses as (instruction, target, form).

    Follow the page register for at most four instructions, stopping at register writes or control flow.
    The LDR form reports the address of the memory slot, not the value loaded from it.
    """
    from capstone import CS_ARCH_ARM64, CS_MODE_ARM, CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT, Cs
    disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
    disassembler.detail = True
    decoded = {}
    register_numbers = {name: i for i in range(31) for name in (f'x{i}', f'w{i}')}
    register_numbers.update(fp=29, lr=30)

    def instruction(index):
        if index not in decoded:
            word = int(words[index])
            insn = next(disassembler.disasm(word.to_bytes(4, 'little'), base + index * 4), None)
            if insn is None:
                decoded[index] = None
            else:
                _, writes = insn.regs_access()
                mask = 0
                for register in writes:
                    number = register_numbers.get(insn.reg_name(register))
                    if number is not None:
                        mask |= 1 << number
                control = any(insn.group(g) for g in (CS_GRP_CALL, CS_GRP_JUMP, CS_GRP_RET, CS_GRP_INT))
                decoded[index] = control, mask
        return decoded[index]

    refs = []
    for index in np.flatnonzero((words & 0x9f000000) == 0x10000000):
        index = int(index)
        word = int(words[index])
        immediate = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
        address = base + index * 4
        refs.append((address, address + signed(immediate, 21), 'adr'))
    for index in np.flatnonzero((words & 0x9f000000) == 0x90000000):
        index = int(index)
        if index + 1 == len(words):
            continue
        word = int(words[index])
        reg = word & 31
        if reg == 31:
            continue
        immediate = ((word >> 29) & 3) | (((word >> 5) & 0x7ffff) << 2)
        address = base + index * 4
        page = (address & ~4095) + signed(immediate, 21) * 4096
        for following_index in range(index + 1, min(len(words), index + distance + 1)):
            following = int(words[following_index])
            if (following >> 5) & 31 == reg:
                if following & 0xff800000 == 0x91000000:
                    value = ((following >> 10) & 4095) << (12 if following & (1 << 22) else 0)
                    refs.append((address, page + value, 'adrp-add'))
                elif following & 0xffc00000 == 0xf9400000:
                    refs.append((address, page + ((following >> 10) & 4095) * 8, 'adrp-ldr64-slot'))
            info = instruction(following_index)
            if info is None or info[0] or info[1] & (1 << reg):
                break
    return refs


class Graph:
    def __init__(self, module):
        import gen
        self.module = module
        self.starts, _ = gen.discover(module)
        self.starts = (module.lo + self.starts * 4).tolist()
        self.ends = dict(zip(self.starts, self.starts[1:] + [module.hi]))
        sources, targets, calls = direct_branches(module.words, module.lo)
        self.callers = {}
        for source, target, call in zip(sources.tolist(), targets.tolist(), calls.tolist()):
            if not module.in_text(target):
                continue
            parent = self.containing(source)
            if parent != self.containing(target):
                self.callers.setdefault(target, []).append({'site': source, 'function': parent,
                                                           'kind': 'bl' if call else 'b'})
        self.refs = {}
        for source, target, form in materializations(module.words, module.lo):
            self.refs.setdefault(target, []).append({'site': source, 'function': self.containing(source), 'form': form})
        self.data_refs = {}
        for offset, kind, symbol, addend in module.nso.relocs:
            if kind == 1027:  # R_AARCH64_RELATIVE: addend is a module-relative pointer before relocation.
                self.data_refs.setdefault(addend, []).append(offset)

    def containing(self, address):
        k = bisect.bisect_right(self.starts, address) - 1
        return self.starts[k] if k >= 0 and address < self.module.hi else None

    def function(self, address):
        from capstone import CS_ARCH_ARM64, CS_MODE_ARM, Cs
        entry = self.containing(address)
        if entry is None:
            return None
        end = self.ends[entry]
        disassembler = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
        code = [{'offset': i.address, 'mnemonic': i.mnemonic, 'operands': i.op_str}
                for i in disassembler.disasm(self.module.image[entry:end], entry)]
        return {'entry': entry, 'end': end, 'callers': self.callers.get(entry, []), 'code': code}

    def references(self, target, depth=2):
        """Follow relocatable data pointers back to code, bounding cycles and recursion."""
        found = []
        pending, seen = [(target, [])], set()
        while pending:
            target, path = pending.pop()
            if target in seen:
                continue
            seen.add(target)
            for ref in self.refs.get(target, []):
                found.append(dict(ref, through=path))
            if len(path) < depth:
                for pointer in self.data_refs.get(target, []):
                    pending.append((pointer, path + [pointer]))
        return found


def main():
    from check_content import GENERATED_MARKER
    from module import Module
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--main', type=Path, required=True, help='extracted main NSO')
    ap.add_argument('--symbol', action='append', default=[], help='import name substring')
    ap.add_argument('--string', action='append', default=[], help='exact NUL-terminated UTF-8 string')
    ap.add_argument('--address', type=lambda s: int(s, 0), action='append', default=[], help='module-relative offset')
    ap.add_argument('--controller-probe', type=lambda s: int(s, 0),
                    help='verified controller-by-name getter entry; emit a fingerprinted observation plan')
    ap.add_argument('--output-publisher', type=lambda s: int(s, 0),
                    help='verified publisher entry; observe uint64 outputs through pointers in its x0 object')
    ap.add_argument('--output-field', type=lambda s: int(s, 0), action='append', default=[],
                    help='verified pointer field offset for --output-publisher; repeat for multiple fields')
    ap.add_argument('--position-path', type=lambda s: int(s, 0), action='append', default=[],
                    help='verified pointer-chain offset from publisher x0 to actor; repeat in dereference order')
    ap.add_argument('--position-offset', type=lambda s: int(s, 0), help='verified float[3] position offset in actor')
    ap.add_argument('--depth', type=int, default=1, help='caller levels, maximum 4')
    ap.add_argument('--state-callback', type=lambda s: int(s, 0), help='verified callback entry; observe a field on original x0 after return')
    ap.add_argument('--state-field', type=lambda s: int(s, 0), help='verified scalar field offset for --state-callback')
    ap.add_argument('--state-size', type=int, choices=[1, 2, 4, 8], default=1, help='state field byte width')
    ap.add_argument('--scalar-probe', type=lambda s: int(s, 0), action='append', default=[],
                    help='verified function entry; observe original x0 argument, caller and actual x0 return')
    ap.add_argument('--out', type=Path, required=True)
    a = ap.parse_args()
    out = a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under build/')
    if not 0 <= a.depth <= 4:
        ap.error('--depth must be between 0 and 4')
    if (a.state_callback is None) != (a.state_field is None):
        ap.error('--state-callback and --state-field are required together')
    if a.state_field is not None and not 0 <= a.state_field <= 65536:
        ap.error('--state-field must be in [0,65536]')
    if bool(a.output_field) != (a.output_publisher is not None):
        ap.error('--output-publisher requires --output-field and vice versa')
    if len(a.output_field) > 8 or any(f < 0 or f > 4096 or f % 8 for f in a.output_field):
        ap.error('output fields must be up to eight aligned offsets in [0,4096]')
    if bool(a.position_path) != (a.position_offset is not None) or (a.position_path and a.output_publisher is None):
        ap.error('position observation requires --output-publisher, --position-path and --position-offset together')
    if len(a.position_path) > 8 or any(p < 0 or p > 65536 or p % 8 for p in a.position_path):
        ap.error('position path must be up to eight aligned offsets in [0,65536]')
    if a.position_offset is not None and (a.position_offset < 0 or a.position_offset > 65536 or a.position_offset % 4):
        ap.error('position offset must be aligned and in [0,65536]')
    module = Module('main', a.main, 0)
    graph = Graph(module)
    roots, functions = [], set()
    for term in a.symbol:
        for offset, kind, symbol, _ in module.nso.relocs:
            name = module.nso.syms[symbol][0] if symbol < len(module.nso.syms) else ''
            if term not in name:
                continue
            refs = graph.references(offset)
            roots.append({'symbol': name, 'slot': offset, 'references': refs})
            functions.update(ref['function'] for ref in refs)
    for term in a.string:
        needle = term.encode() + b'\0'
        position = 0
        while (position := module.image.find(needle, position)) >= 0:
            refs = graph.references(position)
            roots.append({'string': term, 'offset': position, 'references': refs})
            functions.update(ref['function'] for ref in refs)
            position += len(needle)
    functions.update(graph.containing(address) for address in a.address)
    for entry in a.scalar_probe:
        if entry not in graph.ends:
            ap.error('--scalar-probe must be an exact discovered function entry')
        functions.add(entry)
    if a.controller_probe is not None:
        if a.controller_probe not in graph.ends:
            ap.error('--controller-probe must be an exact discovered function entry')
        functions.add(a.controller_probe)
    if a.output_publisher is not None:
        if a.output_publisher not in graph.ends:
            ap.error('--output-publisher must be an exact discovered function entry')
        functions.add(a.output_publisher)
    if a.state_callback is not None:
        if a.state_callback not in graph.ends:
            ap.error('--state-callback must be an exact discovered function entry')
        functions.add(a.state_callback)
    functions.discard(None)
    frontier = set(functions)
    for _ in range(a.depth):
        frontier = {r['function'] for f in frontier for r in graph.callers.get(f, [])} - functions
        frontier.discard(None)
        functions.update(frontier)
    report = {'marker': GENERATED_MARKER, 'build_id': module.nso.build_id, 'address_space': 'main module offsets',
              'roots': roots, 'functions': [graph.function(f) for f in sorted(functions)]}
    probes = []
    for entry, kind in ([(a.controller_probe, 'controller_by_name'), (a.output_publisher, 'pointer_u64_outputs'),
                         (a.state_callback, 'object_state')] +
                        [(entry, 'scalar_result') for entry in sorted(set(a.scalar_probe))]):
        if entry is not None:
            probe = {'offset': entry, 'kind': kind, 'end': graph.ends[entry],
                     'sha256': hashlib.sha256(module.image[entry:graph.ends[entry]]).hexdigest()}
            if kind == 'pointer_u64_outputs':
                probe['fields'] = a.output_field
                if a.position_path:
                    probe['position'] = {'path': a.position_path, 'offset': a.position_offset}
            elif kind == 'object_state':
                probe.update(field=a.state_field, size=a.state_size)
            probes.append(probe)
    if probes:
        report['probe_plan'] = {'build_id': module.nso.build_id, 'sites': probes}
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_text(json.dumps(report, indent=2) + '\n')
    print(f'{len(roots)} roots, {len(functions)} functions: {out}')


if __name__ == '__main__':
    main()
