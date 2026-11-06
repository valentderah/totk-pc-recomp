"""Launch the research build and record terminal annotations alongside its input-query log."""
import argparse
import datetime
import hashlib
import json
import os
import subprocess
import time
import shutil
from pathlib import Path
from run_control_experiment import profile_fingerprint, validate_jump_contract

ROOT = Path(__file__).resolve().parents[1]


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--run', type=Path, default=ROOT / 'build/input-logger/run')
    ap.add_argument('--profile-source', type=Path, help='separate baseline profile under build/; copied for each session')
    ap.add_argument('--jump-profile', type=Path, help='explicit local experimental Jump binding profile under build/')
    args = ap.parse_args()
    run = args.run.resolve()
    if not run.is_relative_to(ROOT / 'build') or not (run / 'probe-sites.json').is_file():
        ap.error('Research build missing; run tools/build_input_logger.py first')
    probes = json.loads((run / 'probe-sites.json').read_text())
    jump = None
    if args.jump_profile:
        if not args.jump_profile.resolve().is_relative_to(ROOT / 'build'):
            ap.error('--jump-profile must be under build/')
        jump = json.loads(args.jump_profile.read_text())
        if jump['build_id'] != probes['build_id']:
            ap.error('Jump profile build ID differs from research executable')
        try:
            jump['channel'] = validate_jump_contract(jump['channel'], probes)
        except (ValueError, KeyError) as error:
            ap.error(f'Invalid Jump profile: {error}')
        if not isinstance(jump.get('key'), str) or not jump['key']:
            ap.error('Jump profile must specify a keyboard key')
    # A copied profile isolates the test from the normal save directory, including inherited environment settings.
    profile = args.profile_source if args.profile_source is not None else run / 'profile'
    if profile.is_symlink() or not profile.resolve().is_relative_to(ROOT / 'build') or not profile.is_dir():
        ap.error('Research profile must be a real directory, not a symlink')
    profile = profile.resolve()
    session = run.parent / 'sessions' / datetime.datetime.now().astimezone().strftime('%Y%m%d-%H%M%S-%f')
    session.mkdir(parents=True)
    profile_hash = profile_fingerprint(profile)
    shutil.copytree(profile, session / 'profile')
    env = dict(os.environ, TOTK_QUERY_LOG=str(session / 'queries.jsonl'), TOTK_PROFILE_DIR=str(session / 'profile'))
    for key in ('TOTK_INPUT_SCRIPT', 'TOTK_INPUT_RECORD', 'TOTK_TEST_PAD', 'TOTK_EXIT_AT', 'TOTK_SHOTS',
                'TOTK_MASK_CLEAR', 'TOTK_JUMP_CHANNEL', 'TOTK_JUMP_SCRIPT', 'TOTK_JUMP_KEY', 'TOTK_CONTEXT_FLAGS',
                'TOTK_CONTEXT_BLOCK_HASH'):
        env.pop(key, None)
    env.pop('TOTK_ACTION_PHASES', None)
    env.pop('TOTK_ACTION_TEST_SCRIPT', None)
    if probes.get('context_flags'):
        contract = probes['context_flags']
        env['TOTK_CONTEXT_FLAGS'] = ':'.join(hex(v) for v in [contract['global'], *contract['hashes']])
        if contract.get('block_hash') is not None:
            env['TOTK_CONTEXT_BLOCK_HASH'] = hex(contract['block_hash'])
    if jump:
        env['TOTK_JUMP_CHANNEL'] = jump['channel']
        env['TOTK_JUMP_KEY'] = jump['key']
    env.pop('TOTK_INPUT', None)
    if probes.get('controller_event_filter'):
        env.setdefault('TOTK_QUERY_CONTROLLERS', '0')
    if os.environ.get('WAYLAND_DISPLAY') and 'SDL_VIDEODRIVER' not in env:
        env['SDL_VIDEODRIVER'] = 'wayland'
    with (run / 'totk').open('rb') as executable:
        executable_hash = hashlib.file_digest(executable, 'sha256').hexdigest()
    metadata = {'run': str(run), 'probe_sites': probes, 'jump_binding': jump,
                'executable_sha256': executable_hash, 'profile_source': str(profile), 'profile_source_sha256': profile_hash,
                'controller_events': not probes.get('controller_event_filter') or env.get('TOTK_QUERY_CONTROLLERS', '1') != '0'}
    (session / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
    with (session / 'game-output.log').open('w') as output, (session / 'markers.jsonl').open('w', buffering=1) as marks:
        child = subprocess.Popen([str(run / 'totk'), '--profile', str(session / 'profile')], cwd=run,
                                 env=env, stdout=output, stderr=subprocess.STDOUT)

        def mark(label):
            marks.write(json.dumps({'type': 'marker', 'ns': time.monotonic_ns(), 'label': label}) + '\n')

        mark('launch')
        print(f'Recording: {session}\nThe game uses a copy of your saves.\n', flush=True)
        if jump:
            print(f'Experimental independent Jump: {jump["key"]}. Original gameplay Jump bit is replaced; menu input remains native.', flush=True)
        print('Type a label and press Enter before each activity. Suggestions:\n'
              '  idle, jump, interact, inventory-confirm, inventory-back, dialogue\n'
              'Return focus to the game, wait two seconds, then repeat the action when practical. Menu layers need no fixed count.\n'
              'Type done to stop annotating, then close the game normally. Ctrl+C also leaves it running.', flush=True)
        try:
            while child.poll() is None:
                label = input('Label> ').strip()
                if label.lower() == 'done':
                    mark('annotations-finished')
                    break
                if label:
                    mark(label)
        except (EOFError, KeyboardInterrupt):
            mark('annotations-finished')
        print(f'Close the research game normally to finish. Logs: {session}', flush=True)
        try:
            result = child.wait()
            metadata.update(exit_code=result, source_profile_unchanged=profile_hash == profile_fingerprint(profile))
            (session / 'session.json').write_text(json.dumps(metadata, indent=2) + '\n')
            mark(f'exit:{result}')
            print(f'Game exited ({result}). Logs: {session}', flush=True)
        except KeyboardInterrupt:
            print(f'Game remains open. Logs: {session}', flush=True)


if __name__ == '__main__':
    main()
