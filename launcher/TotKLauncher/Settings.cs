using System.IO;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace TotKLauncher;

/// What the player chose last time, in %LOCALAPPDATA%\TotK Recomp\launcher.json. On the first run the paths come from the
/// setup.json of the official installer window (installer/recent.py), when there is one.
sealed class Settings
{
    public const string DefaultRepo = "valentderah/totk-pc-recomp";

    public string? Dest { get; set; }
    public string? Base { get; set; }
    public string? Update { get; set; }
    public string? Keys { get; set; }
    public string? Firmware { get; set; }
    public string? Lang { get; set; }
    public bool Dlss { get; set; } = true;
    public bool DesktopShortcut { get; set; } = true;
    public bool StartShortcut { get; set; } = true;
    /// owner/repo the releases come from (TOTK_LAUNCHER_REPO takes precedence)
    public string? Repo { get; set; }

    public static string Folder => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "TotK Recomp");
    static string FilePath => Path.Combine(Folder, "launcher.json");
    static string UpstreamRecent => Path.Combine(Folder, "setup.json");

    [JsonIgnore]
    public string RepoName =>
        Environment.GetEnvironmentVariable("TOTK_LAUNCHER_REPO") is { Length: > 0 } env ? env.Trim()
        : !string.IsNullOrWhiteSpace(Repo) ? Repo.Trim()
        : DefaultRepo;

    public static Settings Load()
    {
        Settings? s = null;
        try { s = JsonSerializer.Deserialize<Settings>(File.ReadAllText(FilePath)); } catch (Exception) { }
        if (s != null) return s;
        s = new Settings();
        // (first run: what the official tkinter window saved)
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(UpstreamRecent));
            string? Get(string name) =>
                doc.RootElement.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() : null;
            s.Dest = Get("dest");
            s.Base = Get("base");
            s.Update = Get("update");
            s.Keys = Get("keys");
            s.Firmware = Get("firmware");
        }
        catch (Exception) { }
        return s;
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Folder);
            var tmp = FilePath + ".tmp";
            File.WriteAllText(tmp, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
            File.Move(tmp, FilePath, overwrite: true);
        }
        catch (Exception) { }
    }
}
