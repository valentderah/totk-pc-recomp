"""Validate and apply a private guard-key split to a copied module before C generation.

Guard split design adapted from Weeping of the Empire at fe2345e82a43156183242abfdcc0eb1f5526eba6.
Copyright (c) 2026 daniel-madrid-07; MIT License, see tools/licenses/weeping-player-state.txt.
Game-specific sites and fingerprints are supplied by an ignored local plan, never embedded in this tool.
"""
from bisect import bisect_right
from collections import OrderedDict
import copy
import hashlib
import struct


def apply(module, config, starts):
    """Return an independent module view and affected function entries; original bytes/cache stay unchanged."""
    bit, original_bit = config.get('bit'), config.get('from_bit')
    sites, evidence = config.get('sites'), config.get('evidence')
    if (type(bit) is not int or type(original_bit) is not int or not 0<=bit<64 or not 0<=original_bit<64 or
            bit==original_bit or not isinstance(sites,list) or not 1<=len(sites)<=32):
        raise ValueError('Guard split requires a private uint64 bit and bounded sites')
    if not isinstance(evidence, list) or not evidence:
        raise ValueError('Guard split requires fingerprinted consumer functions')
    start_list = sorted(starts)
    ranges = {}
    for entry in evidence:
        off, end = entry['offset'], entry['end']
        if type(off) is not int or type(end) is not int or off < 0 or end <= off or end > len(module.image):
            raise ValueError('Invalid guard consumer range')
        absolute = module.base + off
        i = bisect_right(start_list, absolute) - 1
        expected_end = start_list[i + 1] if i + 1 < len(start_list) else module.hi
        if i < 0 or start_list[i] != absolute or module.base + end != expected_end:
            raise ValueError('Guard consumer boundary mismatch')
        if hashlib.sha256(module.image[off:end]).hexdigest() != entry['sha256']:
            raise ValueError('Guard consumer fingerprint mismatch')
        ranges[absolute] = (off, end)
    image = bytearray(module.image)
    affected, seen = set(), set()
    for site in sites:
        off, before, after, kind = site['offset'], site['before'], site['after'], site['kind']
        if (type(off) is not int or off % 4 or off in seen or not module.in_text(module.base + off) or
                type(before) is not int or type(after) is not int or not 0 <= before < 1 << 32 or not 0 <= after < 1 << 32):
            raise ValueError('Invalid guard patch site')
        seen.add(off)
        if struct.unpack_from('<I', module.image, off)[0] != before:
            raise ValueError('Guard patch original word mismatch')
        if kind == 'query':
            if before != 0x52800001 | (original_bit << 5) or after != 0x52800001 | (bit << 5):
                raise ValueError('Expected a virtual-key argument MOV W1 replacement')
        elif kind == 'byte':
            if before != 0x39400108 | ((original_bit // 8) << 10) or after != 0x39400108 | ((bit // 8) << 10):
                raise ValueError('Expected the guard byte read')
        elif kind == 'test':
            # Keep the register, target and branch sense; change only the tested bit within the selected byte.
            if (before & 0x7f00001f) != 0x37000008 or (before >> 19) & 31 != original_bit % 8 or after != (before & ~(31 << 19)) | ((bit % 8) << 19):
                raise ValueError('Expected the guard byte bit-test')
        else:
            raise ValueError('Unknown guard patch kind')
        owner = next((entry for entry, (lo, hi) in ranges.items() if lo <= off < hi), None)
        if owner is None:
            raise ValueError('Guard patch has no fingerprinted consumer')
        affected.add(owner)
        struct.pack_into('<I', image, off, after)
    if {s['kind'] for s in sites} != {'query', 'byte', 'test'}:
        raise ValueError('Guard split must include its queries and inline shield-draw read')
    patched = copy.copy(module)
    patched.image = bytes(image)
    # numpy is already a generator dependency. Create a new view over new immutable storage, with no shared lift cache.
    import numpy as np
    patched.words = np.frombuffer(patched.image, dtype='<u4', count=len(module.words), offset=module.lo-module.base)
    patched._cache = OrderedDict()
    return patched, affected
