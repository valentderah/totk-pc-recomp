using System.IO;
using System.Text.Json;

namespace TotKLauncher;

/// A setup package: the unpacked TotK-Setup-&lt;version&gt;-windows.zip (python\, installer\, tools\, llvm\, runtime\, VERSION...).
/// The launcher builds nothing itself: it runs "python -m installer" from the package (installer/cli.py).
sealed record SetupPackage(string Root, string Version)
{
    public string Python
    {
        get
        {
            var py = Path.Combine(Root, "python", "python.exe");
            return File.Exists(py) ? py : "python";   // (TOTK_SETUP_DIR pointing at a repository checkout: the Python on PATH)
        }
    }

    /// Where the launcher keeps the packages it downloads: %LOCALAPPDATA%\TotK Recomp\setups\&lt;tag&gt;
    public static string Store => Path.Combine(Settings.Folder, "setups");

    public static bool IsPackage(string dir) =>
        File.Exists(Path.Combine(dir, "installer", "__main__.py")) &&
        (File.Exists(Path.Combine(dir, "python", "python.exe")) || File.Exists(Path.Combine(dir, ".git", "HEAD")) || File.Exists(Path.Combine(dir, "VERSION")));

    public static SetupPackage? At(string? dir)
    {
        if (string.IsNullOrWhiteSpace(dir) || !Directory.Exists(dir) || !IsPackage(dir)) return null;
        string version;
        try { version = File.ReadAllText(Path.Combine(dir, "VERSION")).Trim(); }
        catch (Exception) { version = "unknown"; }
        return new SetupPackage(Path.GetFullPath(dir), version);
    }

    /// The packages on this PC: TOTK_SETUP_DIR, a "setup" folder next to the launcher (or the package itself, when the launcher
    /// was put inside one) and the ones it downloaded
    public static List<SetupPackage> Local()
    {
        var found = new List<SetupPackage>();
        void Add(string? dir)
        {
            if (At(dir) is { } p && !found.Any(f => string.Equals(f.Root, p.Root, StringComparison.OrdinalIgnoreCase))) found.Add(p);
        }
        Add(Environment.GetEnvironmentVariable("TOTK_SETUP_DIR"));
        var here = AppContext.BaseDirectory.TrimEnd('\\', '/');
        Add(here);
        Add(Path.GetDirectoryName(here));
        Add(Path.Combine(here, "setup"));
        try
        {
            if (Directory.Exists(Store))
                foreach (var dir in Directory.GetDirectories(Store))
                    if (!dir.EndsWith(".part", StringComparison.OrdinalIgnoreCase)) Add(dir);
        }
        catch (Exception) { }
        return found;
    }

    public static SetupPackage? Newest()
    {
        SetupPackage? best = null;
        foreach (var p in Local())
            if (best == null || Versions.Compare(p.Version, best.Version) > 0) best = p;
        return best;
    }

    /// The package that installed this version (to rebuild for mods without updating), else the newest
    public static SetupPackage? For(string? release) =>
        Local().FirstOrDefault(p => release != null && p.Version == release) ?? Newest();

    /// After a successful run: downloaded packages older than keep are removed (each takes several hundred MB)
    public static void RemoveOlderThan(SetupPackage keep, Action<string> log)
    {
        try
        {
            if (!Directory.Exists(Store)) return;
            foreach (var dir in Directory.GetDirectories(Store))
            {
                var p = At(dir);
                var stale = dir.EndsWith(".part", StringComparison.OrdinalIgnoreCase) || p != null && Versions.Compare(p.Version, keep.Version) < 0;
                if (!stale || string.Equals(Path.GetFullPath(dir), keep.Root, StringComparison.OrdinalIgnoreCase)) continue;
                try
                {
                    Directory.Delete(dir, true);
                    log($"old package removed: {dir}");
                }
                catch (Exception e) { log($"old package not removed ({dir}): {e.Message}"); }
            }
        }
        catch (Exception) { }
    }
}

/// What totk-install.json (installer/state.py) says about an installed folder
static class Installed
{
    public const string StateFile = "totk-install.json";

    public static bool Has(string? folder) =>
        !string.IsNullOrWhiteSpace(folder) && File.Exists(Path.Combine(folder, StateFile));

    static JsonDocument? Read(string folder)
    {
        try { return JsonDocument.Parse(File.ReadAllText(Path.Combine(folder, StateFile))); }
        catch (Exception) { return null; }
    }

    public static string? Release(string? folder) => Value(folder, "release");
    public static string? Lang(string? folder) => Value(folder, "lang");

    static string? Value(string? folder, string name)
    {
        if (!Has(folder)) return null;
        using var doc = Read(folder!);
        return doc != null && doc.RootElement.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
    }

    public static string GameExe(string folder) => Path.Combine(folder, "totk.exe");

    public static bool HasDlss(string folder) => File.Exists(Path.Combine(folder, "nvngx_dlss.dll"));
}
