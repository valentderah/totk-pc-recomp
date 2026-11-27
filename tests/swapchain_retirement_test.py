"""Reproduce swapchain replacement failure on a private game folder and a copied profile.

Run this in the signed-in interactive session. --expect invalid is the unchanged-runtime negative control; it stops
that process as soon as the layer catches an invalid retired-handle use. --expect recovered requires normal exit,
gameplay and a screenshot. Layer activation affects only the child process; no loader registry settings are written.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import time

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('--game', type=Path, required=True, help='private prepared folder containing totk.exe')
ap.add_argument('--layer', type=Path, help='failure-injection layer folder; required except with --expect ordinary')
ap.add_argument('--profile', type=Path, required=True, help='source profile; copied before the game starts')
ap.add_argument('--exefs', type=Path, required=True)
ap.add_argument('--romfs', type=Path, required=True)
ap.add_argument('--out', type=Path, required=True, help='new result directory, normally under build/')
ap.add_argument('--expect', choices=('invalid', 'recovered', 'ordinary'), required=True)
ap.add_argument('--validation-layer', type=Path, help='optional folder containing the Khronos validation manifest')
ap.add_argument('--seconds', type=float, default=20)
ap.add_argument('--timeout', type=float, default=180)
a = ap.parse_args()
for field in ('game', 'profile', 'exefs', 'romfs', 'out'):
    setattr(a, field, getattr(a, field).resolve())
if a.layer:
    a.layer = a.layer.resolve()
if a.expect != 'ordinary' and (not a.layer or not (a.layer / 'VkLayer_totk_test_swapchain_fail.json').is_file()):
    ap.error('--layer must contain VkLayer_totk_test_swapchain_fail.json')
if a.out == a.profile or a.out.is_relative_to(a.profile) or a.profile.is_relative_to(a.out):
    ap.error('the output and source profile must be separate trees')
if os.name == 'nt':
    tasklist = subprocess.check_output(['tasklist', '/FI', 'IMAGENAME eq totk.exe', '/NH'], text=True)
    if 'totk.exe' in tasklist.lower():
        ap.error('another totk.exe is running; run one game at a time')


def hashes(folder):
    return {str(p.relative_to(folder)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in folder.rglob('*') if p.is_file()}


a.out.mkdir(parents=True, exist_ok=False)
before = hashes(a.profile)
copy = a.out / 'profile'
shutil.copytree(a.profile, copy)
log = a.out / 'game.log'
shot = a.out / 'gameplay.png'
env = {k: v for k, v in os.environ.items() if not k.startswith('TOTK_')}
layer_paths = []
if a.expect != 'ordinary':
    layer_paths.append(str(a.layer))
    env['VK_INSTANCE_LAYERS'] = 'VK_LAYER_TOTK_test_swapchain_fail'
if a.validation_layer:
    layer_paths.append(str(a.validation_layer.resolve()))
    env['TOTK_VK_VALIDATION'] = '1'
if layer_paths:
    env['VK_LAYER_PATH'] = os.pathsep.join(layer_paths)
env.update(TOTK_ASSUME_FOCUS='1', TOTK_INPUT='none', TOTK_BACKGROUND_WINDOW='1', TOTK_FULLSCREEN='0',
           TOTK_WINDOW_SIZE='1280x720', TOTK_OUTPUT_RES='1280x720', TOTK_LANG='en-US',
           TOTK_FRAME_GEN='off',
           TOTK_INPUT_SCRIPT=str(ROOT / 'tools' / 'input' / 'load_save.txt'),
           TOTK_MENU_TEST_SET='1:VK_PRESENT=mailbox;600:VK_PRESENT=fifo;900:VK_PRESENT=immediate;1200:VK_PRESENT=fifo')
command = [str(a.game / 'totk.exe'), '--profile', str(copy), '--exefs', str(a.exefs), '--romfs', str(a.romfs),
           '--seconds', str(a.seconds), '--shot', str(shot)]
start = time.monotonic()
stop = None
with log.open('w', encoding='utf-8') as stream:
    proc = subprocess.Popen(command, cwd=a.game, env=env, stdout=stream, stderr=subprocess.STDOUT)
    while proc.poll() is None:
        text = log.read_text(encoding='utf-8', errors='replace')
        if a.expect == 'invalid' and 'swapchain-test: INVALID:' in text:
            stop = 'negative control reproduced invalid retired-handle use'
            proc.kill()
            break
        if time.monotonic() - start > a.timeout:
            stop = 'wall timeout'
            proc.kill()
            break
        time.sleep(0.1)
    code = proc.wait()
text = log.read_text(encoding='utf-8', errors='replace')
report = dict(expect=a.expect, runtime_exit_code=code, stopped=stop,
              wall_seconds=round(time.monotonic() - start, 2),
              injected='swapchain-test: injected replacement failure' in text,
              invalid='swapchain-test: INVALID:' in text,
              retired_destroyed='swapchain-test: retired swapchain destroyed' in text,
              recovered='swapchain-test: recovered: fresh swapchain' in text,
              world_loaded='world clock starts' in text, screenshot=shot.is_file(),
              present_modes=[line for line in text.splitlines() if 'gfx: present mode ' in line],
              validation_requested=bool(a.validation_layer),
              validation_errors=text.count('vk validation error:'),
              validation_warnings=text.count('vk validation warning:'),
              validation_messages=sorted({line for line in text.splitlines() if 'vk validation ' in line}),
              original_profile_unchanged=before == hashes(a.profile),
              exe_sha256=hashlib.sha256((a.game / 'totk.exe').read_bytes()).hexdigest())
ok = report['original_profile_unchanged']
if a.expect == 'invalid':
    ok = ok and report['injected'] and report['invalid']
elif a.expect == 'recovered':
    ok = (ok and report['injected'] and code == 0 and not report['invalid'] and report['retired_destroyed'] and report['recovered'] and
          report['world_loaded'] and report['screenshot'])
else:
    modes = '\n'.join(report['present_modes'])
    ok = (ok and code == 0 and not report['injected'] and report['world_loaded'] and report['screenshot'] and
          all(f'present mode {mode} ' in modes for mode in ('mailbox', 'fifo', 'immediate')))
report['passed'] = bool(ok)
(a.out / 'result.json').write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
print(json.dumps(report, indent=2))
raise SystemExit(0 if ok else 1)
