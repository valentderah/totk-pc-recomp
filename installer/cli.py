"""Command line: python -m installer --base FILE --update FILE --keys FILE --lang CODE --dest DIR [--firmware DIR]
                                     [--root DIR] [--temp DIR] [--verify] [--dlss] [--dlssg] [--fsr]

A first install needs every option. --firmware names the Switch firmware folder (an unpacked firmware or an emulator's
Contents/registered); the setup extracts the system shared fonts from it when the language is Chinese, Korean or
Traditional Chinese. On a folder with totk-install.json (resume after a cancel or failure, or update with a newer
release) only --dest is needed: --lang defaults to the recorded language, and the game files are needed only when the
extraction has to run again. Prints JSON events on stdout (events.py). Exit code: 0 done, 1 failed, 2 bad command
line, 3 cancelled (Ctrl+C or Ctrl+Break).

--dlss and --dlssg download NVIDIA's DLSS upscaling and frame-generation libraries (nvngx_dlss.dll and
nvngx_dlssg.dll) from NVIDIA's DLSS SDK repository into the install folder on Windows with an NVIDIA RTX GPU (steps.py,
Dlss). --fsr downloads AMD's FSR loader and upscaler
(amd_fidelityfx_loader_vk.dll, amd_fidelityfx_upscaler_vk.dll) from AMD's FSR SDK repository into the install folder on
Windows, for scene_upscaler = fsr4; the libraries decide which GPUs get FSR 4 (steps.py, Fsr).
"""
import argparse
import signal
import sys
import time
from pathlib import Path

from . import layout as layouts
from . import state as statefile
from .events import Reporter
from .runner import Context, run_steps
from .steps import adopt_old_temp, all_steps, migrate_state

EXIT = {'ok': 0, 'failed': 1, 'cancelled': 3}


class Parser(argparse.ArgumentParser):
    def error(self, message):
        Reporter().error('cli.usage', {'detail': message})
        sys.exit(2)


def cancel(signum, frame):
    raise KeyboardInterrupt


def main(argv=None):
    ap = Parser(prog='python -m installer', description=__doc__.splitlines()[0])
    ap.add_argument('--base', type=Path, help='base game, XCI or NSP (or NSZ, XCZ: unpacked first)')
    ap.add_argument('--update', type=Path, help='the update NSP or NSZ: 1.1.0, 1.2.1 or 1.4.3')
    ap.add_argument('--keys', type=Path, help='prod.keys')
    ap.add_argument('--firmware', type=Path, help='Switch firmware folder (Contents/registered) for the shared fonts '
                                                   'of Chinese, Korean and Traditional Chinese text')
    ap.add_argument('--lang', help='game language, a Switch language code (en-US, en-GB, ja, de, ru, ...)')
    ap.add_argument('--dest', type=Path, required=True, help='install folder')
    ap.add_argument('--root', type=Path, help='release folder or repository (default: the folder above installer/)')
    ap.add_argument('--temp', type=Path, help='temp folder (default: build-cache/work in the install folder on Windows, '
                                              '.setup-temp there on Linux, build/install-temp in a repository)')
    ap.add_argument('--verify', action='store_true', help='hash every kept file, also those this release made')
    ap.add_argument('--dlss', action='store_true', help="download NVIDIA's nvngx_dlss.dll (NVIDIA RTX SDK license) next "
                                                      'to the game, for dlaa and the DLSS modes')
    ap.add_argument('--dlssg', action='store_true', help="download NVIDIA's nvngx_dlssg.dll (NVIDIA RTX SDK license) "
                                                        'next to the game, for DLSS frame generation')
    ap.add_argument('--fsr', action='store_true', help="download AMD's FSR loader and upscaler DLLs (AMD FSR SDK "
                                                     'license) next to the game, for scene_upscaler = fsr4')
    args = ap.parse_args(argv)

    # The window cancels with Ctrl+Break to the installer's process group (CREATE_NEW_PROCESS_GROUP), which also reaches
    # the tools it runs; Python's default for it ends the process without recording anything.
    for name in ('SIGBREAK', 'SIGTERM'):
        if hasattr(signal, name):
            signal.signal(getattr(signal, name), cancel)
    layout = layouts.find(args.root)
    dest = args.dest.resolve()
    temp = layouts.temp_for(layout, dest, args.temp)
    adopted = adopt_old_temp(layout, dest, temp)
    log = temp / 'logs' / time.strftime('install-%Y%m%d-%H%M%S.log')
    rep = Reporter(log_path=log)
    if adopted:
        rep.log(adopted)
    t0 = time.monotonic()
    try:
        state = statefile.load(dest)
    except statefile.StateError as e:
        rep.error(e.id, {'path': str(e.path)})
        rep.emit('end', result='failed', seconds=0)
        return 1
    ctx = Context(layout, dest, temp, rep, args, state=state, verify=args.verify)
    migrate_state(ctx)
    steps = all_steps()
    rep.emit('start', release=layout.release, mode=ctx.mode, dev=layout.dev, dest=str(dest), log=str(log),
             steps=[s.id for s in steps])
    result = run_steps(steps, ctx)
    rep.emit('end', result=result, seconds=round(time.monotonic() - t0, 1))
    rep.close()
    return EXIT[result]
