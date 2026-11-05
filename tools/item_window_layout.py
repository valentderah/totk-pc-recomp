"""The inventory's item window with ten rows, for armour sets (runtime/armor_sets.c, TOTK_ARMOR_SETS).

blyt/PaPouchActionWindow_00.bflyt has four button rows (Pa_Button_00..03) and its Pattern animation picks 2, 3 or 4 of
them (frames 0, 1, 2). Armour sets fills the window with nine sets and Cancel, so this clones Pa_Button_03 into
Pa_Button_04..09 (87 apart, as the others; added to the group G_Pattern_00) and adds Pattern frame 8 = ten rows, the
window 86 taller per row. The game counts the Pa_Button_NN panes when it sets the window up (main+0x96e518, up to the
first missing one), so no code sets the row count, and frames 0..2 are unchanged: with the setting off the window is the
game's own. The archive grows by about 24 KB (make_romfs_patches.py raises its resource size table entry).

apply(archive, members, replace): the archive with both files edited; members is make_romfs_patches.sarc_members and
replace its sarc_replace.
"""
import struct

LAYOUT = 'blyt/PaPouchActionWindow_00.bflyt'
ANIM = 'anim/PaPouchActionWindow_00_Pattern.bflan'
ROWS, FRAME = 10, 8                # the last Pattern frame is ROWS - 2
PITCH, GROW = 87.0, 86.0           # button spacing, window growth per row
EXTRA = ROWS - 4                   # rows past the 4 of frame 2
# The window hangs from the item: its top is 108 above it, its bottom 808 below at ten rows. The game keeps N_Range_00
# inside N_DisplayArea_00 (936 tall; main+0x21134a0 moves the window up when it would leave the screen), so at ten rows
# N_Range_00 covers the window downwards: y -409, height 818.
RANGE_BOTTOM = 818.0


def sections(d):
    o, n = struct.unpack_from('<H', d, 6)[0], struct.unpack_from('<H', d, 0x10)[0]
    out = []
    for _ in range(n):
        size = struct.unpack_from('<I', d, o + 4)[0]
        out.append(bytes(d[o:o + size]))
        o += size
    assert o == len(d), (o, len(d))
    return d[:struct.unpack_from('<H', d, 6)[0]], out


def join(head, secs):
    body = b''.join(secs)
    h = bytearray(head)
    struct.pack_into('<I', h, 0xc, len(h) + len(body))
    struct.pack_into('<H', h, 0x10, len(secs))
    return bytes(h) + body


def edit_layout(d):
    head, secs = sections(d)
    i = next(k for k, s in enumerate(secs) if s[:4] == b'prt1' and s[0xc:0x24].split(b'\0')[0] == b'Pa_Button_03')
    base, y = secs[i], struct.unpack_from('<f', secs[i], 0x30)[0]
    clones = []
    for n in range(4, ROWS):
        c = bytearray(base.replace(b'Pa_Button_03', b'Pa_Button_%02d' % n).replace(b'T_Button_03', b'T_Button_%02d' % n))
        assert len(c) == len(base)
        struct.pack_into('<f', c, 0x30, y - PITCH * (n - 3))
        clones.append(bytes(c))
    secs[i:i] = clones[::-1]
    g = next(k for k, s in enumerate(secs) if s[:4] == b'grp1' and s[8:41].split(b'\0')[0] == b'G_Pattern_00')
    s = bytearray(secs[g])
    count = struct.unpack_from('<H', s, 8 + 0x22)[0]
    assert len(s) == 8 + 0x24 + 24 * count
    for n in range(4, ROWS):
        s += (b'Pa_Button_%02d' % n).ljust(24, b'\0')
    struct.pack_into('<I', s, 4, len(s))
    struct.pack_into('<H', s, 8 + 0x22, count + EXTRA)
    secs[g] = bytes(s)
    return join(head, secs)


# ---- BFLAN: read the Pattern animation into plain data and write it back
def read_pai(p):
    frames, loop, pad, ntex, nent, ent_off = struct.unpack_from('<HBBHHI', p, 8)
    assert ntex == 0
    entries = []
    for i in range(nent):
        eo = struct.unpack_from('<I', p, ent_off + i * 4)[0]
        name = p[eo:eo + 28].split(b'\0')[0]
        ntags, kind = p[eo + 28], p[eo + 29]
        assert p[eo + 30:eo + 32] == b'\0\0'
        tags = []
        for t in range(ntags):
            to = eo + struct.unpack_from('<I', p, eo + 32 + t * 4)[0]
            mag, ngrp = p[to:to + 4], p[to + 4]
            groups = []
            for g in range(ngrp):
                go = to + struct.unpack_from('<I', p, to + 8 + g * 4)[0]
                tgt, idx, curve, pad2, nkeys, pad3, koff = struct.unpack_from('<BBBBHHI', p, go)
                size = 12 if curve == 2 else 8
                keys = [p[go + koff + k * size:go + koff + (k + 1) * size] for k in range(nkeys)]
                groups.append([tgt, idx, curve, pad2, pad3, keys])
            tags.append([mag, groups])
        entries.append([name, kind, tags])
    return frames, loop, pad, ent_off, entries


def write_pai(frames, loop, pad, ent_off, entries):
    table = len(entries) * 4
    body = bytearray()
    offs = []
    for name, kind, tags in entries:
        offs.append(ent_off + table + len(body))
        e = bytearray(name.ljust(28, b'\0') + bytes([len(tags), kind, 0, 0]) + bytes(4 * len(tags)))
        for t, (mag, groups) in enumerate(tags):
            struct.pack_into('<I', e, 32 + t * 4, len(e))
            tag = bytearray(mag + bytes([len(groups), 0, 0, 0]) + bytes(4 * len(groups)))
            for gi, (tgt, idx, curve, pad2, pad3, keys) in enumerate(groups):
                struct.pack_into('<I', tag, 8 + gi * 4, len(tag))
                tag += struct.pack('<BBBBHHI', tgt, idx, curve, pad2, len(keys), pad3, 12) + b''.join(keys)
            e += tag
        body += e
    head = struct.pack('<HBBHHI', frames, loop, pad, 0, len(entries), ent_off)
    out = bytearray(b'pai1' + bytes(4) + head)
    out += bytes(ent_off - len(out))
    out += b''.join(struct.pack('<I', o) for o in offs) + body
    while len(out) % 4:
        out += b'\0'
    struct.pack_into('<I', out, 4, len(out))
    return bytes(out)


def key(frame, value):
    return struct.pack('<fff', frame, value, 0.0)


def edit_anim(d):
    head, secs = sections(d)
    k = next(i for i, s in enumerate(secs) if s[:4] == b'pai1')
    frames, loop, pad, ent_off, entries = read_pai(secs[k])
    assert write_pai(frames, loop, pad, ent_off, entries) == secs[k], 'bflan round trip'
    done = frames == FRAME
    assert done or frames == 2
    for name, kind, tags in entries:
        for mag, groups in tags:
            for g in groups:
                tgt, idx, curve, _, _, keys = g
                if name == b'N_Range_00':
                    if done:
                        keys.pop()
                    keys.append(key(float(FRAME), {1: -RANGE_BOTTOM / 2, 9: RANGE_BOTTOM}[idx]))
                    continue
                if done:
                    continue
                if curve != 2 or name == b'N_DisplayArea_00' or (name == b'N_WindowArea_00' and idx in (0, 8)):
                    continue
                last = struct.unpack_from('<f', keys[-1], 4)[0]
                step = {1: 43.0, 9: GROW}[idx]
                if name == b'N_Pattern_00':
                    continue                                   # stays 0 from frame 1 on
                if idx == 1:
                    step = -step
                keys.append(key(float(FRAME), last + step * EXTRA))
    for n in range(4, ROWS) if not done else ():
        vis = [struct.pack('<fHH', 0.0, 0, 0), struct.pack('<fHH', float(FRAME), 1, 0)]
        entries.append([b'Pa_Button_%02d' % n, 0, [[b'FLVI', [[0, 0, 1, 0, 0, vis]]]]])
    secs[k] = write_pai(FRAME, loop, pad, ent_off, entries)
    return join(head, secs)


def apply(archive, members, replace):
    m = members(archive)
    lay = edit_layout(archive[slice(*m[LAYOUT])])
    ani = edit_anim(archive[slice(*m[ANIM])])
    out = replace(archive, {LAYOUT: lay, ANIM: ani})
    n = members(out)
    assert out[slice(*n[LAYOUT])] == lay and out[slice(*n[ANIM])] == ani
    return out
