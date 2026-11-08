using System.Globalization;
using System.Text;
using ApexTsp.Ipc;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Primitives;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform.Storage;
using Avalonia.Styling;
using Avalonia.Threading;

namespace ApexTsp.Gui;

// App shell (Sprint 6 M6.2): toolbar, tabs Solve | Runs | Log, instance and run settings in the
// window, run history. Built in code (no XAML), so NativeAOT needs no reflection.
public sealed partial class MainWindow : Window
{
    private readonly GuiOptions _opt;
    private readonly AppStore _store;
    private readonly AppSettings _settings;
    private readonly MainViewModel _vm = new();
    private readonly MapView _map = new();
    private readonly BoundsChart _chart = new();
    private readonly CutsChart _cuts = new();
    private readonly TreePanel _tree = new();
    private readonly TextBlock _cutText = new() { TextWrapping = TextWrapping.Wrap, FontSize = 11 };
    private readonly TextBlock _treeText = new() { TextWrapping = TextWrapping.Wrap, FontSize = 11 };
    private readonly Border _cutBadge = new() { CornerRadius = new CornerRadius(10), Padding = new Thickness(8, 2) };
    private readonly TextBlock _cutBadgeText = new() { Foreground = Brushes.White, FontWeight = FontWeight.SemiBold, FontSize = 11 };
    private readonly Border _ubBadge = new() { CornerRadius = new CornerRadius(10), Padding = new Thickness(8, 2) };
    private readonly TextBlock _ubBadgeText = new() { Foreground = Brushes.White, FontWeight = FontWeight.SemiBold, FontSize = 11 };
    private readonly CheckBox _showTour = new() { Content = "tour", IsChecked = true }, _showNodes = new() { Content = "nodes", IsChecked = true },
        _showLp = new() { Content = "LP support", IsChecked = true }, _flashLp = new() { Content = "flash new LP edges", IsChecked = true },
        _logGap = new() { Content = "log gap", IsChecked = true };
    private readonly ComboBox _logCat = new() { ItemsSource = new[] { "all", "heuristic", "lp", "cuts", "tree", "system" }, SelectedIndex = 0, MinWidth = 110 };
    private bool _solverDirty;
    private IDisposable? _flashOff;
    private Palette _pal = Palette.Light;

    // Solve tab: instance card
    private readonly TextBlock _instTitle = new() { FontSize = 18, FontWeight = FontWeight.SemiBold, TextTrimming = TextTrimming.CharacterEllipsis };
    private readonly TextBlock _instDetails = new() { TextWrapping = TextWrapping.Wrap };
    // settings card
    private readonly ComboBox _mode = new() { ItemsSource = RunSettings.Modes, MinWidth = 140 };
    private readonly NumericUpDown _time = new() { Minimum = 1, Maximum = 86400 * 7, Increment = 60, FormatString = "0", MinWidth = 140 };
    private readonly NumericUpDown _threads = new() { Minimum = 1, Maximum = 256, Increment = 1, FormatString = "0", MinWidth = 140 };
    private readonly NumericUpDown _seed = new() { Minimum = 0, Maximum = uint.MaxValue, Increment = 1, FormatString = "0", MinWidth = 140 };
    private readonly NumericUpDown _mem = new() { Minimum = 64, Maximum = 1 << 20, Increment = 512, FormatString = "0", MinWidth = 140 };
    private readonly TextBox _ckDir = new() { Watermark = "none", MinWidth = 140 };
    private readonly CheckBox _resume = new() { Content = "resume from checkpoint" };
    private readonly TextBlock _settingsError = new() { TextWrapping = TextWrapping.Wrap };
    // bounds card
    private readonly TextBlock _bounds = new() { FontSize = 15, FontWeight = FontWeight.SemiBold }, _phase = new(), _stats = new(), _status = new(), _ramText = new();
    private readonly Border _badge = new() { CornerRadius = new CornerRadius(10), Padding = new Thickness(8, 2) };
    private readonly TextBlock _badgeText = new() { Foreground = Brushes.White, FontWeight = FontWeight.SemiBold, FontSize = 11 };
    private readonly ProgressBar _ram = new() { Minimum = 0, Maximum = 1, Height = 6 };
    private readonly TextBox _report = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, Height = 180, FontFamily = new FontFamily("monospace"), FontSize = 11 };
    private readonly CheckBox _overlay = new() { Content = "clusters", IsChecked = true };
    // toolbar
    private readonly Button _startBtn = new(), _pauseBtn = new(), _resumeBtn = new(), _cancelBtn = new(), _ckBtn = new();
    private readonly Button _recentBtn = new() { Content = "Recent ▾" };
    private readonly ComboBox _theme = new() { ItemsSource = ThemeChoice.Names, MinWidth = 100 };
    private readonly TextBlock _statusBar = new() { VerticalAlignment = VerticalAlignment.Center };
    // runs / log tabs
    private readonly ListBox _runs = new() { FontFamily = new FontFamily("monospace"), FontSize = 12 };
    private readonly TextBlock _runDetail = new() { TextWrapping = TextWrapping.Wrap };
    private readonly CheckBox _allRuns = new() { Content = "all instances" };
    private readonly ListBox _log = new() { FontFamily = new FontFamily("monospace"), FontSize = 11 };
    private readonly TextBox _logFilter = new() { Watermark = "filter", MinWidth = 220 };
    private readonly System.Collections.ObjectModel.ObservableCollection<LogLine> _logItems = [];
    private readonly TabControl _tabs = new();

    private InstanceInfo? _inst;
    private RunSession? _run;
    private string _runId = "";
    private RunSettings _runSettings = new();
    private readonly List<(string Dir, RunRecord Run)> _runList = [];

    public MainWindow(GuiOptions opt)
    {
        _opt = opt;
        _store = new AppStore(opt.Home);
        _settings = _store.LoadSettings();
        if (opt.Engine is not null) _settings.EnginePath = opt.Engine;
        Title = "APEX-TSP";
        Width = 1480;
        Height = 940;
        MinWidth = 900;
        MinHeight = 600;
        _badge.Child = _badgeText;
        _ubBadge.Child = _ubBadgeText;
        _cutBadge.Child = _cutBadgeText;
        _log.ItemsSource = _logItems;
        _log.ItemTemplate = new Avalonia.Controls.Templates.FuncDataTemplate<LogLine>((l, _) =>
            new TextBlock { Text = l.Text, Foreground = _pal.Brush(CategoryColor(l.Category)), FontFamily = new FontFamily("monospace"), FontSize = 11 });
        _tree.NodeClicked += n => _treeText.Text = TreePanel.Describe(n);

        Content = BuildLayout();
        LoadSettingsIntoForm(_settings.Run);
        _theme.SelectedItem = ThemeChoice.Names.Contains(_settings.Theme) ? _settings.Theme : "system";
        _theme.SelectionChanged += (_, _) => SetTheme(_theme.SelectedItem as string ?? "system");
        ActualThemeVariantChanged += (_, _) => ApplyPalette();
        ApplyPalette();

        _vm.PropertyChanged += (_, _) => UpdatePanels();
        _vm.HistoryChanged += () => _solverDirty = true;
        _vm.SolverChanged += () => _solverDirty = true;
        // Charts redraw at most 10 times per second however fast events arrive.
        new DispatcherTimer(TimeSpan.FromMilliseconds(100), DispatcherPriority.Background, (_, _) => RefreshSolverViews()).Start();
        void Layer(CheckBox c, Action<bool> set) => c.IsCheckedChanged += (_, _) => { set(c.IsChecked == true); _map.Invalidate(); };
        Layer(_showTour, v => _map.Renderer.ShowTour = v);
        Layer(_showNodes, v => _map.Renderer.ShowNodes = v);
        Layer(_showLp, v => _map.Renderer.ShowLp = v);
        _logGap.IsCheckedChanged += (_, _) => { _chart.LogGap = _logGap.IsChecked == true; _chart.InvalidateVisual(); };
        _logCat.SelectionChanged += (_, _) => RefilterLog();
        _vm.ClusterBox += (id, x0, y0, x1, y1) => _map.Scene?.SetCluster(id, x0, y0, x1, y1);
        _vm.LogAdded += AddLogLine;
        _overlay.IsCheckedChanged += (_, _) => { _map.Renderer.ClusterOverlay = _overlay.IsChecked == true; _map.Invalidate(); };
        _allRuns.IsCheckedChanged += (_, _) => RefreshRuns();
        _runs.SelectionChanged += (_, _) => ShowRunDetail();
        _runs.DoubleTapped += (_, _) => LoadSelectedRun();
        _logFilter.TextChanged += (_, _) => RefilterLog();

        KeyBindings.Add(Bind(Key.O, KeyModifiers.Control, () => _ = OpenDialog()));
        KeyBindings.Add(Bind(Key.F5, KeyModifiers.None, StartRun));
        KeyBindings.Add(Bind(Key.F6, KeyModifiers.None, () => Send(MsgType.Pause)));
        KeyBindings.Add(Bind(Key.F7, KeyModifiers.None, () => Send(MsgType.Resume)));
        KeyBindings.Add(Bind(Key.F5, KeyModifiers.Shift, () => Send(MsgType.Cancel)));
        KeyBindings.Add(Bind(Key.S, KeyModifiers.Control, () => _ = SaveTourDialog()));
        DragDrop.SetAllowDrop(this, true);
        AddHandler(DragDrop.DropEvent, OnDrop);

        UpdatePanels();
        Opened += (_, _) => OnOpened();
        Closing += (_, _) => { SaveSettings(); _demo?.Stop(); _run?.Dispose(); };
    }

    private static KeyBinding Bind(Key k, KeyModifiers m, Action a) =>
        new() { Gesture = new KeyGesture(k, m), Command = new RelayCommand(a) };

    // ---- layout ---------------------------------------------------------------------------

    private Control BuildLayout()
    {
        Button Tool(string text, string tip, Action a, Button? b = null)
        {
            b ??= new Button();
            b.Content = text;
            b.Margin = new Thickness(0, 0, 4, 0);
            b.Padding = new Thickness(10, 5);
            ToolTip.SetTip(b, tip);
            b.Click += (_, _) => a();
            return b;
        }
        Control Sep() => new Border { Width = 1, Margin = new Thickness(6, 4), Background = Brushes.Gray, Opacity = 0.3 };
        var bar = new StackPanel { Orientation = Orientation.Horizontal, Margin = new Thickness(10, 8), Spacing = 0 };
        bar.Children.AddRange([
            Tool("Open…", "Open a TSPLIB instance (Ctrl+O, or drop a .tsp on the window)", () => _ = OpenDialog()),
            Tool("Recent ▾", "Recently opened instances", ShowRecent, _recentBtn), Sep(),
            Tool("▶ Start", "Start a run with the settings on the right (F5)", StartRun, _startBtn),
            Tool("Pause", "Pause the solver (F6)", () => Send(MsgType.Pause), _pauseBtn),
            Tool("Resume", "Resume (F7)", () => Send(MsgType.Resume), _resumeBtn),
            Tool("Cancel", "Stop the run; the best tour so far is kept (Shift+F5)", () => Send(MsgType.Cancel), _cancelBtn),
            Tool("Checkpoint", "Write a checkpoint now (needs a checkpoint directory)", () => Send(MsgType.Checkpoint), _ckBtn), Sep(),
            Tool("Save tour…", "Save the current tour as TSPLIB .tour (Ctrl+S)", () => _ = SaveTourDialog()),
            Tool("Export ▾", "Export the map (PNG / SVG) or the run report (JSON)", ShowExport), Sep(),
            new TextBlock { Text = "theme", VerticalAlignment = VerticalAlignment.Center, Margin = new Thickness(4, 0) }, _theme,
        ]);

        // Solve tab
        var side = new StackPanel { Spacing = 10, Margin = new Thickness(12) };
        var form = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*"), RowSpacing = 6, ColumnSpacing = 10 };
        void Row(int r, string label, Control c)
        {
            form.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var t = new TextBlock { Text = label, VerticalAlignment = VerticalAlignment.Center };
            Grid.SetRow(t, r);
            Grid.SetRow(c, r);
            Grid.SetColumn(c, 1);
            form.Children.Add(t);
            form.Children.Add(c);
        }
        var ckRow = new DockPanel();
        var browse = new Button { Content = "…", Margin = new Thickness(4, 0, 0, 0) };
        browse.Click += async (_, _) => await PickCheckpointDir();
        DockPanel.SetDock(browse, Dock.Right);
        ckRow.Children.AddRange([browse, _ckDir]);
        Row(0, "mode", _mode);
        Row(1, "time limit (s)", _time);
        Row(2, "threads", _threads);
        Row(3, "seed", _seed);
        Row(4, "memory ceiling (MB)", _mem);
        Row(5, "checkpoint dir", ckRow);
        Row(6, "", _resume);
        var buttons = new WrapPanel();
        side.Children.AddRange([
            Card("Instance", new StackPanel { Spacing = 4, Children = { _instTitle, _instDetails } }),
            Card("Run settings", new StackPanel { Spacing = 6, Children = { form, _settingsError } }),
            Card("Solver", new StackPanel
            {
                Spacing = 6,
                Children =
                {
                    new WrapPanel { Children = { _ubBadge, new Border { Width = 6 }, _badge } },
                    _bounds, _phase, _stats, _status,
                    new TextBlock { Text = "RAM vs ceiling", Opacity = 0.7 }, _ram, _ramText,
                },
            }),
            Card("Map layers", LayerPanel()),
            ToursCard(),
            Card("Final report", _report),
        ]);
        Grid Fill(Control c) { var g = new Grid(); g.Children.Add(c); return g; }
        var cutsBody = new DockPanel();
        var cutsTop = new StackPanel { Spacing = 4, Children = { _cutBadge, _cutText } };
        DockPanel.SetDock(cutsTop, Dock.Top);
        cutsBody.Children.AddRange([cutsTop, _cuts]);
        var treeBody = new DockPanel();
        DockPanel.SetDock(_treeText, Dock.Bottom);
        treeBody.Children.AddRange([_treeText, _tree]);
        var chartBody = new DockPanel();
        DockPanel.SetDock(_logGap, Dock.Top);
        chartBody.Children.AddRange([_logGap, _chart]);
        var strip = new Grid { ColumnDefinitions = new ColumnDefinitions("*,*,*"), ColumnSpacing = 10, Margin = new Thickness(0, 10, 0, 0) };
        var c0 = Card("Bounds over time", chartBody, fill: true);
        var c1 = Card("Cuts", cutsBody, fill: true);
        var c2 = Card("Branch-and-cut tree", treeBody, fill: true);
        Grid.SetColumn(c1, 1);
        Grid.SetColumn(c2, 2);
        strip.Children.AddRange([c0, c1, c2]);
        var leftCol = new Grid { RowDefinitions = new RowDefinitions("*,300"), Margin = new Thickness(12, 0, 0, 12) };
        leftCol.Children.Add(Fill(new Border { Child = _map, ClipToBounds = true, CornerRadius = new CornerRadius(8) }));
        Grid.SetRow(strip, 1);
        leftCol.Children.Add(strip);
        var solve = new Grid { ColumnDefinitions = new ColumnDefinitions("*,400") };
        solve.Children.Add(leftCol);
        var scroll = new ScrollViewer { Content = side };
        Grid.SetColumn(scroll, 1);
        solve.Children.Add(scroll);

        // Runs tab
        var load = new Button { Content = "Load run", Margin = new Thickness(0, 0, 6, 0) };
        load.Click += (_, _) => LoadSelectedRun();
        var openDir = new Button { Content = "Open folder" };
        openDir.Click += (_, _) => OpenRunFolder();
        var runsTop = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 0, 0, 8), Children = { load, openDir, _allRuns } };
        var runsGrid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,420"), Margin = new Thickness(12) };
        var runsLeft = new DockPanel();
        DockPanel.SetDock(runsTop, Dock.Top);
        runsLeft.Children.AddRange([runsTop, _runs]);
        runsGrid.Children.Add(runsLeft);
        var detail = Card("Run", new ScrollViewer { Content = _runDetail });
        detail.Margin = new Thickness(12, 0, 0, 0);
        Grid.SetColumn(detail, 1);
        runsGrid.Children.Add(detail);

        // Log tab
        var logPanel = new DockPanel { Margin = new Thickness(12) };
        var logTop = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 0, 0, 8), Children = { _logFilter, _logCat } };
        DockPanel.SetDock(logTop, Dock.Top);
        logPanel.Children.AddRange([logTop, _log]);

        _tabs.ItemsSource = new[]
        {
            new TabItem { Header = "Solve", Content = solve },
            new TabItem { Header = "Runs", Content = runsGrid },
            new TabItem { Header = "Log", Content = logPanel },
        };
        var status = new Border { Padding = new Thickness(12, 4), Child = _statusBar };
        var root = new DockPanel();
        DockPanel.SetDock(bar, Dock.Top);
        DockPanel.SetDock(status, Dock.Bottom);
        root.Children.AddRange([bar, status, _tabs]);
        return root;
    }

    private readonly List<Border> _cards = [];

    private WrapPanel LayerPanel()
    {
        var p = new WrapPanel();
        foreach (var c in new[] { _showTour, _showNodes, _showLp, _flashLp, _overlay })
        {
            c.Margin = new Thickness(0, 0, 14, 4);
            p.Children.Add(c);
        }
        var legend = new TextBlock { FontSize = 11, Opacity = 0.75, TextWrapping = TextWrapping.Wrap, Margin = new Thickness(0, 4, 0, 0) };
        legend.Text = "tour: orange = heuristic, blue = LP, green = proven · earlier tours: own colors (Tours found) · last improvement: red dashed = edges cut out, green = edges added · LP: blue x = 1, red 0 < x < 1 (width and opacity by x), yellow = new since the last LP";
        p.Children.Add(legend);
        return p;
    }

    private Border Card(string title, Control body, bool fill = false)
    {
        var head = new TextBlock { Text = title.ToUpperInvariant(), FontSize = 11, FontWeight = FontWeight.SemiBold, Opacity = 0.65, Margin = new Thickness(0, 0, 0, 8) };
        Control content;
        if (fill)
        {
            var d = new DockPanel();
            DockPanel.SetDock(head, Dock.Top);
            d.Children.AddRange([head, body]);
            content = d;
        }
        else content = new StackPanel { Children = { head, body } };
        var b = new Border
        {
            CornerRadius = new CornerRadius(8),
            Padding = new Thickness(12),
            BorderThickness = new Thickness(1),
            Child = content,
        };
        _cards.Add(b);
        return b;
    }

    // ---- theme --------------------------------------------------------------------------

    private void SetTheme(string name)
    {
        _settings.Theme = name;
        if (Application.Current is { } app) app.RequestedThemeVariant = ThemeChoice.Variant(name);
        ApplyPalette();
        SaveSettings();
    }

    private void ApplyPalette()
    {
        _pal = Palette.For(ActualThemeVariant == ThemeVariant.Dark ? ThemeVariant.Dark : ThemeVariant.Light);
        Background = _pal.Brush(_pal.Background);
        foreach (var c in _cards)
        {
            c.Background = _pal.Brush(_pal.Surface);
            c.BorderBrush = _pal.Brush(_pal.Border);
        }
        _map.Renderer.Background = Palette.Sk(_pal.Surface);
        _map.Renderer.NodeColor = Palette.Sk(_pal.MapNode);
        _map.Renderer.TourColor = Palette.Sk(_pal.MapTour);
        _chart.Pal = _cuts.Pal = _tree.Pal = _pal;
        _map.Renderer.LpIntegralColor = Palette.Sk(_pal.Lp);
        _map.Renderer.LpFractionalColor = Palette.Sk(_pal.Danger);
        _map.Renderer.LpNewColor = Palette.Sk(Color.Parse("#F9AB00"));
        _map.Renderer.RemovedColor = Palette.Sk(_pal.Danger);
        _map.Renderer.AddedColor = Palette.Sk(_pal.Proven);
        RebuildTourRows();
        _solverDirty = true;
        RefreshSolverViews();
        RefilterLog();
        _map.Invalidate();
        UpdatePanels();
    }

    // ---- instance -----------------------------------------------------------------------

    private void OnOpened()
    {
        var inst = _opt.Instance;
        if (inst is not null) OpenInstance(inst);
        else if (_settings.Recent.FirstOrDefault(File.Exists) is { } last) OpenInstance(last);
        if (_opt.Attach is not null) AttachRun();
        else if (_opt.DemoOut is not null) { ApplyEngineArgs(_opt.EngineArgs); StartRun(); }
        else if (_opt.DemoApp is not null) StartAppDemo();
        RefreshRuns();
    }

    public bool OpenInstance(string path)
    {
        try
        {
            var info = InstanceInfo.Read(path);
            var scene = Scene.LoadTsplib(path);
            if (_run is { Running: true }) { Toast("stop the current run before opening another instance"); return false; }
            _inst = info;
            _map.Scene = scene;
            _map.Fit();
            _vm.Reset();
            ResetTours();
            _settings.Touch(Path.GetFullPath(path));
            SaveSettings();
            Title = $"APEX-TSP — {info.Name}";
            RefreshRuns();
            UpdatePanels();
            return true;
        }
        catch (Exception e) when (e is IOException or InvalidDataException or FormatException or UnauthorizedAccessException)
        {
            Toast("cannot open " + path + ": " + e.Message);
            return false;
        }
    }

    private async Task OpenDialog()
    {
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions
        {
            Title = "Open TSPLIB instance",
            AllowMultiple = false,
            FileTypeFilter = [new FilePickerFileType("TSPLIB") { Patterns = ["*.tsp"] }, FilePickerFileTypes.All],
        });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } p) OpenInstance(p);
    }

    private void OnDrop(object? sender, DragEventArgs e)
    {
        var f = e.DataTransfer.TryGetFiles()?.Select(x => x.TryGetLocalPath()).FirstOrDefault(p => p is not null);
        if (f is not null) OpenInstance(f);
    }

    private void ShowRecent()
    {
        var menu = new MenuFlyout();
        foreach (var r in _settings.Recent.Where(File.Exists))
        {
            var item = new MenuItem { Header = r };
            var path = r;
            item.Click += (_, _) => OpenInstance(path);
            menu.Items.Add(item);
        }
        if (menu.Items.Count == 0) menu.Items.Add(new MenuItem { Header = "(none)", IsEnabled = false });
        menu.ShowAt(_recentBtn);
    }

    // ---- run settings -------------------------------------------------------------------

    private void LoadSettingsIntoForm(RunSettings s)
    {
        _mode.SelectedItem = RunSettings.Modes.Contains(s.Mode) ? s.Mode : "exact";
        _time.Value = (decimal)s.TimeLimitS;
        _threads.Value = s.Threads;
        _seed.Value = s.Seed;
        _mem.Value = s.MemLimitMb;
        _ckDir.Text = s.CheckpointDir;
        _resume.IsChecked = s.Resume;
    }

    private RunSettings ReadForm() => new()
    {
        Mode = _mode.SelectedItem as string ?? "exact",
        TimeLimitS = (double)(_time.Value ?? 600),
        Threads = (int)(_threads.Value ?? 1),
        Seed = (ulong)(_seed.Value ?? 1),
        MemLimitMb = (int)(_mem.Value ?? 4096),
        CheckpointDir = _ckDir.Text?.Trim() ?? "",
        Resume = _resume.IsChecked == true,
    };

    // Demo / command line: engine args after "--" fill the form.
    private void ApplyEngineArgs(IReadOnlyList<string> a)
    {
        var s = ReadForm();
        for (var i = 0; i + 1 < a.Count; ++i)
        {
            var v = a[i + 1];
            switch (a[i])
            {
                case "--mode": s.Mode = v; break;
                case "--time-limit": s.TimeLimitS = double.Parse(v, CultureInfo.InvariantCulture); break;
                case "--threads": s.Threads = int.Parse(v, CultureInfo.InvariantCulture); break;
                case "--seed": s.Seed = ulong.Parse(v, CultureInfo.InvariantCulture); break;
                case "--mem-limit-mb": s.MemLimitMb = int.Parse(v, CultureInfo.InvariantCulture); break;
                case "--checkpoint": s.CheckpointDir = v; break;
            }
        }
        LoadSettingsIntoForm(s);
    }

    private async Task PickCheckpointDir()
    {
        var dirs = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Checkpoint directory" });
        if (dirs.Count > 0 && dirs[0].TryGetLocalPath() is { } p) _ckDir.Text = p;
    }

    // ---- runs ---------------------------------------------------------------------------

    private void StartRun()
    {
        if (_inst is null) { Toast("open an instance first"); return; }
        if (_run is { Running: true }) { Toast("a run is already active"); return; }
        var s = ReadForm();
        if (s.Validate() is { } why) { _settingsError.Text = why; _settingsError.Foreground = _pal.Brush(_pal.Danger); return; }
        _settingsError.Text = "";
        var engine = EngineLocator.Find(_settings.EnginePath);
        if (engine is null) { _ = LocateEngine(); return; }
        _settings.EnginePath = engine;
        _settings.Run = s.Clone();
        SaveSettings();
        _run?.Dispose();
        _vm.Reset();
        ResetTours();
        _logItems.Clear();
        _runSettings = s;
        var dir = _store.NewRunDir(_inst.Path, out _runId);
        try { _run = RunSession.Launch(engine, _inst.Path, s, dir); }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception)
        {
            Toast("cannot start the engine: " + e.Message);
            return;
        }
        WireSession(_run);
        _run.ConnectAsync(TimeSpan.FromSeconds(60));
        Toast($"run {_runId} started: {s.Mode}, {s.TimeLimitS} s, {s.Threads} threads");
    }

    private void AttachRun()
    {
        if (_inst is null) return;
        var dir = _store.NewRunDir(_inst.Path, out _runId);
        _run = RunSession.Attach(_opt.Attach!, _opt.SharedTour!, dir);
        _runSettings = ReadForm();
        WireSession(_run);
        _run.ConnectAsync(TimeSpan.FromSeconds(60));
    }

    private void WireSession(RunSession s)
    {
        s.Connected += c =>
        {
            c.Telemetry += p => Dispatcher.UIThread.Post(() => _vm.OnTelemetry(p));
            c.Log += l => Dispatcher.UIThread.Post(() => _vm.OnLog(l));
            c.ClusterMap += m => Dispatcher.UIThread.Post(() =>
            {
                foreach (var e in m) _map.Scene?.SetClusterState(e.Id, e.State);
                _map.Invalidate();
            });
            c.TourUpdate += (_, _) => ReadTour(s);
            c.Finished += j => Dispatcher.UIThread.Post(() => OnFinished(s, j));
            c.Closed += r => Dispatcher.UIThread.Post(() => _vm.OnClosed(r));
            c.SolverEvent += e => Dispatcher.UIThread.Post(() =>
            {
                _vm.OnSolverEvent(e);
                if (e.Kind == EventKind.UbImproved) OnUbEvent(e);
            });
            c.CutRound += r => Dispatcher.UIThread.Post(() => _vm.OnCutRound(r));
            c.TreeNode += n => Dispatcher.UIThread.Post(() => _vm.OnTreeNode(n));
            c.LpSolution += (_, e) =>
            {
                var seg = new LpSegment[e.Length];
                for (var i = 0; i < e.Length; ++i) seg[i] = new LpSegment(e[i].U, e[i].V, e[i].X);
                Dispatcher.UIThread.Post(() => OnLp(s, seg));
            };
            Dispatcher.UIThread.Post(() =>
            {
                _vm.RamCeilingBytes = (ulong)c.EngineHello.MemLimitMb << 20;
                UpdatePanels();
                if (_opt.DemoOut is not null) StartDemo();
            });
        };
        s.Acked += t => Dispatcher.UIThread.Post(() => _vm.OnAck(t));
        s.Failed += why => Dispatcher.UIThread.Post(() => _vm.OnClosed(why));
    }

    private void OnLp(RunSession s, LpSegment[] seg)
    {
        if (_run != s || _map.Scene is null) return;
        _map.Scene.SetLp(seg);
        _map.Scene.LpFlash = _flashLp.IsChecked == true;
        _map.Invalidate();
        _flashOff?.Dispose();
        _flashOff = DispatcherTimer.RunOnce(() => { if (_map.Scene is { } sc) { sc.LpFlash = false; _map.Invalidate(); } }, TimeSpan.FromMilliseconds(600));
        ++LpFrames;
    }

    public int LpFrames { get; private set; }

    private void RefreshSolverViews()
    {
        if (!_solverDirty) return;
        _solverDirty = false;
        _chart.Samples = _vm.History.ToArray();
        _chart.Events = _vm.Events.ToArray();
        _chart.InvalidateVisual();
        _cuts.Rounds = _vm.CutRounds.ToArray();
        _cuts.InvalidateVisual();
        _tree.Nodes = new Dictionary<uint, TreeNodePacket>(_vm.Tree);
        _tree.InvalidateVisual();
        var vm = _vm;
        var last = vm.CutRounds.Count > 0 ? vm.CutRounds[^1] : default;
        (string text, Color col) = vm.CutState switch
        {
            "separating" => ("CUTS FOUND", CutsChart.Sec),
            "no violated cuts" => ("NO VIOLATED CUTS", _pal.Danger),
            "tailing off" => ("TAILING OFF", _pal.Heuristic),
            _ => ("NO SEPARATION YET", _pal.Muted),
        };
        _cutBadgeText.Text = text;
        _cutBadge.Background = _pal.Brush(col);
        _cutText.Text = vm.CutRounds.Count == 0 ? "" : string.Create(CultureInfo.InvariantCulture,
            $"round {last.Round}: SEC {last.Sec}, blossom {last.Blossom}, component {last.Component}, pool {last.Pool} · LP {Draw.Num(last.Lp)} · rows {last.Rows}\ntotal SEC {vm.TotalSec} · blossom {vm.TotalBlossom} · component {vm.TotalComponent} · {vm.CutRounds.Count} rounds");
        var tour = vm.Proven || vm.Status == "OPTIMAL_PROVEN" ? _pal.Proven : vm.UbHeuristic ? _pal.Heuristic : _pal.Lp;
        var sk = Palette.Sk(tour);
        if (_map.Renderer.TourColor != sk) { _map.Renderer.TourColor = sk; _map.Invalidate(); }
        UpdatePanels();
    }

    private Color CategoryColor(LogCategory c) => c switch
    {
        LogCategory.Heuristic => _pal.Heuristic,
        LogCategory.Lp => _pal.Lp,
        LogCategory.Cuts => CutsChart.Sec,
        LogCategory.Tree => _pal.Lagrangian,
        _ => _pal.Muted,
    };

    private void ReadTour(RunSession s)
    {
        if (!s.TryReadTour(out var buf, out var count)) return;
        Dispatcher.UIThread.Post(() =>
        {
            if (_run != s || _map.Scene is null) return;
            OnTourRead(buf, count);
            _map.Scene.SetTour(buf, count);
            _map.Invalidate();
        });
    }

    private void OnFinished(RunSession s, string json)
    {
        _vm.OnFinished(json);
        ReadTour(s);
        if (_inst is not null)
        {
            AppStore.SaveRecord(s.RunDir, _vm.ToRecord(_runId, _inst.Path, _inst.Name, s.Started, _runSettings));
            RefreshRuns();
        }
        Toast($"run {_runId}: {_vm.Status}, UB {MainViewModel.Fmt(_vm.UpperBound)}");
        if (_opt.DemoOut is not null) DispatcherTimer.RunOnce(FinishDemo, TimeSpan.FromMilliseconds(800));
        DemoAppOnFinished();
    }

    private void Send(MsgType t)
    {
        if (_run is null || !_run.Send(t)) Toast("no engine connected");
    }

    private async Task LocateEngine()
    {
        Toast("apex_tsp not found: pick the engine binary");
        var files = await StorageProvider.OpenFilePickerAsync(new FilePickerOpenOptions { Title = "Locate apex_tsp", AllowMultiple = false });
        if (files.Count > 0 && files[0].TryGetLocalPath() is { } p)
        {
            _settings.EnginePath = p;
            SaveSettings();
            StartRun();
        }
    }

    // ---- history ------------------------------------------------------------------------

    private void RefreshRuns()
    {
        _runList.Clear();
        _runList.AddRange(_store.ListRuns(_allRuns.IsChecked == true || _inst is null ? null : _inst.Path));
        _runs.ItemsSource = _runList.Select(x => RunLine(x.Run)).ToList();
        ShowRunDetail();
    }

    public static string RunLine(RunRecord r) => string.Create(CultureInfo.InvariantCulture,
        $"{r.Started.LocalDateTime:yyyy-MM-dd HH:mm:ss}  {r.InstanceName,-14} {r.Settings.Mode,-8} {r.Status,-15} UB {MainViewModel.Fmt(r.Ub),-12} gap {MainViewModel.Fmt(r.GapPct)}%  {r.WallS:0.0} s");

    private void ShowRunDetail()
    {
        var i = _runs.SelectedIndex;
        if (i < 0 || i >= _runList.Count) { _runDetail.Text = _runList.Count == 0 ? "no runs yet" : "select a run (double click loads it)"; return; }
        var (dir, r) = _runList[i];
        var s = r.Settings;
        _runDetail.Text = string.Create(CultureInfo.InvariantCulture,
            $"{r.InstanceName}  ({r.Instance})\nstarted {r.Started:yyyy-MM-dd HH:mm:ss zzz}\nstatus {r.Status}\nUB {MainViewModel.Fmt(r.Ub)}\nLB {MainViewModel.Fmt(r.Lb)} ({r.LbSource})\ngap {MainViewModel.Fmt(r.GapPct)}%\nwall {r.WallS:0.###} s\n\nmode {s.Mode}, time limit {s.TimeLimitS} s, threads {s.Threads}, seed {s.Seed}, memory {s.MemLimitMb} MB{(s.CheckpointDir.Length > 0 ? ", checkpoint " + s.CheckpointDir : "")}\n\n{dir}");
    }

    public bool LoadRun(string dir)
    {
        var r = AppStore.LoadRecord(dir);
        if (r is null) return false;
        if (_inst is null || Path.GetFullPath(_inst.Path) != Path.GetFullPath(r.Instance))
            if (!File.Exists(r.Instance) || !OpenInstance(r.Instance)) { Toast("instance file missing: " + r.Instance); return false; }
        var report = Path.Combine(dir, "report.json");
        _vm.LoadRecord(r, File.Exists(report) ? File.ReadAllText(report) : "");
        ResetTours();
        var tour = Path.Combine(dir, "tour.tour");
        if (File.Exists(tour) && _map.Scene is not null)
        {
            var t = TourFiles.Read(tour);
            _map.Scene.SetTour(t, t.Length);
            _map.Invalidate();
        }
        LoadSettingsIntoForm(r.Settings);
        _tabs.SelectedIndex = 0;
        Toast($"loaded run {r.Id}");
        return true;
    }

    private void LoadSelectedRun()
    {
        var i = _runs.SelectedIndex;
        if (i >= 0 && i < _runList.Count) LoadRun(_runList[i].Dir);
    }

    private void OpenRunFolder()
    {
        var i = _runs.SelectedIndex;
        if (i < 0 || i >= _runList.Count) return;
        _ = Launcher.LaunchDirectoryInfoAsync(new DirectoryInfo(_runList[i].Dir));
    }

    // ---- save / export ------------------------------------------------------------------

    private async Task SaveTourDialog()
    {
        if (_map.Scene is not { TourCount: > 0 } s || _inst is null) { Toast("no tour to save"); return; }
        var f = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
        {
            Title = "Save tour", SuggestedFileName = _inst.Name + ".tour", DefaultExtension = "tour",
            FileTypeChoices = [new FilePickerFileType("TSPLIB tour") { Patterns = ["*.tour"] }],
        });
        if (f?.TryGetLocalPath() is { } p) SaveTour(p);
    }

    public void SaveTour(string path)
    {
        var s = _map.Scene!;
        TourFiles.Write(path, _inst!.Name, s.Tour.AsSpan(0, s.TourCount), $"apex_tsp {_vm.Status} ub {MainViewModel.Fmt(_vm.UpperBound)}");
        Toast("tour saved: " + path);
    }

    private void ShowExport()
    {
        var menu = new MenuFlyout();
        void Item(string text, string ext, string type, Action<string> save)
        {
            var it = new MenuItem { Header = text };
            it.Click += async (_, _) =>
            {
                var f = await StorageProvider.SaveFilePickerAsync(new FilePickerSaveOptions
                {
                    Title = text, SuggestedFileName = (_inst?.Name ?? "apex") + "." + ext, DefaultExtension = ext,
                    FileTypeChoices = [new FilePickerFileType(type) { Patterns = ["*." + ext] }],
                });
                if (f?.TryGetLocalPath() is { } p) { save(p); Toast("saved " + p); }
            };
            menu.Items.Add(it);
        }
        Item("Map as PNG", "png", "PNG image", _map.SavePng);
        Item("Tour as SVG", "svg", "SVG image", p =>
        {
            if (_map.Scene is { } s) TourFiles.WriteSvg(p, s, s.Tour.AsSpan(0, s.TourCount), _pal.MapTour.ToString());
        });
        Item("Run report (JSON)", "json", "JSON", p => File.WriteAllText(p, _vm.ReportJson));
        menu.ShowAt(this, true);
    }

    // ---- panels -------------------------------------------------------------------------

    private void UpdatePanels()
    {
        var vm = _vm;
        var (lbText, lbSrc) = vm.Status == "CANCELLED" && !vm.Proven ? ("CANCELLED · " + vm.LbBadge.Text, vm.LbBadge.Source) : vm.LbBadge;
        _badgeText.Text = lbText;
        _badge.Background = _pal.Brush(_pal.ForSource(lbSrc));
        var (ubText, ubSrc) = vm.UbBadge;
        _ubBadgeText.Text = ubText;
        _ubBadge.Background = _pal.Brush(ubSrc == "heuristic" ? _pal.Heuristic : _pal.ForSource(ubSrc));
        _phase.Text = $"{vm.Mode} · phase {vm.Phase}";
        _bounds.Text = $"UB {MainViewModel.Fmt(vm.UpperBound)}   LB {MainViewModel.Fmt(vm.LowerBound)}   gap {MainViewModel.Fmt(vm.GapPct)}%";
        var it = vm.IterPerSec.ToString("0", CultureInfo.InvariantCulture);
        _stats.Text = $"cut pool {vm.CutPoolSize}   open nodes {vm.OpenNodes}   tree {vm.Tree.Count}   simplex it/s {it}";
        _ram.Value = vm.RamFraction;
        _ramText.Text = $"{vm.RamBytes >> 20} MB / {vm.RamCeilingBytes >> 20} MB";
        _status.Text = "status: " + vm.Status;
        _report.Text = vm.Finished ? vm.ReportJson : "";
        var running = _run is { Running: true } && !vm.Finished;
        _startBtn.IsEnabled = _inst is not null && !running;
        _pauseBtn.IsEnabled = _resumeBtn.IsEnabled = _cancelBtn.IsEnabled = running;
        _ckBtn.IsEnabled = running && (_runSettings.CheckpointDir.Length > 0);
        if (_inst is { } i)
        {
            _instTitle.Text = i.Name;
            var sc = _map.Scene;
            _instDetails.Text = string.Create(CultureInfo.InvariantCulture,
                $"{i.Dimension} nodes · {i.EdgeWeightType}\n{(sc is null ? "" : $"x {sc.MinX:0.##} .. {sc.MaxX:0.##}, y {sc.MinY:0.##} .. {sc.MaxY:0.##}\n")}{i.Comment}\n{i.Path}");
        }
        else
        {
            _instTitle.Text = "No instance";
            _instDetails.Text = "Open a .tsp (Ctrl+O) or drop one on the window.";
        }
        _statusBar.Text = $"{(_inst?.Name ?? "-")} · {vm.Status} · engine {(EngineLocator.Find(_settings.EnginePath) ?? "not found")} · history {_store.Home}";
        _statusBar.Foreground = _pal.Brush(_pal.Muted);
    }

    private void AddLogLine(string line)
    {
        var f = _logFilter.Text;
        if (!string.IsNullOrEmpty(f) && !line.Contains(f, StringComparison.OrdinalIgnoreCase)) return;
        var cat = LogLine.Classify(line);
        if (_logCat.SelectedIndex > 0 && (int)cat != _logCat.SelectedIndex - 1) return;
        _logItems.Add(new LogLine(line, cat));
        if (_logItems.Count > _vm.LogCapacity) _logItems.RemoveAt(0);
    }

    private void RefilterLog()
    {
        _logItems.Clear();
        foreach (var l in _vm.LogTail) AddLogLine(l);
    }

    private void Toast(string text)
    {
        _vm.OnLog("[app] " + text);
        _statusBar.Text = text;
        _statusBar.Foreground = _pal.Brush(_pal.Text);
        AppDemoLog(text);
    }

    private void SaveSettings()
    {
        try { _settings.Run = ReadForm(); _store.SaveSettings(_settings); }
        catch (IOException) { }
    }
}

// Minimal ICommand for key bindings.
public sealed class RelayCommand(Action run) : System.Windows.Input.ICommand
{
    public event EventHandler? CanExecuteChanged { add { } remove { } }
    public bool CanExecute(object? parameter) => true;
    public void Execute(object? parameter) => run();
}

public static class FrameStats
{
    public static double Percentile(IReadOnlyList<double> v, double q)
    {
        if (v.Count == 0) return double.NaN;
        var s = v.OrderBy(x => x).ToArray();
        return s[Math.Min(s.Length - 1, (int)Math.Ceiling(q * s.Length) - 1)];
    }

    public static string Describe(IReadOnlyList<double> ms) =>
        string.Create(CultureInfo.InvariantCulture,
            $"n={ms.Count} p50={Percentile(ms, 0.5):0.00} p95={Percentile(ms, 0.95):0.00} p99={Percentile(ms, 0.99):0.00} max={(ms.Count > 0 ? ms.Max() : double.NaN):0.00} ms");

    public static string Describe(MapView map, int nodes)
    {
        var sb = new StringBuilder();
        sb.AppendLine($"nodes {nodes}, map {(int)map.Bounds.Width}x{(int)map.Bounds.Height} px, CPU render (SkiaSharp -> WriteableBitmap)");
        sb.AppendLine("render ms per frame: " + Describe(map.FrameMs));
        sb.AppendLine("input-to-rendered-frame ms (scripted input through the pointer path): " + Describe(map.InputLatencyMs));
        foreach (var (level, count) in map.LevelFrames.OrderBy(k => k.Key)) sb.AppendLine($"frames at {level}: {count}");
        return sb.ToString();
    }
}
