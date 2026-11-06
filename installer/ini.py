"""totk.ini: set the game language and keep everything else (keys and syntax as runtime/cfg.c reads them).

A new file starts from totk.ini.example, whose lines are all commented defaults. The language goes into `lang`
(a Switch language code, runtime/ov_system.c): an active `lang = ...` line is rewritten in place, else the commented
`# lang = ...` line of the example is activated, else the line is added to [language] (created at the end if
missing). Other lines, comments and line order stay as they are.
"""
import os
import re
from pathlib import Path

# runtime/ov_system.c: the codes TOTK_LANG offers. Each language appears once, under the code the game accepts.
LANGUAGES = ('ja', 'en-US', 'en-GB', 'fr', 'fr-CA', 'de', 'it', 'es', 'es-419', 'nl', 'pt', 'pt-BR', 'ru', 'ko',
             'zh-Hans', 'zh-Hant')
# Older installer builds wrote these two codes; they are read as the canonical ones and rewritten on the next install.
ALIASES = {'zh-CN': 'zh-Hans', 'zh-TW': 'zh-Hant'}
# Windows locales and the Switch code each one is the game language of (runtime/ui_text.c ui_text_lang). TotK's own
# language codes are the LANGUAGES ones; a totk.ini may name a Windows locale instead, and update-totk.sh passes it.
WINDOWS_LOCALES = {
    'de-DE': 'de', 'de-AT': 'de', 'de-CH': 'de',
    'it-IT': 'it', 'nl-NL': 'nl', 'nl-BE': 'nl', 'ru-RU': 'ru', 'ja-JP': 'ja', 'ko-KR': 'ko',
    'es-ES': 'es', 'es-MX': 'es-419', 'es-US': 'es-419',
    'fr-FR': 'fr', 'fr-BE': 'fr', 'fr-CH': 'fr',
    'pt-PT': 'pt', 'en-AU': 'en-GB', 'en-NZ': 'en-GB', 'en-IE': 'en-GB', 'en-CA': 'en-US',
    'zh-CN': 'zh-Hans', 'zh-SG': 'zh-Hans', 'zh-TW': 'zh-Hant', 'zh-HK': 'zh-Hant', 'zh-MO': 'zh-Hant',
}
# Their text falls back to the Switch shared fonts (runtime/ov_system.c), which the fonts step extracts from the
# firmware folder when the player gives one.
NEED_SHARED_FONTS = ('ko', 'zh-Hans', 'zh-Hant')

ACTIVE = re.compile(r'^(\s*)(?:totk_)?lang(\s*=\s*)(.*?)(\s*)$', re.IGNORECASE)
COMMENTED = re.compile(r'^\s*[#;]\s*(?:totk_)?lang\s*=.*$', re.IGNORECASE)
SECTION = re.compile(r'^\s*\[(.*)\]\s*$')


def canonical_language(lang):
    """The code LANGUAGES lists for lang: an alias of an older installer build or a Windows locale is mapped, anything
    else is kept."""
    lang = ALIASES.get(lang, lang)
    if lang in LANGUAGES:
        return lang
    if lang in WINDOWS_LOCALES:
        return WINDOWS_LOCALES[lang]
    base = lang.split('-')[0]
    return base if base in LANGUAGES else lang


def set_language(text, lang):
    """text with `lang = <lang>` (an alias written as its canonical code); see the module docstring."""
    lang = canonical_language(lang)
    lines = text.splitlines()
    for i, line in enumerate(lines):
        m = ACTIVE.match(line)
        if m:
            lines[i] = f'{m.group(1)}lang{m.group(2)}{lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        if COMMENTED.match(line):
            lines[i] = f'lang = {lang}'
            return '\n'.join(lines) + '\n'
    for i, line in enumerate(lines):
        m = SECTION.match(line)
        if m and m.group(1).strip().lower() == 'language':
            lines.insert(i + 1, f'lang = {lang}')
            return '\n'.join(lines) + '\n'
    if lines and lines[-1].strip():
        lines.append('')
    lines += ['[language]', f'lang = {lang}']
    return '\n'.join(lines) + '\n'


def get_language(text):
    for line in text.splitlines():
        m = ACTIVE.match(line)
        if m:
            return m.group(3)
    return None


def write(dest, lang, example=None):
    """Write dest/totk.ini with lang; an existing file keeps its other lines. Returns the path."""
    path = Path(dest) / 'totk.ini'
    if path.is_file():
        text = path.read_text(encoding='utf-8-sig')
    elif example and Path(example).is_file():
        text = Path(example).read_text(encoding='utf-8')
    else:
        text = ''
    new = set_language(text, lang)
    if path.is_file() and new == text:
        return path
    tmp = path.with_name('totk.ini.tmp')
    tmp.write_text(new, encoding='utf-8', newline='\n')
    os.replace(tmp, path)
    return path
