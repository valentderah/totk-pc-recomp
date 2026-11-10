using System.Runtime.InteropServices;
using System.Windows;
using System.Windows.Interop;
using System.Windows.Media;
using Microsoft.Win32;

namespace TotKLauncher;

/// The window frame through DWM: dark title colours and round corners everywhere, and the acrylic material behind the window on
/// Windows 11 22H2 and later. Older systems, light mode and high contrast get the solid Bg page instead.
static class Backdrop
{
    [DllImport("dwmapi.dll")] static extern int DwmSetWindowAttribute(IntPtr hwnd, int attribute, ref int value, int size);
    [DllImport("dwmapi.dll")] static extern int DwmExtendFrameIntoClientArea(IntPtr hwnd, ref Margins margins);
    [DllImport("user32.dll", EntryPoint = "GetWindowLongPtrW")] static extern nint GetWindowStyle(IntPtr hwnd, int index);
    [DllImport("user32.dll", EntryPoint = "SetWindowLongPtrW")] static extern nint SetWindowStyle(IntPtr hwnd, int index, nint value);

    struct Margins { public int Left, Right, Top, Bottom; }

    // DWMWINDOWATTRIBUTE and its values (dwmapi.h)
    const int UseImmersiveDarkMode = 20, WindowCornerPreference = 33, SystemBackdropType = 38;
    const int CornerRound = 2, BackdropAcrylic = 3;
    const int StyleIndex = -16;
    const nint SystemMenu = 0x00080000;

    /// true when the acrylic is on and the page must stay see-through; false when the caller should paint the Bg brush
    public static bool TryApply(Window window)
    {
        var hwnd = new WindowInteropHelper(window).Handle;
        if (hwnd == IntPtr.Zero) return false;
        // WindowChrome keeps the native frame, which the backdrop needs; without WS_SYSMENU Windows draws no caption buttons of its
        // own under the window's minimize and close buttons
        SetWindowStyle(hwnd, StyleIndex, GetWindowStyle(hwnd, StyleIndex) & ~SystemMenu);
        int dark = 1, corner = CornerRound;
        DwmSetWindowAttribute(hwnd, UseImmersiveDarkMode, ref dark, sizeof(int));
        DwmSetWindowAttribute(hwnd, WindowCornerPreference, ref corner, sizeof(int));
        if (Environment.OSVersion.Version.Build < 22621 || SystemParameters.HighContrast || SystemUsesLightTheme()) return false;
        if (HwndSource.FromHwnd(hwnd)?.CompositionTarget is { } target) target.BackgroundColor = Colors.Transparent;
        var whole = new Margins { Left = -1, Right = -1, Top = -1, Bottom = -1 };
        DwmExtendFrameIntoClientArea(hwnd, ref whole);
        var type = BackdropAcrylic;
        return DwmSetWindowAttribute(hwnd, SystemBackdropType, ref type, sizeof(int)) == 0;
    }

    /// The acrylic follows the system colour mode; its light variant is too bright for the light text of this palette
    static bool SystemUsesLightTheme()
    {
        try
        {
            return Registry.GetValue(@"HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Themes\Personalize", "SystemUsesLightTheme", 0)
                is int light && light != 0;
        }
        catch (Exception)
        {
            return false;
        }
    }
}
