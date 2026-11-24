"""The screen shapes and button prompts this player needs RomFS patch folders for (steps.Shapes).

A game resolution of another shape than 16:9 takes romfs-patches-<w>x<h>, the UI laid out for that shape
(tools/make_romfs_patches.py --aspect), and the prompts' folder of that shape (romfs-patches-<style>-<w>x<h>). The
layout depends only on the aspect ratio (tools/ultrawide_layout.py apply), so the setup writes one folder per shape,
named after the shape's size in ASPECT_SIZES (shape_size); runtime/romfs.c overlay_init maps the game resolution the
same way (cfg.c cfg_shape_size). It writes them only for the shapes of the connected displays and the totk.ini
output_res, and the prompts' ones only for the style totk.ini selects (prompts_style).
"""
import ctypes
import os
import re
import shutil
import subprocess
from pathlib import Path

# The game resolutions of another shape than 16:9 that runtime/ui/menu.cpp row_output_res lists, in the order of
# runtime/cfg.c SHAPED: 21:9 and 32:9 monitors, 16:10 screens (the Steam Deck's 1280x800, handhelds and laptops) and 3:2
# screens (Surface and Framework laptops and tablets), 4:3 screens (CRT monitors, iPads) and 19.5:9 and 20:9 phone screens
# (streamed to a phone)
ASPECT_SIZES = ('2560x1080', '3440x1440', '3840x1600', '5120x2160', '3840x1080', '5120x1440',
                '1280x800', '1680x1050', '1920x1200', '2560x1600',
                '1920x1280', '2160x1440', '2256x1504', '2880x1920', '3000x2000',
                '1440x1080', '1600x1200', '2048x1536', '2732x2048',
                '2340x1080', '3120x1440', '2400x1080', '3200x1440')
SHAPED = tuple(tuple(int(v) for v in s.split('x')) for s in ASPECT_SIZES)
TOLERANCE = 0.01   # cfg.c and resolution.c res_is_16x9: shapes within 1% are the same
# cfg.c SHAPE_MIN / SHAPE_MAX: the aspect ratios a UI layout can be made for (tools/ultrawide_layout.py), 4:3 to 32:9
SHAPE_MIN, SHAPE_MAX = 4 / 3 * 0.99, 32 / 9 * 1.01


def is_16x9(w, h):
    return abs((w / h) / (16 / 9) - 1) <= TOLERANCE


def shape_size(w, h, listed=False):
    """The folder size of w x h's shape: of ASPECT_SIZES within 1% of its aspect ratio, the closest, the first of equal
    ones (5120x2160 is 2560x1080's shape); another shape from 4:3 to 32:9 its own size (an output_res of a phone's
    3120x1440, say), unless listed. None for 16:9 and for a shape no layout can be made for (portrait, narrower than
    4:3, wider than 32:9). The same rule as runtime/cfg.c cfg_shape_size."""
    if not w or not h or is_16x9(w, h):
        return None
    r, best, err = w / h, None, None
    for sw, sh in SHAPED:
        e = abs(sw / sh / r - 1)
        if e <= TOLERANCE and (err is None or e < err):
            best, err = f'{sw}x{sh}', e
    if best or listed or not SHAPE_MIN <= r <= SHAPE_MAX:
        return best
    return f'{w}x{h}'


def size_order(size):
    """Sort key: the ASPECT_SIZES order, then the sizes of unlisted shapes by width and height."""
    w, h = (int(v) for v in size.split('x'))
    return (ASPECT_SIZES.index(size), 0, 0) if size in ASPECT_SIZES else (len(ASPECT_SIZES), w, h)


def default_size(w, h):
    """The output_res the game picks on a display of w x h while totk.ini sets none (runtime/cfg.c screen_default): of
    ASPECT_SIZES within 1% of its shape, the smallest at least 1080 high that fits, else the largest that fits."""
    if not w or not h or is_16x9(w, h):
        return None
    r, best, fit = w / h, None, None
    for sw, sh in SHAPED:
        if abs(sw / sh / r - 1) > TOLERANCE or sw > w or sh > h:
            continue
        if fit is None or sw > fit[0]:
            fit = (sw, sh)
        if sh >= 1080 and (best is None or sw < best[0]):
            best = (sw, sh)
    best = best or fit
    return f'{best[0]}x{best[1]}' if best else None


# ---- the connected displays, as (width, height) of their current mode, the long side first (a portrait monitor or
# the Steam Deck's panel reports its height first; the game window is landscape)

class DISPLAY_DEVICEW(ctypes.Structure):
    _fields_ = [('cb', ctypes.c_uint32), ('DeviceName', ctypes.c_wchar * 32), ('DeviceString', ctypes.c_wchar * 128),
                ('StateFlags', ctypes.c_uint32), ('DeviceID', ctypes.c_wchar * 128), ('DeviceKey', ctypes.c_wchar * 128)]


ATTACHED_TO_DESKTOP = 0x1
ENUM_CURRENT_SETTINGS = -1
DEVMODEW_SIZE = 220            # DEVMODEW: dmSize at 68, dmPelsWidth at 172, dmPelsHeight at 176
DEVMODE_PELS = 172


def windows_displays():
    """Every display attached to the desktop: EnumDisplayDevicesW over the adapters, EnumDisplaySettingsW for the
    current mode, which is in pixels whatever the DPI awareness of this process."""
    user32 = ctypes.WinDLL('user32')
    out, i = [], 0
    while True:
        dev = DISPLAY_DEVICEW()
        dev.cb = ctypes.sizeof(dev)
        if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(dev), 0):
            break
        i += 1
        if not dev.StateFlags & ATTACHED_TO_DESKTOP:
            continue
        mode = ctypes.create_string_buffer(DEVMODEW_SIZE)
        ctypes.c_uint16.from_buffer(mode, 68).value = DEVMODEW_SIZE
        if user32.EnumDisplaySettingsW(dev.DeviceName, ENUM_CURRENT_SETTINGS, mode):
            out.append((ctypes.c_uint32.from_buffer(mode, DEVMODE_PELS).value,
                        ctypes.c_uint32.from_buffer(mode, DEVMODE_PELS + 4).value))
    return out


def windows_adapter_names():
    """The names of the display adapters Windows lists (EnumDisplayDevicesW, "AMD Radeon RX 9060 XT"), without
    starting Vulkan; [] elsewhere or when the call fails. The setup window decides its defaults by them before the host
    check has run."""
    if os.name != 'nt':
        return []
    try:
        user32 = ctypes.WinDLL('user32')
        out, i = [], 0
        while i < 64:
            dev = DISPLAY_DEVICEW()
            dev.cb = ctypes.sizeof(dev)
            if not user32.EnumDisplayDevicesW(None, i, ctypes.byref(dev), 0):
                break
            i += 1
            if dev.DeviceString and dev.DeviceString not in out:
                out.append(dev.DeviceString)
        return out
    except (OSError, AttributeError, ValueError):
        return []


XRANDR = re.compile(r'\sconnected\s+(?:primary\s+)?(\d+)x(\d+)\+')


def linux_displays():
    """xrandr's connected outputs (X11 and XWayland); without xrandr the preferred mode of each connected DRM
    connector."""
    if shutil.which('xrandr'):
        try:
            text = subprocess.run(['xrandr', '--current'], capture_output=True, text=True, timeout=10).stdout
            found = [(int(w), int(h)) for w, h in XRANDR.findall(text)]
            if found:
                return found
        except (OSError, subprocess.SubprocessError):
            pass
    out = []
    for conn in sorted(Path('/sys/class/drm').glob('card*-*')):
        try:
            if (conn / 'status').read_text().strip() != 'connected':
                continue
            m = re.match(r'(\d+)x(\d+)', (conn / 'modes').read_text())
        except OSError:
            continue
        if m:
            out.append((int(m.group(1)), int(m.group(2))))
    return out


def displays():
    try:
        found = windows_displays() if os.name == 'nt' else linux_displays()
    except (OSError, AttributeError, ValueError):
        found = []
    return sorted({(max(w, h), min(w, h)) for w, h in found if w and h})


# ---- totk.ini (runtime/cfg.c reads the same keys, in any section, with or without the totk_ prefix)

def ini_value(text, name):
    """The last nonempty value, with matching quotes removed, as runtime/cfg.c cfg_load and add read it."""
    value = None
    for line in text.splitlines():
        m = re.match(rf'^\s*(?:totk_)?{re.escape(name)}\s*=(.*)$', line, re.IGNORECASE)
        if m:
            candidate = m.group(1).strip()
            if len(candidate) >= 2 and candidate[0] == candidate[-1] == '"':
                candidate = candidate[1:-1]
            if candidate:
                value = candidate
    return value


# The pad prompts' folder suffixes (make_romfs_patches.py --prompts), each written at 16:9 on every install
# (steps.Prepare): the Xbox and PlayStation ones, for the Nintendo pad layout and for xbox and auto
PAD_PROMPTS = ('xbox', 'xbox-label', 'playstation', 'playstation-xbox')
# Every prompts folder suffix: the pad ones and the keyboard and mouse one (steps.PcPrompts)
PROMPTS_STYLES = PAD_PROMPTS + ('pc',)


def prompts_style(text):
    """The prompts' folder suffix the settings select, as runtime/input_map.c input_prompts_pick and
    input_prompts_folder pick it at start: 'pc', 'xbox' or 'playstation' (Nintendo pad layout), 'xbox-label' or
    'playstation-xbox' (any other), or None for the game's own prompts, the default without a totk.ini."""
    p, layout = (ini_value(text, 'prompts') or '').lower(), (ini_value(text, 'pad_layout') or '').lower()
    if p == 'pc':
        return 'pc'
    nintendo = layout in ('nintendo', 'xbox_prompts')
    if p in ('playstation', 'ps'):
        return 'playstation' if nintendo else 'playstation-xbox'
    if not (p == 'xbox' if p else layout == 'xbox_prompts'):
        return None
    return 'xbox' if nintendo else 'xbox-label'


def wanted(dest, screens=None):
    """(sizes, style): the shape sizes to write layouts for, sorted, and the prompts style. A shape is wanted for each
    display's own size and for the output_res the game defaults to there, and for the totk.ini output_res."""
    screens = displays() if screens is None else screens
    try:
        text = (Path(dest) / 'totk.ini').read_text(encoding='utf-8-sig', errors='replace')
    except OSError:
        text = ''
    sizes = set()
    for w, h in screens:
        # A display of an unlisted shape keeps the 1920x1080 default (cfg.c screen_default): no folder for it unless
        # output_res asks for its size.
        w, h = max(w, h), min(w, h)
        sizes.add(shape_size(w, h, listed=True))
        d = default_size(w, h)
        if d:
            sizes.add(shape_size(*(int(v) for v in d.split('x'))))
    m = re.fullmatch(r'(\d+)x(\d+)', (ini_value(text, 'output_res') or '').lower())
    if m:
        sizes.add(shape_size(int(m.group(1)), int(m.group(2))))
    sizes.discard(None)
    return sorted(sizes, key=size_order), prompts_style(text)
