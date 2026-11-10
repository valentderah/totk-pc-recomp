using System.Text.RegularExpressions;

namespace TotKLauncher;

/// Port versions: GitHub tags ("v0.3.14"), a setup package's VERSION ("0.3.14+ae30a61") and the "release" in an installed folder's
/// totk-install.json (the same text as VERSION). Only the X.Y.Z core is compared: the commit after "+" (or a "-dev") does not make
/// a version newer.
static class Versions
{
    static readonly Regex Core = new(@"(\d+)(?:\.(\d+))?(?:\.(\d+))?");

    public static (int, int, int)? Parse(string? v)
    {
        if (string.IsNullOrWhiteSpace(v)) return null;
        var m = Core.Match(v.Trim().TrimStart('v', 'V'));
        if (!m.Success) return null;
        static int N(Group g) => g.Success ? int.Parse(g.Value) : 0;
        return (N(m.Groups[1]), N(m.Groups[2]), N(m.Groups[3]));
    }

    /// &lt;0: a is older than b; 0: same version (or neither readable); &gt;0: a is newer. An unreadable version counts as the older.
    public static int Compare(string? a, string? b)
    {
        var pa = Parse(a);
        var pb = Parse(b);
        if (pa == null && pb == null) return 0;
        if (pa == null) return -1;
        if (pb == null) return 1;
        return pa.Value.CompareTo(pb.Value);
    }

    public static bool Newer(string? candidate, string? than) => Compare(candidate, than) > 0;

    /// "0.3.14" from "v0.3.14" or "0.3.14+ae30a61"; the text itself when it holds no version
    public static string Short(string? v)
    {
        var p = Parse(v);
        return p is { } x ? $"{x.Item1}.{x.Item2}.{x.Item3}" : v ?? "?";
    }
}
