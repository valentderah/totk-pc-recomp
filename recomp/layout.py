"""Guest address layout of the loaded modules, computed from their sizes.

Guest addresses equal host addresses, and the lifter writes absolute addresses into the generated C, so the
generator and the runtime must agree on every module base. This is the single place that decides them: the generator
calls layout() and writes the result into generated/<version>/layout.h and layout.json, which the runtime and the
build read.

Modules are placed in Horizon's load order (rtld is dropped; the host links the modules):
main, subsdk0..9, sdk. A mod's code modules (exlaunch plugins such as an exefs/subsdk9) follow sdk as plugin0, plugin1,
... in mod load order, so the game modules keep their bases with or without them (after the plugin design in #845 by
itaskaev2). Each base is aligned to 2 MiB. Everything must stay below 2^39, the limit of the addresses IPC buffers can
encode.
"""
import hashlib
import json
from pathlib import Path

from nso import Nso

FIRST_BASE = 0x7000000000
ALIGN = 0x200000
ADDRESS_LIMIT = 1 << 39
ORDER = ['main'] + [f'subsdk{i}' for i in range(10)] + ['sdk']


def align_up(v, a=ALIGN):
    return (v + a - 1) & ~(a - 1)


def plugin_name(i):
    return f'plugin{i}'


def layout(exefs, plugins=()):
    """[(name, base, size)] for the modules present in the ExeFS directory, then the plugin NSOs (paths, in mod load
    order) as plugin0..N, in load order."""
    exefs = Path(exefs)
    paths = [(name, exefs / name) for name in ORDER if (exefs / name).exists()]
    paths += [(plugin_name(i), Path(p)) for i, p in enumerate(plugins)]
    out, base = [], FIRST_BASE
    for name, path in paths:
        size = Nso(str(path)).size
        out.append((name, base, size))
        base = align_up(base + size)
    if base > ADDRESS_LIMIT:
        raise ValueError(f'modules end at {base:#x}, above 2^39')
    return out


def plugin_files(plugins):
    """{plugin<N>: (absolute path, SHA-256 of the NSO)} for layout.json: the build and the runtime read a plugin from its
    mod folder, not from the ExeFS directory, and check that it is the file the code was lifted from."""
    return {plugin_name(i): (Path(p).resolve().as_posix(), hashlib.sha256(Path(p).read_bytes()).hexdigest())
            for i, p in enumerate(plugins)}


def write(mods, outdir, files=None):
    """layout.json for Python tools and layout.h for the runtime. files: plugin_files() for the plugin entries, which
    carry them as "file" and "sha256"; without plugins both files are what they were before plugins existed."""
    files = files or {}
    outdir = Path(outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    def entry(n, b, s):
        e = {'name': n, 'base': f'{b:#x}', 'size': f'{s:#x}'}
        if n in files:
            e['file'], e['sha256'] = files[n]
        return e

    (outdir / 'layout.json').write_text(json.dumps([entry(n, b, s) for n, b, s in mods], indent=1) + '\n')
    lines = ['// Module layout chosen by recomp/layout.py. Generated; do not edit.', '#pragma once', '']
    for n, b, s in mods:
        lines.append(f'#define MOD_{n.upper()}_BASE {b:#x}ull')
        lines.append(f'#define MOD_{n.upper()}_SIZE {s:#x}ull')
    lines += ['', f'#define MOD_COUNT {len(mods)}',
              '#define MOD_LIST(X) ' + ' '.join(f'X({n}, {n.upper()})' for n, _, _ in mods), '']
    (outdir / 'layout.h').write_text('\n'.join(lines))


def read(outdir):
    """[(name, base, size)] from a layout.json written by write() (a plugin's file and hash left out)."""
    data = json.loads((Path(outdir) / 'layout.json').read_text())
    return [(m['name'], int(m['base'], 16), int(m['size'], 16)) for m in data]
