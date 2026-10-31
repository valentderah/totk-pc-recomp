"""Prepare the default variant of every game shader and the variants of the MacNativeGames shared shader list for a
build, once, before play.

The list (refs/MacNativeGames games/totk/assets/shader_list.tsv, MIT; docs/shader-list.md there) names shader variants
players met: a shader key (sha256 of the NVN control section and code, as tools/shader_census.py names shaders) and
the translator answers it was translated for. This tool reads the shaders from the extracted RomFS and ExeFS
(tools/shader_census.py inventory), keeps the list's variants whose shader is in this game version, adds each shader's
default variant (every texture slot a 2D texture, float vertex inputs and colour outputs, as MacNativeGames' seed
translates every shader for its default bind state: prepare_shader_seed.py all_default), and writes them next to the
build's totk.exe in the record layouts of runtime/gfx_shader.c ("answers met in play"):

    seed-programs.bin  "TOTKSPRG": stage, length, c1 length, runtime code hash, control section, code, c1
    seed-answers.bin   "TOTKSANS": stage, code hash, input id (zero: the runtime computes it), answers, priority

Priority (the pad byte): 0 shaders the list's warm rows flag as title/menu (t), 1 other warm rows (S, C, R), 2 the
rest; a default variant takes its shader's priority. Then it runs `totk.exe --prepare-shaders`, which translates every record into the build's shader cache, writes
shader-prepared.bin (one record per variant, in priority order) and builds every variant's stage library or compute
pipeline once, so the GPU driver's own disk cache holds them (vk_pipeline.c "stage library prebuild"). The seed files
can be deleted afterwards.

Game-derived output stays in the build directory (ignored). The list is read at preparation time from tools/data/shader_list.tsv when that file exists (the installer archive
bundles it; it stays out of the repository), else from refs/ through git with the rows its history dropped
(tools/shader_list.py); nothing of it is copied into the repository. Needs Python 3.14 (compression.zstd), so run it in .venv-py314 or the embeddable 3.14:

    .venv-py314\\Scripts\\python tools\\prepare_shaders.py build\\v121 [--version v121] [--list <tsv>] [--no-run]
                                                     [--romfs DIR --exefs DIR]   # instead of build\\extracted\\<version>
"""
import argparse
import hashlib
import json
import os
import struct
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
while not (ROOT / 'tools' / 'shader_census.py').is_file() and ROOT != ROOT.parent:
    ROOT = ROOT.parent
sys.path.insert(0, str(ROOT / 'tools'))
import shader_list  # noqa: E402
from shader_census import inventory  # noqa: E402

STAGES = {'vs': 0, 'ps': 4, 'cs': 5}                       # runtime/gfx.h GFX_STAGE_*
TEX = [None, '1d', '1darray', 'buffer', '2d', '2darray', '2dms', '2dmsarray', '3d', 'cube', 'cubearray']
FMT = [None, 'R8Unorm', 'R8Snorm', 'R8Uint', 'R8Sint', 'R16Float', 'R16Unorm', 'R16Snorm', 'R16Uint', 'R16Sint',
       'R32Float', 'R32Uint', 'R32Sint', 'R8G8Unorm', 'R8G8Snorm', 'R8G8Uint', 'R8G8Sint', 'R16G16Float', 'R16G16Unorm',
       'R16G16Snorm', 'R16G16Uint', 'R16G16Sint', 'R32G32Float', 'R32G32Uint', 'R32G32Sint', 'R8G8B8A8Unorm',
       'R8G8B8A8Snorm', 'R8G8B8A8Uint', 'R8G8B8A8Sint', 'R16G16B16A16Float', 'R16G16B16A16Unorm', 'R16G16B16A16Snorm',
       'R16G16B16A16Uint', 'R16G16B16A16Sint', 'R32G32B32A32Float', 'R32G32B32A32Uint', 'R32G32B32A32Sint',
       'R10G10B10A2Unorm', 'R10G10B10A2Uint', 'R11G11B10Float']   # gfx_shader.c fmt_names
TYPES = {'float': 0, 'sint': 1, 'uint': 2}
DEFAULT = {'tex': {str(slot): '2d' for slot in range(32)}}   # answers outside a shader's use mask are dropped
CONTROL = 0x880
ANS_REC = 4 + 32 + 32 + 147 + 1


BUNDLED_LIST = ROOT / 'tools' / 'data' / 'shader_list.tsv'   # the installer archive carries a copy (no git, no refs/)
if not BUNDLED_LIST.is_file():
    BUNDLED_LIST = ROOT / 'data' / 'shader_list.tsv'          # release folder layout (tools/make_release.py)


def read_list(path):
    if path:
        return Path(path).read_text(encoding='utf-8')
    if BUNDLED_LIST.is_file():
        return BUNDLED_LIST.read_text(encoding='utf-8')
    return shader_list.history(ROOT / 'refs' / 'MacNativeGames')


def pipeline_variants(key):
    """The vertex and fragment variant names of an R/B row's pipeline key. Since MacNativeGames f024ef04 (2026-10-28) the
    TotK list keeps keys packed, "<vs variant>:<fs variant>:<the rest in hex>"; older revisions and contribution files
    hold the whole key in hex, "<vs variant>\\0<fs variant>\\0" + a Metal descriptor (docs/shader-list.md there)."""
    if ':' in key:
        return key.split(':', 2)[:2]
    return [n.decode('ascii', 'replace') for n in bytes.fromhex(key).split(b'\0')[:2]]


def parse_list(text):
    """V rows as (key, deps); priority per shader key from the warm rows (variant names start with the key)."""
    variants, prio = [], {}

    def warm(key, flags):
        p = 0 if 't' in flags else 1
        prio[key] = min(prio.get(key, 2), p)

    for line in text.splitlines():
        f = line.split('\t')
        if f[0] == 'V' and len(f) == 3:
            variants.append((f[1], json.loads(f[2])))
        elif f[0] == 'S' and len(f) == 4:
            warm(f[1][:64], f[3])
        elif f[0] == 'C' and len(f) == 3:
            warm(f[1][:64], f[2])
        elif f[0] in ('R', 'B') and len(f) >= 3:
            for n in pipeline_variants(f[1]):
                if len(n) >= 64:
                    warm(n[:64], f[-1])
    return variants, prio


def answers(deps, stage, control):
    """GfxShaderAnswers in gfx_shader.c pack_answers layout (147 bytes) from the list's deps JSON."""
    tex, img, fmt, attr, fout = bytearray(32), bytearray(32), bytearray(32), bytearray(32), bytearray(8)
    for slot, t in deps.get('tex', {}).items():
        t = t.removesuffix('_depth')
        tex[int(slot)] = TEX.index('2d' if t == '*' else t)   # "*": any kind, translated as the plain 2D it stands for
    for slot, o in deps.get('img', {}).items():
        img[int(slot)] = TEX.index(o['type'])
        fmt[int(slot)] = FMT.index(o['fmt']) if o.get('fmt') in FMT else 0
    for loc, t in deps.get('attr', {}).items():
        attr[int(loc)] = TYPES[t]
    for idx, t in deps.get('fout', {}).items():
        fout[int(idx)] = TYPES[t]
    local = [0, 0, 0]   # vk_exec_draw.c answers_for: zero for graphics stages (they are in the input id)
    if stage == 5:      # compute: the control section's local size, 0 read as 1
        local = [v or 1 for v in struct.unpack_from('<III', control, 0x748)]
    packed = bytes(tex + img + fmt + attr + fout) + struct.pack('<HHHIB', *local, 0, 0)
    assert len(packed) == 147
    return packed


def code_hash(stage, control, code, length, c1):
    """gfx_shader.c build_code_hash."""
    return hashlib.sha256(struct.pack('<I', stage) + control + struct.pack('<Q', length) + code[:length] +
                          struct.pack('<I', len(c1)) + c1).digest()


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('build', type=Path, help='build directory holding totk.exe (build\\v121)')
    ap.add_argument('--version', default='v121', help='build/extracted/<version>')
    ap.add_argument('--romfs', type=Path, help='RomFS directory (default build/extracted/<version>/romfs)')
    ap.add_argument('--exefs', type=Path, help='ExeFS directory (default build/extracted/<version>/exefs)')
    ap.add_argument('--list', help='a shader_list.tsv instead of tools/data/shader_list.tsv or refs/MacNativeGames')
    ap.add_argument('--no-run', action='store_true', help='write the seed files only')
    a = ap.parse_args()
    t0 = time.monotonic()
    variants, prio = parse_list(read_list(a.list))
    extracted = ROOT / 'build' / 'extracted' / a.version
    listed = len(variants)
    shaders, _ = inventory(a.romfs or extracted / 'romfs', a.exefs or extracted / 'exefs', everything=False)
    # A missing list row says nothing about the walk: the list covers several versions, and 1.1.1 holds 17,587 of its
    # 41,414 rows, so most rows are absent because the game does not have them, not because the walk missed them. A walk
    # that found next to nothing does say something: the shader directories have moved.
    if len(shaders) < max(64, listed // 20):
        print(f'only {len(shaders)} shaders found of the {listed} listed variants: reading every RomFS file instead',
              flush=True)
        shaders, _ = inventory(a.romfs or extracted / 'romfs', a.exefs or extracted / 'exefs')
    t1 = time.monotonic()
    progs, recs, seen, skipped = {}, [], set(), {'not in this version': 0, 'stage': 0}
    variants += [(key, DEFAULT) for key in sorted(shaders)]
    for key, deps in variants:
        s = shaders.get(key)
        if not s:
            skipped['not in this version'] += 1
            continue
        stage = STAGES.get(s['stage'])
        if stage is None:
            skipped['stage'] += 1
            continue
        control, code = s['control'][:CONTROL], s['code']
        c1_off, length = struct.unpack_from('<II', control, 0x700)
        c1 = code[c1_off:length]
        if key not in progs:
            h = code_hash(stage, control, code, length, c1)
            progs[key] = (h, struct.pack('<III', stage, length, len(c1)) + h + control + code[:length] + c1)
        h = progs[key][0]
        packed = answers(deps, stage, control)
        if (h, packed) in seen:   # a list row of the default bind state
            continue
        seen.add((h, packed))
        rec = struct.pack('<I', stage) + h + bytes(32) + packed + bytes([prio.get(key, 2)])
        assert len(rec) == ANS_REC
        recs.append((prio.get(key, 2), rec))
    recs.sort(key=lambda r: r[0])   # stable: list order within a priority
    out = a.build.resolve()
    with open(out / 'seed-programs.bin', 'wb') as f:
        f.write(b'TOTKSPRG' + struct.pack('<II', 1, 0))
        for _, body in progs.values():
            f.write(body)
    with open(out / 'seed-answers.bin', 'wb') as f:
        f.write(b'TOTKSANS' + struct.pack('<II', 1, ANS_REC))
        for _, rec in recs:
            f.write(rec)
    by = [sum(1 for p, _ in recs if p == i) for i in range(3)]
    print(f'{listed} listed variants and {len(shaders)} default ones: {len(recs)} of {len(progs)} shaders in {a.version} '
          f'(priority t {by[0]}, warm {by[1]}, rest {by[2]}), skipped {skipped}; inventory {t1 - t0:.1f} s, '
          f'seed written in {time.monotonic() - t0:.1f} s', flush=True)
    if a.no_run:
        return 0
    exe = out / ('totk.exe' if os.name == 'nt' else 'totk')
    r = subprocess.run([str(exe), '--prepare-shaders'], cwd=str(out))
    print(f'totk.exe --prepare-shaders: rc={r.returncode}; total {time.monotonic() - t0:.1f} s')
    return r.returncode


if __name__ == '__main__':
    sys.exit(main())
