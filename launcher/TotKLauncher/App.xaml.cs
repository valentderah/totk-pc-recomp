using System.Diagnostics;
using System.IO;
using System.Windows;

namespace TotKLauncher;

public partial class App : Application
{
    /// Where the launcher lives: the shortcuts and build-cache\rebuild.txt point here, so the downloaded .exe can be deleted
    public static string InstallDir =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Programs", "TotK Recomp");

    protected override void OnStartup(StartupEventArgs e)
    {
        if (Relocate(e.Args))
        {
            StartupUri = null;
            Shutdown();
            return;
        }
        base.OnStartup(e);
    }

    /// Opened from outside InstallDir (the downloaded .exe): copies itself there and restarts the copy with the same arguments.
    /// Only the published single-file .exe does this; a run from bin\ (dotnet run) stays where it is.
    static bool Relocate(string[] args)
    {
        var exe = Environment.ProcessPath;
#pragma warning disable IL3000   // (empty in a single-file .exe is exactly the test)
        if (exe == null || typeof(App).Assembly.Location.Length > 0) return false;
#pragma warning restore IL3000
        var target = Path.Combine(InstallDir, "TotKLauncher.exe");
        if (string.Equals(Path.GetFullPath(exe), target, StringComparison.OrdinalIgnoreCase)) return false;
        try
        {
            Directory.CreateDirectory(InstallDir);
            var tmp = target + ".tmp";
            File.Copy(exe, tmp, overwrite: true);
            File.Move(tmp, target, overwrite: true);
        }
        // (the installed copy is running: open that one; the new one replaces it the next time it is closed)
        catch (Exception)
        {
            try { File.Delete(target + ".tmp"); } catch (Exception) { }
            if (!File.Exists(target)) return false;   // (could not install: run from here)
        }

        var psi = new ProcessStartInfo(target) { WorkingDirectory = InstallDir, UseShellExecute = false };
        foreach (var a in args) psi.ArgumentList.Add(a);
        // (a "setup" package folder next to the downloaded .exe still counts: see SetupPackage.Local)
        var setup = Path.Combine(Path.GetDirectoryName(exe)!, "setup");
        if (Directory.Exists(setup) && string.IsNullOrEmpty(Environment.GetEnvironmentVariable("TOTK_SETUP_DIR")))
            psi.Environment["TOTK_SETUP_DIR"] = setup;
        try { Process.Start(psi); return true; }
        catch (Exception) { return false; }
    }
}
