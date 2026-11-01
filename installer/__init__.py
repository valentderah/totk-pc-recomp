"""The TotK installer: builds a playable folder from the player's own game files (docs/installer.md).

    python -m installer --base FILE --update FILE --keys FILE --lang CODE --dest DIR [--firmware DIR] [--root DIR]
                        [--temp DIR]

Modules: layout (where the bundled tools are, release or repository), events (JSON progress on stdout and the log
file), state (totk-install.json), runner (which steps run: resume and update), steps (the seven steps), ini (totk.ini),
cli (the command line). text/en.json holds the English text of every message id the events carry.

Standard library only, apart from what the extraction tools import (cryptography). Runs on Python 3.14.
"""
