"""Prepare verified player data for the native runtime, entirely in ignored folders."""
import json
import os
import shutil
import sys
from pathlib import Path

from macos.build import ROOT, GENERATED_MARKER, run, digest, write


def llvm_bin():
    """Resolve an installed LLVM before starting expensive game preparation."""
    explicit = os.environ.get('LLVM_BIN')
    if explicit:
        candidates = [Path(explicit)]
    else:
        found = shutil.which('llvm-objcopy')
        candidates = ([Path(found).parent] if found else []) + [Path('/opt/homebrew/opt/llvm/bin')]
        candidates += sorted(Path('/opt/homebrew/opt').glob('llvm@*/bin'), reverse=True)
    for folder in candidates:
        if all(os.access(folder / tool, os.X_OK) for tool in ('llvm-objcopy', 'llvm-objdump')):
            return folder.resolve()
    raise RuntimeError('LLVM tools are missing; install Homebrew llvm or set LLVM_BIN to its bin directory')


def write_layout(tree, out, version):
    sys.path.insert(0, str(ROOT / 'recomp'))
    from layout import native_layout
    sizes = {name: (tree / f'rewritten/{version}/{name}.patched.bin').stat().st_size
             for name in ('main', 'subsdk0', 'sdk')}
    modules = native_layout(sizes)
    rows = ',\n'.join(f'{{"{n}", UINT64_C({b}), UINT64_C({s})}}' for n, b, s in modules)
    write(out / 'native_layout.h', f'// {GENERATED_MARKER}\n'
          'static const struct { const char *name; uint64_t base, size; } native_modules[] = {\n' + rows + '\n};\n')
    write(out / 'native-layout.json', json.dumps({'generated_from': GENERATED_MARKER, 'modules': modules}, indent=2) + '\n')


def link(path, target):
    path, target = Path(path), Path(target).resolve()
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_symlink():
        if path.resolve() == target:
            return
        path.unlink()
    elif path.exists():
        raise RuntimeError(f'Refusing to replace existing directory: {path}')
    path.symlink_to(target, target_is_directory=True)


def check_inputs(version, extracted):
    expected = json.loads((ROOT / 'tools/data' / f'expected-v{"1.4.3" if version == "v143" else "1.2.1"}.json').read_text())
    base = extracted / version
    for name in ('main', 'subsdk0', 'sdk'):
        file = base / 'exefs' / name
        if not file.is_file():
            raise RuntimeError(f'Missing extracted {version}/{name}; configure config.local.toml and run tools/extract.py')
        if digest(file) != expected['exefs_sha256'][name]:
            raise RuntimeError(f'Extracted {version}/{name} does not match the supported game version')
    manifest = base / 'romfs/index.json'
    if not manifest.is_file():
        raise RuntimeError(f'Missing verified {version} RomFS index')
    data = json.loads(manifest.read_text())
    romfs = base / 'romfs/romfs.bin'
    if (data.get('status') != 'pass' or not romfs.is_file() or
            romfs.stat().st_size != expected['romfs']['size'] or
            digest(romfs) != expected['romfs']['sha256']):
        raise RuntimeError('RomFS integrity verification failed')
    if not (extracted / 'firmware/shared-fonts/manifest.json').is_file():
        raise RuntimeError('Native builds require verified shared fonts; set input.firmware and run tools/extract_fonts.py')


def prepare_game(tree, out, version, jobs, extracted):
    if sys.version_info < (3, 14):
        raise RuntimeError('Game preparation requires Python 3.14 or later (compression.zstd)')
    llvm = llvm_bin()
    check_inputs(version, extracted)
    out.mkdir(parents=True, exist_ok=True)
    generated = ROOT / 'generated/macos-arm64'
    generated.mkdir(parents=True, exist_ok=True)
    link(tree / 'extracted', extracted)
    link(tree / 'rewritten', generated)
    link(tree / 'build' / version, out)
    env = dict(os.environ, DM_RULESET='native', DM_PRIVATE_GATES='0', TOTK_PYTHON=sys.executable, LLVM_BIN=str(llvm))
    commands = [
        ['scripts/prepare_modules.py', f'extracted/{version}/exefs', '--output', f'extracted/{version}/elf'],
        *[['third_party/d2r/patch_nso.py', f'extracted/{version}/exefs/{name}', f'rewritten/{version}', name]
          for name in ('main', 'subsdk0', 'sdk')],
        ['scripts/verify_rewrites.py', '--version', version, '--jobs', str(jobs)],
        ['scripts/prepare_runtime.py', '--version', version],
        ['scripts/prepare_romfs_backend.py', '--version', version],
        ['scripts/make_romfs_patches.py', '--version', version],
        ['scripts/make_output_res_overlay.py', '3840x2160', '--version', version],
    ]
    for command in commands:
        run([sys.executable, *command], cwd=tree, env=env)
    write_layout(tree, out, version)
    from macos.prompts import prepare_prompts
    prepare_prompts(extracted, out, version)
    run([sys.executable, 'scripts/prepare_shared_fonts.py'], cwd=tree, env=env)
    shutil.copy2(tree / 'build/shared_fonts_config.h', out / 'shared_fonts_config.h')
    for name in ('runtime_tables.h', 'romfs_config.h', 'shared_fonts_config.h'):
        p = out / name
        if GENERATED_MARKER not in p.read_text():
            write(p, f'// {GENERATED_MARKER}\n' + p.read_text())
    # Binary and tabular rewriter output cannot carry a C comment. A marked
    # manifest fingerprints every artifact and keeps their provenance together.
    files = {p.name: digest(p) for p in sorted((generated / version).iterdir())
             if p.is_file() and p.name != 'provenance.json'}
    write(generated / version / 'provenance.json', json.dumps(
        {'generated_from': GENERATED_MARKER, 'version': version, 'files': files}, indent=2) + '\n')
    from macos.shaders import prepare_shaders
    prepare_shaders(tree, version, jobs)
    write(out / 'preparation.json', json.dumps({'generated_from': GENERATED_MARKER, 'version': version,
          'source_id': (tree.parent.parent / '.source-id').read_text(),
          'layout_sha256': digest(ROOT / 'recomp/layout.py'),
          'headers': {name: digest(out / name) for name in
                      ('runtime_tables.h','romfs_config.h','shared_fonts_config.h','native_layout.h','native_prompt_textures.h')}}, indent=2) + '\n')
