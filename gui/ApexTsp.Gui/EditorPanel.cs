using System.Globalization;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;

namespace ApexTsp.Gui;

// Instance being edited: points in TSPLIB coordinates (y up) + name + metric. Plain C#, tested.
public sealed class EditableInstance
{
    public string Name { get; set; } = "edited";
    public string Metric { get; set; } = "EUC_2D";   // EUC_2D | CEIL_2D (the engine's metrics)
    public List<(double X, double Y)> Points { get; } = [];

    public static readonly string[] Metrics = ["EUC_2D", "CEIL_2D"];
    public static readonly string[] Kinds = ["uniform", "clustered", "grid"];

    // Random instance in [0, size]^2, integer coordinates (deterministic for a seed).
    public void Generate(string kind, int n, int seed, double size = 1000)
    {
        Points.Clear();
        var rng = new Random(seed);
        switch (kind)
        {
            case "grid":
            {
                var side = Math.Max(1, (int)Math.Ceiling(Math.Sqrt(n)));
                var step = size / Math.Max(1, side - 1);
                for (var i = 0; i < n; ++i) Points.Add((Math.Round(i % side * step), Math.Round(i / side * step)));
                break;
            }
            case "clustered":
            {
                var k = Math.Max(1, (int)Math.Round(Math.Sqrt(n) / 2));
                var centers = Enumerable.Range(0, k).Select(_ => (rng.NextDouble() * size, rng.NextDouble() * size)).ToArray();
                var sigma = size / (6 * Math.Sqrt(k));
                for (var i = 0; i < n; ++i)
                {
                    var (cx, cy) = centers[rng.Next(k)];
                    double x, y;
                    do  // Gaussian around the centre (Box-Muller), redrawn when outside the square
                    {
                        double u = 1 - rng.NextDouble(), v = rng.NextDouble();
                        var r = Math.Sqrt(-2 * Math.Log(u)) * sigma;
                        x = cx + r * Math.Cos(2 * Math.PI * v);
                        y = cy + r * Math.Sin(2 * Math.PI * v);
                    } while (x < 0 || y < 0 || x > size || y > size);
                    Points.Add((Math.Round(x), Math.Round(y)));
                }
                break;
            }
            default:
                for (var i = 0; i < n; ++i) Points.Add((Math.Round(rng.NextDouble() * size), Math.Round(rng.NextDouble() * size)));
                break;
        }
        // duplicate points make zero-length edges; nudge them apart
        var seen = new HashSet<(double, double)>();
        for (var i = 0; i < Points.Count; ++i)
        {
            var p = Points[i];
            while (!seen.Add(p)) p = (p.X + 1, p.Y);
            Points[i] = p;
        }
    }

    public string ToTsplib()
    {
        var sb = new StringBuilder();
        var ci = CultureInfo.InvariantCulture;
        sb.Append("NAME : ").Append(Name).Append('\n');
        sb.Append("COMMENT : created in the APEX-TSP editor\n");
        sb.Append("TYPE : TSP\n");
        sb.Append(ci, $"DIMENSION : {Points.Count}\n");
        sb.Append("EDGE_WEIGHT_TYPE : ").Append(Metric).Append('\n');
        sb.Append("NODE_COORD_SECTION\n");
        for (var i = 0; i < Points.Count; ++i) sb.Append(ci, $"{i + 1} {Points[i].X:0.###} {Points[i].Y:0.###}\n");
        sb.Append("EOF\n");
        return sb.ToString();
    }

    public void Save(string path)
    {
        if (Points.Count < 3) throw new InvalidOperationException("an instance needs at least 3 points");
        File.WriteAllText(path, ToTsplib());
    }

    public static EditableInstance FromScene(ApexTsp.Render.Scene s, InstanceInfo? info)
    {
        var e = new EditableInstance { Name = (info?.Name ?? "edited") + "_edit", Metric = info?.EdgeWeightType == "CEIL_2D" ? "CEIL_2D" : "EUC_2D" };
        for (var i = 0; i < s.Count; ++i) e.Points.Add((s.XD?[i] ?? s.X[i], s.YD?[i] ?? s.Y[i]));
        return e;
    }
}

// Editing canvas: left click adds a point (or picks one to drag), right click deletes, hover shows
// the id and coordinates. View fits the points (or the default 0..1000 square).
public sealed class EditorCanvas : Control
{
    public EditableInstance Model { get; set; } = new();
    public Palette Pal { get; set; } = Palette.Light;
    public event Action? Changed;
    private int _drag = -1, _hover = -1;
    private double _x0, _y0, _scale = 1;

    public EditorCanvas()
    {
        ClipToBounds = true;
        Focusable = true;
    }

    private void Fit()
    {
        double minX = 0, minY = 0, maxX = 1000, maxY = 1000;
        if (Model.Points.Count >= 2)
        {
            minX = Model.Points.Min(p => p.X); maxX = Model.Points.Max(p => p.X);
            minY = Model.Points.Min(p => p.Y); maxY = Model.Points.Max(p => p.Y);
        }
        var w = Math.Max(1e-9, maxX - minX);
        var h = Math.Max(1e-9, maxY - minY);
        _scale = Math.Min((Bounds.Width - 40) / w, (Bounds.Height - 40) / h);
        _x0 = minX - (Bounds.Width / _scale - w) / 2;
        _y0 = minY - (Bounds.Height / _scale - h) / 2;
    }

    private Point ToScreen((double X, double Y) p) => new((p.X - _x0) * _scale, Bounds.Height - (p.Y - _y0) * _scale);
    private (double X, double Y) ToWorld(Point p) => (Math.Round(p.X / _scale + _x0, 3), Math.Round((Bounds.Height - p.Y) / _scale + _y0, 3));

    private int Hit(Point p)
    {
        var best = -1;
        var bd = 64.0;
        for (var i = 0; i < Model.Points.Count; ++i)
        {
            var s = ToScreen(Model.Points[i]);
            var d = (s.X - p.X) * (s.X - p.X) + (s.Y - p.Y) * (s.Y - p.Y);
            if (d < bd) { bd = d; best = i; }
        }
        return best;
    }

    public override void Render(DrawingContext ctx)
    {
        if (_drag < 0) Fit();
        ctx.FillRectangle(new SolidColorBrush(Pal.Surface), new Rect(Bounds.Size));
        var node = new SolidColorBrush(Pal.MapNode);
        var accent = new SolidColorBrush(Pal.Accent);
        var muted = new SolidColorBrush(Pal.Muted);
        var labels = Model.Points.Count <= 300;
        for (var i = 0; i < Model.Points.Count; ++i)
        {
            var s = ToScreen(Model.Points[i]);
            ctx.DrawEllipse(i == _hover || i == _drag ? accent : node, null, s, 3.5, 3.5);
            if (labels) ctx.DrawText(Draw.Text((i + 1).ToString(CultureInfo.InvariantCulture), 9, muted), new Point(s.X + 4, s.Y - 12));
        }
        var info = _hover >= 0 && _hover < Model.Points.Count
            ? string.Create(CultureInfo.InvariantCulture, $"node {_hover + 1}: ({Model.Points[_hover].X}, {Model.Points[_hover].Y})")
            : "left click: add / drag a point · right click: delete";
        ctx.DrawText(Draw.Text($"{Model.Points.Count} points · {Model.Metric} · {info}", 11, muted), new Point(8, 6));
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        var p = e.GetPosition(this);
        var hit = Hit(p);
        if (e.GetCurrentPoint(this).Properties.IsRightButtonPressed)
        {
            if (hit >= 0) { Model.Points.RemoveAt(hit); _hover = -1; Changed?.Invoke(); }
        }
        else if (hit >= 0) _drag = hit;
        else
        {
            Model.Points.Add(ToWorld(p));
            Changed?.Invoke();
        }
        InvalidateVisual();
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        var p = e.GetPosition(this);
        if (_drag >= 0) { Model.Points[_drag] = ToWorld(p); Changed?.Invoke(); }
        else _hover = Hit(p);
        InvalidateVisual();
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e) => _drag = -1;
}

// Editor tab (M6.4).
public sealed partial class MainWindow
{
    private readonly EditorCanvas _editor = new();
    private readonly TextBox _edName = new() { Text = "edited", MinWidth = 160 };
    private readonly ComboBox _edMetric = new() { ItemsSource = EditableInstance.Metrics, SelectedIndex = 0, MinWidth = 120 };
    private readonly ComboBox _edKind = new() { ItemsSource = EditableInstance.Kinds, SelectedIndex = 0, MinWidth = 120 };
    private readonly NumericUpDown _edN = new() { Minimum = 3, Maximum = 200000, Value = 100, Increment = 50, FormatString = "0", Width = 130 };
    private readonly NumericUpDown _edSeed = new() { Minimum = 0, Maximum = int.MaxValue, Value = 1, Increment = 1, FormatString = "0", Width = 130 };

    private Control BuildEditorTab()
    {
        _editor.Changed += () => _editor.InvalidateVisual();
        _edMetric.SelectionChanged += (_, _) => { _editor.Model.Metric = _edMetric.SelectedItem as string ?? "EUC_2D"; _editor.InvalidateVisual(); };
        _edName.TextChanged += (_, _) => _editor.Model.Name = string.IsNullOrWhiteSpace(_edName.Text) ? "edited" : _edName.Text.Trim().Replace(' ', '_');
        Button B(string text, Action a)
        {
            var b = new Button { Content = text, Margin = new Thickness(0, 0, 6, 6) };
            b.Click += (_, _) => a();
            return b;
        }
        var form = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), RowSpacing = 6, ColumnSpacing = 10 };
        void Row(int r, string label, Control c)
        {
            form.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var t = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetRow(t, r);
            Grid.SetRow(c, r);
            Grid.SetColumn(c, 1);
            form.Children.AddRange([t, c]);
        }
        Row(0, "name", _edName);
        Row(1, "metric", _edMetric);
        var gen = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), RowSpacing = 6, ColumnSpacing = 10 };
        void GRow(int r, string label, Control c)
        {
            gen.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var t = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetRow(t, r);
            Grid.SetRow(c, r);
            Grid.SetColumn(c, 1);
            gen.Children.AddRange([t, c]);
        }
        GRow(0, "points", _edN);
        GRow(1, "layout", _edKind);
        GRow(2, "seed", _edSeed);
        var side = new StackPanel { Spacing = 10, Margin = new Thickness(12, 0, 12, 12) };
        side.Children.AddRange([
            Card("Instance", form),
            Card("Generate", new StackPanel { Spacing = 6, Children = { gen, new WrapPanel { Children = { B("Generate", Generate), B("Clear", () => { _editor.Model.Points.Clear(); _editor.InvalidateVisual(); }) } } } }),
            Card("Use", new WrapPanel
            {
                Children =
                {
                    B("Copy current instance", CopyCurrentInstance),
                    B("Save .tsp…", () => _ = SaveEdited()),
                    B("Open in Solve", () => OpenEdited(false)),
                    B("Solve exact now", () => OpenEdited(true)),
                },
            }),
        ]);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,400") };
        grid.Children.Add(new Border { Child = _editor, ClipToBounds = true, CornerRadius = new CornerRadius(8), Margin = new Thickness(12, 0, 0, 12) });
        var scroll = new ScrollViewer { Content = side };
        Grid.SetColumn(scroll, 1);
        grid.Children.Add(scroll);
        return grid;
    }

    private void Generate()
    {
        _editor.Model.Generate(_edKind.SelectedItem as string ?? "uniform", (int)(_edN.Value ?? 100), (int)(_edSeed.Value ?? 1));
        _editor.InvalidateVisual();
    }

    private void CopyCurrentInstance()
    {
        if (_map.Scene is null) { Toast("no instance open"); return; }
        _editor.Model = EditableInstance.FromScene(_map.Scene, _inst);
        _edName.Text = _editor.Model.Name;
        _edMetric.SelectedItem = _editor.Model.Metric;
        _editor.InvalidateVisual();
    }

    private async Task SaveEdited()
    {
        var f = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Save instance", SuggestedFileName = _editor.Model.Name + ".tsp", DefaultExtension = "tsp",
            FileTypeChoices = [new FilePickerFileType("TSPLIB") { Patterns = ["*.tsp"] }],
        });
        if (f?.TryGetLocalPath() is not { } p) return;
        try { _editor.Model.Save(p); Toast("saved " + p); }
        catch (Exception e) when (e is IOException or InvalidOperationException or UnauthorizedAccessException) { Toast(e.Message); }
    }

    // Saves under <home>/instances/ and opens it in the Solve tab (optionally starting exact).
    public string? OpenEdited(bool solve)
    {
        try
        {
            var dir = Path.Combine(_store.Home, "instances");
            Directory.CreateDirectory(dir);
            var path = Path.Combine(dir, _editor.Model.Name + ".tsp");
            _editor.Model.Save(path);
            if (!OpenInstance(path)) return null;
            _tabs.SelectedIndex = 0;
            if (solve)
            {
                var s = ReadForm();
                s.Mode = "exact";
                LoadSettingsIntoForm(s);
                StartRun();
            }
            return path;
        }
        catch (Exception e) when (e is IOException or InvalidOperationException or UnauthorizedAccessException)
        {
            Toast(e.Message);
            return null;
        }
    }
}
