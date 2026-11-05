"""Code modules of mods that the port runs as its own code: the plugin builds it knows, by SHA-256, and the hooks each
puts into the game's main per game version.

On the Switch a mod's exefs/subsdk<n> plugin is loaded into the game's process and patches main's code from its own
start-up. Lifted code cannot be patched while it runs and the port loads no such module, so tools/exefs_patches.py
turns the hooks of a build listed here into hook sites of the lifted game (recomp/patches.py `hook` lines), and
runtime/plugins.c has a handler for each of the build's callbacks. A build not listed here stays unsupported, as any
code module was before: its mod is off unless mods_without_code is on (runtime/mods.c).

Standard library only.
"""
from collections import namedtuple

# One build: what the setup and the menu call it, the prefix of its handlers in runtime/plugins.c, and per game
# version the hook arguments and sites (Hooks).
Plugin = namedtuple('Plugin', 'name prefix versions')
# arguments: the hook line's arguments after the handler, as recomp/patches.py takes them; sites: [(main offset,
# callback)], the handler being prefix + callback.
Hooks = namedtuple('Hooks', 'arguments sites')

# MSFix, the plugin of the mod The Second Quest (GameBanana 479009): seven exlaunch inline hooks, two of callback a,
# one of c, three of b, one of d (runtime/plugins.c describes them). Both builds read their main offsets from tables
# of one 8-byte word per game version (the 1.2.1 package's subsdk9 at +0x8828 to +0x89a8, versions 1.0.0 to 1.2.1;
# the 1.4.3 package's at +0x9ed0 to +0xa150, 1.0.0 to 1.4.3) and agree on the versions both have. Every hook gets the
# game functions callbacks a and d call (fn1, fn2) and MSFix's index of the game version.
MSFIX_CALLBACKS = ('a', 'a', 'c', 'b', 'b', 'b', 'd')   # in the order the plugin installs its hooks
MSFIX = {   # game version: (MSFix's index, fn1, fn2, the seven hook sites in MSFIX_CALLBACKS order)
    '1.1.0': (1, 0x850AB4, 0xA26BB0, (0x199BEA0, 0x199BD7C, 0x6D2330, 0x1D60F58, 0x1D61934, 0x1D92BC4, 0x7FB8FC)),
    '1.2.1': (5, 0x80EB0C, 0xEC98CC, (0x198C59C, 0x198C6C0, 0x66589C, 0x1D53D98, 0x1D54774, 0x1D85A64, 0x9F1CB0)),
    '1.4.3': (9, 0x16FDCC, 0x1B0834, (0x1FD0C04, 0x1FD0D88, 0x457308, 0x23DEE24, 0x23DF784, 0x24114D4, 0x31D2D0)),
}


def msfix_hooks(version):
    index, fn1, fn2, sites = MSFIX[version]
    return Hooks((f'fn:main+{fn1:x}', f'fn:main+{fn2:x}', f'{index:x}'), list(zip(sites, MSFIX_CALLBACKS)))


KNOWN = {   # SHA-256 of the module file: the build
    '0e2d3b75dfa135155c8ab0da1dcbcf2da74ffd5455434214e214e0511a6758da':
        Plugin('MSFix of The Second Quest 1.2.1', 'msfix1_', {v: msfix_hooks(v) for v in ('1.1.0', '1.2.1')}),
    '1490a83ee46e33fe1937cc1d5a70dacc6f1732cd5b9b1e0d4460b028c26eb921':
        Plugin('MSFix of The Second Quest 1.4.3', 'msfix2_', {v: msfix_hooks(v) for v in ('1.1.0', '1.2.1', '1.4.3')}),
}
