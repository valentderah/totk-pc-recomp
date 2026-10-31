"""Run the native runtime's game-free GPU probes, one GPU process at a time."""
import argparse
import json
import os
import subprocess
import tempfile
import time
from pathlib import Path

from macos.build import BUILD, ROOT

GPU_PROBES = ('midclear', 'ds-clear', 'copy-alias', 'dispatch-chain', 'storage-view', 'texture-retire',
              'pool-retire', 'ubo-snapshot', 'linear-rt', 'depth-output', 'srgb-view', 'vertex-streams',
              'arg-epoch', 'tex-slots', 'layer-merge', 'cube-merge', 'compressed-alias', 'upload-batch',
              'ub-rename', 'drv-c0', 'widen', 'winding', 'blockrt')


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--binary', type=Path, default=BUILD / 'probes-v143/totk-native')
    ap.add_argument('--gpu', nargs='*', choices=GPU_PROBES, default=list(GPU_PROBES))
    args = ap.parse_args(argv)
    binary = args.binary.resolve()
    logs = binary.parent / 'checks' / time.strftime('%Y%m%d-%H%M%S')
    logs.mkdir(parents=True, exist_ok=True)
    (logs / 'build').mkdir(exist_ok=True)  # native save probe's disposable profile parent
    results = []
    with tempfile.TemporaryDirectory(dir=logs, prefix='profile-') as profile:
        env = dict(os.environ, TOTK_PROFILE_DIR=profile, TOTK_NATIVE_4K144='0', TOTK_HEADLESS='1',
                   MTL_DEBUG_LAYER='1', MTL_DEBUG_LAYER_ERROR_MODE='assert')
        for probe in ['selftest', 'probe-policy', 'probe-clock', 'probe-pool-cache', 'probe-save', 'probe-warm-pipelines', 'probe-attachment-ledger', 'probe-attachment-feedback',
                      *[f'probe-gpu-{name}' for name in args.gpu]]:
            log = logs / f'{probe}.log'
            with log.open('w') as output:
                try:
                    r = subprocess.run([str(binary), f'--{probe}'], cwd=logs, env=env,
                                       stdout=output, stderr=subprocess.STDOUT, timeout=120)
                    code = r.returncode
                except subprocess.TimeoutExpired:
                    code = 124
            reports = []
            for line in log.read_text().splitlines():
                try:
                    reports.append(json.loads(line))
                except ValueError:
                    pass
            passed = code == 0 and reports and reports[-1].get('status') == 'pass'
            results.append({'probe': probe, 'passed': bool(passed), 'exit': code, 'reports': reports})
            print(f'{probe}: {"PASS" if passed else "FAIL"}', flush=True)
            if not passed:
                print(log.read_text()[-4000:], flush=True)
    (logs / 'results.json').write_text(json.dumps(results, indent=2) + '\n')
    return 0 if all(r['passed'] for r in results) else 1


if __name__ == '__main__':
    raise SystemExit(main())
