"""Run a bounded scripted control experiment with its own copied profile and recording folder."""
import argparse
import datetime
import hashlib
import json
import os
import signal
import shutil
import subprocess
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def profile_fingerprint(profile):
    digest = hashlib.sha256()
    for path in sorted(profile.rglob('*')):
        if path.is_symlink():
            raise ValueError('Profile snapshots cannot contain symlinks')
        if path.is_file():
            digest.update(path.relative_to(profile).as_posix().encode() + b'\0')
            digest.update(hashlib.sha256(path.read_bytes()).digest())
    return digest.hexdigest()


def validate_jump_contract(contract, probes):
    """Require a single action bit and distinct output fields on a fingerprinted publisher in this build."""
    if not probes.get('experimental_action_channel'):
        raise ValueError('build was not enabled for an action channel')
    site, pressed, held, released, bit = (int(s, 0) for s in contract.split(':'))
    fields = [pressed, held, released]
    if not 0 < bit < 1 << 64 or bit & (bit - 1) or len(set(fields)) != 3:
        raise ValueError('expected three distinct fields and one uint64 bit')
    if not any(int(p['address'], 16) == site and set(fields) <= set(p.get('fields', [])) for p in probes['sites']):
        raise ValueError('publisher or fields are absent from verified build probes')
    return ':'.join(hex(v) for v in (site, *fields, bit))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', type=Path, default=ROOT / 'build/input-logger/run')
    ap.add_argument('--script', type=Path, required=True)
    ap.add_argument('--seconds', type=float, default=10)
    ap.add_argument('--fps-limit', type=int, help='explicit frame-rate cap for controlled experiments, 30..240')
    ap.add_argument('--deadline', type=float, default=90)
    ap.add_argument('--profile-source', type=Path, help='snapshot source; defaults to research run/profile')
    ap.add_argument('--video-driver', choices=['auto', 'wayland', 'x11'], default='auto')
    ap.add_argument('--action-test-script', type=Path, help='bounded physical key/pad fixture for gameplay-binding builds')
    ap.add_argument('--real-input', action='store_true', help='allow physical input in addition to the script')
    ap.add_argument('--mask-clear', help='research only: clear publisher bits, site:bits (integer literals)')
    ap.add_argument('--watch-output', help='trace bounded reads of a matching publisher output under GDB: site:field')
    ap.add_argument('--watch-value-mask', type=lambda s: int(s, 0), default=0xffffffffffffffff,
                    help='arm the output watch only when any of these bits is set')
    ap.add_argument('--watch-reads', type=int, default=12, help='maximum watchpoint traces, 1..64')
    ap.add_argument('--action-channel', '--jump-channel', dest='jump_channel',
                    help='research single-action contract: site:pressed-field:held-field:released-field:bit-mask')
    ap.add_argument('--action-script', '--jump-script', dest='jump_script', type=Path,
                    help='separate virtual action pulses: world-ms duration-ms per line')
    ap.add_argument('--action-key', '--jump-key', dest='jump_key', help='independent keyboard provider, requires --real-input')
    ap.add_argument('--action-phases', default='pressed,held,released',
                    help='comma-separated native phases to replace: pressed,held,released')
    ap.add_argument('--shot-interval', type=float, default=0, help='game-clock seconds between frame captures; 0 disables')
    ap.add_argument('--startup-shots', action='store_true', help='capture every 150 presents through present 6000, including loading')
    a = ap.parse_args()
    phase_names = {name.strip().lower() for name in a.action_phases.split(',')}
    if not phase_names or not phase_names <= {'pressed', 'held', 'released'}:
        ap.error('--action-phases must select pressed, held and/or released')
    phase_mask = sum(1 << i for i, name in enumerate(('pressed', 'held', 'released')) if name in phase_names)
    if not a.jump_channel and phase_mask != 7:
        ap.error('--action-phases requires --action-channel')
    if a.fps_limit is not None and not 30 <= a.fps_limit <= 240:
        ap.error('--fps-limit must be between 30 and 240')
    if not 0 < a.watch_value_mask < 1 << 64 or not 1 <= a.watch_reads <= 64:
        ap.error('watch mask must be a nonzero uint64 and trace count must be 1..64')
    run, script = a.run.resolve(), a.script.resolve()
    if not run.is_relative_to(ROOT / 'build') or not (run / 'probe-sites.json').is_file():
        ap.error('--run must name an isolated research build under build/')
    probes = json.loads((run / 'probe-sites.json').read_text())
    if a.action_test_script and not probes.get('gameplay_bindings'):
        ap.error('--action-test-script requires a gameplay-binding build')
    if a.mask_clear and not probes.get('experimental_mask_filter'):
        ap.error('--mask-clear requires a build explicitly enabled for mask experiments')
    if a.mask_clear:
        try:
            site, bits = (int(s, 0) for s in a.mask_clear.split(':'))
            if not 0 < bits < 1 << 64 or not any(int(p['address'], 16) == site and p.get('fields')
                                               for p in probes['sites']):
                raise ValueError()
        except (ValueError, KeyError):
            ap.error('--mask-clear must be a verified publisher address and nonzero uint64 mask: site:bits')
        a.mask_clear = f'{site:#x}:{bits:#x}'
    if bool(a.jump_channel) != bool(a.jump_script or a.jump_key):
        ap.error('--jump-channel requires a separate --jump-script or --jump-key provider and vice versa')
    if a.jump_key and not a.real_input:
        ap.error('--jump-key requires --real-input')
    if a.jump_channel:
        try:
            a.jump_channel = validate_jump_contract(a.jump_channel, probes)
        except (ValueError, KeyError):
            ap.error('--jump-channel requires a verified publisher, three distinct fields and one uint64 bit')
    watch = None
    if a.watch_output:
        try:
            site, field = (int(s, 0) for s in a.watch_output.split(':'))
            if not any(int(p['address'], 16) == site and field in p.get('fields', []) for p in probes['sites']):
                raise ValueError()
            watch = site, field
        except (ValueError, KeyError):
            ap.error('--watch-output must identify a verified publisher and pointer field: site:field')
    if not 0 < a.seconds <= 60 or not 0 < a.deadline <= 300:
        ap.error('game duration must be in (0,60], wall deadline in (0,300]')
    if a.shot_interval < 0 or (a.shot_interval and (a.shot_interval < 0.1 or a.seconds / a.shot_interval > 200)):
        ap.error('frame interval must be at least 0.1s and request no more than 200 captures')
    if a.startup_shots and a.shot_interval:
        ap.error('--startup-shots and --shot-interval use different clocks; choose one')
    profile = (a.profile_source or run / 'profile').resolve()
    if not profile.is_dir():
        ap.error('profile source missing')
    before = profile_fingerprint(profile)
    timestamp = datetime.datetime.now().astimezone().strftime('%Y%m%d-%H%M%S-%f')
    folder = ROOT / 'build/input-logger/experiments' / timestamp
    folder.mkdir(parents=True)
    shutil.copytree(profile, folder / 'profile')
    shutil.copy2(script, folder / 'input.txt')
    if a.jump_script:
        shutil.copy2(a.jump_script, folder / 'jump-input.txt')
    if a.action_test_script:
        shutil.copy2(a.action_test_script, folder / 'action-devices.txt')
    env = dict(os.environ, TOTK_PROFILE_DIR=str(folder / 'profile'), TOTK_INPUT_SCRIPT=str(folder / 'input.txt'),
               TOTK_QUERY_LOG=str(folder / 'queries.jsonl'))
    if probes.get('controller_event_filter'):
        env.setdefault('TOTK_QUERY_CONTROLLERS', '0')
    if a.fps_limit is not None:
        env['TOTK_FPS_LIMIT'] = str(a.fps_limit)
    if not a.real_input:
        env['TOTK_INPUT'] = 'none'
    for key in ('TOTK_INPUT_RECORD', 'TOTK_EXIT_AT', 'TOTK_SHOTS', 'TOTK_SHOTS_DIR', 'TOTK_TEST_PAD'):
        env.pop(key, None)
    env.pop('TOTK_MASK_CLEAR', None)
    env.pop('TOTK_CONTEXT_FLAGS', None)
    env.pop('TOTK_CONTEXT_BLOCK_HASH', None)
    if probes.get('context_flags'):
        contract = probes['context_flags']
        env['TOTK_CONTEXT_FLAGS'] = ':'.join(hex(v) for v in [contract['global'], *contract['hashes']])
        if contract.get('block_hash') is not None:
            env['TOTK_CONTEXT_BLOCK_HASH'] = hex(contract['block_hash'])
    for key in ('TOTK_JUMP_CHANNEL', 'TOTK_JUMP_SCRIPT', 'TOTK_JUMP_KEY'):
        env.pop(key, None)
    env.pop('TOTK_ACTION_PHASES', None)
    env.pop('TOTK_ACTION_TEST_SCRIPT', None)
    if a.action_test_script:
        env['TOTK_ACTION_TEST_SCRIPT'] = str(folder / 'action-devices.txt')
        env['TOTK_ASSUME_FOCUS'] = '1'
    if a.mask_clear:
        env['TOTK_MASK_CLEAR'] = a.mask_clear
    if a.jump_channel:
        env['TOTK_JUMP_CHANNEL'] = a.jump_channel
        env['TOTK_ACTION_PHASES'] = str(phase_mask)
        if a.jump_script:
            env['TOTK_JUMP_SCRIPT'] = str(folder / 'jump-input.txt')
        if a.jump_key:
            env['TOTK_JUMP_KEY'] = a.jump_key
    driver = a.video_driver
    if driver == 'auto' and env.get('WAYLAND_DISPLAY'):
        driver = 'wayland'
    if driver != 'auto':
        env['SDL_VIDEODRIVER'] = driver
    if a.shot_interval:
        env['TOTK_SHOTS'] = f'w0-{a.seconds}:{a.shot_interval}'
        env['TOTK_SHOTS_DIR'] = str(folder / 'shots')
    if a.startup_shots:
        env['TOTK_SHOTS'] = '0-6000:150'
        env['TOTK_SHOTS_DIR'] = str(folder / 'shots')
    command = [str(run / 'totk'), '--profile', str(folder / 'profile'), '--seconds', str(a.seconds),
               '--shot', str(folder / 'last-frame.png')]
    if watch:
        site, field = watch
        # The target runs as GDB's child, respecting ptrace_scope without a system-policy change. The scalar callback
        # ABI is x86-64 SysV: rdi site, rsi object, rdx field, rcx caller and r8 result. Break before it clobbers registers.
        commands = folder / 'read-watch.gdb'
        commands.write_text(f'''set pagination off
set confirm off
handle SIG38 nostop noprint pass
handle SIG39 nostop noprint pass
handle SIGSEGV nostop noprint pass
break *input_output_observe
condition 1 $rdi == {site} && $rdx == {field} && ($r8 & {a.watch_value_mask}) != 0
run
set $watched = *(unsigned long long*)($rsi + {field})
printf "Publisher output address: %p\\n", $watched
disable 1
rwatch -location *(unsigned long long*)$watched
set $hits = 0
commands 2
silent
set $hits = $hits + 1
printf "Output read %d\\n", $hits
bt 5
if $hits >= {a.watch_reads}
disable 2
end
continue
end
continue
''')
        command = ['gdb', '-q', '-batch', '-x', str(commands), '--args', *command]
    with (run / 'totk').open('rb') as executable:
        executable_digest = hashlib.file_digest(executable, 'sha256').hexdigest()
    manifest = {'run': str(run), 'profile_source': str(profile), 'profile_source_sha256': before,
                'executable_sha256': executable_digest,
                'script_sha256': hashlib.sha256(script.read_bytes()).hexdigest(), 'seconds': a.seconds,
                'deadline_s': a.deadline, 'video_driver': driver, 'start_ns': time.monotonic_ns(),
                'real_input': a.real_input,
                'controller_events': not probes.get('controller_event_filter') or env.get('TOTK_QUERY_CONTROLLERS', '1') != '0',
                'action_test_script_sha256': hashlib.sha256((folder / 'action-devices.txt').read_bytes()).hexdigest() if a.action_test_script else None,
                'gameplay_bindings': {k: v for k, v in env.items() if k.startswith('TOTK_ACTION_') or k == 'TOTK_GAMEPLAY_ACTIONS'},
                'fps_limit_override': env.get('TOTK_FPS_LIMIT'),
                'startup_shots': a.startup_shots,
                'mask_clear': a.mask_clear, 'watch_output': a.watch_output, 'probe_sites': probes,
                'watch_value_mask': a.watch_value_mask if watch else None, 'watch_reads': a.watch_reads if watch else None,
                'jump_channel': a.jump_channel, 'jump_key': a.jump_key,
                'action_phase_mask': phase_mask if a.jump_channel else None,
                'jump_script_sha256': hashlib.sha256((folder / 'jump-input.txt').read_bytes()).hexdigest() if a.jump_script else None}
    (folder / 'experiment.json').write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Experiment: {folder}', flush=True)
    timed_out = False
    with (folder / 'game-output.log').open('w') as output:
        child = subprocess.Popen(command, cwd=run, env=env, stdout=output, stderr=subprocess.STDOUT,
                                 start_new_session=True)
        try:
            child.wait(timeout=a.deadline)
        except (subprocess.TimeoutExpired, KeyboardInterrupt):
            timed_out = True
            # The runtime did not reliably honor SIGTERM in a stalled startup. Bound the entire process group.
            os.killpg(child.pid, signal.SIGKILL)
            child.wait(timeout=5)
    manifest.update(exit_code=child.returncode, timed_out=timed_out, end_ns=time.monotonic_ns(),
                    source_profile_unchanged=before == profile_fingerprint(profile))
    (folder / 'experiment.json').write_text(json.dumps(manifest, indent=2) + '\n')
    markers = [{'type': 'marker', 'ns': manifest['start_ns'], 'label': 'scripted-experiment'},
               {'type': 'marker', 'ns': manifest['end_ns'], 'label': f'exit:{child.returncode}'}]
    (folder / 'markers.jsonl').write_text(''.join(json.dumps(m) + '\n' for m in markers))
    print(f'Exit {child.returncode}; timeout {timed_out}; source profile unchanged {manifest["source_profile_unchanged"]}', flush=True)
    if not manifest['source_profile_unchanged']:
        raise SystemExit('Source profile changed during experiment; investigate before another run')
    if child.returncode:
        raise SystemExit(1)


if __name__ == '__main__':
    main()
