using System.Diagnostics;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Media;
using System.Windows.Shapes;
using Path = System.Windows.Shapes.Path;

namespace TotKLauncher;

/// One installer step in the progress list: a status mark, the step name with a detail line under it, and the elapsed time.
/// The states follow the installer's step events (installer/events.py): pending, running, done, skipped, stopped.
sealed class StageRow
{
    public readonly Border Root;
    readonly Grid mark = new() { Width = 18, Height = 18, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(0, 1, 0, 0) };
    readonly TextBlock title = new() { FontWeight = FontWeights.Medium };
    readonly TextBlock detail = new() { FontSize = 12, Margin = new Thickness(0, 2, 0, 0), TextTrimming = TextTrimming.CharacterEllipsis, Visibility = Visibility.Collapsed };
    readonly TextBlock timer = new() { FontSize = 12, VerticalAlignment = VerticalAlignment.Top, Margin = new Thickness(12, 1, 0, 0) };
    readonly Stopwatch clock = new();
    bool active;

    public bool Started { get; private set; }
    public bool Finished { get; private set; }

    static Brush Res(string key) => (Brush)Application.Current.FindResource(key);

    public StageRow(string name, bool last)
    {
        title.Text = name;
        timer.FontFamily = (FontFamily)Application.Current.FindResource("Mono");
        var grid = new Grid();
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(30) });
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var texts = new StackPanel { Children = { title, detail } };
        Grid.SetColumn(texts, 1);
        Grid.SetColumn(timer, 2);
        grid.Children.Add(mark);
        grid.Children.Add(texts);
        grid.Children.Add(timer);
        Root = new Border { Child = grid, Padding = new Thickness(0, 11, 0, 11), BorderBrush = Res("Line") };
        SetLast(last);
        ShowPending();
    }

    /// The last row has no separator line under it
    public void SetLast(bool last) => Root.BorderThickness = new Thickness(0, 0, 0, last ? 0 : 1);

    public void Detail(string text)
    {
        detail.Text = text;
        detail.Visibility = text.Length > 0 ? Visibility.Visible : Visibility.Collapsed;
    }

    void Mark(UIElement glyph)
    {
        mark.Children.Clear();
        mark.Children.Add(glyph);
    }

    static Ellipse Circle(double size, Brush? fill, Brush? stroke = null) =>
        new() { Width = size, Height = size, Fill = fill, Stroke = stroke, StrokeThickness = 1.5 };

    static string Format(double seconds) => $"{(int)(seconds / 60)}:{(int)seconds % 60:00}";

    void ShowPending()
    {
        Mark(Circle(10, null, Res("LineStrong")));
        title.Foreground = Res("TextMuted");
        detail.Foreground = Res("TextMuted");
        timer.Foreground = Res("TextMuted");
    }

    public void Begin()
    {
        Started = active = true;
        Finished = false;
        clock.Restart();
        Mark(new Grid { Children = { Circle(18, Res("AccentSoft")), Circle(8, Res("Accent")) } });
        title.Foreground = Res("Text");
        title.FontWeight = FontWeights.SemiBold;
        detail.Foreground = Res("TextDim");
        timer.Foreground = Res("AccentLight");
        Tick();
    }

    public void Tick()
    {
        if (active) timer.Text = Format(clock.Elapsed.TotalSeconds);
    }

    /// seconds: the time the installer measured for the step, when its done event carries one
    public void Done(double? seconds = null)
    {
        if (active) Tick();
        if (seconds is { } s) timer.Text = Format(s);
        active = false;
        Started = Finished = true;
        clock.Stop();
        Mark(new Path
        {
            Data = Geometry.Parse("M1,9 L6,14 L16,3"), Stroke = Res("Ok"), StrokeThickness = 2.2,
            StrokeStartLineCap = PenLineCap.Round, StrokeEndLineCap = PenLineCap.Round, StrokeLineJoin = PenLineJoin.Round,
        });
        title.Foreground = Res("Text");
        title.FontWeight = FontWeights.Medium;
        detail.Foreground = Res("TextDim");
        timer.Foreground = Res("TextMuted");
    }

    /// reason "current": the step's output is already there and checks out; anything else: the step is not needed
    public void Skipped(string reason)
    {
        active = false;
        Started = Finished = true;
        clock.Stop();
        Mark(new Rectangle { Width = 10, Height = 2, Fill = Res("TextMuted") });
        title.Foreground = Res("TextDim");
        title.FontWeight = FontWeights.Medium;
        timer.Text = "";
        Detail(reason == "current" ? "Already done: nothing changed" : "Not needed");
    }

    public void Stopped()
    {
        active = false;
        clock.Stop();
        Mark(new Grid
        {
            Children =
            {
                Circle(18, Res("BadSoft")),
                new TextBlock { Text = "!", FontSize = 12, FontWeight = FontWeights.Bold, Foreground = Res("Bad"),
                                HorizontalAlignment = HorizontalAlignment.Center, VerticalAlignment = VerticalAlignment.Center },
            },
        });
        timer.Foreground = Res("Bad");
    }
}
