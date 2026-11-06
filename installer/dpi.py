"""The display scale of the installer window: process DPI awareness and the pixel sizes that follow it.

Windows draws a DPI-unaware process at 96 DPI and stretches its windows to the display's scale, which is what made the
setup window blurry at 125% and above. gui.py calls enable() before it imports tkinter, so the process is system DPI
aware before its first window exists; Tk then reads the display's DPI when it starts (`tk scaling` is 2.667 at 200%)
and every font size in points comes out at the right size. The pixel sizes of the layout have no such scaling of their
own, so gui.py passes them through px().

System DPI awareness, not per-monitor: Tk 9.0.4, the one the release bundles, has no per-monitor support (its DLL
imports no DPI-awareness function), so it draws everything at the system DPI. Per-monitor awareness would leave the
window crisp on a display at another scale but with the primary display's font sizes; system awareness draws the
primary display exactly and lets Windows stretch the window only on a display whose scale differs from it, as before.

On Linux the window stays 1:1 (px returns n as it is): Tk and the desktop scale what they draw.
"""
import ctypes
import os

DPI_96 = 96                          # the scale Windows calls 100%
DPI_AWARENESS_CONTEXT_SYSTEM_AWARE = -2
PROCESS_SYSTEM_DPI_AWARE = 1         # SetProcessDpiAwareness' value for the same thing (Windows 8.1)
LOGPIXELSX = 88                      # GetDeviceCaps' horizontal DPI, the only source before Windows 10 1607
LOAD_LIBRARY_SEARCH_SYSTEM32 = 0x00000800   # ctypes.WinDLL's winmode: System32 only, never the installer's folder

_scale = None


def enable():
    """Make the process system DPI aware, then return the display scale (1.0 at 96 DPI). Must run before the first
    window; a later call only returns the scale."""
    global _scale
    if _scale is None:
        dpi = _system_dpi() if os.name == 'nt' else DPI_96
        _scale = dpi / DPI_96
    return _scale


def scale():
    """The display scale: 1.0 at 100%, 2.0 at 200%."""
    return enable()


def px(n):
    """n pixels at 100% as this display's pixels: 14 stays 14 at 100% and becomes 28 at 200%."""
    return int(round(n * enable()))


def awareness():
    """What Windows says the process is: 0 unaware, 1 system aware, 2 per-monitor aware."""
    if os.name != 'nt':
        return 0
    try:   # Windows 10 1607
        user32 = ctypes.WinDLL('user32')
        user32.GetThreadDpiAwarenessContext.restype = ctypes.c_void_p
        user32.GetAwarenessFromDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        return int(user32.GetAwarenessFromDpiAwarenessContext(user32.GetThreadDpiAwarenessContext()))
    except (AttributeError, OSError):
        pass
    try:   # Windows 8.1
        value = ctypes.c_int()
        shcore = ctypes.WinDLL('shcore', winmode=LOAD_LIBRARY_SEARCH_SYSTEM32)
        shcore.GetProcessDpiAwareness.argtypes = [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        if shcore.GetProcessDpiAwareness(None, ctypes.byref(value)) == 0:
            return value.value
    except (AttributeError, OSError):
        pass
    try:   # Windows Vista
        return 1 if ctypes.WinDLL('user32').IsProcessDPIAware() else 0
    except (AttributeError, OSError):
        return 0


def _system_dpi():
    """Make the process system DPI aware and return the display DPI. Each call that this Windows version has is tried;
    one failing means the process already is aware (a manifest, or the call before it) and changes nothing."""
    try:
        user32 = ctypes.WinDLL('user32', use_last_error=True)
    except OSError:
        return DPI_96
    try:   # Windows 10 1607, and the only way to ask for the system context explicitly
        user32.SetProcessDpiAwarenessContext.argtypes = [ctypes.c_void_p]
        user32.SetProcessDpiAwarenessContext.restype = ctypes.c_bool
        user32.SetProcessDpiAwarenessContext(ctypes.c_void_p(DPI_AWARENESS_CONTEXT_SYSTEM_AWARE))
    except (AttributeError, OSError):
        pass
    try:   # Windows 8.1
        ctypes.WinDLL('shcore', winmode=LOAD_LIBRARY_SEARCH_SYSTEM32).SetProcessDpiAwareness(PROCESS_SYSTEM_DPI_AWARE)
    except (AttributeError, OSError):
        pass
    try:   # Windows Vista
        user32.SetProcessDPIAware()
    except (AttributeError, OSError):
        pass
    try:   # Windows 10 1607
        return int(user32.GetDpiForSystem()) or DPI_96
    except (AttributeError, OSError):
        pass
    try:   # before that
        gdi32 = ctypes.WinDLL('gdi32')
        user32.GetDC.restype = ctypes.c_void_p
        user32.GetDC.argtypes = [ctypes.c_void_p]
        user32.ReleaseDC.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        gdi32.GetDeviceCaps.argtypes = [ctypes.c_void_p, ctypes.c_int]
        dc = user32.GetDC(None)
        dpi = gdi32.GetDeviceCaps(ctypes.c_void_p(dc), LOGPIXELSX)
        user32.ReleaseDC(None, ctypes.c_void_p(dc))
        return dpi or DPI_96
    except (AttributeError, OSError):
        return DPI_96
