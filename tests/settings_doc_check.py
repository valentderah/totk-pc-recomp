"""Every setting the runtime knows is described: in totk.ini.example for players or in docs/settings.md for the rest.

    python tests/settings_doc_check.py

Fails when a name of known[] (runtime/cfg.c) is in neither file, when docs/settings.md lists a name the runtime does
not know, or when a default written in docs/settings.md differs from the one the code holds: the metas[] table of
runtime/cfg.c or an rt_knob(name, default) call. No game files, no build.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def known_names(cfg):
    start = cfg.index('static const char *const known[]')
    return set(re.findall(r'"([A-Z0-9_]+)"', cfg[start:cfg.index('};', start)]))


def code_defaults(cfg):
    """{name: default} of metas[] and of the rt_knob calls; metas[] wins, as it is where a default lives."""
    found = {}
    for path in (ROOT / 'runtime').rglob('*'):
        if path.suffix in ('.c', '.cpp', '.h'):
            text = path.read_text(encoding='utf-8', errors='replace')
            found.update(re.findall(r'rt_knob\("(?:TOTK_)?([A-Z0-9_]+)", *"([^"]*)"\)', text))
    found.update(re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)", (?:"[^"]*"|NULL)', cfg))
    return found


def main():
    cfg = (ROOT / 'runtime' / 'cfg.c').read_text(encoding='utf-8')
    example = (ROOT / 'totk.ini.example').read_text(encoding='utf-8')
    doc = (ROOT / 'docs' / 'settings.md').read_text(encoding='utf-8')
    known = known_names(cfg)
    in_example = {name.upper() for name in re.findall(r'^#? ?([a-z][a-z0-9_]*) = ', example, re.M)}
    rows = re.findall(r'^\| `([a-z0-9_<>]+)`(?: \(default ([^)]*)\))? \|(.*)$', doc, re.M)
    in_doc = {name.upper() for name, _, _ in rows if '<' not in name}
    # The row of the button bindings names each key in its text.
    for name, _, text in rows:
        if '<' in name:
            in_doc |= {key.upper() for key in re.findall(r'`(bind_[a-z_]+)`', text)}

    problems = [f'{name}: in runtime/cfg.c known[], described in neither totk.ini.example nor docs/settings.md'
                for name in sorted(known - in_example - in_doc)]
    problems += [f'{name}: in docs/settings.md, not in runtime/cfg.c known[]' for name in sorted(in_doc - known)]
    defaults = code_defaults(cfg)
    for name, written, _ in rows:
        code = defaults.get(name.upper())
        if code and written != f'`{code}`':
            problems.append(f'{name}: docs/settings.md says default {written or "none"}, the code says {code}')

    for problem in problems:
        print(problem)
    print(f'settings_doc_check: {len(known)} names, {len(known & in_example)} in totk.ini.example, '
          f'{len(known & in_doc)} in docs/settings.md, {len(problems)} problems')
    return 1 if problems else 0


if __name__ == '__main__':
    sys.exit(main())
