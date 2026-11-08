using System.Globalization;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;

namespace ApexTsp.Gui;

// One tour the solver produced (each distinct tour read from the shared mapping).
public sealed class TourSnapshot
{
    public int Index;
    public double Seconds;
    public double Ub;
    public string Source = "";
    public bool Heuristic = true;
    public uint[] Tour = [];
    public int Count;
    public Color Color;
    public bool Visible;
    public int Removed, Added;   // edges cut out of / brought into the previous tour
    public bool Pending;         // source not confirmed by a UB event yet
}

// Tour history (M6.3b, Concorde-style): every tour found gets its own color and can be overlaid;
// the edges the latest improvement cut out of the tour are drawn red (dashed), the new ones green.
public sealed partial class MainWindow
{
    public static readonly Color[] TourColors =
        [.. new[] { "#7F3C8D", "#11A579", "#3969AC", "#F2B701", "#E73F74", "#80BA5A", "#008695", "#CF1C90", "#4B4B8F", "#A5AA99" }.Select(Color.Parse)];

    public const int MaxTours = 40;
    private readonly List<TourSnapshot> _tours = [];
    private readonly StackPanel _tourRows = new() { Spacing = 2 };
    private readonly CheckBox _showDiff = new() { Content = "cut / added edges (last improvement)", IsChecked = true };
    private readonly TextBlock _tourSummary = new() { FontSize = 11, Opacity = 0.75, TextWrapping = TextWrapping.Wrap };

    public IReadOnlyList<TourSnapshot> Tours => _tours;

    private Control ToursCard()
    {
        _showDiff.IsCheckedChanged += (_, _) => { _map.Renderer.ShowDiff = _showDiff.IsChecked == true; InvalidateMaps(); };
        var all = new Button { Content = "show all", Padding = new Thickness(8, 2), Margin = new Thickness(0, 0, 6, 0) };
        all.Click += (_, _) => SetAllTours(true);
        var none = new Button { Content = "hide all", Padding = new Thickness(8, 2) };
        none.Click += (_, _) => SetAllTours(false);
        return Card("Tours found", new StackPanel
        {
            Spacing = 6,
            Children =
            {
                _showDiff,
                new StackPanel { Orientation = Orientation.Horizontal, Children = { all, none } },
                _tourSummary,
                new ScrollViewer { Content = _tourRows, MaxHeight = 220 },
            },
        });
    }

    // Called on the UI thread with each tour read from the shared mapping.
    private void OnTourRead(uint[] tour, int count)
    {
        if (count < 3) return;
        var last = _tours.Count > 0 ? _tours[^1] : null;
        if (last is not null && last.Count == count && last.Tour.AsSpan(0, count).SequenceEqual(tour.AsSpan(0, count))) return;
        var snap = new TourSnapshot
        {
            Index = (last?.Index ?? 0) + 1,
            Seconds = _vm.History.Count > 0 ? _vm.History[^1].Seconds : 0,
            Ub = _map.Scene?.TourLength(tour.AsSpan(0, count)) ?? _vm.UpperBound,
            Source = _vm.Phase,  // until the UB event with this length names the heuristic
            Pending = true,
            Tour = tour.AsSpan(0, count).ToArray(),
            Count = count,
            Color = TourColors[_tours.Count % TourColors.Length],
        };
        if (last is not null && _map.Scene is { } sc)
        {
            sc.SetDiff(last.Tour, last.Count, snap.Tour, snap.Count);
            snap.Removed = sc.Removed.Length;
            snap.Added = sc.Added.Length;
        }
        Attribute(snap);
        _tours.Add(snap);
        if (_tours.Count > MaxTours) _tours.RemoveAt(1);  // keep the first (construction) tour
        RebuildTourRows();
        ApplyTourLayers();
    }

    // Source of a tour = the UB event with its length (the event may arrive before or after the tour).
    private void Attribute(TourSnapshot t)
    {
        var e = _vm.Events.LastOrDefault(e => e.Kind == ApexTsp.Ipc.EventKind.UbImproved && e.Value == t.Ub);
        if (e is null) return;
        t.Pending = false;
        t.Source = e.Detail;
        t.Heuristic = e.Heuristic;
    }

    private void OnUbEvent(ApexTsp.Ipc.SolverEvent e)
    {
        var changed = false;
        foreach (var t in _tours.Where(t => t.Pending && t.Ub == e.Value))
        {
            t.Pending = false;
            t.Source = e.Detail;
            t.Heuristic = e.Heuristic;
            changed = true;
        }
        if (changed) RebuildTourRows();
    }

    private void ResetTours()
    {
        _tours.Clear();
        _map.Scene?.Layers.Clear();
        _map.Scene?.ClearDiff();
        RebuildTourRows();
    }

    private void SetAllTours(bool visible)
    {
        foreach (var t in _tours.Take(_tours.Count - 1)) t.Visible = visible;  // the current tour is always drawn
        RebuildTourRows();
        ApplyTourLayers();
    }

    private void ApplyTourLayers()
    {
        if (_map.Scene is not { } sc) return;
        sc.Layers.Clear();
        foreach (var t in _tours.Take(_tours.Count - 1).Where(t => t.Visible))
            sc.Layers.Add(new TourLayer(t.Tour, t.Count, Palette.Sk(t.Color).WithAlpha(210)));
        InvalidateMaps();
    }

    private void RebuildTourRows()
    {
        _tourRows.Children.Clear();
        for (var i = _tours.Count - 1; i >= 0; --i)
        {
            var t = _tours[i];
            var current = i == _tours.Count - 1;
            var swatch = new Border
            {
                Width = 14, Height = 14, CornerRadius = new CornerRadius(3), VerticalAlignment = VerticalAlignment.Center,
                Background = _pal.Brush(current ? (t.Heuristic ? _pal.Heuristic : _pal.Lp) : t.Color),
            };
            var label = string.Create(CultureInfo.InvariantCulture,
                $"#{t.Index} {t.Source,-11} {MainViewModel.Fmt(t.Ub),-11} {t.Seconds,6:0.0} s{(t.Index > 1 ? $"  −{t.Removed}/+{t.Added}" : "")}{(current ? "  (current)" : "")}");
            var box = new CheckBox { IsChecked = current || t.Visible, IsEnabled = !current, Content = new TextBlock { Text = label, FontFamily = new FontFamily("monospace"), FontSize = 11 } };
            var snap = t;
            box.IsCheckedChanged += (_, _) => { snap.Visible = box.IsChecked == true; ApplyTourLayers(); };
            _tourRows.Children.Add(new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { swatch, box } });
        }
        var cur = _tours.Count > 0 ? _tours[^1] : null;
        _tourSummary.Text = cur is null ? "tours appear as the solver finds them"
            : cur.Index == 1 ? $"{_tours.Count} tour"
            : $"{_tours.Count} tours · last improvement cut {cur.Removed} edges (red, dashed) and added {cur.Added} (green)";
    }
}
