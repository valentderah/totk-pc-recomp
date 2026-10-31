"""Extract the Switch system shared fonts (nn::pl) from the user's firmware into build/extracted/firmware/shared-fonts.

TotK needs them only with Simplified Chinese, Korean or Traditional Chinese text: those font archives fall back to the
Standard shared font (runtime/ov_system.c). The fonts come from the system data NCAs 0100000000000810..814 of a
firmware NCA directory (an unpacked firmware, or an emulator's installed `registered` directory such as
%APPDATA%\\Ryujinx\\bis\\system\\Contents\\registered). The keys file named in config.local.toml decrypts them; key
values are never printed. Every NCA FS header, all six IVFC levels and each BFTTF header are verified
(tools/vendor/macnativegames/extract_shared_fonts.py).

Runs under .venv-py314:  python tools\\extract_fonts.py --firmware <dir>   (or set input.firmware in config.local.toml)
"""
import argparse
import sys
import tomllib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
VENDOR = ROOT / 'tools' / 'vendor' / 'macnativegames'
# The vendored scripts import each other by bare module name, so their directory goes on the path before importing them.
sys.path.insert(0, str(VENDOR))
import extract_shared_fonts  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--config', type=Path, default=ROOT / 'config.local.toml')
    parser.add_argument('--firmware', type=Path, help='firmware NCA directory (default: input.firmware of the config)')
    parser.add_argument('--out', type=Path, help='default: <work dir>/extracted/firmware/shared-fonts')
    args = parser.parse_args()
    config = tomllib.loads(args.config.read_text(encoding='utf-8'))
    firmware = args.firmware or config['input'].get('firmware')
    if not firmware:
        sys.exit('error: give --firmware <NCA directory> or set input.firmware in config.local.toml')
    firmware = Path(firmware).expanduser()
    out = args.out or ROOT / config.get('work', {}).get('dir', 'build') / 'extracted' / 'firmware' / 'shared-fonts'
    if (out / 'manifest.json').is_file():
        print(f'{out} already holds verified shared fonts')
        return
    out.parent.mkdir(parents=True, exist_ok=True)
    # The vendored script's own command line; the keys path goes in, the key values stay in that process.
    sys.argv = ['extract_shared_fonts.py', '--keys', str(Path(config['input']['keys']).expanduser()), '--firmware', str(firmware),
                '--output', str(out)]
    extract_shared_fonts.main()


if __name__ == '__main__':
    try:
        main()
    except ValueError as error:
        sys.exit(f'error: {error}')
