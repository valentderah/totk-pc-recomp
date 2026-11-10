using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.Json;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Media.Animation;
using System.Windows.Shapes;
using System.Windows.Threading;
using Microsoft.Win32;
using Path = System.Windows.Shapes.Path;
using IOPath = System.IO.Path;

namespace TotKLauncher;

/// The launcher and installer window: Files > Install > Play for a new install, and the launcher page (Play, plus Update when a new
/// release is out) for an installed folder. The work itself is "python -m installer" from a setup package (Installer.cs); the
/// packages come from the GitHub releases (Releases.cs).
///
///     TotKLauncher.exe                     the launcher of the last installed folder, else the install page
///     TotKLauncher.exe --launch FOLDER     the launcher of that folder (what the shortcuts run)
///     TotKLauncher.exe --update FOLDER     update that folder to the latest release
///     TotKLauncher.exe --rebuild FOLDER    rebuild for code mods and start the game (totk.exe runs it through build-cache\rebuild.txt)
public partial class MainWindow : Window
{
    enum Kind { Install, Resume, Update, Verify, Rebuild }

    static readonly string[] StepOrder = { "check", "fonts", "extract", "lift", "compile", "prepare", "dlss", "cleanup" };
    /// (names used until the setup package's text catalog is loaded)
    static readonly Dictionary<string, string> StepNames = new()
    {
        ["download"] = "Download the setup package",
        ["check"] = "Checking the computer and the game files",
        ["fonts"] = "Extracting the system fonts",
        ["extract"] = "Extracting the game",
        ["lift"] = "Translating the game code to C",
        ["compile"] = "Compiling totk.exe",
        ["prepare"] = "Preparing the shaders",
        ["dlss"] = "Downloading NVIDIA DLSS",
        ["cleanup"] = "Finishing",
    };
    /// Each step's share of the overall progress: compiling and the shaders take most of the time
    static readonly Dictionary<string, double> Weight = new()
    {
        ["download"] = 6, ["check"] = 2, ["fonts"] = 1, ["extract"] = 14, ["lift"] = 14, ["compile"] = 40, ["prepare"] = 20, ["dlss"] = 2, ["cleanup"] = 1,
    };
    const long NeedFirst = 36_000_000_000, NeedUpdate = 20_000_000_000;   // (docs/installer.md: 36 GiB at the peak of a first install)

    readonly Settings settings = Settings.Load();
    Catalog cat = new();
    SetupPackage? localPackage;
    Release? latest;
    bool latestChecked;
    Task<Release?>? latestTask;

    readonly DispatcherTimer clock = new() { Interval = TimeSpan.FromSeconds(1) };
    readonly Stopwatch elapsed = new();
    readonly Dictionary<string, StageRow> rows = new();
    readonly List<string> order = new();
    readonly HashSet<string> finished = new();
    readonly StringBuilder log = new();
    readonly List<string> warnings = new();
    readonly HashSet<string> touched = new();
    bool programmatic, langTouched, closing, closed;

    // the current run
    Kind kind;
    InstallerRun? run;
    CancellationTokenSource? download;
    SetupPackage? runPackage;
    List<string> runArgs = new();
    string runDest = "", launchFolder = "", downloadNote = "", lastFailure = "", lastPhase = "";
    string? current, runRelease, logPath, result;
    double currentFraction;
    (string Id, JsonElement? Params)? error;

    public MainWindow()
    {
        InitializeComponent();
        LanguageBox.ItemsSource = GameLanguages.All;
        clock.Tick += (_, _) => Tick();
        // (text boxes refuse dropped files: the window handles the drag before they see it)
        PreviewDragOver += (_, e) =>
        {
            if (!e.Data.GetDataPresent(DataFormats.FileDrop)) return;
            e.Effects = DragDropEffects.Copy;
            e.Handled = true;
        };
        PreviewDrop += Dropped;
        Closing += OnClosing;
        Closed += (_, _) => closed = true;
        SourceInitialized += (_, _) =>
        {
            if (Backdrop.TryApply(this)) return;
            Background = (Brush)FindResource("Bg");
            Root.Background = (Brush)FindResource("Bg");
        };
        Loaded += (_, _) => Start();
        SetStep(0);
    }

    // ================= splash =================

    async void Start()
    {
        SpinTurn.BeginAnimation(RotateTransform.AngleProperty, new DoubleAnimation(0, 360, TimeSpan.FromSeconds(1.1)) { RepeatBehavior = RepeatBehavior.Forever });
        var started = Stopwatch.StartNew();
        var args = Environment.GetCommandLineArgs();
        string? Arg(string name)
        {
            var i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? FullPath(args[i + 1].Trim().Trim('"').TrimEnd('\\', '/')) : null;
        }
        localPackage = SetupPackage.Newest();
        ApplySettings();
        latestTask = CheckLatest();
        if (Arg("--rebuild") is { } rebuild)
        {
            SplashText.Text = "Rebuilding for your mods…";
            await Task.Delay(700);
            ShowMain();
            launchFolder = rebuild;
            BeginFor(rebuild, Kind.Rebuild);
            return;
        }
        if (Arg("--update") is { } update)
        {
            SplashText.Text = "Getting the update…";
            await Task.Delay(700);
            ShowMain();
            launchFolder = update;
            BeginFor(update, Kind.Update);
            return;
        }
        var launch = Arg("--launch") ?? (Installed.Has(settings.Dest) ? settings.Dest : null);
        if (launch != null)
        {
            SplashText.Text = "Starting TotK Recomp…";
            await Task.Delay(700);
            ShowMain();
            ShowLauncher(launch);
            return;
        }
        SplashText.Text = localPackage != null ? $"Setup package {Versions.Short(localPackage.Version)} found" : "Looking for the latest version…";
        var wait = 1500 - (int)started.ElapsedMilliseconds;
        if (wait > 0) await Task.Delay(wait);
        ShowMain();
        ShowFiles();
    }

    /// The latest GitHub release, once per run; the pages refresh when it arrives
    async Task<Release?> CheckLatest()
    {
        try { latest = await Releases.Latest(settings.RepoName); }
        catch (Exception) { latest = null; }
        latestChecked = true;
        RefreshPackageNote();
        RefreshMode();
        RefreshLaunchStatus();
        return latest;
    }

    /// The version an install or update gets now: the GitHub release when it is newer than the package on this PC
    string? TargetVersion()
    {
        var have = localPackage?.Version;
        return latest != null && (have == null || Versions.Newer(latest.Tag, have)) ? latest.Tag : have;
    }

    void ApplySettings()
    {
        programmatic = true;
        BasePath.Text = settings.Base ?? "";
        UpdatePath.Text = settings.Update ?? "";
        KeysPath.Text = settings.Keys ?? "";
        FirmwarePath.Text = settings.Firmware ?? "";
        OutPath.Text = settings.Dest ?? IOPath.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Games", "TotK");
        LanguageBox.SelectedItem = GameLanguages.Find(settings.Lang) ?? GameLanguages.Find("en-US");
        DlssTick.IsChecked = settings.Dlss;
        DesktopTick.IsChecked = settings.DesktopShortcut;
        StartTick.IsChecked = settings.StartShortcut;
        programmatic = false;
    }

    void SaveSettings()
    {
        settings.Base = Value(BasePath);
        settings.Update = Value(UpdatePath);
        settings.Keys = Value(KeysPath);
        settings.Firmware = Value(FirmwarePath);
        settings.Dest = FullPath(OutPath.Text.Trim());
        settings.Lang = (LanguageBox.SelectedItem as GameLanguages.Item)?.Code;
        settings.Dlss = DlssTick.IsChecked == true;
        settings.DesktopShortcut = DesktopTick.IsChecked == true;
        settings.StartShortcut = StartTick.IsChecked == true;
        settings.Save();
    }

    static string? Value(TextBox box) => box.Text.Trim() is { Length: > 0 } v ? FullPath(v) : null;

    static string FullPath(string path)
    {
        try { return IOPath.GetFullPath(path); }
        catch (Exception) { return path; }
    }

    /// The splash fades out and the first page slides in, in the same window
    void ShowMain()
    {
        var fade = new DoubleAnimation(0, TimeSpan.FromMilliseconds(350));
        fade.Completed += (_, _) =>
        {
            Splash.Visibility = Visibility.Collapsed;
            SpinTurn.BeginAnimation(RotateTransform.AngleProperty, null);
        };
        Splash.BeginAnimation(OpacityProperty, fade);
        var ease = new QuadraticEase { EasingMode = EasingMode.EaseOut };
        var delay = TimeSpan.FromMilliseconds(120);
        Main.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(400)) { BeginTime = delay });
        MainLift.BeginAnimation(TranslateTransform.YProperty, new DoubleAnimation(0, TimeSpan.FromMilliseconds(450)) { BeginTime = delay, EasingFunction = ease });
        Main.IsHitTestVisible = true;
    }

    // ================= the stepper: 1 Files - 2 Install - 3 Play =================

    void SetStep(int on)
    {
        Stepper.Children.Clear();
        var names = new[] { "Files", "Install", "Play" };
        for (var i = 0; i < names.Length; i++)
        {
            if (i > 0)
                Stepper.Children.Add(new Rectangle { Width = 28, Height = 1, Fill = (Brush)FindResource(i <= on ? "Accent" : "LineStrong"),
                                                     VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(10, 0, 10, 0) });
            var done = i < on;
            var badge = new Grid { Width = 22, Height = 22, Margin = new Thickness(0, 0, 8, 0) };
            badge.Children.Add(new Ellipse
            {
                Fill = (Brush)FindResource(i == on ? "Accent" : done ? "AccentSoft" : "Surface"),
                Stroke = i > on ? (Brush)FindResource("LineStrong") : null, StrokeThickness = 1,
            });
            if (done)
                badge.Children.Add(new Path { Data = Geometry.Parse("M0,4 L3.5,7.5 L10,0"), Stroke = (Brush)FindResource("AccentLight"), StrokeThickness = 1.8,
                                              HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
            else
                badge.Children.Add(new TextBlock { Text = (i + 1).ToString(), FontSize = 11, FontWeight = FontWeights.Bold,
                                                   Foreground = (Brush)FindResource(i == on ? "AccentText" : "TextMuted"),
                                                   HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center });
            var label = new TextBlock { Text = names[i], FontSize = 13, VerticalAlignment = VerticalAlignment.Center,
                                        FontWeight = i == on ? FontWeights.SemiBold : FontWeights.Medium,
                                        Foreground = (Brush)FindResource(i == on ? "Text" : done ? "TextDim" : "TextMuted") };
            Stepper.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, Children = { badge, label } });
        }
        Stepper.Visibility = Visibility.Visible;
        VersionTag.Visibility = Visibility.Collapsed;
    }

    /// An update, verify or rebuild shows what it works on instead of the stepper
    void ShowTag(string text)
    {
        Stepper.Visibility = Visibility.Collapsed;
        VersionTag.Visibility = Visibility.Visible;
        VersionTagText.Text = text;
    }

    void ShowPage(FrameworkElement page)
    {
        foreach (var p in new FrameworkElement[] { FilesPage, BuildPage, ReadyPage, ProblemPage, LaunchPage })
            p.Visibility = p == page ? Visibility.Visible : Visibility.Collapsed;
        page.Opacity = 0;
        page.BeginAnimation(OpacityProperty, new DoubleAnimation(1, TimeSpan.FromMilliseconds(220)));
    }

    // ================= launcher =================

    void ShowLauncher(string folder)
    {
        launchFolder = folder;
        Stepper.Visibility = Visibility.Collapsed;
        VersionTag.Visibility = Visibility.Collapsed;
        var installed = Installed.Release(folder);
        LaunchVersion.Text = installed != null ? $"TotK Recomp {Versions.Short(installed)} · {folder}" : folder;
        LaunchPlay.IsEnabled = File.Exists(Installed.GameExe(folder));
        ShowPage(LaunchPage);
        RefreshLaunchStatus();
    }

    void RefreshLaunchStatus()
    {
        if (LaunchPage.Visibility != Visibility.Visible || launchFolder.Length == 0) return;
        LaunchUpdate.Visibility = Visibility.Collapsed;
        LaunchStatus.Foreground = (Brush)FindResource("TextDim");
        LaunchStatus.FontWeight = FontWeights.Normal;
        var installed = Installed.Release(launchFolder);
        if (installed == null)
        {
            LaunchStatus.Text = "This folder has no TotK Recomp install. Use \"Install to another folder\".";
            return;
        }
        var target = TargetVersion();
        if (target != null && Versions.Newer(target, installed))
        {
            var fetch = latest != null && target == latest.Tag && (localPackage == null || Versions.Newer(latest.Tag, localPackage.Version));
            LaunchStatus.Text = $"Update available: {Versions.Short(target)}" + (fetch ? $" · {latest!.SizeText} download" : "");
            LaunchStatus.Foreground = (Brush)FindResource("AccentLight");
            LaunchStatus.FontWeight = FontWeights.SemiBold;
            LaunchUpdate.Visibility = Visibility.Visible;
        }
        else if (!latestChecked) LaunchStatus.Text = "Looking for updates…";
        else if (latest == null) LaunchStatus.Text = "Could not look for updates (GitHub did not answer).";
        else LaunchStatus.Text = $"Up to date · {latest.Tag} is the latest version";
    }

    void LaunchUpdateClick(object sender, RoutedEventArgs e) => BeginFor(launchFolder, Kind.Update);

    void LaunchVerifyClick(object sender, RoutedEventArgs e) => BeginFor(launchFolder, Kind.Verify);

    void LaunchPlayClick(object sender, RoutedEventArgs e)
    {
        if (StartGame(launchFolder)) Application.Current.Shutdown();
    }

    void LaunchFolderClick(object sender, RoutedEventArgs e) => OpenExplorer(launchFolder);

    void LaunchOtherClick(object sender, RoutedEventArgs e) => ShowFiles();

    void BackToLauncherClick(object sender, RoutedEventArgs e)
    {
        if (launchFolder.Length > 0) ShowLauncher(launchFolder);
    }

    bool StartGame(string folder)
    {
        try
        {
            Process.Start(new ProcessStartInfo(Installed.GameExe(folder)) { UseShellExecute = false, WorkingDirectory = folder });
            return true;
        }
        catch (Exception x)
        {
            MessageBox.Show(this, $"The game could not be started: {x.Message}", "TotK Recomp");
            return false;
        }
    }

    static void OpenExplorer(string folder)
    {
        try { Process.Start(new ProcessStartInfo("explorer.exe", $"\"{folder}\"") { UseShellExecute = true }); }
        catch (Exception) { }
    }

    // ================= 1: game files =================

    void ShowFiles()
    {
        SetStep(0);
        FormError.Visibility = Visibility.Collapsed;
        RefreshPackageNote();
        Refresh();
        ShowPage(FilesPage);
    }

    void RefreshPackageNote()
    {
        if (PackageNote == null) return;
        var have = localPackage;
        PackageNote.Foreground = (Brush)FindResource("TextDim");
        if (!latestChecked)
            PackageNote.Text = have != null
                ? $"Setup package {Versions.Short(have.Version)} on this PC. Looking for newer versions in the GitHub releases…"
                : $"Looking for the setup package in the GitHub releases ({settings.RepoName})…";
        else if (latest != null && (have == null || Versions.Newer(latest.Tag, have.Version)))
            PackageNote.Text = $"The install downloads package {latest.Tag} from the GitHub releases ({latest.SizeText}) to {SetupPackage.Store}." +
                               (have != null ? $" This PC has {Versions.Short(have.Version)}." : "");
        else if (have != null)
            PackageNote.Text = $"Setup package {Versions.Short(have.Version)} on this PC" +
                               (latest == null ? " (GitHub did not answer: newer versions could not be checked)." : ", the latest version.");
        else
        {
            PackageNote.Text = $"No setup package on this PC and GitHub did not answer ({settings.RepoName}). Check the connection, or unpack a " +
                               "TotK-Setup-<version>-windows.zip into a \"setup\" folder next to this launcher.";
            PackageNote.Foreground = (Brush)FindResource("Bad");
        }
    }

    void PathsChanged(object sender, TextChangedEventArgs e)
    {
        if (!programmatic)
        {
            if (sender == BasePath) touched.Add("base");
            else if (sender == UpdatePath) touched.Add("update");
        }
        Refresh();
    }

    void LanguageChanged(object sender, SelectionChangedEventArgs e)
    {
        if (!programmatic) langTouched = true;
    }

    void DlssChanged(object sender, RoutedEventArgs e) => SaveSettings();

    void Refresh()
    {
        if (BuildButton == null || SpaceText == null) return;
        RefreshSpace();
        RefreshMode();
    }

    void RefreshSpace()
    {
        var dest = OutPath.Text.Trim();
        var need = Installed.Has(dest) ? NeedUpdate : NeedFirst;
        try
        {
            var root = IOPath.GetPathRoot(IOPath.GetFullPath(dest))!;
            var free = new DriveInfo(root).AvailableFreeSpace;
            var enough = free >= need;
            SpaceBar.Value = Math.Min(1, need / (double)Math.Max(1, free));
            SpaceBar.Foreground = (Brush)FindResource(enough ? "Ok" : "Bad");
            SpaceText.Text = $"Needs {need / 1_000_000_000} GB · {root.TrimEnd('\\')} has {free / 1_000_000_000} GB free";
            SpaceText.Foreground = (Brush)FindResource(enough ? "TextDim" : "Bad");
        }
        catch (Exception)
        {
            SpaceBar.Value = 0;
            SpaceText.Text = $"Needs {need / 1_000_000_000} GB";
            SpaceText.Foreground = (Brush)FindResource("TextDim");
        }
    }

    /// The page title and the button: install, continue an install, or update the chosen folder
    void RefreshMode()
    {
        if (BuildButton == null || FilesTitle == null || run != null) return;
        var dest = OutPath.Text.Trim();
        var installed = Installed.Has(dest) ? Installed.Release(dest) ?? "?" : null;
        var target = TargetVersion();
        if (installed != null)
        {
            if (!langTouched && GameLanguages.Find(Installed.Lang(dest)) is { } recorded)
            {
                programmatic = true;
                LanguageBox.SelectedItem = recorded;
                programmatic = false;
            }
            if (target != null && Versions.Newer(target, installed))
            {
                FilesTitle.Text = "Update available";
                FilesSub.Text = $"This folder has TotK Recomp {Versions.Short(installed)} and {Versions.Short(target)} is out. The update keeps the " +
                                "extracted game, your saves and your settings, and redoes only what changed.";
                BuildButton.Content = "Update";
            }
            else
            {
                FilesTitle.Text = "This folder already has the game";
                FilesSub.Text = $"TotK Recomp {Versions.Short(installed)}. Continue resumes an interrupted install and checks what is already done; " +
                                "the game files are only needed if they have to be extracted again.";
                BuildButton.Content = "Continue";
            }
        }
        else
        {
            FilesTitle.Text = "Install your game";
            FilesSub.Text = "Pick your game files and where the game should go. The installer builds the game on your PC, once.";
            BuildButton.Content = "Install the game";
        }
        BuildButton.IsEnabled = dest.Length > 2;
        BackToLauncher.Visibility = launchFolder.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    TextBox Box(string which) => which switch { "base" => BasePath, "update" => UpdatePath, "keys" => KeysPath, _ => FirmwarePath };

    void BrowseFile(object sender, RoutedEventArgs e)
    {
        var which = (string)((Button)sender).Tag;
        var d = new OpenFileDialog
        {
            Title = which switch { "base" => "Choose the base game file", "update" => "Choose the 1.1.0, 1.2.1 or 1.4.3 update", _ => "Choose prod.keys" },
            Filter = which == "keys"
                ? "Key files (*.keys)|*.keys|All files|*.*"
                : "Game files (NSP, XCI, NSZ, XCZ)|*.nsp;*.xci;*.nsz;*.xcz|All files|*.*",
        };
        var now = Box(which).Text.Trim();
        if (now.Length > 0 && IOPath.GetDirectoryName(now) is { } dir && Directory.Exists(dir)) d.InitialDirectory = dir;
        if (d.ShowDialog(this) == true) Box(which).Text = d.FileName;
    }

    void BrowseFirmware(object sender, RoutedEventArgs e)
    {
        if (PickFolder("Choose the Switch firmware folder (Contents/registered)") is { } f) FirmwarePath.Text = f;
    }

    void BrowseOut(object sender, RoutedEventArgs e)
    {
        if (PickFolder("Choose where the game goes") is { } f) OutPath.Text = f;
    }

    static string? PickFolder(string title)
    {
        var d = new OpenFolderDialog { Title = title };
        return d.ShowDialog() == true ? d.FolderName : null;
    }

    /// Files dropped on the window: .keys, game files (the update recognised by its name), the firmware folder or the install folder
    void Dropped(object sender, DragEventArgs e)
    {
        if (run != null || download != null || FilesPage.Visibility != Visibility.Visible) return;
        if (e.Data.GetData(DataFormats.FileDrop) is not string[] { Length: > 0 } paths) return;
        e.Handled = true;
        foreach (var p in paths)
        {
            var ext = IOPath.GetExtension(p).ToLowerInvariant();
            if (File.Exists(p) && ext == ".keys") KeysPath.Text = p;
            else if (File.Exists(p) && ext is ".nsp" or ".xci" or ".nsz" or ".xcz")
            {
                if (LooksLikeUpdate(p)) UpdatePath.Text = p;
                else if (BasePath.Text.Trim().Length == 0 || ext is ".xci" or ".xcz") BasePath.Text = p;
                else UpdatePath.Text = p;
            }
            else if (Directory.Exists(p))
            {
                if (IOPath.GetFileName(p).Equals("registered", StringComparison.OrdinalIgnoreCase) ||
                    Directory.EnumerateFiles(p, "*.nca").Any() || Directory.EnumerateDirectories(p, "*.nca").Any()) FirmwarePath.Text = p;
                else OutPath.Text = p;
            }
        }
    }

    /// (the TotK update's title id ends in 800, and file names usually carry the version)
    static bool LooksLikeUpdate(string path)
    {
        var name = IOPath.GetFileName(path);
        return new[] { "0100F2C0115B6800", "update", "upd", "1.1.0", "1.2.1", "1.4.3", "v131072", "v262144", "v655360" }
            .Any(k => name.Contains(k, StringComparison.OrdinalIgnoreCase));
    }

    List<string> Problems(string dest, bool install)
    {
        var p = new List<string>();
        if (dest.Length < 3) p.Add("Choose the install folder.");
        foreach (var (box, need, missing) in new[]
                 {
                     (BasePath, "Choose the base game file.", "The base game file does not exist."),
                     (UpdatePath, "Choose the 1.1.0, 1.2.1 or 1.4.3 update file.", "The update file does not exist."),
                     (KeysPath, "Choose the prod.keys file.", "The keys file does not exist."),
                 })
        {
            var v = box.Text.Trim();
            if (v.Length == 0) { if (install) p.Add(need); }
            else if (!File.Exists(v)) p.Add(missing);
        }
        if (FirmwarePath.Text.Trim() is { Length: > 0 } fw && !Directory.Exists(fw)) p.Add("The firmware folder does not exist.");
        if (LanguageBox.SelectedItem is not GameLanguages.Item) p.Add("Choose the game language.");
        return p;
    }

    void StartInstall(object sender, RoutedEventArgs e)
    {
        var dest = FullPath(OutPath.Text.Trim());
        var installed = Installed.Release(dest);
        var install = !Installed.Has(dest);
        var problems = Problems(dest, install);
        FormError.Text = string.Join(" ", problems);
        FormError.Visibility = problems.Count > 0 ? Visibility.Visible : Visibility.Collapsed;
        if (problems.Count > 0) return;
        SaveSettings();
        var args = new List<string> { "--dest", dest };
        void Add(string flag, string? value)
        {
            if (!string.IsNullOrWhiteSpace(value)) args.AddRange(new[] { flag, FullPath(value.Trim()) });
        }
        // (for an installed folder the base game and update are passed only when picked now: paths kept from another install would
        // make the extraction run again, as installer/gui.py match_prefill avoids too)
        if (install || touched.Contains("base")) Add("--base", BasePath.Text);
        if (install || touched.Contains("update")) Add("--update", UpdatePath.Text);
        Add("--keys", KeysPath.Text);
        Add("--firmware", FirmwarePath.Text);
        if (LanguageBox.SelectedItem is GameLanguages.Item lang) args.AddRange(new[] { "--lang", lang.Code });
        if (DlssTick.IsChecked == true) args.Add("--dlss");
        var k = install ? Kind.Install : Versions.Newer(TargetVersion(), installed) ? Kind.Update : Kind.Resume;
        _ = Begin(dest, args, k);
    }

    // ================= 2: the run =================

    /// Update, verify or rebuild an installed folder: only --dest (the language and inputs come from its totk-install.json)
    void BeginFor(string folder, Kind k)
    {
        var args = new List<string> { "--dest", folder };
        if (Installed.HasDlss(folder)) args.Add("--dlss");
        if (settings.Keys is { } keys && File.Exists(keys)) args.AddRange(new[] { "--keys", keys });
        if (k == Kind.Verify) args.Add("--verify");
        _ = Begin(folder, args, k);
    }

    async Task Begin(string dest, List<string> args, Kind k)
    {
        kind = k;
        runDest = dest;
        runArgs = args;
        runPackage = null;
        error = null;
        result = runRelease = logPath = current = null;
        currentFraction = 0;
        lastPhase = downloadNote = "";
        warnings.Clear();
        log.Clear();
        Log.Clear();
        WarnText.Visibility = Visibility.Collapsed;
        ShowLog.IsChecked = false;
        var installed = Installed.Release(dest);
        switch (k)
        {
            case Kind.Install:
            case Kind.Resume:
                SetStep(1);
                break;
            case Kind.Update:
                ShowTag($"{Versions.Short(installed)} → {Versions.Short(TargetVersion())}");
                break;
            case Kind.Verify:
                ShowTag($"Verify · {Versions.Short(installed)}");
                break;
            case Kind.Rebuild:
                ShowTag($"Mods · {Versions.Short(installed)}");
                break;
        }
        BuildTitle.Text = k switch
        {
            Kind.Install => "Installing your game",
            Kind.Resume => "Continuing the install",
            Kind.Update => "Updating your game",
            Kind.Verify => "Verifying the files",
            _ => "Rebuilding for your mods",
        };
        BuildNote.Text = "Getting ready…";
        CancelButton.IsEnabled = true;
        CancelButton.Content = "Cancel";
        rows.Clear();   // (a new run: nothing of the previous run's rows is kept)
        finished.Clear();
        MakeRows(withDownload: false, StepOrder);
        SetProgress(0);
        ShowPage(BuildPage);
        elapsed.Restart();
        clock.Start();

        var package = await EnsurePackage(k);
        if (package == null || closed) return;
        runPackage = package;
        cat = Catalog.Load(package);
        MakeRows(rows.ContainsKey("download"), StepOrder);   // (the step names in the package's own text)
        if (k == Kind.Update) ShowTag($"{Versions.Short(installed)} → {Versions.Short(package.Version)}");
        run = new InstallerRun(package, args);
        AddLog("> " + run.CommandLine);
        run.Event += ev => Dispatcher.BeginInvoke(() => OnEvent(ev));
        run.Line += line => Dispatcher.BeginInvoke(() => AddLog(line));
        run.Exited += code => Dispatcher.BeginInvoke(() => Ended(code));
        run.Start();
    }

    /// The package for this run: the one on this PC, or the latest GitHub release's when that is newer (or there is none).
    /// Verify and rebuild use the package of the installed version: they do not update.
    async Task<SetupPackage?> EnsurePackage(Kind k)
    {
        if (!latestChecked && latestTask != null)
        {
            BuildNote.Text = "Looking for the latest version on GitHub…";
            await latestTask;
        }
        var keep = k is Kind.Verify or Kind.Rebuild;
        var have = keep ? SetupPackage.For(Installed.Release(runDest)) : SetupPackage.Newest();
        var fetch = latest != null && (have == null || !keep && Versions.Newer(latest.Tag, have.Version));
        if (!fetch)
        {
            if (have != null) return have;
            clock.Stop();
            ShowProblem("No setup package",
                        $"This PC has no TotK Recomp setup package and the releases of {settings.RepoName} did not answer.\n\n" +
                        "Check the connection and try again, or download TotK-Setup-<version>-windows.zip from the releases page and unpack it " +
                        $"into a \"setup\" folder next to this launcher ({AppContext.BaseDirectory}).");
            return null;
        }
        var release = latest!;
        MakeRows(withDownload: true, StepOrder);
        current = "download";
        currentFraction = 0;
        rows["download"].Begin();
        downloadNote = $"{release.Tag} · {release.SizeText}";
        rows["download"].Detail(downloadNote);
        download = new CancellationTokenSource();
        var token = download.Token;
        try
        {
            var package = await Task.Run(() => Releases.Fetch(release,
                f => Dispatcher.BeginInvoke(() => DownloadProgress(f)),
                n => Dispatcher.BeginInvoke(() => { downloadNote = n; AddLog(n); rows.GetValueOrDefault("download")?.Detail(n); }),
                token), token);
            rows["download"].Done();
            rows["download"].Detail($"Package {release.Tag} ready");
            Finish("download");
            localPackage = SetupPackage.Newest();
            return package;
        }
        catch (OperationCanceledException)
        {
            clock.Stop();
            rows.GetValueOrDefault("download")?.Stopped();
            if (closing) Close();
            else AfterCancel();
            return null;
        }
        catch (Exception x)
        {
            rows.GetValueOrDefault("download")?.Stopped();
            AddLog("download: " + x.Message);
            if (have != null && k is Kind.Install or Kind.Resume)
            {
                // (installing with the package this PC already has beats stopping)
                rows["download"].Detail($"Failed ({x.Message}): using package {Versions.Short(have.Version)} from this PC");
                return have;
            }
            clock.Stop();
            ShowProblem("The setup package could not be downloaded",
                        x.Message + "\n\nNothing was changed in the game folder. Try again in a few minutes, or download the zip from the releases page.");
            return null;
        }
        finally
        {
            download = null;
        }
    }

    void DownloadProgress(double f)
    {
        if (current != "download") return;
        currentFraction = f;
        rows.GetValueOrDefault("download")?.Detail($"{downloadNote} · {(int)(f * 100)}%");
        UpdateOverall();
    }

    string StepName(string id) =>
        id != "download" && cat.Has("step." + id) ? cat.Text("step." + id) : StepNames.TryGetValue(id, out var n) ? n : id;

    /// The rows of the steps; rows that already exist keep their state
    void MakeRows(bool withDownload, IEnumerable<string> steps)
    {
        var old = new Dictionary<string, StageRow>(rows);
        var ids = new List<string>();
        if (withDownload) ids.Add("download");
        ids.AddRange(steps.Where(s => s != "download"));
        StageList.Children.Clear();
        rows.Clear();
        order.Clear();
        for (var i = 0; i < ids.Count; i++)
        {
            var last = i == ids.Count - 1;
            var row = old.TryGetValue(ids[i], out var had) ? had : new StageRow(StepName(ids[i]), last);
            row.SetLast(last);
            rows[ids[i]] = row;
            order.Add(ids[i]);
            StageList.Children.Add(row.Root);
        }
    }

    StageRow Row(string id)
    {
        if (!rows.ContainsKey(id)) MakeRows(rows.ContainsKey("download"), order.Append(id).ToList());
        return rows[id];
    }

    void Finish(string id)
    {
        finished.Add(id);
        if (current == id) current = null;
    }

    double W(string id) => Weight.TryGetValue(id, out var w) ? w : 2;

    void UpdateOverall()
    {
        var total = order.Sum(W);
        var done = order.Where(id => finished.Contains(id) && rows.ContainsKey(id)).Sum(W);
        if (current != null && !finished.Contains(current)) done += W(current) * Math.Clamp(currentFraction, 0, 1);
        SetProgress(total > 0 ? done / total : 0);
    }

    static string Str(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.String ? v.GetString() ?? "" : "";

    static double? Num(JsonElement e, string name) =>
        e.TryGetProperty(name, out var v) && v.ValueKind == JsonValueKind.Number ? v.GetDouble() : null;

    static JsonElement? Params(JsonElement e) => e.TryGetProperty("params", out var p) ? p : null;

    /// One installer event (installer/events.py): start, plan, step, progress, space, warning, error, end
    void OnEvent(JsonElement e)
    {
        switch (Str(e, "type"))
        {
            case "start":
                runRelease = Str(e, "release") is { Length: > 0 } rel ? rel : null;
                logPath = Str(e, "log");
                if (e.TryGetProperty("steps", out var steps) && steps.ValueKind == JsonValueKind.Array)
                {
                    var ids = steps.EnumerateArray().Select(s => s.GetString() ?? "").Where(s => s.Length > 0).ToList();
                    if (!ids.SequenceEqual(order.Where(id => id != "download"))) MakeRows(rows.ContainsKey("download"), ids);
                }
                if (Str(e, "mode") == "update" && kind is Kind.Install or Kind.Resume) BuildTitle.Text = "Updating your game";
                break;
            case "plan":
                if (e.TryGetProperty("skip", out var skip) && skip.ValueKind == JsonValueKind.Object)
                    foreach (var s in skip.EnumerateObject())
                        if (rows.TryGetValue(s.Name, out var r) && !r.Started)
                        {
                            r.Skipped(s.Value.ValueKind == JsonValueKind.String ? s.Value.GetString() ?? "" : "");
                            Finish(s.Name);
                        }
                break;
            case "step":
            {
                var id = Str(e, "step");
                if (id.Length == 0) break;
                var row = Row(id);
                switch (Str(e, "state"))
                {
                    case "started":
                        row.Begin();
                        current = id;
                        currentFraction = 0;
                        finished.Remove(id);
                        break;
                    case "done":
                        row.Done(Num(e, "seconds"));
                        Finish(id);
                        break;
                    case "skipped":
                        row.Skipped(Str(e, "reason"));
                        Finish(id);
                        break;
                }
                break;
            }
            case "progress":
            {
                var id = Str(e, "step");
                if (id.Length == 0) break;
                var row = Row(id);
                if (!row.Started) row.Begin();
                current = id;
                var text = cat.Text($"phase.{id}.{Str(e, "phase")}");
                if (Num(e, "total") is double total && total > 0)
                {
                    var done = Num(e, "done") ?? 0;
                    text += "  " + Amount(done, total, Str(e, "unit"));
                    currentFraction = done / total;
                }
                row.Detail(text);
                break;
            }
            case "warning":
                warnings.Add(cat.Text(Str(e, "id"), Params(e)));
                WarnText.Text = string.Join("\n", warnings);
                WarnText.Visibility = Visibility.Visible;
                break;
            case "error":
                error = (Str(e, "id"), Params(e));
                if (rows.TryGetValue(Str(e, "step"), out var failed)) failed.Stopped();
                break;
            case "end":
                result = Str(e, "result");
                break;
        }
        if (Describe(e) is { Length: > 0 } line) AddLog(line);
        UpdateOverall();
    }

    static string Amount(double done, double total, string unit) => unit switch
    {
        "bytes" => string.Format(CultureInfo.InvariantCulture, "{0:0.0} / {1:0.0} GB", done / 1e9, total / 1e9),
        "objects" => $"{done:0} / {total:0} files",
        "shaders" => $"{done:0} / {total:0} shaders",
        "chunks" => $"{done:0} / {total:0} parts",
        _ => $"{done:0} / {total:0} {unit}".TrimEnd(),
    };

    /// The log line for an event (as installer/guilogic.py describe_event writes it); progress only when the phase changes
    string Describe(JsonElement e)
    {
        switch (Str(e, "type"))
        {
            case "start":
                return $"installer {Str(e, "release")} ({Str(e, "mode")}) in {Str(e, "dest")} · log: {Str(e, "log")}";
            case "step":
                var extra = Num(e, "seconds") is { } sec ? string.Format(CultureInfo.InvariantCulture, " ({0:0.0} s)", sec)
                          : Str(e, "reason") is { Length: > 0 } reason ? $" ({reason})" : "";
                return $"{StepName(Str(e, "step"))}: {Str(e, "state")}{extra}";
            case "progress":
                var key = Str(e, "step") + "." + Str(e, "phase");
                if (key == lastPhase) return "";
                lastPhase = key;
                return "  " + cat.Text($"phase.{key}");
            case "space":
                return string.Format(CultureInfo.InvariantCulture, "Space needed: {0:0.0} GB, free: {1:0.0} GB",
                                     (Num(e, "need") ?? 0) / 1e9, (Num(e, "free") ?? 0) / 1e9);
            case "warning":
                return "Warning: " + cat.Text(Str(e, "id"), Params(e));
            case "error":
                return "Error: " + cat.Text(Str(e, "id"), Params(e)) + $" [{Str(e, "id")}]";
            case "end":
                return $"End: {Str(e, "result")}";
            case "plan":
                return "";
            default:
                return e.GetRawText();
        }
    }

    void AddLog(string line)
    {
        log.AppendLine(line);
        if (log.Length > 600_000) log.Remove(0, log.Length - 400_000);
        if (Log.Visibility == Visibility.Visible)
        {
            Log.AppendText(line + "\n");
            Log.ScrollToEnd();
        }
    }

    void SetProgress(double f)
    {
        f = Math.Clamp(f, 0, 1);
        PercentText.Text = $"{(int)(f * 100)}%";
        OverallBar.Value = f;
    }

    void Tick()
    {
        foreach (var r in rows.Values) r.Tick();
        if (run == null && download == null) return;
        var hint = current switch
        {
            "download" => "downloading the setup package",
            "compile" => "compiling is the longest step",
            "prepare" => "the shaders are ready before you play",
            _ => "you can keep using your PC",
        };
        BuildNote.Text = $"Elapsed {Clock(elapsed.Elapsed)} · {hint}";
    }

    static string Clock(TimeSpan t) => $"{(int)t.TotalMinutes}:{t.Seconds:00}";

    static string Span(TimeSpan t) =>
        t.TotalHours >= 1 ? $"{(int)t.TotalHours} h {t.Minutes} min" : t.TotalMinutes >= 1 ? $"{(int)t.TotalMinutes} min" : $"{t.Seconds} s";

    void Ended(int code)
    {
        clock.Stop();
        elapsed.Stop();
        run = null;
        result = string.IsNullOrEmpty(result) ? code == 3 ? "cancelled" : code == 0 ? "ok" : "failed" : result;
        if (result == "failed" && error == null)
        {
            using var doc = JsonDocument.Parse($"{{\"code\": {code}}}");
            error = ("ui.process_died", doc.RootElement.Clone());
        }
        if (result != "ok" && current != null && rows.TryGetValue(current, out var row)) row.Stopped();
        if (closing)
        {
            Close();
            return;
        }
        switch (result)
        {
            case "ok":
                AfterSuccess();
                break;
            case "cancelled":
                AfterCancel();
                break;
            default:
                var (id, ps) = error ?? ("ui.process_died", (JsonElement?)null);
                var text = cat.Has(id) ? cat.Text(id, ps) : $"The installer stopped unexpectedly (exit code {code}).";
                if (!string.IsNullOrEmpty(logPath)) text += $"\n\nLog file: {logPath}";
                ShowProblem(kind switch { Kind.Update => "The update stopped", Kind.Verify => "The verification stopped", Kind.Rebuild => "The rebuild stopped", _ => "The install stopped" },
                            text);
                break;
        }
    }

    void AfterSuccess()
    {
        foreach (var id in order)
            if (rows[id] is { Finished: false } r) r.Done();
        SetProgress(1);
        WriteRebuildCommand(runDest);
        if (runPackage != null) SetupPackage.RemoveOlderThan(runPackage, AddLog);
        localPackage = SetupPackage.Newest();
        settings.Dest = runDest;
        settings.Save();
        launchFolder = runDest;
        if (kind == Kind.Rebuild)
        {
            if (StartGame(runDest)) Application.Current.Shutdown();
            return;
        }
        SetStep(2);
        ReadyTitle.Text = kind switch { Kind.Update => "Your game was updated", Kind.Verify => "The files were verified", _ => "Your game is ready" };
        var verb = kind switch { Kind.Update => "Updated", Kind.Verify => "Verified", _ => "Installed" };
        ReadyNote.Text = $"{verb} in {Span(elapsed.Elapsed)} · TotK Recomp {Versions.Short(runRelease ?? runPackage?.Version)} · {runDest}";
        Shortcuts.Make(runDest, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
        if (Shortcuts.Error != null) AddLog("shortcut not written: " + Shortcuts.Error);
        ShowPage(ReadyPage);
    }

    void AfterCancel()
    {
        if (kind == Kind.Rebuild)
        {
            ShowProblem("The rebuild was cancelled", "Code mods and ExeFS patches stay out of the game until the next rebuild. " +
                        "The game can start with the code it already has.", head: "Cancelled");
            return;
        }
        if (launchFolder.Length > 0 && kind is Kind.Update or Kind.Verify)
        {
            ShowLauncher(launchFolder);
            return;
        }
        ShowFiles();
    }

    /// build-cache\rebuild.txt points to this launcher, so totk.exe opens it (--rebuild) when a mod needs new code. The installer
    /// writes the command of its own tkinter window there; a run from the launcher replaces it.
    void WriteRebuildCommand(string dest)
    {
        try
        {
            var exe = Environment.ProcessPath;
            if (exe == null || !IOPath.GetFileName(exe).Equals("TotKLauncher.exe", StringComparison.OrdinalIgnoreCase)) return;
            var cache = IOPath.Combine(dest, "build-cache");
            Directory.CreateDirectory(cache);
            var tmp = IOPath.Combine(cache, "rebuild.txt.tmp");
            File.WriteAllText(tmp, string.Join("\n", exe, "--rebuild", dest) + "\n", new UTF8Encoding(false));
            File.Move(tmp, IOPath.Combine(cache, "rebuild.txt"), overwrite: true);
            AddLog("build-cache\\rebuild.txt: " + exe + " --rebuild");
        }
        catch (Exception x) { AddLog("build-cache\\rebuild.txt not written: " + x.Message); }
    }

    void LogToggled(object sender, RoutedEventArgs e)
    {
        var show = ShowLog.IsChecked == true;
        Log.Visibility = show ? Visibility.Visible : Visibility.Collapsed;
        StepCard.Visibility = show ? Visibility.Collapsed : Visibility.Visible;
        ShowLog.Content = show ? "Hide details" : "Show details";
        if (show)
        {
            Log.Text = log.ToString();
            Log.ScrollToEnd();
        }
    }

    void CancelRun(object sender, RoutedEventArgs e)
    {
        CancelButton.IsEnabled = false;
        CancelButton.Content = "Cancelling…";
        download?.Cancel();
        run?.Cancel();
    }

    // ================= 3: play =================

    void Play(object sender, RoutedEventArgs e)
    {
        if (StartGame(runDest)) Application.Current.Shutdown();
    }

    void OpenFolder(object sender, RoutedEventArgs e) => OpenExplorer(runDest);

    void ShortcutsChanged(object sender, RoutedEventArgs e)
    {
        SaveSettings();
        if (runDest.Length > 0) Shortcuts.Make(runDest, DesktopTick.IsChecked == true, StartTick.IsChecked == true);
    }

    // ================= when something goes wrong =================

    void ShowProblem(string title, string text, string? head = null)
    {
        lastFailure = text;
        ProblemTitle.Text = title;
        ProblemHead.Text = head ?? "What went wrong";
        ProblemText.Text = text;
        ProblemSteps.Children.Clear();
        ProblemSub.Text = kind == Kind.Rebuild
            ? "The game keeps the code it already had."
            : "Finished steps are kept: trying again continues where it stopped.";
        PlayAnyway.Visibility = kind is Kind.Rebuild or Kind.Update or Kind.Verify && File.Exists(Installed.GameExe(runDest))
            ? Visibility.Visible : Visibility.Collapsed;
        ProblemDetails.Content = "Show details";
        CopyReport.Content = "Copy for a bug report";
        if (kind is Kind.Install or Kind.Resume) SetStep(1);
        ShowPage(ProblemPage);
    }

    void ProblemShowDetails(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        ProblemSteps.Children.Clear();
        ProblemSteps.Children.Add(new TextBox
        {
            Text = string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 80))), IsReadOnly = true, FontFamily = (FontFamily)FindResource("Mono"), FontSize = 11,
            Foreground = (Brush)FindResource("TextDim"), Background = new SolidColorBrush(Color.FromArgb(0x40, 0, 0, 0)), BorderThickness = new Thickness(0),
            Padding = new Thickness(8), TextWrapping = TextWrapping.NoWrap, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
        });
    }

    void CopyForReport(object sender, RoutedEventArgs e)
    {
        var lines = log.ToString().Split('\n');
        var text = $"TotK Launcher {typeof(MainWindow).Assembly.GetName().Version} · package {runPackage?.Version ?? "?"} · " +
                   $"Windows {Environment.OSVersion.Version}\n{ProblemTitle.Text}: {lastFailure}\n\n" +
                   string.Join("\n", lines.Skip(Math.Max(0, lines.Length - 200)));
        try { Clipboard.SetText(text); CopyReport.Content = "Copied"; } catch (Exception) { }
    }

    void Back(object sender, RoutedEventArgs e)
    {
        if (kind is Kind.Install or Kind.Resume || launchFolder.Length == 0) ShowFiles();
        else ShowLauncher(launchFolder);
    }

    void Retry(object sender, RoutedEventArgs e) => _ = Begin(runDest, runArgs, kind);

    void PlayAnywayClick(object sender, RoutedEventArgs e)
    {
        if (StartGame(runDest)) Application.Current.Shutdown();
    }

    // ================= the window =================

    void Minimize(object sender, RoutedEventArgs e) => WindowState = WindowState.Minimized;

    void CloseWindow(object sender, RoutedEventArgs e) => Close();

    void OnClosing(object? sender, CancelEventArgs e)
    {
        if (closing || run == null && download == null) return;
        if (MessageBox.Show(this, "The install is still running. Cancel it and close?", "TotK Recomp", MessageBoxButton.YesNo) != MessageBoxResult.Yes)
        {
            e.Cancel = true;
            return;
        }
        closing = true;
        download?.Cancel();
        if (run != null)
        {
            // (the installer records what finished before it exits; Ended closes the window)
            e.Cancel = true;
            run.Cancel();
        }
    }
}
