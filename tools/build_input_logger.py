"""Build Linux input-query probes against an installed recomp's matching AppImage SDK.

The AppImage must be extracted under --sdk (runtime/, recomp/, llvm/). Original generated files and objects are
read-only. A generator subclass regenerates only affected chunks and requires byte-identical baseline output first.
All game-derived output is confined to build/. Runtime action changes require explicit research options and providers.
"""
import argparse
import hashlib
import importlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def observed_function(lines, entry, scalar=False):
    """Wrap a generated function, logging copied arguments and its actual result after it returns."""
    name = f'f_{entry:x}'
    original = f'query_original_{entry:x}'
    definition = f'void {name}(Cpu *c) {{'
    if lines.count(definition) != 1:
        raise ValueError(f'{name}: unsupported generator function shape')
    lines = list(lines)
    lines[lines.index(definition)] = f'static __attribute__((noinline)) void {original}(Cpu *c) {{'
    if scalar:
        return lines + [f'void {name}(Cpu *c) {{',
                        '    uint64_t object = c->x[0], caller = c->x[30];',
                        f'    {original}(c);',
                        '    extern void input_scalar_observe(uint64_t, uint64_t, uint64_t, uint64_t);',
                        f'    input_scalar_observe({entry:#x}ull, object, caller, c->x[0]);', '}']
    return lines + [
        f'void {name}(Cpu *c) {{',
        '    uint64_t object = c->x[0], id = c->x[1], caller = c->x[30];',
        f'    {original}(c);',
        '    extern void input_query_observe(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);',
        f'    input_query_observe({entry:#x}ull, object, id, caller, c->x[0]);',
        '}',
    ]


def query_candidates(mod):
    """Find leaf queries: load a field pointer, load a bitset, test bit x1, return a boolean.

    This recognizes machine behavior only. Neither a field's meaning nor the tested IDs are named as actions.
    """
    import numpy as np
    w = mod.words
    match = ((w[:-4] & 0xffc003ff) == 0xf9400008)
    for delta, word in enumerate((0xf9400108, 0x9ac12508, 0x12000100, 0xd65f03c0), 1):
        match &= w[delta:len(w) - 4 + delta] == word
    return {mod.lo + int(i) * 4: ((int(w[i]) >> 10) & 4095) * 8 for i in np.flatnonzero(match)}


def observed_controller(lines, entry):
    """Observe a verified named-controller getter: SafeString* in x2, pad index in x1, controller* returned in x0.

    Offsets reflect the controller contract verified at its consumers: held at 0x114, pressed at 8, released at 12,
    and repeat at 16. This wrapper is enabled only by an explicit, fingerprinted research plan.
    """
    name = f'f_{entry:x}'
    definition = f'void {name}(Cpu *c) {{'
    if lines.count(definition) != 1:
        raise ValueError(f'{name}: unsupported controller getter shape')
    lines = list(lines)
    original = f'controller_original_{entry:x}'
    lines[lines.index(definition)] = f'static __attribute__((noinline)) void {original}(Cpu *c) {{'
    return lines + [
        f'void {name}(Cpu *c) {{',
        '    uint64_t caller = c->x[30], index = c->x[1];',
        '    const char *source = (const char *)(uintptr_t)LD64(c->x[2]);',
        '    char label[64]; unsigned n = 0;',
        '    if (source) { while (n < sizeof label - 1 && source[n]) { label[n] = source[n]; n++; } }',
        '    label[n] = 0;',
        f'    {original}(c);',
        '    uint64_t object = c->x[0];',
        '    uint32_t held = 0, pressed = 0, released = 0, repeat = 0;',
        '    if (object) { held = LD32(object + 0x114); pressed = LD32(object + 8);',
        '                  released = LD32(object + 12); repeat = LD32(object + 16); }',
        '    extern void input_controller_observe(uint64_t, uint64_t, uint64_t, const char *, uint64_t,',
        '                                         uint32_t, uint32_t, uint32_t, uint32_t);',
        f'    input_controller_observe({entry:#x}ull, caller, index, label, object, held, pressed, released, repeat);',
        '}',
    ]


def observed_outputs(lines, entry, fields, experimental=False, action_channel=False, position=None, player_state=False,
                     gameplay_bindings=False):
    """Observe verified uint64_t output pointers in the original x0 object after a publisher returns."""
    definition = f'void f_{entry:x}(Cpu *c) {{'
    if lines.count(definition) != 1:
        raise ValueError('Unsupported publisher function shape')
    if not fields or len(fields) > 8 or any(type(f) is not int or f < 0 or f > 4096 or f % 8 for f in fields):
        raise ValueError('Output fields must be one to eight aligned offsets in [0,4096]')
    lines = list(lines)
    lines[lines.index(definition)] = f'static __attribute__((noinline)) void outputs_original_{entry:x}(Cpu *c) {{'
    wrapper = [f'void f_{entry:x}(Cpu *c) {{', '    uint64_t object = c->x[0], caller = c->x[30];',
               f'    outputs_original_{entry:x}(c);',
               '    extern void input_output_observe(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);']
    if gameplay_bindings:
        wrapper += ['    extern void input_actions_bridge_begin(uint64_t, uint64_t);',
                    '    extern uint64_t input_actions_bridge_apply(unsigned, uint64_t);',
                    f'    input_actions_bridge_begin({entry:#x}ull, object);']
    if action_channel:
        wrapper += ['    extern void input_action_channel_begin(uint64_t, uint64_t);',
                    '    extern uint64_t input_action_channel_apply(uint64_t, uint64_t, uint64_t);',
                    f'    input_action_channel_begin({entry:#x}ull, object);']
    for field in fields:
        wrapper += [f'    uint64_t pointer_{field:x} = object ? LD64(object + {field}) : 0;',
                    f'    input_output_observe({entry:#x}ull, object, {field}, caller,',
                    f'                         pointer_{field:x} ? LD64(pointer_{field:x}) : 0);']
        if experimental:
            wrapper += ['    extern uint64_t input_output_filter(uint64_t, uint64_t, uint64_t);',
                        f'    if (pointer_{field:x}) ST64(pointer_{field:x},',
                        f'        input_output_filter({entry:#x}ull, {field}, LD64(pointer_{field:x})));']
        if action_channel:
            wrapper += [f'    if (pointer_{field:x}) ST64(pointer_{field:x},',
                        f'        input_action_channel_apply({entry:#x}ull, {field}, LD64(pointer_{field:x})));']
        if gameplay_bindings:
            wrapper += [f'    if (pointer_{field:x}) ST64(pointer_{field:x},',
                        f'        input_actions_bridge_apply({fields.index(field)}, LD64(pointer_{field:x})));']
    if position:
        path, offset = position['path'], position['offset']
        if not path or len(path) > 8 or any(type(v) is not int or v < 0 or v > 65536 or v % 8 for v in path):
            raise ValueError('Position pointer path must contain one to eight aligned offsets in [0,65536]')
        if type(offset) is not int or offset < 0 or offset > 65536 or offset % 4:
            raise ValueError('Position offset must be aligned and in [0,65536]')
        wrapper += ['    extern int plat_mem_read(uint64_t, void *, size_t);',
                    '    extern void input_position_observe(uint64_t, uint64_t, float, float, float);',
                    '    uint64_t actor = object;']
        for step in path:
            wrapper += [f'    if (!actor || !plat_mem_read(actor + {step}, &actor, sizeof actor)) actor = 0;']
        if player_state:
            wrapper += ['    extern void input_player_state_frame(uint64_t, uint64_t);',
                        f'    input_player_state_frame({entry:#x}ull, actor);']
        wrapper += ['    float position[3];',
                    f'    if (actor && plat_mem_read(actor + {offset}, position, sizeof position))',
                    f'        input_position_observe({entry:#x}ull, actor, position[0], position[1], position[2]);']
    return lines + wrapper + ['}']


def observed_state(lines, entry, field, size, returned=None, interaction=False):
    """Observe a verified scalar field on the original x0 object after a callback returns, preserving guest CPU state."""
    definition = f'void f_{entry:x}(Cpu *c) {{'
    if lines.count(definition) != 1:
        raise ValueError('Unsupported state callback shape')
    if type(field) is not int or not 0 <= field <= 65536 or type(size) is not int or size not in (1, 2, 4, 8):
        raise ValueError('State field must be bounded and have a scalar byte width')
    if returned is not None:
        if not isinstance(returned, dict) or not {'path', 'field', 'size'} <= returned.keys():
            raise ValueError('Returned field requires path, field and size')
        path, offset, width = returned['path'], returned['field'], returned['size']
        if (not isinstance(path, list) or len(path) > 8 or
                any(type(p) is not int or not 0 <= p <= 65536 or p % 8 for p in path) or
                type(offset) is not int or not 0 <= offset <= 65536 or
                type(width) is not int or width not in (1, 2, 4, 8)):
            raise ValueError('Returned field requires a bounded pointer path and scalar width')
    if interaction and (size!=4 or returned is None):
        raise ValueError('Interaction selection requires a four-byte index and returned entry contract')
    lines = list(lines)
    lines[lines.index(definition)] = f'static __attribute__((noinline)) void state_original_{entry:x}(Cpu *c) {{'
    wrapper = [f'void f_{entry:x}(Cpu *c) {{', '    uint64_t object = c->x[0], caller = c->x[30];',
                    f'    state_original_{entry:x}(c);',
                    '    extern int plat_mem_read(uint64_t, void *, size_t);',
                    '    extern void input_state_observe(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);',
                    '    uint64_t value = 0;',
                    f'    if (object && plat_mem_read(object + {field}, &value, {size}))',
                    f'        input_state_observe({entry:#x}ull, object, {field}, caller, value);']
    if interaction:
        wrapper += ['    int32_t selected = -2;',
                    f'    if (!object || !plat_mem_read(object + {field}, &selected, 4)) selected = -2;']
    if returned is not None:
        wrapper += ['    uint64_t target = c->x[0];',
                    '    extern void input_return_field_observe(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, int);']
        for step in path:
            wrapper += [f'    if (!target || !plat_mem_read(target + {step}, &target, sizeof target)) target = 0;']
        wrapper += ['    value = 0;',
                    f'    int valid = target && plat_mem_read(target + {offset}, &value, {width});',
                    f'    input_return_field_observe({entry:#x}ull, target, {offset}, caller, value, valid);']
        if interaction:
            wrapper += ['    extern void input_actions_interaction_observe(uint64_t, int64_t, int);',
                        '    input_actions_interaction_observe(object, selected, valid);']
    return lines + wrapper + ['}']


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--install', type=Path, required=True)
    ap.add_argument('--sdk', type=Path, required=True)
    ap.add_argument('--out', type=Path, default=ROOT / 'build/input-logger/run')
    ap.add_argument('--plan', type=Path, help='input_graph report with fingerprinted probe_plan (under build/)')
    ap.add_argument('--allow-mask-filter', action='store_true',
                    help='research only: allow opt-in clearing of publisher bits using TOTK_MASK_CLEAR=site:bits')
    ap.add_argument('--allow-action-channel', action='store_true', help='research only: enable an opt-in virtual Jump channel')
    ap.add_argument('--gameplay-bindings', action='store_true', help='build the existing Controls UI with verified gameplay action bindings')
    a = ap.parse_args()
    install, sdk, out = a.install.resolve(), a.sdk.resolve(), a.out.resolve()
    if not out.is_relative_to(ROOT / 'build'):
        ap.error('--out must be under this checkout\'s build/ directory')
    out.mkdir(parents=True, exist_ok=True)
    work = out.parent / (out.name + '-objects')
    work.mkdir(exist_ok=True)
    source_snapshot = work / 'research-source'
    source_snapshot.mkdir(exist_ok=True)
    research_sources = {}
    for filename in ('build_input_logger.py', 'input_query_logger.c', 'input_action_channel.c',
                     'input_action_channel.h', 'input_context_flags.c', 'input_player_state.c',
                     'input_player_state.h', 'player_state_plan.py', 'input_guard_plan.py', 'input_special_plan.py'):
        source = ROOT / 'tools' / filename
        destination = source_snapshot / filename
        shutil.copy2(source, destination)
        research_sources[filename] = hashlib.sha256(destination.read_bytes()).hexdigest()
    if a.gameplay_bindings:
        for name in ('runtime/input.c','runtime/input_actions.c','runtime/input_actions.h','runtime/input_actions.def','runtime/cfg.c',
                     'runtime/ui/binds_page.cpp','tools/input_actions_bridge.c','tools/input_actions_test_devices.c'):
            dst=source_snapshot/name;dst.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(ROOT/name,dst)
            research_sources[name]=hashlib.sha256(dst.read_bytes()).hexdigest()
    gen_dir = install / '.setup-temp/gen'
    old_build = install / '.setup-temp/build'
    metadata = json.loads((install / 'totk-install.json').read_text())
    for p in (sdk / 'recomp', sdk / 'tools'):
        sys.path.insert(0, str(p))
    gen = importlib.import_module('gen')
    from module import Module
    from layout import layout
    import patches
    mods = layout(install / 'exefs')
    installed_layout = json.loads((gen_dir / 'layout.json').read_text())
    if [(n, b, s) for n, b, s in mods] != [(m['name'], int(m['base'], 16), int(m['size'], 16))
                                          for m in installed_layout]:
        raise SystemExit('Installed generated layout does not match ExeFS')
    name, base, _ = mods[0]
    if name != 'main':
        raise SystemExit('Expected main as the first module')
    mod = Module('main', install / 'exefs/main', base)
    sites = query_candidates(mod)
    if not sites:
        raise SystemExit('No matching field-bit queries; no executable produced')
    starts, _ = gen.discover(mod)
    absolute = (mod.lo + starts * 4).tolist()
    ends = dict(zip(absolute, absolute[1:] + [mod.hi]))
    if any(site not in ends or ends[site] != site + 20 for site in sites):
        raise SystemExit('A candidate is not exactly one discovered five-instruction function')
    controller_sites = set()
    output_sites = {}
    state_sites = {}
    scalar_sites = set()
    context_flags = None
    player_state = None
    if a.plan:
        if not a.plan.resolve().is_relative_to(ROOT / 'build'):
            ap.error('--plan must be under build/')
        plan = json.loads(a.plan.read_text())['probe_plan']
        if plan['build_id'] != mod.nso.build_id:
            raise SystemExit('Probe plan build ID does not match ExeFS')
        context_flags = plan.get('context_flags')
        if plan.get('player_state') is not None:
            import player_state_plan
            player_state = player_state_plan.validate(plan['player_state'], mod)
        if context_flags:
            address, hashes = context_flags['global'], context_flags['hashes']
            if (type(address) is not int or address % 8 or not base <= address < base + len(mod.image) or
                    not 1 <= len(hashes) <= 8 or any(type(h) is not int or not 0 <= h <= 0xffffffff for h in hashes)):
                raise SystemExit('Invalid local context flag observation contract')
            if context_flags.get('block_hash') is not None and context_flags['block_hash'] not in hashes:
                raise SystemExit('Context gate hash must also be observed by the verified local plan')
        seen_entries = set(sites)
        for site in plan['sites']:
            entry, end = base + site['offset'], base + site['end']
            if entry in seen_entries:
                raise SystemExit('Probe plan repeats a function entry')
            seen_entries.add(entry)
            if (site['kind'] not in ('controller_by_name', 'pointer_u64_outputs', 'object_state', 'scalar_result') or ends.get(entry) != end or
                    hashlib.sha256(mod.image[site['offset']:site['end']]).hexdigest() != site['sha256']):
                raise SystemExit('Probe plan function fingerprint does not match ExeFS')
            if site['kind'] == 'controller_by_name':
                controller_sites.add(entry)
            elif site['kind'] == 'scalar_result':
                scalar_sites.add(entry)
            elif site['kind'] == 'object_state':
                observed_state([f'void f_{entry:x}(Cpu *c) {{', '}'], entry, site['field'], site['size'], site.get('returned'))
                state_sites[entry] = site
            else:
                fields = site['fields']
                observed_outputs([f'void f_{entry:x}(Cpu *c) {{', '}'], entry, fields, position=site.get('position'))
                output_sites[entry] = site
    if player_state and (len(output_sites) != 1 or not next(iter(output_sites.values())).get('position')):
        raise SystemExit('Player-state observation requires one fingerprinted publisher with a verified actor path')
    action_binding_plan = plan.get('action_bindings') if a.plan else None
    if a.gameplay_bindings:
        if not player_state or not context_flags or not context_flags.get('block_hash') or not action_binding_plan:
            raise SystemExit('Gameplay bindings require state, pause and action contracts')
        if a.allow_action_channel:raise SystemExit('Do not combine the single-action research channel with UI gameplay bindings')
        interaction_site=base+action_binding_plan['interaction_site']
        if interaction_site not in state_sites or not state_sites[interaction_site].get('returned'):
            raise SystemExit('Gameplay bindings require a verified interaction selection observer')
        ride_verb=action_binding_plan.get('ride_verb')
        verb_rows=player_state['verb_table']['rows']
        if type(ride_verb) is not int or not 0<=ride_verb<len(verb_rows) or verb_rows[ride_verb]['name']!='Ride':
            raise SystemExit('Riding context must name the verified Ride verb')
        from input_special_plan import source as special_source
        special_function=special_source(action_binding_plan.get('special_contexts'),verb_rows)
        if len(action_binding_plan.get('lockout_masks',[]))!=3 or any(type(v) is not int or not 0<=v<1<<64 for v in action_binding_plan['lockout_masks']):
            raise SystemExit('Invalid phase lockout masks')
        for name in ('ui_pause_hash','menu_pause_hash','input_off_hash'):
            value=action_binding_plan.get(name)
            if type(value) is not int or not 0<value<1<<32:raise SystemExit('Invalid explicit UI pause row')
        offset=action_binding_plan['lockout_offset']
        if type(offset) is not int or not 0<=offset<=65536:raise SystemExit('Invalid lockout field')
        if len(output_sites)!=1:raise SystemExit('Gameplay bindings require exactly one verified publisher')
        if next(iter(output_sites.values()))['fields']!=action_binding_plan['fields']:raise SystemExit('Action field order mismatch')
        for row in action_binding_plan['masks']:
            if len(row)!=9 or any(type(v) is not int or not 0<=v<1<<64 for v in row):raise SystemExit('Invalid action masks')
        if len(action_binding_plan['masks'])!=3:raise SystemExit('Expected three action phases')
        context_jump=action_binding_plan.get('jump_masks',{})
        allowed_contexts={'FOOT','CLIMB','SWIM','AIR','GLIDE','BOW','RIDING','MENU','DIALOGUE'}
        if not isinstance(context_jump,dict) or not set(context_jump)<=allowed_contexts:
            raise SystemExit('Unknown contextual Jump masks')
        for phases in context_jump.values():
            if not isinstance(phases,list) or len(phases)!=3 or any(type(v) is not int or not 0<=v<1<<64 for v in phases):
                raise SystemExit('Contextual Jump requires three uint64 phase masks')
    version = metadata['version'].replace('.', '')
    table = patches.load(sdk / f'recomp/patches-v{version}.txt', mods)
    patches.check(table, mod, 'main')

    class Observer(gen.Emitter):
        def function(self, entry, end):
            lines, refs = super().function(entry, end)
            if entry in controller_sites:
                return observed_controller(lines, entry), refs
            if entry in output_sites:
                probe = output_sites[entry]
                return observed_outputs(lines, entry, probe['fields'], a.allow_mask_filter, a.allow_action_channel,
                                        probe.get('position'), player_state=bool(player_state),
                                        gameplay_bindings=a.gameplay_bindings), refs
            if entry in state_sites:
                probe = state_sites[entry]
                return observed_state(lines, entry, probe['field'], probe['size'], probe.get('returned'),
                                      interaction=a.gameplay_bindings and entry==interaction_site), refs
            if entry in scalar_sites:
                return observed_function(lines, entry, scalar=True), refs
            return (observed_function(lines, entry) if entry in sites else lines), refs

    kwargs = dict(patched=table.for_module('main', base), spins=table.spins_for('main', base))
    plain = gen.Emitter(mod, set(absolute), **kwargs)
    guard_functions=set();observer_module=mod
    if a.gameplay_bindings:
        import input_guard_plan
        observer_module,guard_functions=input_guard_plan.apply(mod,action_binding_plan['guard_split'],absolute)
        if any(base+s['offset'] in kwargs['patched'] for s in action_binding_plan['guard_split']['sites']):
            raise SystemExit('Guard split overlaps an installed patch')
    observer = Observer(observer_module, set(absolute), **kwargs)
    cc = sdk / 'llvm/bin/clang'
    common = ['--target=x86_64-linux-gnu', f'--sysroot={sdk / "llvm/sysroot"}', '-march=x86-64-v3',
              '-fno-strict-aliasing', '-I', str(sdk / 'runtime'), '-I', str(gen_dir), '-I', str(source_snapshot)]
    replacements = {}
    for path in sorted((gen_dir / 'main').glob('main_*.c')):
        if path.name == 'main_table.c':
            continue
        text = path.read_text()
        fns = [int(m, 16) for m in re.findall(r'^void f_([0-9a-f]+)\(Cpu \*c\) \{', text, re.M)]
        if not set(fns) & (sites.keys() | controller_sites | output_sites.keys() | state_sites.keys() | scalar_sites | guard_functions):
            continue
        number = int(path.stem.split('_')[1])
        bounds = [(entry, ends[entry]) for entry in fns]
        baseline = gen.chunk_text('main', number, plain, bounds)
        if baseline != text:
            raise SystemExit(f'{path.name}: baseline differs from install; use its matching SDK')
        source = work / path.name
        source.write_text(gen.chunk_text('main', number, observer, bounds))
        obj = source.with_suffix('.o')
        subprocess.run([str(cc), *common, '-O1', '-Wno-return-type', '-Wno-unused-label', '-fvisibility=hidden',
                        '-c', str(source), '-o', str(obj)], check=True)
        replacements[f'main/{obj.name}'] = obj
        print(f'Regenerated and compiled {path.name}', flush=True)
    # Verify every reused object against the installer's own manifest before linking.
    objects = []
    manifest = old_build / 'obj/objects.sha256'
    hashes = {line.split()[3]: line.split()[0] for line in manifest.read_text().splitlines() if len(line.split()) == 4}
    ninja = (old_build / 'build.ninja').read_text()
    link = next(line for line in ninja.splitlines() if ': link_lib ' in line)
    for value in link.split(': link_lib ', 1)[1].split(' | ', 1)[0].split():
        old = Path(value)
        relative = old.relative_to(old_build / 'obj').as_posix()
        if relative not in hashes or hashlib.sha256(old.read_bytes()).hexdigest() != hashes[relative]:
            raise SystemExit(f'Object integrity check failed: {relative}')
        objects.append(replacements.get(relative, old))
    logger = work / 'input_query_logger.o'
    subprocess.run([str(cc), *common, '-O2', '-Wall', '-Wextra', '-c', str(source_snapshot / 'input_query_logger.c'),
                    '-o', str(logger)], check=True)
    extra = []
    runtime_lib=sdk/'runtime/libtotkrt.a'
    if a.gameplay_bindings:
        overlay=source_snapshot/'runtime'
        headers=work/'headers'
        shutil.copytree('/usr/include/SDL3',headers/'SDL3',dirs_exist_ok=True)
        # Copies of authored sources compile against the install's headers, preserving the archive's UI/renderer ABI.
        include=['-I',str(overlay),'-I',str(sdk/'runtime'),'-I',str(gen_dir),'-I',str(source_snapshot),
                 '-I',str(sdk/'runtime/third_party/imgui'),'-I',str(sdk/'runtime/deps/include'),'-I',str(sdk/'runtime/deps/include/vma'),'-I',str(headers)]
        base_flags=['--target=x86_64-linux-gnu',f'--sysroot={sdk/"llvm/sysroot"}','-march=x86-64-v3',
                    '-fno-strict-aliasing',*include]
        archive_objects=[]
        for rel in ('input.c','input_actions.c','cfg.c','ui/binds_page.cpp'):
            src=overlay/rel;obj=work/(src.stem+'.o')
            flags=['-std=c++17','-fno-exceptions','-fno-rtti'] if src.suffix=='.cpp' else []
            subprocess.run([str(cc),*base_flags,*flags,'-O2','-Wall','-Wextra','-c',str(src),'-o',str(obj)],check=True)
            archive_objects.append(obj)
        runtime_lib=work/'libtotkrt-actions.a';shutil.copy2(sdk/'runtime/libtotkrt.a',runtime_lib)
        subprocess.run([str(sdk/'llvm/bin/llvm-ar'),'rcs',str(runtime_lib),*map(str,archive_objects)],check=True)
        for rel in ('input_actions_bridge.c','input_actions_test_devices.c'):
            src=source_snapshot/'tools'/rel;obj=work/(src.stem+'.o')
            subprocess.run([str(cc),*base_flags,'-O2','-Wall','-Wextra','-c',str(src),'-o',str(obj)],check=True);extra.append(obj)
        config=work/'input_actions_config.c'
        rows=action_binding_plan['masks'];owned=[]
        for phase,row in enumerate(rows):
            mask=0
            for v in row:mask|=v
            for phases in context_jump.values():mask|=phases[phase]
            owned.append(mask)
        contract=':'.join(hex(v) for v in [context_flags['global'],*context_flags['hashes']])
        jump_function='uint64_t input_actions_jump_mask(int kind,unsigned phase){switch(kind){'
        for name,phases in context_jump.items():
            jump_function+='case IC_'+name+':{static const uint64_t v[3]={'+','.join(str(v)+'ull' for v in phases)+'};return v[phase];}'
        jump_function+='default:return masks[phase][IA_JUMP];}}\n'
        config.write_text('// '+gen.GENERATED_MARKER+'\n#include <stdint.h>\n#include "input_actions.h"\n'+
            'static const uint64_t masks[3][9]={'+','.join('{'+','.join(str(v)+'ull' for v in row)+'}' for row in rows)+'};\n'+
            'static const uint64_t owned[3]={'+','.join(str(v)+'ull' for v in owned)+'};\n'+
            'const uint64_t *input_actions_virtual_masks(unsigned phase){return masks[phase];}\n'+
            'uint64_t input_actions_virtual_owned(unsigned phase){return owned[phase];}\n'+jump_function+special_function+
            f'uint32_t input_actions_lockout_offset(void){{return {offset};}}\n'+
            f'int input_actions_ride_verb(void){{return {action_binding_plan["ride_verb"]};}}\n'+
            f'uint64_t input_actions_guard_mask(void){{return {1<<action_binding_plan["guard_split"]["bit"]}ull;}}\n'+
            f'uint64_t input_actions_guard_source(void){{return {1<<action_binding_plan["guard_split"]["from_bit"]}ull;}}\n'+
            'uint64_t input_actions_lockout_mask(unsigned phase){static const uint64_t m[3]={'+','.join(str(v)+'ull' for v in action_binding_plan['lockout_masks'])+'};return m[phase];}\n'+
            f'uint32_t input_actions_off_hash(void){{return {action_binding_plan["input_off_hash"]}u;}}\n'+
            f'uint32_t input_actions_ui_pause_hash(void){{return {action_binding_plan["ui_pause_hash"]}u;}}\n'+
            f'uint32_t input_actions_menu_pause_hash(void){{return {action_binding_plan["menu_pause_hash"]}u;}}\n'+
            f'const char *input_context_default_contract(void){{return "{contract}";}}\n'+
            f'const char *input_context_default_block(void){{return "{context_flags["block_hash"]:#x}";}}\n')
        obj=work/'input_actions_config.o';subprocess.run([str(cc),*base_flags,'-O2','-c',str(config),'-o',str(obj)],check=True);extra.append(obj)
    if player_state:
        config = work / 'input_player_state_config.c'
        config.write_text(player_state_plan.source(player_state, mod, next(iter(output_sites)), gen.GENERATED_MARKER))
        for src in (source_snapshot / 'input_player_state.c', config):
            obj = work / (src.stem + '.o')
            subprocess.run([str(cc), *common, '-O2', '-Wall', '-Wextra', '-Werror', '-c', str(src), '-o', str(obj)], check=True)
            extra.append(obj)
    if a.allow_action_channel:
        channel = work / 'input_action_channel.o'
        subprocess.run([str(cc), *common, '-O2', '-Wall', '-Wextra', '-c', str(source_snapshot / 'input_action_channel.c'),
                        '-o', str(channel)], check=True)
        extra.append(channel)
    if context_flags:
        context = work / 'input_context_flags.o'
        subprocess.run([str(cc), *common, '-O2', '-Wall', '-Wextra', '-c', str(source_snapshot / 'input_context_flags.c'),
                        '-o', str(context)], check=True)
        extra.append(context)
    rsp = work / 'link.rsp'
    rsp.write_text('\n'.join(json.dumps(str(p)) for p in [*objects, logger, *extra]))
    subprocess.run([str(cc), *common, '-fuse-ld=lld', '-no-pie', '-Wl,-z,noexecstack', '-Wl,-rpath,$ORIGIN',
                    '-Wl,--wrap=input_poll', '@' + str(rsp), '-Wl,--whole-archive', str(runtime_lib),
                    '-Wl,--no-whole-archive', '-L', str(sdk / 'runtime/deps/lib'), '-lvulkan', '-lSDL3',
                    '-l:libopus.a', '-l:libvpx.a', '-l:libzstd.a', '-l:libstdc++.a', '-lm', '-lpthread', '-ldl',
                    '-o', str(out / 'totk')], check=True)
    # Writable state lives in the research folder. Large immutable assets remain in the install.
    for path in install.iterdir():
        if path.name in ('exefs', 'romfs', 'libSDL3.so.0', 'totk_xlate', 'totk_xlate.id', 'romfs-index.bin') or path.name.startswith('romfs-patches'):
            dst = out / path.name
            if not dst.exists():
                dst.symlink_to(path, target_is_directory=path.is_dir())
    for name in ('totk.ini', 'profile'):
        dst = out / name
        if not dst.exists():
            if (install / name).is_dir():
                shutil.copytree(install / name, dst)
            else:
                shutil.copy2(install / name, dst)
    for name in ('shader-cache', 'pipeline-cache.bin', 'pipeline-keys.bin', 'shader-answers.bin', 'shader-programs.bin',
                 'shader-prepared.bin', 'seed-answers.bin', 'seed-programs.bin'):
        dst = out / name
        if not dst.exists() and (install / name).exists():
            if (install / name).is_dir():
                shutil.copytree(install / name, dst)
            else:
                shutil.copy2(install / name, dst)
    report = {'marker': gen.GENERATED_MARKER, 'version': metadata['version'], 'release': metadata['release'],
              'experimental_mask_filter': a.allow_mask_filter,
              'experimental_action_channel': a.allow_action_channel,
              'context_flags': context_flags,
              'player_state': player_state,
              'gameplay_bindings': a.gameplay_bindings,
              'controller_event_filter': True,
              'research_sources': research_sources,
              'build_id': mod.nso.build_id, 'baseline_verified': True,
              'sites': [{'address': hex(site), 'field': hex(field), 'meaning': 'unverified bit query'}
                        for site, field in sites.items()] +
                       [{'address': hex(site), 'meaning': 'named controller masks'} for site in controller_sites] +
                       [{'address': hex(site), 'meaning': 'observed output masks', 'fields': probe['fields'],
                         'position': probe.get('position')} for site, probe in output_sites.items()] +
                       [{'address': hex(site), 'meaning': 'object state', 'field': p['field'], 'size': p['size'],
                         'returned': p.get('returned')}
                        for site, p in state_sites.items()] +
                       [{'address': hex(site), 'meaning': 'scalar return'} for site in sorted(scalar_sites)],
              'replaced_objects': list(replacements)}
    (out / 'probe-sites.json').write_text(json.dumps(report, indent=2) + '\n')
    print(f'Built {out / "totk"}: {len(sites)} candidate queries, {len(controller_sites)} named-controller probes, '
          f'{len(output_sites)} output publishers, {len(state_sites)} state callbacks, {len(scalar_sites)} scalar returns. '
          'Original install unchanged.', flush=True)


if __name__ == '__main__':
    main()
