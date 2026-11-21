"""Write the settings table of the settings window, totk-config (runtime/configurator/): which tab and group of the
in-game menu shows each setting, its label and description text ids, its kind of row, and the descriptions of the
settings the menu does not show.

    python tools/gen_configurator.py --out build/configurator_rows.c   # write the table
    python tools/gen_configurator.py --check                           # exit 1 when the menu has a row it cannot read

The table is never written by hand. This script reads the runtime's own sources:

- runtime/ui/menu.cpp: the tab functions (tab_screen ... tab_mods) and the row helpers they call (row_*, rows_*,
  mods_*). A setting row is a call whose first two arguments are "TOTK_<KEY>" and a UI_* label id: opt_toggle,
  opt_choice, opt_choice_list, opt_slider, binds_key_row, row_threads and row_set_or_not. opt_group(UI_*) starts a group.
  How such a row must be written for this script to read it: docs/configurator.md, "Adding a setting".
- runtime/ui/amiibo_page.cpp: the keys it sets (folder rows).
- runtime/cfg.c: known[] (every key) and metas[] (default, values; the window reads metas through cfg_meta itself).
- runtime/input_map.c: the actions of the BIND_* rows.
- docs/settings.md and totk.ini.example: the descriptions of the keys the menu does not show (the Expert tab).
- rt_knob("TOTK_<KEY>", "<default>") anywhere in runtime/: the defaults of keys outside metas[].

Labels and descriptions stay text ids: the window links runtime/ui_text.c and shows them in the menu language. Standard
library only.
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
# The menu's tabs that hold settings, in menu order (menu.cpp kTabNames), and the function that draws each. The window's
# tab enum (runtime/configurator/cfgr.h CR_TAB_*) has the same order, then Expert and Other keys.
TABS = [('SCREEN', 'tab_screen'), ('GAME', 'tab_game'), ('SOUND', 'tab_sound'), ('CONTROLS', 'tab_controls'),
        ('AMIIBO', None), ('MENU', 'tab_menu'), ('ADVANCED', 'tab_advanced'), ('MODS', 'tab_mods')]
# The calls that are setting rows, and the row kind each makes. Any other call with "TOTK_<KEY>", UI_* arguments is an
# error of --check: the window would miss the row.
ROW_CALLS = {'opt_toggle': 'TOGGLE', 'opt_choice': 'CHOICE', 'opt_choice_list': 'CHOICE', 'opt_slider': 'SLIDER',
             'binds_key_row': 'KEY', 'row_threads': 'CHOICE', 'row_set_or_not': 'CHOICE'}
HELPER = re.compile(r'\b((?:row|rows|mods)_\w+)\s*\(')


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def block(text, start_marker):
    """The text between the '{' after start_marker and its matching '}'."""
    open_at = text.index('{', text.index(start_marker))
    depth = 0
    for i in range(open_at, len(text)):
        depth += text[i] == '{'
        depth -= text[i] == '}'
        if not depth:
            return text[open_at + 1:i]
    raise ValueError(f'unbalanced braces after {start_marker}')


def strip_comments(code):
    """C code without // and /* */ comments; string and char literals stay as they are."""
    out, i, n = [], 0, len(code)
    while i < n:
        c = code[i]
        if c in '"\'':
            j = i + 1
            while j < n and code[j] != c:
                j += 2 if code[j] == '\\' else 1
            out.append(code[i:j + 1])
            i = j + 1
        elif code.startswith('//', i):
            i = code.find('\n', i)
            i = n if i < 0 else i
        elif code.startswith('/*', i):
            i = code.index('*/', i) + 2
        else:
            out.append(c)
            i += 1
    return ''.join(out)


def c_unescape(s):
    return bytes(s, 'utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def c_string(s):
    """s as a C string literal (UTF-8 bytes above ASCII as octal escapes, so the file stays ASCII)."""
    out = []
    for b in s.encode('utf-8'):
        c = chr(b)
        if c in '"\\':
            out.append('\\' + c)
        elif 32 <= b < 127:
            out.append(c)
        else:
            out.append(f'\\{b:03o}')
    return '"' + ''.join(out) + '"'


def functions(code):
    """{name: body} of the top-level functions of a C++ file."""
    out = {}
    for m in re.finditer(r'^(?:static |extern "C" )?(?:const )?(?:void|bool|int|char)\s*\*?\s*(\w+)\s*\([^;{]*\)\s*\{',
                         code, re.M):
        start = m.end() - 1
        depth = 0
        for i in range(start, len(code)):
            depth += code[i] == '{'
            depth -= code[i] == '}'
            if not depth:
                out[m.group(1)] = code[start + 1:i]
                break
    return out


def split_args(text):
    """The arguments of a call whose text starts after '(' (top-level commas), up to the closing ')'."""
    args, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == '"':
            j = i + 1
            while text[j] != '"':
                j += 2 if text[j] == '\\' else 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in '({[':
            depth += 1
        elif c in ')}]':
            if not depth:
                args.append(''.join(cur).strip())
                return args
            depth -= 1
        if c == ',' and not depth:
            args.append(''.join(cur).strip())
            cur = []
        else:
            cur.append(c)
        i += 1
    return args


def arrays(body):
    """{name: [element source]} of the array initializers 'name[...] = {...}' in a function body."""
    out = {}
    for m in re.finditer(r'(\w+)\[[^\]=]*\]\s*=\s*\{', body):
        out[m.group(1)] = split_args(body[m.end():].replace('}', ')', 1))
    return out


def literal(src):
    m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', src)
    return c_unescape(m.group(1)) if m else None


def name_of(src):
    """A choice name's source: ('id', UI_...), ('text', literal) or None when it is made at run time."""
    text = literal(src)
    if text is not None:
        return ('text', text)
    m = re.fullmatch(r'(?:menu_text\()?(UI_\w+)\)?', src)
    return ('id', m.group(1)) if m else None


def number(src):
    try:
        return float(src.rstrip('f'))
    except ValueError:
        return None


def walk(funcs, name, tab, state, rows, errors, seen=frozenset()):
    """The setting rows of a tab function in drawing order; recurses into the file's own row helpers."""
    if name in seen or name not in funcs:
        return
    seen = seen | {name}
    body = funcs[name]
    arrs = arrays(body)
    pattern = re.compile(r'\bopt_group\((UI_\w+)\)|(\w+)\s*\(\s*"TOTK_([A-Z0-9_]+)"\s*,\s*(UI_\w+)|' + HELPER.pattern)
    for m in pattern.finditer(body):
        if m.group(1):
            state['group'] = m.group(1)
            continue
        if m.group(5):
            if m.group(5) != name:
                walk(funcs, m.group(5), tab, state, rows, errors, seen)
            continue
        fn, key, label = m.group(2), m.group(3), m.group(4)
        kind = ROW_CALLS.get(fn)
        if kind is None:
            if not HELPER.fullmatch(fn + '('):
                errors.append(f'runtime/ui/menu.cpp {name}: {fn}("TOTK_{key}", {label}, ...) is no row this '
                              f'script knows ({", ".join(sorted(ROW_CALLS))})')
            continue
        args = split_args(body[body.index('(', m.start(2)) + 1:])
        row = {'key': key, 'tab': tab, 'group': state['group'], 'label': label, 'kind': kind,
               'desc': next((a for a in args[2:] if re.fullmatch(r'UI_\w+', a)), None)}
        if fn == 'opt_toggle':
            row['inverted'] = len(args) > 3 and args[3] == 'true'
        elif fn == 'opt_slider':
            row['lo'], row['hi'] = number(args[2]), number(args[3])
            row['step'] = number(args[5]) if len(args) > 5 else 1.0
            row['fmt'] = literal(args[6]) if len(args) > 6 else None
            row['fallback'] = (literal(args[8]) or '')[5:] or None if len(args) > 8 else None
            if row['lo'] is None or row['hi'] is None:
                errors.append(f'runtime/ui/menu.cpp {name}: the range of TOTK_{key} is not two numbers')
        elif fn == 'opt_choice':
            names = arrs.get(args[2])
            if names is None:
                errors.append(f'runtime/ui/menu.cpp {name}: the names of TOTK_{key} ({args[2]}) are no array of this '
                              'function')
            row['names'] = [name_of(e) for e in names or []]
        elif fn == 'opt_choice_list':
            vals = arrs.get(args[2])
            if vals and all(literal(v) is not None for v in vals):
                row['values'] = [literal(v) for v in vals]
                names = [name_of(e) for e in arrs[args[3]]] if args[3] in arrs else []
                names += [None] * (len(vals) - len(names))
                for i, ident in re.findall(rf'\b{re.escape(args[3])}\[(\d+)\]\s*=\s*menu_text\((UI_\w+)\)', body):
                    names[int(i)] = ('id', ident)
                if not any(names):   # names[i] = menu_text(ids[i]) over a UiTextId array as long as the values
                    ids = [a for a in arrs.values() if len(a) == len(vals) and all(x.startswith('UI_') for x in a)]
                    if ids:
                        names = [('id', x) for x in ids[0]]
                row['names'] = names[:len(vals)]
        elif fn == 'row_threads':
            vals = [literal(v) for v in arrays(funcs['row_threads'])['vals']]
            row['values'] = vals
            row['names'] = [('id', 'UI_MENU_AUTO')] + [None] * (len(vals) - 1)
        elif fn == 'row_set_or_not':
            on = literal(args[3]) if len(args) > 3 else None
            row['values'] = ['', on if on is not None else '1']
            row['names'] = [('id', 'UI_MENU_OFF'), ('id', 'UI_MENU_ON')]
            row['set_or_not'] = True
        rows.append(row)


def parse_cfg():
    cfg = strip_comments(read('runtime/cfg.c'))
    known = re.findall(r'"([A-Z0-9_]+)"', block(cfg, 'static const char *const known[]'))
    metas = {k: {'default': d, 'values': v or None} for k, d, v in re.findall(
        r'\{"([A-Z0-9_]+)", "([^"]*)", (?:"([^"]*)"|NULL), (?:LIVE|RESTART)\}', block(cfg, 'static const CfgMeta metas[]'))}
    return known, metas


def knob_defaults():
    found = {}
    for path in sorted((ROOT / 'runtime').rglob('*')):
        if path.suffix in ('.c', '.cpp', '.h') and 'third_party' not in path.parts:
            found.update(re.findall(r'rt_knob\("(?:TOTK_)?([A-Z0-9_]+)", *"([^"]*)"\)',
                                    path.read_text(encoding='utf-8', errors='replace')))
    return found


def example_descriptions():
    """{KEY: (section, text)} from totk.ini.example: the comment block above each key line."""
    out, lines, last, section = {}, [], '', None
    for line in read('totk.ini.example').splitlines():
        s = line.strip()
        m = re.match(r'^#? ?([a-z][a-z0-9_]*) = (.*)$', s)
        if m:
            text = ' '.join(lines).strip() or last
            out.setdefault(m.group(1).upper(), (section, text))
            last, lines = text, []
        elif s.startswith('['):
            section, lines, last = s.strip('[]'), [], ''
        elif s.startswith(('#', ';')):
            lines.append(s.lstrip('#;').strip())
        elif not s:
            lines, last = [], ''
    return out


def doc_rows():
    """{KEY: (heading, text, default)} from the tables of docs/settings.md."""
    out, heading = {}, None
    for line in read('docs/settings.md').splitlines():
        if line.startswith('## '):
            heading = line[3:].strip()
            continue
        m = re.match(r'^\| `([a-z0-9_<>]+)`(?: \(default `([^`]*)`\))? \| (.*?) \| (.*?) \| (.*?) \|\s*$', line)
        if m and '<' not in m.group(1):
            text = f'{m.group(3)} {m.group(4)}'.strip().replace('`', '')
            if m.group(5).strip() and m.group(5).strip() != '-':
                text += f' ({m.group(5).strip()})'
            out[m.group(1).upper()] = (heading, text, m.group(2))
    return out


def bind_rows():
    """The BIND_* rows of the Controls tab (ui/binds_page.cpp binds_rows): one per action of input_map.c, with the
    label and description ids binds_page.cpp gives it."""
    code = strip_comments(read('runtime/input_map.c'))
    actions = re.findall(r'"(\w+)"', block(code, 'action_names'))
    rows = []
    for i, action in enumerate(actions):
        desc = (f'UI_BIND_DESC_{action}' if i <= actions.index('MINUS') else
                'UI_BIND_DESC_DPAD' if i <= actions.index('DOWN') else
                'UI_BIND_DESC_SHAKE' if action == 'SHAKE' else
                'UI_BIND_DESC_MOVE' if action.startswith('LSTICK') else 'UI_BIND_DESC_CAMERA')
        group = ('UI_BINDS_GROUP_MOVE' if action.startswith('LSTICK') else
                 'UI_BINDS_GROUP_CAMERA' if action.startswith('RSTICK') else 'UI_BINDS_GROUP_BUTTONS')
        rows.append({'key': f'BIND_{action}', 'tab': 'CONTROLS', 'group': group, 'label': f'UI_BIND_{action}',
                     'desc': desc, 'kind': 'BIND'})
    return rows


def build():
    errors = []
    code = strip_comments(read('runtime/ui/menu.cpp'))
    funcs = functions(code)
    rows = []
    for tab, fn in TABS:
        if fn:
            if fn not in funcs:
                errors.append(f'runtime/ui/menu.cpp: no function {fn}')
            walk(funcs, fn, tab, {'group': None}, rows, errors)
        if tab == 'CONTROLS':
            rows += bind_rows()
        if tab == 'AMIIBO':
            for key in sorted(set(re.findall(r'cfg_set\("TOTK_([A-Z0-9_]+)"', read('runtime/ui/amiibo_page.cpp')))):
                rows.append({'key': key, 'tab': 'AMIIBO', 'group': None, 'label': 'UI_MENU_TAB_AMIIBO', 'desc': None,
                             'kind': 'FOLDER'})
    window_sizes = re.findall(r'\{(\d+), (\d+)\}', block(code, 'kSizes[][2] ='))
    known, metas = parse_cfg()
    in_menu = set()
    unique = []
    for r in rows:
        if r['key'] in in_menu:
            continue   # a row a tab draws twice (a key in two helpers): the first place counts
        in_menu.add(r['key'])
        if r['key'] not in known:
            errors.append(f'TOTK_{r["key"]}: a menu row of a key outside cfg.c known[]')
        if r['kind'] != 'BIND' and r['key'] not in metas:
            errors.append(f'TOTK_{r["key"]}: a menu row of a key without a cfg.c metas[] row')
        if r['key'] == 'WINDOW_SIZE':
            r['kind'] = 'TEXT'
            r['values'] = [f'{w}x{h}' for w, h in window_sizes]
            r['names'] = [('text', f'{w} × {h}') for w, h in window_sizes]
        if r['key'] in ('GPU_INDEX', 'AUDIO_DEVICE', 'LANG') and r['kind'] == 'CHOICE' and 'values' not in r:
            r['dynamic'] = True   # the window lists these itself (cfgr_rows.c, ui.c choices_of)
        unique.append(r)
    for key in metas:
        if key not in in_menu and key not in known:
            errors.append(f'TOTK_{key}: a metas[] row outside known[]')
    docs, example, knobs = doc_rows(), example_descriptions(), knob_defaults()
    expert = []
    for key in known:
        if key in in_menu:
            continue
        heading, text, default = docs.get(key, (None, None, None))
        if text is None and key in example:
            heading, text = (example[key][0] or '').capitalize() or None, example[key][1]
        meta = metas.get(key)
        values = meta['values'] if meta else None
        kind = ('TOGGLE' if values == '0|1' else 'SLIDER' if values and '..' in values and '|' not in values else
                'CHOICE' if values else 'FOLDER' if key.endswith('_DIR') else 'TEXT')
        row = {'key': key, 'tab': 'EXPERT', 'group': None, 'label': None, 'desc': None, 'kind': kind,
               'doc': text, 'doc_group': heading,
               'default': None if meta else (knobs.get(key) if key in knobs else default)}
        if kind == 'SLIDER':
            lo, hi = values.split('..')
            row['lo'], row['hi'] = float(lo), float(hi)
            row['step'] = 1.0 if '.' not in values else 0.01
        expert.append(row)
    # the Expert tab in the order of docs/settings.md's sections, then totk.ini.example's, alphabetical within one
    headings = list(dict.fromkeys(h for h, _, _ in docs.values() if h))
    expert.sort(key=lambda r: (headings.index(r['doc_group']) if r['doc_group'] in headings else len(headings),
                               r['doc_group'] or '\uffff', r['key']))
    for r in unique:
        if r['key'] in docs or r['key'] in example:
            r['doc'] = docs[r['key']][1] if r['key'] in docs else example[r['key']][1]
    return unique + expert, errors


def emit(rows):
    out = ['/* The settings table of totk-config: written by tools/gen_configurator.py from runtime/ui/menu.cpp,',
           '   runtime/cfg.c, runtime/input_map.c, docs/settings.md and totk.ini.example. Do not edit. */',
           '#include "configurator/cfgr.h"', '']
    body = []
    for i, r in enumerate(rows):
        values, names = r.get('values'), r.get('names')
        choices = 'NULL'
        n = 0
        if values is not None or names:
            if values is None:   # opt_choice: one name per metas[] value; an empty default puts Automatic first
                n = len(names)
                vals = [None] * n
            else:
                vals, n = values, len(values)
                names = (names or []) + [None] * (n - len(names or []))
            items = []
            for v, nm in zip(vals, names):
                ident = nm[1] if nm and nm[0] == 'id' else '-1'
                text = c_string(nm[1]) if nm and nm[0] == 'text' else 'NULL'
                items.append(f'{{{c_string(v) if v is not None else "NULL"}, {ident}, {text}}}')
            out.append(f'static const CrChoice c{i}[] = {{{", ".join(items)}}};')
            choices = f'c{i}'
        def opt(v):
            return c_string(v) if v is not None else 'NULL'
        flags = ' | '.join(f for f, on in (('CR_INVERTED', r.get('inverted')), ('CR_DYNAMIC', r.get('dynamic')),
                                            ('CR_SET_OR_NOT', r.get('set_or_not'))) if on) or '0'
        body.append(f'    {{{c_string(r["key"])}, CR_TAB_{r["tab"]}, {r.get("group") or -1}, {r.get("label") or -1}, '
                    f'{r.get("desc") or -1}, CR_{r["kind"]}, {flags}, {r.get("lo") or 0:g}, {r.get("hi") or 0:g}, '
                    f'{r.get("step") or 1:g}, {opt(r.get("fmt"))}, {opt(r.get("fallback"))}, {n}, {choices}, '
                    f'{opt(r.get("doc"))}, {opt(r.get("doc_group"))}, {opt(r.get("default"))}}},')
    out += ['', 'const CrRow cr_rows[] = {', *body, '};', f'const int cr_row_count = {len(rows)};', '']
    return '\n'.join(out)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--out', help='the C file to write (unchanged files are not rewritten)')
    ap.add_argument('--check', action='store_true', help='only check that every menu row can be read')
    args = ap.parse_args()
    rows, errors = build()
    for e in errors:
        print(f'gen_configurator: {e}', file=sys.stderr)
    if errors:
        return 1
    if args.out:
        text = emit(rows)
        out = Path(args.out)
        if not out.is_file() or out.read_text(encoding='utf-8') != text:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text, encoding='utf-8', newline='\n')
    menu = sum(r['tab'] != 'EXPERT' for r in rows)
    print(f'gen_configurator: {menu} menu rows, {len(rows) - menu} expert rows')
    return 0


if __name__ == '__main__':
    sys.exit(main())
