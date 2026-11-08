using System.Diagnostics;
using System.Globalization;
using System.Text;
using ApexTsp.Ipc;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace ApexTsp.Gui;

public sealed class MainWindow : Window
{
    private readonly GuiOptions _opt;
    private readonly MainViewModel _vm = new();
    private readonly MapView _map = new();
    private readonly GapChart _chart = new() { Height = 150 };
    private readonly TextBlock _bounds = new(), _phase = new(), _stats = new(), _status = new(), _ramText = new();
    private readonly Border _badge = new() { CornerRadius = new CornerRadius(4), Padding = new Thickness(6, 2) };
    private readonly TextBlock _badgeText = new() { Foreground = Brushes.White, FontWeight = FontWeight.SemiBold };
    private readonly ProgressBar _ram = new() { Minimum = 0, Maximum = 1, Height = 10 };
    private readonly TextBox _report = new() { IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap, Height = 220, FontFamily = new FontFamily("monospace"), FontSize = 11 };
    private readonly CheckBox _overlay = new() { Content = "cluster heatmap overlay", IsChecked = true };
    private readonly Dictionary<ulong, MsgType> _sent = [];
    private EngineClient? _client;
    private SharedTourReader? _tourReader;
    private Process? _engine;
    private string _sharedTour = "";
    private DispatcherTimer? _demo;
    private int _demoStep;

    public MainWindow(GuiOptions opt)
    {
        _opt = opt;
        Title = "APEX-TSP";
        Width = 1400;
        Height = 900;
        _map.Scene = Scene.LoadTsplib(opt.Instance);
        _badge.Child = _badgeText;

        Button Ctl(string text, MsgType t)
        {
            var b = new Button { Content = text, Margin = new Thickness(0, 0, 6, 6) };
            b.Click += (_, _) => Send(t);
            return b;
        }
        _overlay.IsCheckedChanged += (_, _) => { _map.Renderer.ClusterOverlay = _overlay.IsChecked == true; _map.Invalidate(); };
        var buttons = new WrapPanel();
        buttons.Children.AddRange([Ctl("Pause", MsgType.Pause), Ctl("Resume", MsgType.Resume), Ctl("Cancel", MsgType.Cancel), Ctl("Checkpoint", MsgType.Checkpoint)]);
        var side = new StackPanel { Spacing = 6, Margin = new Thickness(10) };
        side.Children.AddRange([
            new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { _badge, _phase } },
            _bounds, _chart, _stats, new TextBlock { Text = "RAM vs ceiling" }, _ram, _ramText, _overlay, buttons, _status,
            new TextBlock { Text = "final report" }, _report,
        ]);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,360") };
        grid.Children.Add(_map);
        var scroll = new ScrollViewer { Content = side };
        Grid.SetColumn(scroll, 1);
        grid.Children.Add(scroll);
        Content = grid;

        _vm.PropertyChanged += (_, _) => UpdatePanels();
        _vm.HistoryChanged += () => { _chart.Samples = _vm.History.ToArray(); _chart.InvalidateVisual(); };
        _vm.ClusterBox += (id, x0, y0, x1, y1) => _map.Scene!.SetCluster(id, x0, y0, x1, y1);
        UpdatePanels();
        Opened += (_, _) => Start();
        Closing += (_, _) => Stop();
    }

    private void Start()
    {
        var endpoint = _opt.Attach ?? _opt.Endpoint ?? (OperatingSystem.IsWindows()
            ? $"apex_tsp_gui_{Environment.ProcessId}"
            : Path.Combine(Path.GetTempPath(), $"apex_tsp_gui_{Environment.ProcessId}.sock"));
        _sharedTour = _opt.SharedTour ?? Path.Combine(Path.GetTempPath(), $"apex_tsp_gui_{Environment.ProcessId}.tour");
        if (_opt.Engine is not null) LaunchEngine(endpoint);
        Task.Run(() =>
        {
            try
            {
                var c = EngineClient.Connect(endpoint, TimeSpan.FromSeconds(60));
                c.Telemetry += p => Dispatcher.UIThread.Post(() => _vm.OnTelemetry(p));
                c.Log += l => Dispatcher.UIThread.Post(() => _vm.OnLog(l));
                c.Ack += seq => Dispatcher.UIThread.Post(() => { if (_sent.Remove(seq, out var t)) _vm.OnAck(t); });
                c.ClusterMap += m => Dispatcher.UIThread.Post(() =>
                {
                    foreach (var e in m) _map.Scene!.SetClusterState(e.Id, e.State);
                    _map.Invalidate();
                });
                c.TourUpdate += (_, _) => ReadTour();
                c.Finished += j => Dispatcher.UIThread.Post(() => OnFinished(j));
                c.Closed += r => Dispatcher.UIThread.Post(() => _vm.OnClosed(r));
                Dispatcher.UIThread.Post(() =>
                {
                    _client = c;
                    _vm.RamCeilingBytes = (ulong)c.EngineHello.MemLimitMb << 20;
                    UpdatePanels();
                    if (_opt.DemoOut is not null) StartDemo();
                });
                ReadTour();
            }
            catch (Exception e) when (e is IOException or TimeoutException or System.Net.Sockets.SocketException)
            {
                Dispatcher.UIThread.Post(() => _vm.OnClosed(e.Message));
            }
        });
    }

    private void LaunchEngine(string endpoint)
    {
        var psi = new ProcessStartInfo(_opt.Engine!) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { "--instance", _opt.Instance, "--ipc", endpoint, "--shared-tour", _sharedTour }) psi.ArgumentList.Add(a);
        foreach (var a in _opt.EngineArgs) psi.ArgumentList.Add(a);
        _engine = Process.Start(psi) ?? throw new IOException("cannot start " + _opt.Engine);
        var logPath = Path.Combine(_opt.DemoOut ?? Path.GetTempPath(), "engine.log");
        if (_opt.DemoOut is not null) Directory.CreateDirectory(_opt.DemoOut);
        var log = new StreamWriter(logPath, false, Encoding.UTF8) { AutoFlush = true };
        _engine.OutputDataReceived += (_, e) => { if (e.Data is not null) lock (log) log.WriteLine(e.Data); };
        _engine.ErrorDataReceived += (_, e) => { if (e.Data is not null) lock (log) log.WriteLine(e.Data); };
        _engine.BeginOutputReadLine();
        _engine.BeginErrorReadLine();
    }

    // Reader thread: copy the SharedTour (seqlock; never blocks the engine), then swap on the UI thread.
    private void ReadTour()
    {
        try
        {
            _tourReader ??= new SharedTourReader(_sharedTour);
            var buf = new uint[_tourReader.Capacity];
            if (!_tourReader.TryRead(ref buf, out var count, out _)) return;
            Dispatcher.UIThread.Post(() => { _map.Scene!.SetTour(buf, count); _map.Invalidate(); });
        }
        catch (IOException) { }
    }

    private void Send(MsgType t)
    {
        if (_client is null) return;
        try { _sent[_client.Send(t)] = t; }
        catch (IOException e) { _vm.OnClosed(e.Message); }
    }

    private void UpdatePanels()
    {
        var vm = _vm;
        _badgeText.Text = "LB: " + vm.LbSource;
        _badge.Background = vm.LbSource == "lp" ? new SolidColorBrush(Color.FromRgb(0x2E, 0x7D, 0x32))
                          : vm.LbSource == "lagrangian" ? new SolidColorBrush(Color.FromRgb(0x6A, 0x1B, 0x9A)) : Brushes.Gray;
        _phase.Text = $"{vm.Mode} · phase {vm.Phase}";
        _bounds.Text = $"UB {MainViewModel.Fmt(vm.UpperBound)}   LB {MainViewModel.Fmt(vm.LowerBound)}   gap {MainViewModel.Fmt(vm.GapPct)}%";
        var it = vm.IterPerSec.ToString("0", CultureInfo.InvariantCulture);
        _stats.Text = $"cut pool {vm.CutPoolSize}   open nodes {vm.OpenNodes}   simplex it/s {it}";
        _ram.Value = vm.RamFraction;
        _ramText.Text = $"{vm.RamBytes >> 20} MB / {vm.RamCeilingBytes >> 20} MB";
        _status.Text = "status: " + vm.Status;
        if (vm.Finished) _report.Text = vm.ReportJson;
    }

    private void OnFinished(string json)
    {
        _vm.OnFinished(json);
        ReadTour();
        if (_opt.DemoOut is not null) DispatcherTimer.RunOnce(FinishDemo, TimeSpan.FromMilliseconds(800));
    }

    // ---- scripted demo (G5.3 evidence): pan/zoom cycles through the input path, LOD levels,
    // screenshots, frame-time distribution, then exit.
    private void StartDemo()
    {
        Directory.CreateDirectory(_opt.DemoOut!);
        _demo = new DispatcherTimer(TimeSpan.FromMilliseconds(33), DispatcherPriority.Background, (_, _) => DemoTick());
        _demo.Start();
    }

    private void DemoTick()
    {
        if (_map.Scene!.TourCount == 0) return;
        var cx = _map.Bounds.Width / 2;
        var cy = _map.Bounds.Height / 2;
        var i = _demoStep++ % 300;
        var first = _demoStep <= 300;
        var shot = (string name) => { if (first) _map.SavePng(Path.Combine(_opt.DemoOut!, name)); };
        switch (i)
        {
            case 0: _map.Fit(); break;
            case 1: shot("map_fit.png"); break;
            case < 100: _map.ZoomAt(1.06, cx, cy); break;
            case 100: shot("map_zoomed.png"); break;
            case < 160: _map.PanBy(8, 4); break;
            case < 250: _map.ZoomAt(1 / 1.06, cx, cy); break;
            case 250: _map.Renderer.Force = LodLevel.Clusters; _map.Fit(); break;
            case 252: shot("map_clusters.png"); break;
            case 260: _map.Renderer.Force = LodLevel.Heatmap; _map.Invalidate(); break;
            case 262: shot("map_heatmap.png"); break;
            case 270: _map.Renderer.Force = null; _map.Invalidate(); break;
            default: _map.PanBy(i % 2 == 0 ? 3 : -3, 0); break;
        }
        if (first && _map.Scene.Clusters.Any(c => c.State != 0) && !File.Exists(Path.Combine(_opt.DemoOut!, "map_cluster_overlay.png")))
            _map.SavePng(Path.Combine(_opt.DemoOut!, "map_cluster_overlay.png"));
    }

    private void FinishDemo()
    {
        _demo?.Stop();
        var dir = _opt.DemoOut!;
        _map.Renderer.Force = null;
        _map.Fit();
        _map.SavePng(Path.Combine(dir, "map_final.png"));
        var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
        using (var rtb = new RenderTargetBitmap(size, new Vector(96, 96)))
        {
            rtb.Render(this);
            rtb.Save(Path.Combine(dir, "window_final.png"));
        }
        File.WriteAllText(Path.Combine(dir, "report.json"), _vm.ReportJson);
        File.WriteAllText(Path.Combine(dir, "frame_stats.txt"), FrameStats.Describe(_map, _map.Scene!.Count));
        Close();
    }

    private void Stop()
    {
        _demo?.Stop();
        try
        {
            if (_engine is { HasExited: false }) Send(MsgType.Shutdown);
            _engine?.WaitForExit(5000);
            if (_engine is { HasExited: false }) _engine.Kill();
        }
        catch (InvalidOperationException) { }
        _client?.Dispose();
        _tourReader?.Dispose();
    }
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
