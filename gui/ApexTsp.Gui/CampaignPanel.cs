using System.Diagnostics;
using System.Globalization;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Platform.Storage;
using Avalonia.Threading;

namespace ApexTsp.Gui;

public sealed record CampaignRun(string Instance, int N, string Solver, double Limit)
{
    public string Key => $"{Instance}__{Solver}";
    public bool IsApex => Solver.StartsWith("apex", StringComparison.Ordinal);
}

// Same plan as engine/tools/history/run_history.py (both write the same per-run JSON).
public static class CampaignPlan
{
    public const int ExactMaxN = 4000;
    public static readonly string[] Solvers = ["apex_exact", "apex_scalable", "concorde", "lkh"];

    public static double TimeBox(int n, bool exact) =>
        exact ? (n < 200 ? 60 : n < 1000 ? 600 : n < 3000 ? 1800 : 3600) : (n < 1000 ? 30 : n < 5000 ? 120 : 600);

    // Seconds as a short duration: "0.07 s", "439 s", "1.2 h", "49.5 d", "136.0 y".
    public static string Dur(double s)
    {
        if (double.IsNaN(s)) return "-";
        var (v, f, u) = s < 100 ? (s, "0.00", "s") : s < 3600 ? (s, "0", "s") : s < 86400 ? (s / 3600, "0.0", "h") : s < 365.25 * 86400 ? (s / 86400, "0.0", "d") : (s / (365.25 * 86400), "0.0", "y");
        return v.ToString(f, CultureInfo.InvariantCulture) + " " + u;
    }

    public static List<CampaignRun> Build(IEnumerable<(string Name, int N)> instances, IReadOnlyCollection<string> solvers) =>
        [.. from i in instances.OrderBy(i => i.N)
            from s in Solvers
            where solvers.Contains(s)
            let exact = s is "apex_exact" or "concorde"
            where !exact || i.N <= ExactMaxN
            select new CampaignRun(i.Name, i.N, s, TimeBox(i.N, exact))];
}

// Benchmarks tab (M6.7): the historical campaign runs from the app, one run at a time, live.
// APEX runs go through the normal run session (map, LP, cuts, tree, bounds as in Solve);
// Concorde / LKH-3 runs (test-environment baselines) stream their log and show their tour.
public sealed partial class MainWindow
{
    private readonly TextBox _campDir = new() { MinWidth = 300 };
    private readonly NumericUpDown _campMinN = new() { Minimum = 0, Maximum = 1000000, Value = 10000, Increment = 1000, FormatString = "0", Width = 130 };
    private readonly NumericUpDown _campMaxN = new() { Minimum = 0, Maximum = 1000000, Value = 1000000, Increment = 10000, FormatString = "0", Width = 130 };
    private readonly Button _campReset = new() { Content = "Reset results" };
    private DateTime _resetArmed;
    private readonly Dictionary<string, CheckBox> _campSolver = [];
    private readonly Button _campStart = new() { Content = "▶ Start campaign" }, _campStop = new() { Content = "Stop" };
    private readonly TextBlock _campStatus = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _campLive = new() { FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar _campProgress = new() { Minimum = 0, Maximum = 1, Height = 6 };
    private readonly Grid _campHead = new();
    private readonly StackPanel _campRows = new();
    private readonly WrapPanel _campKpis = new();
    private readonly ListBox _campLog = new() { FontFamily = new FontFamily("monospace"), FontSize = 10 };
    private readonly System.Collections.ObjectModel.ObservableCollection<string> _campLogItems = [];
    private readonly MapView _benchMap = new();
    private readonly BoundsChart _benchChart = new();
    private List<CampaignRun> _plan = [];
    private CampaignRun? _current;
    private bool _campaignActive;
    private Process? _baseProc;
    private StreamWriter? _baseLog;
    private DateTime _runStart;
    private string _campaignRoot = "";

    private static string? RepoRoot() => ReferenceTable.Locate() is { } p ? Path.GetFullPath(Path.Combine(Path.GetDirectoryName(p)!, "..", "..", "..")) : null;
    private static string Baseline(string root, string solver) =>
        solver == "concorde" ? Path.Combine(root, "third_party_test", "concorde", "TSP", "concorde") : Path.Combine(root, "third_party_test", "lkh", "LKH");

    private Control BuildCampaignTab()
    {
        var root = RepoRoot();
        _campDir.Text = root is null ? Path.Combine(_store.Home, "campaigns", "default") : Path.Combine(root, "out", "history", "final");
        var labels = new Dictionary<string, string> { ["apex_exact"] = "APEX exact", ["apex_scalable"] = "APEX scalable", ["concorde"] = "Concorde", ["lkh"] = "LKH-3" };
        var solverRow = new WrapPanel();
        foreach (var s in CampaignPlan.Solvers)
        {
            var baseline = s is "concorde" or "lkh";
            var available = !baseline || (root is not null && File.Exists(Baseline(root, s)));
            var cb = new CheckBox { Content = labels[s] + (available ? "" : " (not built)"), IsChecked = available, IsEnabled = available, Margin = new Thickness(0, 0, 14, 0) };
            if (baseline) ToolTip.SetTip(cb, "test-environment baseline (engine/tools/baselines), never part of the product");
            _campSolver[s] = cb;
            solverRow.Children.Add(cb);
        }
        var browse = new Button { Content = "…", Margin = new Thickness(4, 0, 0, 0) };
        browse.Click += async (_, _) =>
        {
            var d = await StorageProvider.OpenFolderPickerAsync(new FolderPickerOpenOptions { Title = "Campaign folder" });
            if (d.Count > 0 && d[0].TryGetLocalPath() is { } p) { _campDir.Text = p; RefreshCampaignTable(); }
        };
        var download = new Button { Content = "Download missing instances" };
        download.Click += async (_, _) => await DownloadInstances();
        var report = new Button { Content = "Generate report" };
        report.Click += (_, _) => GenerateReport();
        var solve = new Button { Content = "Watch in Solve tab" };
        solve.Click += (_, _) => _tabs.SelectedIndex = 0;
        _campStart.Click += (_, _) => StartCampaign();
        _campStop.Click += (_, _) => StopCampaign();
        _campLog.ItemsSource = _campLogItems;
        _campMaxN.ValueChanged += (_, _) => RefreshCampaignTable();
        _campMinN.ValueChanged += (_, _) => RefreshCampaignTable();
        _campReset.Click += (_, _) => ResetResults();
        ToolTip.SetTip(_campReset, "delete the results (json, log, tour) of the instances in the n range for the checked solvers, so Start runs them again; published history columns are kept");
        _benchMap.Scene = null;

        var dirRow = new DockPanel();
        DockPanel.SetDock(browse, Dock.Right);
        dirRow.Children.AddRange([browse, _campDir]);
        solverRow.Children.Insert(0, new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(0, 0, 24, 0),
            Children =
            {
                new TextBlock { Text = "n from", VerticalAlignment = VerticalAlignment.Center }, _campMinN,
                new TextBlock { Text = "to", VerticalAlignment = VerticalAlignment.Center }, _campMaxN,
            } });
        foreach (var c in solverRow.Children) c.VerticalAlignment = VerticalAlignment.Center;
        var actions = new WrapPanel { Children = { _campStart, new Border { Width = 6 }, _campStop, new Border { Width = 18 }, download, new Border { Width = 6 }, report, new Border { Width = 6 }, solve, new Border { Width = 18 }, _campReset } };
        var controls = new StackPanel { Spacing = 8, Children = { dirRow, solverRow, actions, _campProgress, _campStatus } };

        var live = new DockPanel();
        DockPanel.SetDock(_campLive, Dock.Top);
        _campLive.Margin = new Thickness(0, 0, 0, 8);
        _campLive.Text = "idle: start a campaign and the current run shows here (map, bounds, log)";
        var liveGrid = new Grid { RowDefinitions = new RowDefinitions("*,150,150"), RowSpacing = 8 };
        liveGrid.Children.Add(new Border { Child = _benchMap, ClipToBounds = true, CornerRadius = new CornerRadius(6) });
        Grid.SetRow(_benchChart, 1);
        liveGrid.Children.Add(_benchChart);
        Grid.SetRow(_campLog, 2);
        liveGrid.Children.Add(_campLog);
        live.Children.AddRange([_campLive, liveGrid]);

        // Comparison: geometric-mean tiles, then the table (fixed header, rows scroll), notes folded at the bottom.
        var rowsScroll = new ScrollViewer { Content = _campRows, HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled };
        var tableDock = new DockPanel();
        DockPanel.SetDock(_campHead, Dock.Top);
        tableDock.Children.AddRange([_campHead, rowsScroll]);
        var tableScroll = new ScrollViewer
        {
            Content = tableDock,
            HorizontalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = Avalonia.Controls.Primitives.ScrollBarVisibility.Disabled,
        };
        var kpiBox = new StackPanel
        {
            Spacing = 4, Margin = new Thickness(0, 0, 0, 10),
            Children = { new TextBlock { Text = "Geometric mean of APEX exact time ÷ the other time, proven optima only (below 1x = APEX faster)", FontSize = 11, Opacity = 0.7 }, _campKpis },
        };
        var notes = new Expander { Header = "Notes and sources", Content = _campSummary, Margin = new Thickness(0, 8, 0, 0), HorizontalAlignment = HorizontalAlignment.Stretch };
        var tableBody = new DockPanel();
        DockPanel.SetDock(kpiBox, Dock.Top);
        DockPanel.SetDock(notes, Dock.Bottom);
        tableBody.Children.AddRange([kpiBox, notes, tableScroll]);

        var left = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), RowSpacing = 10, Margin = new Thickness(12, 0, 6, 12) };
        left.Children.Add(Card("Campaign (historical benchmark)", controls));
        var table = Card("Comparison (live)", tableBody, fill: true);
        Grid.SetRow(table, 1);
        left.Children.Add(table);
        var right = Card("Running now", live, fill: true);
        right.Margin = new Thickness(6, 0, 12, 12);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,340") };
        grid.Children.Add(left);
        Grid.SetColumn(right, 1);
        grid.Children.Add(right);
        new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => CampaignTick()).Start();
        RefreshCampaignTable();
        return grid;
    }

    // ---- plan / files -------------------------------------------------------------------

    private List<(string Name, int N)> CampaignInstances()
    {
        int min = (int)(_campMinN.Value ?? 0), max = (int)(_campMaxN.Value ?? 1002);
        var path = ReferenceTable.Locate();
        if (path is null) return [];
        var lines = File.ReadAllLines(path);
        var head = ReferenceTable.SplitCsv(lines[0]);
        int c(string n) => Array.IndexOf(head, n);
        return [.. lines.Skip(1).Select(ReferenceTable.SplitCsv)
            .Where(f => f[c("in_set")] == "yes")
            .Select(f => (Name: f[c("instance")], N: int.Parse(f[c("n")], CultureInfo.InvariantCulture)))
            .Where(i => i.N >= min && i.N <= max)];
    }

    // Two clicks within 5 s: the first arms (shows the count), the second deletes.
    private void ResetResults()
    {
        if (_campaignActive) { Toast("stop the campaign first"); return; }
        var solvers = _campSolver.Where(kv => kv.Value.IsChecked == true).Select(kv => kv.Key).ToList();
        var runs = CampaignPlan.Build(CampaignInstances(), solvers).Where(r => File.Exists(RunBase(r) + ".json") || Directory.Exists(RunBase(r))).ToList();
        if (runs.Count == 0) { Toast("no results to reset in this n range for the checked solvers"); return; }
        if (DateTime.UtcNow - _resetArmed > TimeSpan.FromSeconds(5))
        {
            _resetArmed = DateTime.UtcNow;
            _campReset.Content = $"Confirm: delete {runs.Count} results";
            DispatcherTimer.RunOnce(() => { if (DateTime.UtcNow - _resetArmed >= TimeSpan.FromSeconds(5)) _campReset.Content = "Reset results"; }, TimeSpan.FromSeconds(5.1));
            return;
        }
        _resetArmed = default;
        _campReset.Content = "Reset results";
        var failed = 0;
        foreach (var r in runs)
        {
            var b = RunBase(r);
            try
            {
                foreach (var ext in new[] { ".json", ".json.tmp", ".log", ".tour" }) File.Delete(b + ext);
                if (Directory.Exists(b)) Directory.Delete(b, true);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { ++failed; }
        }
        RefreshCampaignTable();
        Toast($"reset: {runs.Count - failed} results deleted in {CampaignDir}" + (failed > 0 ? $", {failed} failed" : ""));
    }

    private string InstancePath(string name) => Path.Combine(RepoRoot() ?? _store.Home, "data", "tsplib", name + ".tsp");
    private string CampaignDir => _campaignActive ? _campaignRoot : (_campDir.Text?.Trim() ?? "");
    private string RunBase(CampaignRun r) => Path.Combine(CampaignDir, r.Key);
    private bool Done(CampaignRun r) => File.Exists(RunBase(r) + ".json");
    private bool CampaignOwns(RunSession s) => _campaignActive && _current is { IsApex: true } c && s.RunDir == RunBase(c);

    private async Task DownloadInstances()
    {
        var root = RepoRoot();
        var manifest = root is null ? null : Path.Combine(root, "engine", "tools", "history", "manifest.csv");
        if (manifest is null || !File.Exists(manifest)) { Toast("manifest.csv not found"); return; }
        using var http = new HttpClient { Timeout = TimeSpan.FromMinutes(10) };
        http.DefaultRequestHeaders.UserAgent.ParseAdd("apex-tsp-gui/1");
        var rows = File.ReadAllLines(manifest).Skip(1).Select(ReferenceTable.SplitCsv).ToList();
        var got = 0;
        foreach (var f in rows)
        {
            var (name, url, sha) = (f[0], f[1], f[2]);
            var dst = InstancePath(name);
            if (File.Exists(dst) && Sha(dst) == sha) continue;
            _campStatus.Text = "downloading " + name + " …";
            var bytes = await http.GetByteArrayAsync(url);
            if (url.EndsWith(".gz", StringComparison.Ordinal))
            {
                using var gz = new GZipStream(new MemoryStream(bytes), CompressionMode.Decompress);
                using var ms = new MemoryStream();
                await gz.CopyToAsync(ms);
                bytes = ms.ToArray();
            }
            if (Convert.ToHexStringLower(SHA256.HashData(bytes)) != sha) { Toast($"{name}: checksum mismatch, not saved"); continue; }
            Directory.CreateDirectory(Path.GetDirectoryName(dst)!);
            await File.WriteAllBytesAsync(dst, bytes);
            ++got;
        }
        _campStatus.Text = $"{got} instances downloaded; {rows.Count} pinned in manifest.csv";
        RefreshCampaignTable();
    }

    private static string Sha(string path)
    {
        using var f = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(f));
    }

    // ---- run loop -----------------------------------------------------------------------

    private void StartCampaign()
    {
        if (_campaignActive) return;
        if (_run is { Running: true } && !_vm.Finished) { Toast("stop the current run first"); return; }
        var solvers = _campSolver.Where(kv => kv.Value.IsChecked == true).Select(kv => kv.Key).ToList();
        var inst = CampaignInstances();
        var missing = inst.Where(i => !File.Exists(InstancePath(i.Name))).Select(i => i.Name).ToList();
        if (missing.Count > 0) { Toast($"{missing.Count} instance files missing ({missing[0]}, …): Download missing instances"); return; }
        _campaignRoot = _campDir.Text?.Trim() ?? "";
        if (_campaignRoot.Length == 0 || solvers.Count == 0) { Toast("choose a folder and at least one solver"); return; }
        Directory.CreateDirectory(_campaignRoot);
        _plan = CampaignPlan.Build(inst, solvers);
        WriteMeta();
        _campaignActive = true;
        _campLogItems.Clear();
        Toast($"campaign: {_plan.Count(r => !Done(r))} runs to do of {_plan.Count} ({_plan.Count(Done)} already done) in {_campaignRoot}");
        NextRun();
    }

    private void WriteMeta()
    {
        var meta = Path.Combine(_campaignRoot, "meta.json");
        if (File.Exists(meta)) return;
        var root = RepoRoot() ?? "";
        var versions = Path.Combine(root, "third_party_test", "VERSIONS.txt");
        string cpu = "";
        try { cpu = File.ReadLines("/proc/cpuinfo").FirstOrDefault(l => l.StartsWith("model name", StringComparison.Ordinal))?.Split(':', 2)[1].Trim() ?? ""; }
        catch (IOException) { }
        var o = new JsonObject
        {
            ["date"] = DateTimeOffset.Now.ToString("yyyy-MM-ddTHH:mm:sszzz", CultureInfo.InvariantCulture),
            ["cpu"] = cpu, ["logical_cpus"] = Environment.ProcessorCount, ["os"] = System.Runtime.InteropServices.RuntimeInformation.OSDescription,
            ["threads_apex"] = 5, ["mem_limit_mb"] = 4096, ["seed"] = 1, ["exact_max_n"] = CampaignPlan.ExactMaxN,
            ["engine"] = EngineLocator.Find(_settings.EnginePath) ?? "", ["started_from"] = "gui",
            ["baselines"] = File.Exists(versions) ? File.ReadAllText(versions) : "",
        };
        File.WriteAllText(meta, o.ToJsonString(new JsonSerializerOptions { WriteIndented = true }));
    }

    private void NextRun()
    {
        if (!_campaignActive) return;
        _current = _plan.FirstOrDefault(r => !Done(r));
        RefreshCampaignTable();
        if (_current is not { } r)
        {
            _campaignActive = false;
            _campLive.Text = "campaign finished";
            Toast($"campaign finished: {_plan.Count} runs in {_campaignRoot}");
            return;
        }
        _run?.Dispose();  // the previous engine has sent FINISHED; let it exit before the next one
        _run = null;
        if (!OpenInstance(InstancePath(r.Instance))) { WriteRunJson(r, new JsonObject { ["status"] = "ERROR", ["error"] = "cannot open instance" }); Next(); return; }
        _benchMap.Scene = _map.Scene;
        _benchMap.Fit();
        _runStart = DateTime.UtcNow;
        _campLogItems.Clear();
        _campLive.Text = $"{r.Instance} (n = {r.N}) · {r.Solver} · time box {r.Limit} s";
        if (r.IsApex)
        {
            var s = new RunSettings { Mode = r.Solver == "apex_exact" ? "exact" : "scalable", TimeLimitS = r.Limit, Threads = 5, MemLimitMb = 4096, Seed = 1 };
            LoadSettingsIntoForm(s);
            if (!StartRun(s, RunBase(r))) { _campaignActive = false; Toast("campaign stopped: the engine did not start"); }
        }
        else StartBaseline(r);
    }

    private void Next() => DispatcherTimer.RunOnce(NextRun, TimeSpan.FromMilliseconds(300));

    private void CampaignOnFinished(RunSession s, string json)
    {
        if (!CampaignOwns(s) || _current is not { } r) return;
        JsonObject o;
        try { o = JsonNode.Parse(json) as JsonObject ?? []; }
        catch (JsonException) { o = new JsonObject { ["status"] = "ERROR", ["error"] = "bad report" }; }
        o["exit_code"] = 0;
        if (File.Exists(s.TourPath)) File.Copy(s.TourPath, RunBase(r) + ".tour", true);
        if (File.Exists(s.LogPath)) File.Copy(s.LogPath, RunBase(r) + ".log", true);
        WriteRunJson(r, o);
        Next();
    }

    private void CampaignOnClosed(RunSession s, string reason)
    {
        if (!CampaignOwns(s) || _current is not { } r || _vm.Finished || Done(r)) return;
        if (!_campaignActive) return;
        WriteRunJson(r, new JsonObject { ["status"] = "ERROR", ["error"] = reason });
        Next();
    }

    private void WriteRunJson(CampaignRun r, JsonObject o)
    {
        o["instance"] = r.Instance;
        o["n"] = r.N;
        o["solver"] = r.Solver;
        o["time_limit_s"] = r.Limit;
        if (r.IsApex) { o["threads"] = 5; o["seed"] = 1; }
        var p = RunBase(r) + ".json";
        File.WriteAllText(p + ".tmp", o.ToJsonString());
        File.Move(p + ".tmp", p, true);
        RefreshCampaignTable();
    }

    private void StartBaseline(CampaignRun r)
    {
        var root = RepoRoot()!;
        var script = Path.Combine(root, "engine", "tools", "baselines", "run_baseline.py");
        var b = RunBase(r);
        var psi = new ProcessStartInfo(OperatingSystem.IsWindows() ? "python" : "python3") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { script, r.Solver, InstancePath(r.Instance), "--time-limit", r.Limit.ToString(CultureInfo.InvariantCulture), "--seed", "1",
                                  "--report", b + ".json.tmp", "--tour-out", b + ".tour", "--stream" })
            psi.ArgumentList.Add(a);
        _baseLog = new StreamWriter(b + ".log", false, Encoding.UTF8) { AutoFlush = true };
        var log = _baseLog;
        try { _baseProc = Process.Start(psi) ?? throw new IOException("cannot start python3"); }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception)
        {
            WriteRunJson(r, new JsonObject { ["status"] = "ERROR", ["error"] = e.Message });
            Next();
            return;
        }
        var p = _baseProc;
        p.EnableRaisingEvents = true;
        void Line(string? l)
        {
            if (l is null) return;
            lock (log) log.WriteLine(l);
            Dispatcher.UIThread.Post(() => { _campLogItems.Add(l); if (_campLogItems.Count > 400) _campLogItems.RemoveAt(0); _campLog.ScrollIntoView(_campLogItems.Count - 1); });
        }
        p.OutputDataReceived += (_, e) => Line(e.Data);
        p.ErrorDataReceived += (_, e) => Line(e.Data);
        p.Exited += (_, _) => Dispatcher.UIThread.Post(() => OnBaselineExit(r, p));
        p.BeginOutputReadLine();
        p.BeginErrorReadLine();
    }

    private void OnBaselineExit(CampaignRun r, Process p)
    {
        p.WaitForExit();
        lock (_baseLog!) _baseLog.Dispose();
        _baseProc = null;
        if (!_campaignActive || _current != r) return;  // stopped: the run is redone next time
        var b = RunBase(r);
        JsonObject o;
        try { o = File.Exists(b + ".json.tmp") ? JsonNode.Parse(File.ReadAllText(b + ".json.tmp")) as JsonObject ?? [] : new JsonObject { ["status"] = $"EXIT_{p.ExitCode}" }; }
        catch (JsonException) { o = new JsonObject { ["status"] = "ERROR", ["error"] = "bad baseline report" }; }
        File.Delete(b + ".json.tmp");
        o["exit_code"] = p.ExitCode;
        if (File.Exists(b + ".tour") && _map.Scene is { } sc)
        {
            var t = TourFiles.Read(b + ".tour");
            sc.SetTour(t, t.Length);
            InvalidateMaps();
            _benchMap.Invalidate();
        }
        WriteRunJson(r, o);
        Next();
    }

    private void StopCampaign()
    {
        if (!_campaignActive) return;
        _campaignActive = false;
        if (_current is { IsApex: true }) Send(ApexTsp.Ipc.MsgType.Cancel);
        try { _baseProc?.Kill(entireProcessTree: true); }
        catch (InvalidOperationException) { }
        _campLive.Text = "campaign stopped (the interrupted run is redone on the next start)";
        Toast("campaign stopped");
    }

    // ---- live view + table --------------------------------------------------------------

    private void CampaignTick()
    {
        _benchChart.Pal = _pal;
        _benchChart.Samples = _vm.History.ToArray();
        _benchChart.Events = _vm.Events.ToArray();
        _benchChart.InvalidateVisual();
        _benchMap.Renderer.TourColor = _map.Renderer.TourColor;
        _benchMap.Invalidate();
        ApplyRendererPalette(_benchMap.Renderer);
        _benchMap.Renderer.TourColor = _map.Renderer.TourColor;
        _campStart.IsEnabled = !_campaignActive;
        _campStop.IsEnabled = _campaignActive;
        if (!_campaignActive || _current is not { } r) return;
        var el = (DateTime.UtcNow - _runStart).TotalSeconds;
        var bound = r.IsApex
            ? $" · {_vm.UbBadge.Text} · {_vm.LbBadge.Text} · UB {MainViewModel.Fmt(_vm.UpperBound)} LB {MainViewModel.Fmt(_vm.LowerBound)} gap {MainViewModel.Fmt(_vm.GapPct)}% · phase {_vm.Phase}"
            : "";
        _campLive.Text = string.Create(CultureInfo.InvariantCulture, $"{r.Instance} (n = {r.N}) · {r.Solver} · {el:0} s of {r.Limit} s{bound}");
        if (r.IsApex)
        {
            var tail = _vm.LogTail.TakeLast(60).ToList();
            if (_campLogItems.Count != tail.Count || (tail.Count > 0 && _campLogItems[^1] != tail[^1]))
            {
                _campLogItems.Clear();
                foreach (var l in tail) _campLogItems.Add(l);
                _campLog.ScrollIntoView(_campLogItems.Count - 1);
            }
        }
        var done = _plan.Count(Done);
        var left = _plan.Where(x => !Done(x)).Sum(x => x.Limit) - Math.Min(el, r.Limit);
        _campProgress.Value = _plan.Count == 0 ? 0 : (double)done / _plan.Count;
        _campStatus.Text = string.Create(CultureInfo.InvariantCulture, $"{done} / {_plan.Count} runs done · at most {left / 3600:0.0} h left (time boxes; most runs stop earlier)");
    }

    private readonly TextBlock _campSummary = new() { TextWrapping = TextWrapping.Wrap, FontSize = 12 };

    // Seconds from a published field ("9.74", "428996.2 (3)"); null for "~4 years", "OPEN", empty.
    private static double? Published(string raw) =>
        double.TryParse(raw.Split(' ')[0], NumberStyles.Float, CultureInfo.InvariantCulture, out var v) ? v : null;

    // Table columns: header, width, header tooltip. Groups span columns: (title, first column, count).
    private static readonly (string Head, double W, string Tip)[] CampCols =
    [
        ("instance", 92, ""), ("n", 52, "number of cities"),
        ("optimum", 84, "optimal tour length (TSPLIB / Waterloo); * = best known tour, not proven optimal"),
        ("year", 44, "year the optimum was first proven (Waterloo milestones, National / VLSI logs)"),
        ("time", 64, "solution time of the first optimality proof, as published (hardware in the cell tooltip)"),
        ("1998", 64, "Applegate, Bixby, Chvátal, Cook, Documenta Math. ICM 1998 (400 MHz AlphaServer)"),
        ("1999", 64, "Concorde 1999 benchmark (500 MHz Alpha)"),
        ("2003", 64, "Concorde 2003 benchmark (2.8 GHz Xeon)"),
        ("Concorde", 72, "Concorde + QSopt on this machine (test-environment baseline)"),
        ("APEX", 72, "APEX exact on this machine (5 threads, seed 1)"),
        ("here", 52, "APEX exact time ÷ Concorde on this machine"),
        ("2003", 46, "APEX exact time ÷ Concorde 2003 published"),
        ("1999", 46, "APEX exact time ÷ Concorde 1999 published"),
        ("1998", 46, "APEX exact time ÷ Concorde 1998 paper"),
        ("first", 46, "APEX exact time ÷ first published optimality proof"),
        ("scalable", 66, "APEX scalable tour vs the reference value"),
        ("LKH-3", 66, "LKH-3 tour vs the reference value (test-environment baseline)"),
    ];
    private static readonly (string Title, int From, int Count)[] CampGroups =
        [("", 0, 3), ("first proof", 3, 2), ("Concorde, published", 5, 3), ("this machine (exact)", 8, 2), ("APEX exact time ÷ Concorde", 10, 5), ("tour vs reference", 15, 2)];

    private static Grid CampGrid()
    {
        var g = new Grid();
        foreach (var c in CampCols) g.ColumnDefinitions.Add(new ColumnDefinition(c.W, GridUnitType.Pixel));
        return g;
    }

    private static TextBlock CampCell(Grid g, int col, string text, IBrush fg, string tip = "", bool bold = false, int span = 1)
    {
        var t = new TextBlock
        {
            Text = text, Foreground = fg, FontSize = 12, Margin = new Thickness(6, 3), TextTrimming = TextTrimming.CharacterEllipsis,
            HorizontalAlignment = col == 0 || span > 1 ? HorizontalAlignment.Left : HorizontalAlignment.Right,
            FontWeight = bold ? FontWeight.SemiBold : FontWeight.Normal,
        };
        if (tip.Length > 0) ToolTip.SetTip(t, tip);
        Grid.SetColumn(t, col);
        Grid.SetColumnSpan(t, span);
        g.Children.Add(t);
        return t;
    }

    private void RefreshCampaignTable()
    {
        var dir = CampaignDir;
        var refs = ReferenceTable.Load();
        IBrush text = _pal.Brush(_pal.Text), muted = _pal.Brush(_pal.Muted), good = _pal.Brush(_pal.Proven), bad = _pal.Brush(_pal.Danger);
        _campHead.Children.Clear();
        _campHead.RowDefinitions = new RowDefinitions("Auto,Auto");
        var groups = CampGrid();
        foreach (var (title, from, count) in CampGroups) CampCell(groups, from, title, muted, span: count).FontSize = 11;
        var heads = CampGrid();
        for (var k = 0; k < CampCols.Length; ++k) CampCell(heads, k, CampCols[k].Head, text, CampCols[k].Tip, bold: true);
        Grid.SetRow(heads, 1);
        _campHead.Children.AddRange([groups, new Border { Child = heads, BorderBrush = _pal.Brush(_pal.Border), BorderThickness = new Thickness(0, 0, 0, 1) }]);
        Grid.SetRow(_campHead.Children[1], 1);
        _campRows.Children.Clear();
        JsonElement? Read(string inst, string solver)
        {
            var p = Path.Combine(dir, $"{inst}__{solver}.json");
            if (!File.Exists(p)) return null;
            try { return JsonDocument.Parse(File.ReadAllText(p)).RootElement.Clone(); }
            catch (Exception e) when (e is IOException or JsonException) { return null; }
        }
        static double Num(JsonElement? e, string k) => e is { } v && v.TryGetProperty(k, out var x) && x.ValueKind == JsonValueKind.Number ? x.GetDouble() : double.NaN;
        static string Str(JsonElement? e, string k) => e is { } v && v.TryGetProperty(k, out var x) && x.ValueKind == JsonValueKind.String ? x.GetString() ?? "" : "";
        static bool Proven(JsonElement? e) => Str(e, "status") == "OPTIMAL_PROVEN";
        static string T(double? s) => s is { } v ? CampaignPlan.Dur(v) : "-";
        (string, IBrush) Time(JsonElement? e) => e is null ? ("-", muted) : Proven(e) ? (T(Num(e, "wall_s")), text) : Str(e, "status") == "TIME_LIMIT" ? ("limit", bad) : (Str(e, "status").ToLowerInvariant(), bad);
        (string, IBrush) R(double? a, double? b) => a is { } x && b is { } y && y > 0 && !double.IsNaN(x)
            ? ((x / y).ToString(x / y >= 10 ? "0" : "0.0#", CultureInfo.InvariantCulture) + "x", x / y <= 1 ? good : bad) : ("-", muted);
        (string, IBrush) VsRef(JsonElement? e, long refv)
        {
            if (e is null) return ("-", muted);
            var ub = Num(e, "ub");
            if (double.IsNaN(ub) || refv <= 0) return (Str(e, "status").ToLowerInvariant(), muted);
            var g = 100 * (ub - refv) / refv;
            return (g.ToString("+0.000;-0.000;0.000", CultureInfo.InvariantCulture) + "%", Math.Abs(g) < 5e-4 ? good : text);
        }
        var inst = CampaignInstances();
        var total = 0;
        var rHere = new List<double>();
        var r03 = new List<double>();
        var r99 = new List<double>();
        var c03 = new List<double>();
        var r98 = new List<double>();
        var rFirst = new List<double>();
        foreach (var (name, n) in inst)
        {
            var refv = refs.TryGetValue(name, out var rr) ? rr.Value : 0;
            JsonElement? ex = Read(name, "apex_exact"), sc = Read(name, "apex_scalable"), co = Read(name, "concorde"), lk = Read(name, "lkh");
            total += new[] { ex, sc, co, lk }.Count(x => x is not null);
            double? apex = Proven(ex) ? Num(ex, "wall_s") : null, conc = Proven(co) ? Num(co, "wall_s") : null;
            double? p99 = rr is null ? null : Published(rr.Concorde99), p03 = rr is null ? null : Published(rr.Concorde03);
            double? p98 = rr is null ? null : Published(rr.Abcc98), pFirst = rr is null ? null : Published(rr.FirstSolve);
            if (apex is { } a8 && p98 is { } q8 && q8 > 0) r98.Add(Math.Max(a8, 1e-3) / q8);
            if (apex is { } af && pFirst is { } qf && qf > 0) rFirst.Add(Math.Max(af, 1e-3) / qf);
            if (apex is { } a && conc is { } c && c > 0) rHere.Add(a / Math.Max(c, 1e-3));
            if (apex is { } a3 && p03 is { } q3 && q3 > 0) r03.Add(Math.Max(a3, 1e-3) / q3);
            if (apex is { } a9 && p99 is { } q9 && q9 > 0) r99.Add(Math.Max(a9, 1e-3) / q9);
            if (conc is { } cc && p03 is { } q && q > 0) c03.Add(Math.Max(cc, 1e-3) / q);
            var row = CampGrid();
            CampCell(row, 0, name, text, rr?.Note ?? "", bold: true);
            CampCell(row, 1, n.ToString(CultureInfo.InvariantCulture), muted);
            if (rr is null) CampCell(row, 2, "-", muted);
            else if (rr.Kind == "optimal") CampCell(row, 2, rr.Value.ToString(CultureInfo.InvariantCulture), text, "proven optimum");
            else CampCell(row, 2, rr.Value.ToString(CultureInfo.InvariantCulture) + "*", muted, "best known tour, not proven optimal" + (rr.Note.Length > 0 ? ": " + rr.Note : ""));
            CampCell(row, 3, rr?.Year is { Length: > 0 } y ? y : "-", rr?.Year is { Length: > 0 } ? text : muted, rr?.SolvedBy ?? "");
            CampCell(row, 4, T(pFirst), pFirst is null ? muted : text, rr?.FirstSolveStated is { Length: > 0 } fs ? $"{fs} ({rr.FirstSolveHw})" : "");
            CampCell(row, 5, T(p98), p98 is null ? muted : text, rr?.Abcc98Stated ?? "");
            CampCell(row, 6, rr?.Concorde99 is { Length: > 0 } c99 && p99 is null ? c99 : T(p99), p99 is null ? muted : text, rr?.Concorde99 ?? "");
            CampCell(row, 7, T(p03), p03 is null ? muted : text);
            var cells = new[] { Time(co), Time(ex), R(apex, conc), R(apex, p03), R(apex, p99), R(apex, p98), R(apex, pFirst), VsRef(sc, refv), VsRef(lk, refv) };
            for (var k = 0; k < cells.Length; ++k) CampCell(row, 8 + k, cells[k].Item1, cells[k].Item2);
            var running = _campaignActive && _current is { } cur && cur.Instance == name;
            var bg = running ? Color.FromArgb(0x40, _pal.Accent.R, _pal.Accent.G, _pal.Accent.B) : _campRows.Children.Count % 2 == 1 ? _pal.Background : Colors.Transparent;
            _campRows.Children.Add(new Border { Child = row, Background = new SolidColorBrush(bg), CornerRadius = new CornerRadius(4) });
        }
        static double? G(List<double> v) => v.Count == 0 ? null : Math.Exp(v.Average(Math.Log));
        _campKpis.Children.Clear();
        foreach (var (label, v, tip) in new[]
        {
            ("Concorde here", rHere, "APEX exact ÷ Concorde + QSopt on this machine"),
            ("Concorde 2003", r03, "APEX exact ÷ Concorde 2003 published (2.8 GHz Xeon)"),
            ("Concorde 1999", r99, "APEX exact ÷ Concorde 1999 published (500 MHz Alpha)"),
            ("Concorde 1998", r98, "APEX exact ÷ Concorde 1998 paper (400 MHz AlphaServer)"),
            ("first proof", rFirst, "APEX exact ÷ the first published optimality proof"),
            ("Concorde here ÷ 2003", c03, "not APEX: Concorde on this machine ÷ Concorde 2003 published (hardware + version)"),
        })
        {
            var g = G(v);
            var tile = new Border
            {
                Background = _pal.Brush(_pal.Background), CornerRadius = new CornerRadius(6), Padding = new Thickness(10, 6), Margin = new Thickness(0, 0, 8, 6), MinWidth = 120,
                Child = new StackPanel
                {
                    Children =
                    {
                        new TextBlock { Text = label, FontSize = 11, Foreground = muted },
                        new TextBlock { Text = g is { } x ? x.ToString("0.##", CultureInfo.InvariantCulture) + "x" : "-", FontSize = 20, FontWeight = FontWeight.SemiBold,
                                        Foreground = g is not { } y ? muted : y <= 1 ? good : bad },
                        new TextBlock { Text = v.Count == 1 ? "1 instance" : $"{v.Count} instances", FontSize = 11, Foreground = muted },
                    },
                },
            };
            ToolTip.SetTip(tile, tip);
            _campKpis.Children.Add(tile);
        }
        _campSummary.Text =
            "Times: proven optimum only (\"limit\" = time box reached); s, h, d, y = seconds, hours, days, years. Ratio = APEX exact time ÷ the other time: above 1x APEX is slower (red), below 1x faster (green). " +
            "Hover a header or a cell for the source and hardware.\n" +
            "First proof: date and solution time of the first optimality proof — National and VLSI instances from their Waterloo 'Log of Computation' pages (Concorde, 2001 Athlon 1.33 GHz / 2002 Athlon 1900+); " +
            "TSPLIB: usa13509, d15112, pla85900 (published CPU years). 1998 Alpha: the 1977-1992 TSPLIB records re-solved by Applegate, Bixby, Chvátal and Cook (Documenta Math. ICM 1998, 400 MHz AlphaServer); " +
            "the original 1977-1991 TSPLIB run times are only in the original papers. Hardware differs per row: ratios compare this machine with the published one.";
        if (!_campaignActive)
            _campStatus.Text = $"{inst.Count} instances (n {(int)(_campMinN.Value ?? 0)} to {(int)(_campMaxN.Value ?? 0)}), {total} results in {dir}";
    }

    private void GenerateReport()
    {
        var root = RepoRoot();
        if (root is null) { Toast("repository not found"); return; }
        var psi = new ProcessStartInfo(OperatingSystem.IsWindows() ? "python" : "python3") { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in new[] { Path.Combine(root, "engine", "tools", "history", "make_history_report.py"), CampaignDir }) psi.ArgumentList.Add(a);
        try
        {
            using var p = Process.Start(psi)!;
            var output = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
            p.WaitForExit();
            Toast(p.ExitCode == 0 ? "report written: " + output.Trim() : "report failed: " + output.Trim());
        }
        catch (Exception e) when (e is IOException or System.ComponentModel.Win32Exception) { Toast(e.Message); }
    }
}
