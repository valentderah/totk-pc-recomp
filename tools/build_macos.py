"""Build the native Apple Silicon runtime; see docs/macos.md."""
from macos.build import main
import subprocess

if __name__ == '__main__':
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        raise SystemExit(str(error)) from None
