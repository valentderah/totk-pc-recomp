"""Write configurator/settings.json: every setting the runtime knows, for the settings configurator (configurator/).

    python tools/gen_configurator_settings.py           # rewrite configurator/settings.json
    python tools/gen_configurator_settings.py --check   # exit 1 when the checked-in file is out of date

The configurator never carries a hand-written list. This script reads the runtime's own sources:

- runtime/cfg.c: known[] (every name), metas[] (default, values, live flag), sections[] (the totk.ini section).
- runtime/ui/menu.cpp and runtime/ui/binds_page.cpp: which tab and group of the in-game menu shows a setting, its
  label and description text ids, slider ranges and steps, and the names of the choices.
- runtime/ui_text.c: the menu texts in English, Russian, Brazilian Portuguese and Simplified Chinese.
- runtime/input_map.c and runtime/input_map.h: action, key and gamepad button names and the default bindings.
- totk.ini.example and docs/settings.md: the descriptions of the settings the menu does not show.
- rt_knob("TOTK_NAME", "default") calls anywhere in runtime/: defaults of the keys outside metas[].

Standard library only. A new setting in cfg.c shows up after a rerun; configurator/src/data.rs has a test that fails
when settings.json and runtime/cfg.c disagree, and so does --check here.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
OUT = ROOT / 'configurator' / 'settings.json'
LANGS = {'en': 'text_en', 'ru': 'text_ru', 'pt-BR': 'text_pt_br', 'zh-Hans': 'text_zh_hans'}
# The in-game menu's tabs that hold settings, in menu order (menu.cpp kTabNames), with the function that draws each.
TABS = [('screen', 'UI_MENU_TAB_SCREEN', 'tab_screen'), ('game', 'UI_MENU_TAB_GAME', 'tab_game'),
        ('sound', 'UI_MENU_TAB_SOUND', 'tab_sound'), ('controls', 'UI_MENU_TAB_CONTROLS', 'tab_controls'),
        ('amiibo', 'UI_MENU_TAB_AMIIBO', 'tab_amiibo'), ('menu', 'UI_MENU_TAB_MENU', 'tab_menu'),
        ('advanced', 'UI_MENU_TAB_ADVANCED', 'tab_advanced'), ('mods', 'UI_MENU_TAB_MODS', 'tab_mods')]
# The tab of a metas[] row the menu does not draw, by its totk.ini section (cfg.c sections[]).
SECTION_TAB = {'display': 'screen', 'frame rate': 'game', 'audio': 'sound', 'input': 'controls', 'language': 'menu',
               'discord': 'menu', 'mods': 'mods', 'performance': 'advanced', 'diagnostics': 'advanced',
               'gameplay tweaks': 'controls'}


def read(rel):
    return (ROOT / rel).read_text(encoding='utf-8')


def block(text, start_marker):
    """The text between the '{' after start_marker and its matching '}'."""
    start = text.index(start_marker)
    open_at = text.index('{', start)
    depth = 0
    for i in range(open_at, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
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
    simple = {'n': '\n', 't': '\t', '"': '"', "'": "'", '\\': '\\', 'r': '\r', '0': '\0'}
    out, i = [], 0
    while i < len(s):
        if s[i] == '\\' and i + 1 < len(s):
            nxt = s[i + 1]
            if nxt == 'x':
                m = re.match(r'[0-9A-Fa-f]{1,2}', s[i + 2:])
                out.append(chr(int(m.group(0), 16)))
                i += 2 + len(m.group(0))
                continue
            out.append(simple.get(nxt, nxt))
            i += 2
        else:
            out.append(s[i])
            i += 1
    return ''.join(out)


TOKEN = re.compile(r'"((?:[^"\\]|\\.)*)"|([A-Za-z_]\w*)|(\S)')


def tokens(code):
    return [(m.group(1), m.group(2), m.group(3)) for m in TOKEN.finditer(code)]


# ---- runtime/cfg.c
def parse_cfg():
    cfg = strip_comments(read('runtime/cfg.c'))
    known = re.findall(r'"([A-Z0-9_]+)"', block(cfg, 'static const char *const known[]'))
    sections = dict(re.findall(r'\{"([A-Z0-9_]+)", "([^"]+)"\}', block(cfg, 'sections[] =')))
    metas = []
    for key, default, values, live in re.findall(
            r'\{"([A-Z0-9_]+)", "([^"]*)", (?:"([^"]*)"|NULL), (LIVE|RESTART)\}', block(cfg, 'static const CfgMeta metas[]')):
        metas.append({'key': key, 'default': default, 'values': values or None, 'live': live == 'LIVE'})
    deck = re.findall(r'\{"([A-Z0-9_]+)", "([^"]*)"\}', block(cfg, 'DECK[] ='))
    return known, sections, metas, dict(deck)


def knob_defaults():
    found = {}
    for path in sorted((ROOT / 'runtime').rglob('*')):
        if path.suffix in ('.c', '.cpp', '.h'):
            text = path.read_text(encoding='utf-8', errors='replace')
            found.update(re.findall(r'rt_knob\("(?:TOTK_)?([A-Z0-9_]+)", *"([^"]*)"\)', text))
    return found


# ---- runtime/ui_text.c
def parse_entries(code, macros):
    """{id: text} of a run of [ID] = "..." "..." entries (string literals and macros concatenated)."""
    toks = tokens(code)
    out, i = {}, 0
    while i < len(toks):
        if toks[i][2] == '[' and i + 3 < len(toks) and toks[i + 1][1] and toks[i + 2][2] == ']' and toks[i + 3][2] == '=':
            ident, j, parts = toks[i + 1][1], i + 4, []
            while j < len(toks) and not (toks[j][2] in (',', '}')):
                s, name, _ = toks[j]
                if s is not None:
                    parts.append(c_unescape(s))
                elif name in macros:
                    parts.append(macros[name])
                j += 1
            out[ident] = ''.join(parts)
            i = j
        else:
            i += 1
    return out


def parse_texts():
    raw = read('runtime/ui_text.c')
    code = strip_comments(raw)
    # Windows names of the platform macros (the first #define of each, under #ifdef _WIN32)
    macros = {}
    for name, value in re.findall(r'#define (DLSS_FILE|EXE_FILE) "([^"]*)"', code):
        macros.setdefault(name, value)
    lang_names = parse_entries(re.search(r'#define LANG_NAMES \\(.*?)\n\n', code, re.S).group(1).replace('\\\n', '\n'),
                               macros)
    texts = {}
    for lang, array in LANGS.items():
        body = block(code, f'static const char *const {array}[UI_TEXT_COUNT]')
        texts[lang] = {**lang_names, **parse_entries(body, macros)}
    return texts


# ---- runtime/ui/*.cpp
def functions(code):
    """{name: body} of the top-level functions of a C++ file."""
    out = {}
    for m in re.finditer(r'^(?:static |extern "C" )?(?:void|bool|int)\s+(\w+)\s*\([^;{]*\)\s*\{', code, re.M):
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
    """{name: [element source]} of the array initializers 'name[] = {...}' in a function body."""
    out = {}
    for m in re.finditer(r'(\w+)\[[^\]=]*\]\s*=\s*\{', body):
        out[m.group(1)] = split_args(body[m.end():].replace('}', ')', 1))
    return out


def element_label(src):
    """A choice name's source: {'text': literal} or {'id': UI_...}; None when built at run time."""
    m = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', src)
    if m:
        return {'text': c_unescape(m.group(1))}
    m = re.fullmatch(r'(?:menu_text\()?(UI_\w+)\)?', src)
    return {'id': m.group(1)} if m else None


def parse_float(src):
    try:
        return float(src.rstrip('f'))
    except ValueError:
        return None


def walk_menu(funcs, name, tab, state, rows, seen):
    """Settings rows in the order the function draws them; recurses into the file's own row helpers."""
    if name in seen or name not in funcs:
        return
    seen = seen | {name}
    body = funcs[name]
    arrs = arrays(body)
    pattern = re.compile(r'\bopt_group\((UI_\w+)\)|"TOTK_([A-Z0-9_]+)"\s*,\s*(UI_\w+)|\b((?:row|rows|mods)_\w+)\s*\(')
    for m in pattern.finditer(body):
        if m.group(1):
            state['group'] = m.group(1)
            continue
        if m.group(4):
            walk_menu(funcs, m.group(4), tab, state, rows, seen)
            continue
        key, label = m.group(2), m.group(3)
        call = re.search(r'(\w+)\s*\($', body[:m.start()])
        args = split_args(body[m.start():])
        fn = call.group(1) if call else ''
        row = {'key': key, 'tab': tab, 'group': state['group'], 'label': label, 'call': fn}
        desc = next((a for a in args[2:] if re.fullmatch(r'UI_\w+_DESC', a)), None)
        row['desc'] = desc
        if fn == 'opt_slider' and len(args) >= 5:
            row['min'], row['max'] = parse_float(args[2]), parse_float(args[3])
            row['step'] = parse_float(args[5]) if len(args) > 5 else 1.0
            if len(args) > 6 and args[6].startswith('"'):
                row['format'] = c_unescape(args[6][1:-1])
            if len(args) > 8 and args[8].startswith('"TOTK_'):
                row['fallback'] = args[8][6:-1]
        elif fn == 'opt_choice' and len(args) >= 3 and args[2] in arrs:
            row['names'] = [element_label(e) for e in arrs[args[2]]]
        elif fn == 'opt_choice_list' and len(args) >= 4:
            vals = arrs.get(args[2])
            if vals and all(v.startswith('"') for v in vals):
                row['values'] = [c_unescape(v[1:-1]) for v in vals]
                names = [element_label(e) for e in arrs[args[3]]] if args[3] in arrs else [None] * len(vals)
                names += [None] * (len(vals) - len(names))
                for i, src in re.findall(rf'\b{re.escape(args[3])}\[(\d+)\]\s*=\s*menu_text\((UI_\w+)\)', body):
                    names[int(i)] = {'id': src}
                if not any(names):   # names[i] = menu_text(ids[i]) from a UiTextId array of the same length
                    ids = [a for a in arrs.values() if len(a) == len(vals) and all(x.startswith('UI_') for x in a)]
                    if ids:
                        names = [{'id': x} for x in ids[0]]
                row['names'] = names
        elif fn == 'row_set_or_not' and len(args) >= 4:
            row['set_or_not'] = c_unescape(args[3][1:-1]) if args[3].startswith('"') else ''
        elif fn == 'row_threads':
            row['values'] = None   # the helper's own list (row_threads): Automatic, then counts
            helper = arrays(funcs['row_threads'])['vals']
            row['values'] = [c_unescape(v[1:-1]) for v in helper]
            row['names'] = [{'id': 'UI_MENU_AUTO'}] + [None] * (len(helper) - 1)
        rows.append(row)


def parse_menu():
    code = strip_comments(read('runtime/ui/menu.cpp'))
    funcs = functions(code)
    rows = []
    for tab, _, fn in TABS:
        walk_menu(funcs, fn, tab, {'group': None}, rows, frozenset())
    amiibo = read('runtime/ui/amiibo_page.cpp')
    for key in sorted(set(re.findall(r'cfg_set\("TOTK_([A-Z0-9_]+)"', amiibo))):
        rows.append({'key': key, 'tab': 'amiibo', 'group': None, 'label': None, 'desc': None, 'call': 'folder'})
    return rows


# ---- runtime/input_map.c / .h
def parse_input():
    header = strip_comments(read('runtime/input_map.h'))
    code = strip_comments(read('runtime/input_map.c'))
    actions = re.findall(r'"([A-Z_]+)"', block(code, 'action_names[ACTION_COUNT] ='))
    pads = [p for p in re.findall(r'"([a-z0-9_]+)"', block(code, 'pad_names[PAD_COUNT] ='))]
    keys = [name for name in re.findall(r'"([^"]+)"', block(code, 'key_names[KEY_COUNT] ='))]
    # action enum names (B_*) in value order: B_A = 0, ..., B_SHAKE = 24
    enum_body = block(header, 'enum')
    action_ids, value = {}, -1
    for name, explicit in re.findall(r'(B_\w+)(?:\s*=\s*(\d+))?', enum_body):
        value = int(explicit) if explicit else value + 1
        if name != 'B_SHAKE' or explicit:
            action_ids[name] = value
    def action_of(enum_name):
        return actions[action_ids[enum_name]]
    default_keys = {a: [] for a in actions}
    keys_define = re.search(r'#define DEFAULT_KEYS \\(.*?)\n\n', code, re.S).group(1)
    for key, act in re.findall(r'\{KEY_(\w+), (B_\w+)\}', keys_define):
        default_keys[action_of(act)].append(key.lower())
    pads_define = re.search(r'#define DEFAULT_PADS\(south, east, west, north\) \\(.*?)\n#define', code, re.S).group(1)
    layouts = {}
    for layout, args in re.findall(r'#define (NINTENDO|XBOX)_PADS DEFAULT_PADS\(([^)]*)\)', code):
        sub = dict(zip(['south', 'east', 'west', 'north'], [a.strip() for a in args.split(',')]))
        table = {a: [] for a in actions}
        for pad, act in re.findall(r'\{PAD_(\w+), (\w+)\}', pads_define):
            table[action_of(sub.get(act, act))].append(pad.lower())
        layouts[layout.lower()] = table
    return {'actions': actions, 'keys': keys, 'pads': pads, 'default_keys': default_keys, 'default_pads': layouts}


# ---- totk.ini.example and docs/settings.md
def example_descriptions():
    out, block_lines, last, section = {}, [], '', None
    for line in read('totk.ini.example').splitlines():
        s = line.strip()
        m = re.match(r'^#? ?([a-z][a-z0-9_]*) = (.*)$', s)
        if m and (s.startswith('#') or '=' in s):
            text = ' '.join(block_lines).strip() or last
            out.setdefault(m.group(1).upper(), {'text': text, 'section': section})
            last, block_lines = text, []
        elif s.startswith('['):
            section, block_lines, last = s.strip('[]'), [], ''
        elif s.startswith('#') or s.startswith(';'):
            block_lines.append(s.lstrip('#;').strip())
        elif not s:
            block_lines, last = [], ''
    return out


def doc_rows():
    out, heading = {}, None
    for line in read('docs/settings.md').splitlines():
        if line.startswith('## '):
            heading = line[3:].strip()
            continue
        m = re.match(r'^\| `([a-z0-9_<>]+)`(?: \(default `([^`]*)`\))? \| (.*?) \| (.*?) \| (.*?) \|\s*$', line)
        if m and '<' not in m.group(1):
            out[m.group(1).upper()] = {'heading': heading, 'default': m.group(2), 'what': m.group(3),
                                       'effect': m.group(4), 'safety': m.group(5)}
    return out


# ---- assembly
def localized(texts, ident):
    if not ident:
        return None
    got = {lang: t[ident] for lang, t in texts.items() if ident in t}
    return got or None


def build():
    known, sections, metas, deck = parse_cfg()
    texts = parse_texts()
    menu_rows = parse_menu()
    inputs = parse_input()
    example = example_descriptions()
    docs = doc_rows()
    knobs = knob_defaults()
    game_langs = dict(re.findall(r'\{"([\w-]+)", (UI_MENU_LANGNAME_\w+), \d+\}', read('runtime/ui_text.c')))
    window_sizes = re.findall(r'\{(\d+), (\d+)\}', block(read('runtime/ui/menu.cpp'), 'kSizes[][2] ='))
    meta_by_key = {m['key']: m for m in metas}
    menu_by_key = {}
    menu_order = {}
    for i, row in enumerate(menu_rows):
        menu_by_key.setdefault(row['key'], row)
        menu_order.setdefault(row['key'], i)

    settings = []
    for index, key in enumerate(known):
        meta = meta_by_key.get(key)
        row = menu_by_key.get(key)
        s = {'key': key, 'known': True, 'meta': meta is not None,
             'default': meta['default'] if meta else knobs.get(key, (docs.get(key) or {}).get('default') or ''),
             'values': meta['values'] if meta else None, 'live': meta['live'] if meta else False,
             'section': sections.get(key), 'deck': deck.get(key),
             # the row's place: the menu's drawing order, the bindings after the menu rows, the rest in known[] order
             'order': menu_order.get(key, 100000 + index)}
        if key.startswith('BIND_') and key[5:] in inputs['actions']:
            action = key[5:]
            index = inputs['actions'].index(action)
            s['kind'] = 'bind'
            s['tab'] = 'controls'
            s['group'] = localized(texts, 'UI_BINDS_GROUP_MOVE' if action.startswith('LSTICK') else
                                   'UI_BINDS_GROUP_CAMERA' if action.startswith('RSTICK') else 'UI_BINDS_GROUP_BUTTONS')
            s['label'] = localized(texts, f'UI_BIND_{action}')
            desc = (f'UI_BIND_DESC_{action}' if index <= inputs['actions'].index('MINUS') else
                    'UI_BIND_DESC_DPAD' if index <= inputs['actions'].index('DOWN') else
                    'UI_BIND_DESC_SHAKE' if action == 'SHAKE' else
                    'UI_BIND_DESC_MOVE' if action.startswith('LSTICK') else 'UI_BIND_DESC_CAMERA')
            s['desc'] = localized(texts, desc)
            s['in_menu'] = True
            s['keys_only'] = index >= inputs['actions'].index('LSTICK_LEFT')
            s['order'] = 5000 + index
        elif row:
            s['in_menu'] = True
            s['tab'] = row['tab']
            s['group'] = localized(texts, row['group'])
            s['label'] = localized(texts, row['label'])
            s['desc'] = localized(texts, row['desc'])
            for k in ('min', 'max', 'step', 'format', 'fallback', 'set_or_not'):
                if k in row:
                    s[k] = row[k]
            if row['call'] == 'opt_toggle':
                s['kind'] = 'toggle'
            elif row['call'] == 'opt_slider':
                s['kind'] = 'slider'
            elif row['call'] == 'folder':
                s['kind'] = 'folder'
            elif row['call'] in ('opt_choice', 'opt_choice_list', 'row_threads', 'row_set_or_not') or s['values']:
                s['kind'] = 'choice'
            else:
                s['kind'] = 'text'
            if 'names' in row or 'values' in row:
                vals = row.get('values')
                if vals is None:
                    vals = ([''] if meta and not meta['default'] else []) + (meta['values'].split('|') if meta and meta['values'] else [])
                names = row.get('names') or [None] * len(vals)
                s['choices'] = [{'value': v, 'label': localized(texts, n['id']) if n and 'id' in n else
                                 ({'en': n['text']} if n else None)} for v, n in zip(vals, names)]
        else:
            s['in_menu'] = False
            s['tab'] = SECTION_TAB.get(sections.get(key) or (example.get(key) or {}).get('section'), 'expert') \
                if meta or key in example else 'expert'
            s['group'] = None
            s['label'] = None
            s['desc'] = None
            values = s['values']
            if values and re.fullmatch(r'0\|1', values):
                s['kind'] = 'toggle'
            elif values and '..' in values and '|' not in values:
                s['kind'] = 'number'
            elif values:
                s['kind'] = 'choice'
            elif key == 'WALK_KEY':
                s['kind'] = 'key'
            elif key.endswith('_DIR'):
                s['kind'] = 'folder'
            else:
                s['kind'] = 'text'
        if key in example:
            s['doc'] = example[key]['text']
        if key in docs:
            d = docs[key]
            s['doc'] = f"{d['what']} {d['effect']}".strip().replace('`', '')   # Markdown code marks read as noise
            s['safety'] = d['safety']
            s['doc_group'] = d['heading']
        if key == 'LANG':   # menu.cpp row_game_lang: Automatic, then each language under its own name (ui_text.c)
            s['choices'] = [{'value': '', 'label': localized(texts, 'UI_MENU_GAME_LANG_AUTO')}] + [
                {'value': v, 'label': localized(texts, game_langs[v]) if v in game_langs else None}
                for v in (s['values'] or '').split('|')]
        if key == 'WINDOW_SIZE':   # menu.cpp row_window_size: kSizes
            s['choices'] = [{'value': f'{w}x{h}', 'label': None} for w, h in window_sizes]
        if s.get('kind') == 'choice' and 'choices' not in s and s['values']:
            vals = ([''] if not s['default'] else []) + s['values'].split('|')
            s['choices'] = [{'value': v, 'label': None} for v in vals]
        settings.append(s)

    missing_in_known = sorted(set(meta_by_key) - set(known))
    if missing_in_known:
        raise SystemExit(f'metas[] rows outside known[]: {missing_in_known}')

    tabs = [{'id': tab, 'label': localized(texts, ident)} for tab, ident, _ in TABS]
    common = {name: localized(texts, ident) for name, ident in [
        ('on', 'UI_MENU_ON'), ('off', 'UI_MENU_OFF'), ('auto', 'UI_MENU_AUTO'), ('default', 'UI_MENU_DEFAULT_MARK'),
        ('after_restart', 'UI_MENU_AFTER_RESTART'), ('set_by_env', 'UI_MENU_SET_BY_ENV')]}
    key_labels = {}
    for name in inputs['keys']:
        label = localized(texts, f'UI_KEY_{name.upper()}')
        if label:
            key_labels[name] = label
    pad_labels = {}
    for name in inputs['pads']:
        label = localized(texts, f'UI_PAD_{name.upper()}')
        if label:
            pad_labels[name] = label
    return {
        'generated_by': 'tools/gen_configurator_settings.py from runtime/cfg.c, runtime/ui/menu.cpp, runtime/ui_text.c, '
                        'runtime/input_map.c, totk.ini.example and docs/settings.md; do not edit by hand',
        'langs': list(LANGS),
        'tabs': tabs,
        'common': common,
        'settings': settings,
        'input': {**inputs, 'key_labels': key_labels, 'pad_labels': pad_labels},
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('--check', action='store_true', help='compare with the checked-in file instead of writing it')
    args = ap.parse_args()
    text = json.dumps(build(), ensure_ascii=False, indent=1) + '\n'
    if args.check:
        have = OUT.read_text(encoding='utf-8') if OUT.exists() else ''
        if have != text:
            print(f'{OUT.relative_to(ROOT)} is out of date: run python tools/gen_configurator_settings.py')
            return 1
        print(f'{OUT.relative_to(ROOT)} is up to date')
        return 0
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_text(text, encoding='utf-8', newline='\n')
    data = json.loads(text)
    in_menu = sum(1 for s in data['settings'] if s.get('in_menu'))
    print(f'wrote {OUT.relative_to(ROOT)}: {len(data["settings"])} settings, {in_menu} in the in-game menu')
    return 0


if __name__ == '__main__':
    sys.exit(main())
