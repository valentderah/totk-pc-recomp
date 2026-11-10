using System.IO;
using System.Runtime.InteropServices;

namespace TotKLauncher;

/// The "TotK Recomp" shortcuts on the Desktop and in the Start menu. They open this launcher with --launch and the game folder, so
/// Play also offers Update when a new release is out. The .lnk files are written through the WScript.Shell COM object.
static class Shortcuts
{
    const string FileName = "TotK Recomp.lnk";

    /// The last shortcut that could not be written, for the log
    public static string? Error;

    public static void Make(string gameFolder, bool desktop, bool startMenu)
    {
        var launcher = Environment.ProcessPath;
        if (launcher == null) return;
        var folder = gameFolder.TrimEnd('\\', '/');
        var game = Installed.GameExe(folder);
        var icon = File.Exists(game) ? game : launcher;
        Write(Environment.SpecialFolder.DesktopDirectory, desktop);
        Write(Environment.SpecialFolder.Programs, startMenu);

        void Write(Environment.SpecialFolder where, bool wanted)
        {
            var path = Path.Combine(Environment.GetFolderPath(where), FileName);
            object? shell = null, link = null;
            try
            {
                if (!wanted)
                {
                    if (File.Exists(path)) File.Delete(path);
                    return;
                }
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                shell = Activator.CreateInstance(Type.GetTypeFromProgID("WScript.Shell", throwOnError: true)!);
                link = ((dynamic)shell!).CreateShortcut(path);
                dynamic l = link!;
                l.TargetPath = launcher;
                l.Arguments = $"--launch \"{folder}\"";
                l.WorkingDirectory = Path.GetDirectoryName(launcher);
                l.IconLocation = icon + ",0";
                l.Description = "The Legend of Zelda: Tears of the Kingdom (TotK Recomp)";
                l.Save();
            }
            catch (Exception e)
            {
                Error = $"{FileName} ({where}): {e.Message}";
            }
            finally
            {
                if (link != null) Marshal.FinalReleaseComObject(link);
                if (shell != null) Marshal.FinalReleaseComObject(shell);
            }
        }
    }
}
