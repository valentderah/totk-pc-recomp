"""Harvest shader variants and warmup priorities from played shader-answers.bin into shader_list TSV rows.

runtime/gfx_shader.c records every shader variant met in play into <dir>/shader-answers.bin ("TOTKSANS") and its raw
program bytes into <dir>/shader-programs.bin ("TOTKSPRG"). Neither hashed key nor packed answer JSON contains game code or
keys. This tool maps each recorded code hash back to its shader_census key and emits:

  - V rows for variants that were not already prepared by the base list / DEFAULT (e.g. non-float attributes, non-2D
    textures, storage image formats, or mvprev=1 DLAA/frame-gen vertex shader variants)
  - optional S / C warm rows (--warm) elevating shaders actually seen in play from priority 2 to priority 1

Run:
    .venv-py314\\Scripts\\python tools\\harvest_shader_list.py build\\v110 --warm
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
if str(ROOT / 'tools') not in sys.path:
    sys.path.insert(0, str(ROOT / 'tools'))
import prepare_shaders  # noqa: E402
import shader_census  # noqa: E402
import shader_list  # noqa: E402

ANS_MAGIC = b'TOTKSANS'
PRG_MAGIC = b'TOTKSPRG'
PRP_MAGIC = b'TOTKSPRP'
ANS_REC = prepare_shaders.ANS_REC   # 216
ATTR_NAMES = {v: k for k, v in prepare_shaders.TYPES.items()}
PRG_HEAD = 12 + 32 + prepare_shaders.CONTROL


def read_cache(path, magic, rec_size):
    """Check the cache header against runtime/gfx_shader.c rec_read before interpreting its records."""
    data = Path(path).read_bytes()
    if len(data) < 16 or data[:8] != magic:
        raise ValueError(f'{path}: expected {magic!r} header')
    version, size = struct.unpack_from('<II', data, 8)
    if version != 1:
        raise ValueError(f'{path}: unsupported cache version {version} (expected 1)')
    if size != rec_size:
        raise ValueError(f'{path}: expected record size {rec_size}, got {size}')
    if size and (len(data) - 16) % size:
        raise ValueError(f'{path}: incomplete final record')
    return data


def read_programs_map(path):
    """Map code_hash -> (census_key, stage) from shader-programs.bin without touching RomFS."""
    data = read_cache(path, PRG_MAGIC, 0)
    out, off, end = {}, 16, len(data)
    while end - off >= PRG_HEAD:
        stage, length, c1_len = struct.unpack_from('<III', data, off)
        code_hash = data[off + 12:off + 44]
        control = data[off + 44:off + 44 + prepare_shaders.CONTROL]
        size = PRG_HEAD + length + c1_len
        if length > (64 << 20) or c1_len > length or off + size > end:
            raise ValueError(f'{path}: invalid program length at offset {off}')
        code = data[off + PRG_HEAD:off + PRG_HEAD + length]
        census_key = hashlib.sha256(control + code).hexdigest()
        out[code_hash] = (census_key, stage)
        off += size
    if off != end:
        raise ValueError(f'{path}: incomplete final program record')
    return out


def inventory_map(romfs, exefs):
    """Map code_hash -> (census_key, stage) from extracted RomFS + ExeFS via shader_census."""
    shaders, _ = shader_census.inventory(Path(romfs), Path(exefs), everything=False)
    out = {}
    for key, s in shaders.items():
        stage = prepare_shaders.STAGES.get(s['stage'])
        if stage is None:
            continue
        control = s['control'][:prepare_shaders.CONTROL]
        c1_off, length = struct.unpack_from('<II', control, 0x700)
        code = s['code'][:length]
        ch = prepare_shaders.code_hash(stage, control, s['code'], length, s['code'][c1_off:length])
        out[ch] = (key, stage)
    return out


def read_prepared_iids(path):
    """Set of 32-byte input ids from shader-prepared.bin."""
    p = Path(path)
    if not p.is_file():
        return set()
    data = read_cache(p, PRP_MAGIC, 36)
    return {data[i + 4:i + 36] for i in range(16, len(data), 36)}


def read_answers(path):
    """Yield (stage, code_hash, iid, packed_147) from shader-answers.bin."""
    data = read_cache(path, ANS_MAGIC, ANS_REC)
    for off in range(16, len(data), ANS_REC):
        stage = struct.unpack_from('<I', data, off)[0]
        code_hash = data[off + 4:off + 36]
        iid = data[off + 36:off + 68]
        packed = data[off + 68:off + 215]
        yield stage, code_hash, iid, packed


def unpack_deps(packed, stage):
    """Reconstruct a minimal prepare_shaders deps dict from a 147-byte packed GfxShaderAnswers record."""
    deps = {}
    tex = {}
    for slot in range(32):
        t = packed[slot]
        if t and t < len(prepare_shaders.TEX):
            tex[str(slot)] = prepare_shaders.TEX[t]
    img = {}
    for slot in range(32):
        it = packed[32 + slot]
        if it and it < len(prepare_shaders.TEX):
            fi = packed[64 + slot]
            img[str(slot)] = {'type': prepare_shaders.TEX[it]}
            if fi:
                if fi >= len(prepare_shaders.FMT):
                    raise ValueError(f'unknown storage image format {fi} at slot {slot}')
                img[str(slot)]['fmt'] = prepare_shaders.FMT[fi]
    attr = {}
    if stage == 0:
        for loc in range(32):
            a = packed[96 + loc]
            if a and a in ATTR_NAMES:
                attr[str(loc)] = ATTR_NAMES[a]
    fout = {}
    if stage == 4:
        for idx in range(8):
            f = packed[128 + idx]
            if f and f in ATTR_NAMES:
                fout[str(idx)] = ATTR_NAMES[f]
    if attr:
        deps['attr'] = attr
    if fout:
        deps['fout'] = fout
    if img:
        deps['img'] = img
    if tex:
        deps['tex'] = tex
    if stage == 0 and (packed[146] & 0x80):
        deps['mvprev'] = True
    return deps


def differs_from_default(deps):
    """True when packed answers could not be produced by prepare_shaders.DEFAULT."""
    if deps.get('img') or deps.get('attr') or deps.get('fout') or deps.get('mvprev'):
        return True
    return any(t != '2d' for t in deps.get('tex', {}).values())


def load_base(path):
    """Read upstream variants and priorities without using the supplement being regenerated."""
    if path:
        text = Path(path).read_text(encoding='utf-8')
    elif prepare_shaders.BUNDLED_LIST.is_file():
        text = prepare_shaders.BUNDLED_LIST.read_text(encoding='utf-8')
    else:
        mng = ROOT / 'refs' / 'MacNativeGames'
        text = shader_list.history(mng) if mng.is_dir() else ''
    variants, prio = prepare_shaders.parse_list(text) if text else ([], {})
    by_key = {}
    for k, deps in variants:
        by_key.setdefault(k, []).append(deps)
    return by_key, prio


def harvest(sources, romfs=None, exefs=None, base_path=None, include_warm=False, check_prepared=True):
    """Harvest missing V rows (and optional S/C warm rows) from game/build directories into deterministic lines."""
    ch_map = {}
    if romfs and exefs:
        ch_map.update(inventory_map(romfs, exefs))
    base_v, base_prio = load_base(base_path)

    known_variant_keys = set()
    for k, deps_list in base_v.items():
        for d in deps_list:
            known_variant_keys.add((k, json.dumps(d, sort_keys=True, separators=(',', ':'))))

    v_lines = {}
    warm_keys = {}
    stats = {'records': 0, 'prepared_hits': 0, 'unmapped': 0, 'v_rows': 0, 'warm_rows': 0}

    for src in sources:
        src = Path(src)
        ans_path = src / 'shader-answers.bin' if src.is_dir() else src
        prg_path = ans_path.with_name('shader-programs.bin')
        if prg_path.is_file():
            ch_map.update(read_programs_map(prg_path))
        prepared = read_prepared_iids(ans_path.with_name('shader-prepared.bin')) if check_prepared else set()

        for stage, ch, iid, packed in read_answers(ans_path):
            stats['records'] += 1
            mapped = ch_map.get(ch)
            if not mapped:
                stats['unmapped'] += 1
                continue
            key, mapped_stage = mapped
            if stage not in (0, 4, 5):
                stage = mapped_stage
            warm_keys.setdefault(key, stage)
            if prepared and iid in prepared:
                stats['prepared_hits'] += 1
                continue
            deps = unpack_deps(packed, stage)
            if not prepared and not differs_from_default(deps):
                continue
            canon = json.dumps(deps, sort_keys=True, separators=(',', ':'))
            if (key, canon) in known_variant_keys:
                continue
            v_lines[(key, canon)] = f'V\t{key}\t{canon}'

    out = [v_lines[k] for k in sorted(v_lines)]
    stats['v_rows'] = len(out)
    if include_warm:
        w_lines = []
        for key, stage in sorted(warm_keys.items()):
            if base_prio.get(key, 2) < 2:
                continue
            if stage == 5:
                w_lines.append(f'C\t{key}-000000000000_b\tw')
            else:
                w_lines.append(f'S\t{key}-000000000000_b\t0\tw')
        stats['warm_rows'] = len(w_lines)
        out.extend(w_lines)
    return out, stats


def write_output(out_path, new_lines, merge_existing=True):
    """Write or merge harvested lines cleanly into out_path."""
    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    header = '# Supplemental shader variants and warmup priorities harvested from played shader-answers.bin.'
    existing = []
    if merge_existing and out_path.is_file():
        existing = [l for l in out_path.read_text(encoding='utf-8').splitlines() if l and not l.startswith('#')]
    seen = set(existing)
    merged = list(existing)
    for l in new_lines:
        if l not in seen:
            seen.add(l)
            merged.append(l)
    # Keep V rows before S/C warm rows in deterministic sorted order.
    v_rows = sorted(l for l in merged if l.startswith('V\t'))
    w_rows = sorted(l for l in merged if not l.startswith('V\t'))
    body = '\n'.join([header, *v_rows, *w_rows]) + '\n'
    out_path.write_text(body, encoding='utf-8', newline='\n')
    return len(v_rows), len(w_rows)


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('sources', nargs='+', type=Path, help='game/build directories or shader-answers.bin files')
    ap.add_argument('--romfs', type=Path, help='optional extracted RomFS directory for code_hash resolution')
    ap.add_argument('--exefs', type=Path, help='optional extracted ExeFS directory for code_hash resolution')
    ap.add_argument('--base', type=Path, help='base shader_list.tsv for deduplication and existing warm priorities')
    ap.add_argument('--warm', action='store_true', help='also emit S/C warm rows (prio 1) for met shaders')
    ap.add_argument('--all', action='store_true', help='ignore prepared IDs and emit candidate variants, not confirmed misses')
    ap.add_argument('--replace', action='store_true', help='replace --out only if there are records and all can be mapped')
    ap.add_argument('--out', type=Path, default=shader_list.SUPPLEMENT, help='output TSV path')
    a = ap.parse_args()
    try:
        lines, stats = harvest(a.sources, romfs=a.romfs, exefs=a.exefs, base_path=a.base, include_warm=a.warm,
                               check_prepared=not a.all)
    except (OSError, ValueError) as exc:
        ap.error(str(exc))
    if a.replace and not stats['records']:
        ap.error('refusing --replace: no recorded shader answers')
    if a.replace and stats['unmapped']:
        ap.error(f"refusing --replace: {stats['unmapped']} of {stats['records']} shader answers could not be mapped; "
                 'provide matching shader-programs.bin or --romfs and --exefs')
    total_v, total_w = write_output(a.out, lines, merge_existing=not a.replace)
    print(f'harvested {stats["v_rows"]} new V rows and {stats["warm_rows"]} warm rows from {stats["records"]} records '
          f'(prepared_hits={stats["prepared_hits"]}, unmapped={stats["unmapped"]}); wrote {total_v} V + {total_w} warm '
          f'rows to {a.out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
