using System.Diagnostics;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace TotKLauncher;

/// "python -m installer ..." from a setup package (installer/cli.py) as a child process: the JSON event on each output line
/// (installer/events.py), every other line as it arrives, and the exit code (0 ok, 1 failed, 2 command line, 3 cancelled).
sealed class InstallerRun
{
    readonly SetupPackage package;
    readonly List<string> args;
    Process? process;
    volatile bool cancelled;

    public event Action<JsonElement>? Event;
    public event Action<string>? Line;
    public event Action<int>? Exited;

    public InstallerRun(SetupPackage package, List<string> args)
    {
        this.package = package;
        this.args = args;
    }

    public string CommandLine => $"{package.Python} -m installer {string.Join(' ', args.Select(a => a.Contains(' ') ? $"\"{a}\"" : a))}";

    public void Start()
    {
        Task.Run(() =>
        {
            try
            {
                var si = new ProcessStartInfo(package.Python)
                {
                    WorkingDirectory = package.Root,
                    UseShellExecute = false,
                    CreateNoWindow = true,   // (a console without a window: the Ctrl+Break of a cancel reaches the installer through it)
                    RedirectStandardOutput = true,
                    RedirectStandardError = true,
                    StandardOutputEncoding = Encoding.UTF8,
                    StandardErrorEncoding = Encoding.UTF8,
                };
                si.Environment["PYTHONIOENCODING"] = "utf-8";
                si.Environment["PYTHONUTF8"] = "1";
                si.Environment["PYTHONUNBUFFERED"] = "1";
                si.ArgumentList.Add("-m");
                si.ArgumentList.Add("installer");
                foreach (var a in args) si.ArgumentList.Add(a);
                using var p = Process.Start(si)!;
                process = p;
                p.ErrorDataReceived += (_, e) => { if (e.Data != null) Line?.Invoke(e.Data); };
                p.BeginErrorReadLine();
                try { p.PriorityClass = ProcessPriorityClass.BelowNormal; } catch (Exception) { }
                while (p.StandardOutput.ReadLine() is { } line)
                {
                    if (Parse(line) is { } ev) Event?.Invoke(ev);
                    else if (line.Trim().Length > 0) Line?.Invoke(line);
                }
                p.WaitForExit();
                Exited?.Invoke(p.ExitCode);
            }
            catch (Exception e)
            {
                Line?.Invoke($"The installer could not be started ({package.Python}): {e.Message}");
                Exited?.Invoke(cancelled ? 3 : -1);
            }
        });
    }

    /// The event on a line (installer/guilogic.py parse_line), or null when the line is not one
    static JsonElement? Parse(string line)
    {
        var t = line.Trim();
        if (!t.StartsWith('{')) return null;
        try
        {
            using var doc = JsonDocument.Parse(t);
            if (doc.RootElement.ValueKind != JsonValueKind.Object || !doc.RootElement.TryGetProperty("type", out var type) ||
                type.ValueKind != JsonValueKind.String) return null;
            return doc.RootElement.Clone();
        }
        catch (JsonException) { return null; }
    }

    /// Ctrl+Break on the installer's console: installer/cli.py takes it as a cancel and records what already finished. If it has
    /// not exited after 60 s, the process tree is killed.
    public async void Cancel()
    {
        if (cancelled) return;
        cancelled = true;
        var p = process;
        if (p == null) return;
        try
        {
            if (p.HasExited) return;
            if (!ConsoleBreak.Send(p.Id)) { p.Kill(entireProcessTree: true); return; }
            for (var i = 0; i < 600 && !p.HasExited; i++) await Task.Delay(100);
            if (!p.HasExited) p.Kill(entireProcessTree: true);
        }
        catch (Exception) { }
    }

    public void Kill()
    {
        cancelled = true;
        try { process?.Kill(entireProcessTree: true); } catch (Exception) { }
    }
}

/// installer/guilogic.py send_break in C#: this window has no console, so it attaches to the installer's for a moment and sends
/// Ctrl+Break to every process on it (the installer and the tools it runs). Its own handler keeps the event from closing this window.
static class ConsoleBreak
{
    delegate bool HandlerRoutine(uint ctrlType);

    static readonly HandlerRoutine Ignore = _ => true;
    static readonly object Gate = new();

    [DllImport("kernel32.dll", SetLastError = true)] static extern bool AttachConsole(uint processId);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool FreeConsole();
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool SetConsoleCtrlHandler(HandlerRoutine handler, bool add);
    [DllImport("kernel32.dll", SetLastError = true)] static extern bool GenerateConsoleCtrlEvent(uint ctrlEvent, uint processGroupId);

    const uint CtrlBreak = 1;

    public static bool Send(int pid)
    {
        lock (Gate)
        {
            FreeConsole();
            if (!AttachConsole((uint)pid)) return false;
            SetConsoleCtrlHandler(Ignore, true);
            var ok = GenerateConsoleCtrlEvent(CtrlBreak, 0);
            Thread.Sleep(500);   // (each process gets the event on a new thread: this one gets it too, and ignores it)
            FreeConsole();
            return ok;
        }
    }
}

/// The installer's message texts: installer/text/&lt;lang&gt;.json of the package, else en.json, else the id itself
/// (installer/guilogic.py Catalog)
sealed class Catalog
{
    readonly Dictionary<string, string> texts = new(), english = new();
    static readonly Regex Placeholder = new(@"\{(\w+)\}");
    static readonly Dictionary<string, string> ParamNames = new()
    {
        ["lang"] = "lang.", ["known"] = "lang.", ["step"] = "step.", ["input"] = "param.input.",
    };

    public static Catalog Load(SetupPackage? package, string lang = "en")
    {
        var c = new Catalog();
        if (package == null) return c;
        Read(Path.Combine(package.Root, "installer", "text", "en.json"), c.english);
        Read(Path.Combine(package.Root, "installer", "text", lang + ".json"), c.texts);
        return c;
    }

    static void Read(string path, Dictionary<string, string> into)
    {
        try
        {
            using var doc = JsonDocument.Parse(File.ReadAllText(path));
            foreach (var p in doc.RootElement.EnumerateObject())
                if (p.Value.ValueKind == JsonValueKind.String) into[p.Name] = p.Value.GetString()!;
        }
        catch (Exception) { }
    }

    public bool Has(string id) => texts.ContainsKey(id) || english.ContainsKey(id);

    public string Raw(string id) => texts.TryGetValue(id, out var t) ? t : english.TryGetValue(id, out var e) ? e : id;

    public string Text(string id, JsonElement? parameters = null)
    {
        var text = Raw(id);
        var values = new Dictionary<string, string> { ["game_exe"] = "totk.exe" };
        if (parameters is { ValueKind: JsonValueKind.Object } ps)
            foreach (var p in ps.EnumerateObject()) values[p.Name] = Param(p.Name, p.Value);
        return Placeholder.Replace(text, m => values.TryGetValue(m.Groups[1].Value, out var v) ? v : m.Value);
    }

    /// installer/guilogic.py Catalog.param: language codes, step ids and input names become the names the catalog gives them
    string Param(string name, JsonElement value)
    {
        if (value.ValueKind == JsonValueKind.Array) return string.Join(", ", value.EnumerateArray().Select(v => Param(name, v)));
        var raw = value.ValueKind switch
        {
            JsonValueKind.String => value.GetString() ?? "",
            JsonValueKind.Number => value.GetRawText(),
            JsonValueKind.True => "True",
            JsonValueKind.False => "False",
            JsonValueKind.Null => "None",
            _ => value.GetRawText(),
        };
        if (!ParamNames.TryGetValue(name, out var prefix)) return raw;
        if (Has(prefix + raw)) return Raw(prefix + raw);
        if (name == "known") return string.Join(", ", raw.Split(", ").Select(c => Has(prefix + c) ? Raw(prefix + c) : c));
        return raw;
    }
}

/// The game languages (installer/ini.py LANGUAGES)
static class GameLanguages
{
    public sealed record Item(string Code, string Name)
    {
        public override string ToString() => Name;
    }

    public static readonly Item[] All =
    {
        new("ja", "Japanese"), new("en-US", "English (US)"), new("en-GB", "English (UK)"), new("fr", "French"),
        new("fr-CA", "French (Canada)"), new("de", "German"), new("it", "Italian"), new("es", "Spanish"),
        new("es-419", "Spanish (Latin America)"), new("nl", "Dutch"), new("pt", "Portuguese"), new("pt-BR", "Portuguese (Brazil)"),
        new("ru", "Russian"), new("ko", "Korean"), new("zh-Hans", "Chinese (Simplified)"), new("zh-Hant", "Chinese (Traditional)"),
    };

    /// (installer/ini.py ALIASES: codes older installers wrote)
    public static Item? Find(string? code) =>
        All.FirstOrDefault(l => l.Code == (code switch { "zh-CN" => "zh-Hans", "zh-TW" => "zh-Hant", _ => code }));

    public static readonly string[] NeedFonts = { "ko", "zh-Hans", "zh-Hant" };
}
