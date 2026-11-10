using System.IO;
using System.IO.Compression;
using System.Net;
using System.Net.Http;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace TotKLauncher;

/// A GitHub release and its Windows setup package (TotK-Setup-&lt;version&gt;-windows.zip, written by tools/make_release.py)
sealed record Release(string Tag, string AssetName, string Url, long Size)
{
    public string SizeText => Size > 0 ? $"{Size / 1048576.0:0} MB" : "unknown size";
}

/// Finds the latest release of the repository and turns its setup package into a folder under SetupPackage.Store
static class Releases
{
    static readonly Regex PackageName = new(@"^TotK-Setup-.+-windows\.zip$", RegexOptions.IgnoreCase);
    static readonly HttpClient Http = NewClient(followRedirects: true);
    static readonly HttpClient NoRedirect = NewClient(followRedirects: false);

    static HttpClient NewClient(bool followRedirects)
    {
        var handler = new SocketsHttpHandler { AllowAutoRedirect = followRedirects, ConnectTimeout = TimeSpan.FromSeconds(15) };
        var client = new HttpClient(handler) { Timeout = Timeout.InfiniteTimeSpan };
        client.DefaultRequestHeaders.UserAgent.ParseAdd("TotKLauncher");
        return client;
    }

    static CancellationTokenSource Deadline(CancellationToken token, int seconds)
    {
        var cts = CancellationTokenSource.CreateLinkedTokenSource(token);
        cts.CancelAfter(TimeSpan.FromSeconds(seconds));
        return cts;
    }

    /// The latest release, or null when GitHub cannot be reached or the repository has none. The REST API gives the asset list and
    /// sizes but allows 60 requests an hour per address; when it refuses, the github.com redirect of /releases/latest still gives
    /// the tag, and the asset name follows the release naming.
    public static async Task<Release?> Latest(string repo, CancellationToken token = default)
    {
        for (var attempt = 1; attempt <= 2; attempt++)
        {
            try
            {
                using var deadline = Deadline(token, 15);
                using var request = new HttpRequestMessage(HttpMethod.Get, $"https://api.github.com/repos/{repo}/releases/latest");
                request.Headers.Accept.ParseAdd("application/vnd.github+json");
                using var response = await Http.SendAsync(request, deadline.Token);
                if (response.IsSuccessStatusCode)
                {
                    using var json = JsonDocument.Parse(await response.Content.ReadAsStringAsync(deadline.Token));
                    var tag = json.RootElement.GetProperty("tag_name").GetString();
                    return string.IsNullOrEmpty(tag) ? null : FromAssets(repo, tag, json.RootElement);
                }
                if ((int)response.StatusCode < 500) break;   // (rate limit or no release: the redirect below)
            }
            catch (OperationCanceledException) when (token.IsCancellationRequested) { throw; }
            catch (Exception) { }
            await Task.Delay(TimeSpan.FromSeconds(2 * attempt), token);
        }
        try
        {
            using var deadline = Deadline(token, 15);
            using var response = await NoRedirect.GetAsync($"https://github.com/{repo}/releases/latest", deadline.Token);
            var target = response.Headers.Location?.OriginalString ?? "";
            var at = target.LastIndexOf("/tag/", StringComparison.Ordinal);
            if (at >= 0 && target.Length > at + 5) return Conventional(repo, Uri.UnescapeDataString(target[(at + 5)..]));
        }
        catch (OperationCanceledException) when (token.IsCancellationRequested) { throw; }
        catch (Exception) { }
        return null;
    }

    static Release FromAssets(string repo, string tag, JsonElement release)
    {
        if (release.TryGetProperty("assets", out var assets) && assets.ValueKind == JsonValueKind.Array)
            foreach (var asset in assets.EnumerateArray())
            {
                var name = asset.TryGetProperty("name", out var n) ? n.GetString() ?? "" : "";
                if (!PackageName.IsMatch(name) || !asset.TryGetProperty("browser_download_url", out var url) || url.GetString() is not { } link) continue;
                return new Release(tag, name, link, asset.TryGetProperty("size", out var size) && size.TryGetInt64(out var bytes) ? bytes : 0);
            }
        return Conventional(repo, tag);
    }

    /// The package name tools/make_release.py gives a release: TotK-Setup-&lt;tag without the v&gt;-windows.zip
    static Release Conventional(string repo, string tag)
    {
        var name = $"TotK-Setup-{tag.TrimStart('v', 'V')}-windows.zip";
        return new Release(tag, name, $"https://github.com/{repo}/releases/download/{Uri.EscapeDataString(tag)}/{name}", 0);
    }

    /// Downloads and unpacks the release's package into SetupPackage.Store\&lt;tag&gt;. progress goes from 0 to 1 (the download is the
    /// first 85%); note gets one line per stage and per retry.
    public static async Task<SetupPackage> Fetch(Release release, Action<double> progress, Action<string> note, CancellationToken token)
    {
        var folder = Path.Combine(SetupPackage.Store, string.Concat(release.Tag.Where(c => char.IsLetterOrDigit(c) || c is '.' or '-' or '_')));
        if (SetupPackage.At(folder) is { } ready) return ready;

        var downloads = Path.Combine(Settings.Folder, "downloads");
        Directory.CreateDirectory(downloads);
        var zip = Path.Combine(downloads, release.AssetName);
        note($"Downloading {release.AssetName} ({release.SizeText})");
        await Download(release, zip, f => progress(0.85 * f), note, token);

        note("Unpacking the setup package");
        var staging = folder + ".part";
        await Task.Run(() => Unpack(zip, staging, f => progress(0.85 + 0.15 * f), token), token);
        var root = PackageRoot(staging) ?? throw new InvalidDataException(
            $"{release.AssetName} has no setup package in it (python\\python.exe and installer\\). An antivirus may have removed files from it.");
        if (Directory.Exists(folder)) Directory.Delete(folder, true);
        Directory.Move(root, folder);
        if (Directory.Exists(staging)) Directory.Delete(staging, true);
        try { File.Delete(zip); } catch (Exception) { }
        progress(1);
        return SetupPackage.At(folder) ?? throw new InvalidDataException("The unpacked setup package could not be read.");
    }

    /// The zip holds everything under one top folder (tools/make_release.py); a flat zip works too
    static string? PackageRoot(string dir) =>
        SetupPackage.IsPackage(dir) ? dir : Directory.GetDirectories(dir).FirstOrDefault(SetupPackage.IsPackage);

    static void Unpack(string zip, string into, Action<double> progress, CancellationToken token)
    {
        if (Directory.Exists(into)) Directory.Delete(into, true);
        var root = Directory.CreateDirectory(into).FullName + Path.DirectorySeparatorChar;
        using var archive = ZipFile.OpenRead(zip);
        for (var i = 0; i < archive.Entries.Count; i++)
        {
            token.ThrowIfCancellationRequested();
            var entry = archive.Entries[i];
            var path = Path.GetFullPath(Path.Combine(root, entry.FullName));
            if (!path.StartsWith(root, StringComparison.OrdinalIgnoreCase)) throw new InvalidDataException($"Unsafe path in the zip: {entry.FullName}");
            if (entry.Name.Length == 0)
                Directory.CreateDirectory(path);
            else
            {
                Directory.CreateDirectory(Path.GetDirectoryName(path)!);
                entry.ExtractToFile(path, overwrite: true);
            }
            if (i % 64 == 0) progress((double)i / archive.Entries.Count);
        }
        progress(1);
    }

    /// Up to five attempts; a server error, rate limit or dropped connection waits and tries again, anything else fails at once
    static async Task Download(Release release, string path, Action<double> progress, Action<string> note, CancellationToken token)
    {
        const int attempts = 5;
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                await DownloadOnce(release, path, progress, token);
                return;
            }
            catch (HttpRequestException e) when (attempt < attempts && Retryable(e))
            {
                var wait = TimeSpan.FromSeconds(3 * attempt);
                note($"GitHub did not answer ({e.Message}). Trying again in {wait.TotalSeconds:0} s ({attempt + 1} of {attempts})...");
                await Task.Delay(wait, token);
            }
        }
    }

    static bool Retryable(HttpRequestException e) =>
        e.StatusCode is null || (int)e.StatusCode >= 500 || e.StatusCode == HttpStatusCode.TooManyRequests;

    static async Task DownloadOnce(Release release, string path, Action<double> progress, CancellationToken token)
    {
        using var response = await Http.GetAsync(release.Url, HttpCompletionOption.ResponseHeadersRead, token);
        if (!response.IsSuccessStatusCode)
            throw new HttpRequestException($"HTTP {(int)response.StatusCode} {response.ReasonPhrase}", null, response.StatusCode);
        var expected = response.Content.Headers.ContentLength ?? release.Size;
        long received = 0;
        var lastReport = Environment.TickCount64;
        await using (var source = await response.Content.ReadAsStreamAsync(token))
        await using (var file = new FileStream(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16, useAsync: true))
        {
            var buffer = new byte[1 << 18];
            while (true)
            {
                int read;
                try { read = await source.ReadAsync(buffer, token); }
                catch (IOException e) { throw new HttpRequestException("the connection dropped", e); }
                if (read == 0) break;
                await file.WriteAsync(buffer.AsMemory(0, read), token);
                received += read;
                if (expected > 0 && Environment.TickCount64 - lastReport >= 250)
                {
                    lastReport = Environment.TickCount64;
                    progress(Math.Min(1, (double)received / expected));
                }
            }
        }
        if (expected > 0 && received != expected)
            throw new HttpRequestException($"the download ended early ({received >> 20} of {expected >> 20} MB)");
        progress(1);
    }
}
