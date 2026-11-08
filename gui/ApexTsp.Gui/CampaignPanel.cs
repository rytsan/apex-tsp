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
    private readonly NumericUpDown _campMaxN = new() { Minimum = 50, Maximum = 200000, Value = 1002, Increment = 500, FormatString = "0", Width = 130 };
    private readonly Dictionary<string, CheckBox> _campSolver = [];
    private readonly Button _campStart = new() { Content = "▶ Start campaign" }, _campStop = new() { Content = "Stop" };
    private readonly TextBlock _campStatus = new() { TextWrapping = TextWrapping.Wrap };
    private readonly TextBlock _campLive = new() { FontWeight = FontWeight.SemiBold, TextWrapping = TextWrapping.Wrap };
    private readonly ProgressBar _campProgress = new() { Minimum = 0, Maximum = 1, Height = 6 };
    private readonly ListBox _campTable = new() { FontFamily = new FontFamily("monospace"), FontSize = 11 };
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
        _benchMap.Scene = null;

        var dirRow = new DockPanel();
        DockPanel.SetDock(browse, Dock.Right);
        dirRow.Children.AddRange([browse, _campDir]);
        var controls = new StackPanel
        {
            Spacing = 8,
            Children =
            {
                dirRow,
                new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Children = { new TextBlock { Text = "instances up to n =", VerticalAlignment = VerticalAlignment.Center }, _campMaxN } },
                solverRow,
                new WrapPanel { Children = { _campStart, new Border { Width = 6 }, _campStop, new Border { Width = 12 }, download, new Border { Width = 6 }, report, new Border { Width = 6 }, solve } },
                _campProgress, _campStatus,
            },
        };
        var live = new DockPanel();
        var liveTop = new StackPanel { Spacing = 4, Children = { _campLive } };
        DockPanel.SetDock(liveTop, Dock.Top);
        var liveGrid = new Grid { RowDefinitions = new RowDefinitions("*,160,140"), RowSpacing = 8 };
        liveGrid.Children.Add(new Border { Child = _benchMap, ClipToBounds = true, CornerRadius = new CornerRadius(6) });
        Grid.SetRow(_benchChart, 1);
        liveGrid.Children.Add(_benchChart);
        Grid.SetRow(_campLog, 2);
        liveGrid.Children.Add(_campLog);
        live.Children.AddRange([liveTop, liveGrid]);

        var left = new Grid { RowDefinitions = new RowDefinitions("Auto,*"), RowSpacing = 10, Margin = new Thickness(12, 0, 6, 12) };
        left.Children.Add(Card("Campaign (historical benchmark)", controls));
        var tableBody = new DockPanel();
        DockPanel.SetDock(_campSummary, Dock.Bottom);
        _campSummary.Margin = new Thickness(0, 8, 0, 0);
        tableBody.Children.AddRange([_campSummary, _campTable]);
        var table = Card("Comparison (live): time vs published Concorde and vs Concorde here", tableBody, fill: true);
        Grid.SetRow(table, 1);
        left.Children.Add(table);
        var right = Card("Running now", live, fill: true);
        right.Margin = new Thickness(6, 0, 12, 12);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("3*,2*") };
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
        var max = (int)(_campMaxN.Value ?? 1002);
        var path = ReferenceTable.Locate();
        if (path is null) return [];
        var lines = File.ReadAllLines(path);
        var head = ReferenceTable.SplitCsv(lines[0]);
        int c(string n) => Array.IndexOf(head, n);
        return [.. lines.Skip(1).Select(ReferenceTable.SplitCsv)
            .Where(f => f[c("in_set")] == "yes" && int.Parse(f[c("n")], CultureInfo.InvariantCulture) <= max)
            .Select(f => (f[c("instance")], int.Parse(f[c("n")], CultureInfo.InvariantCulture)))];
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

    private void RefreshCampaignTable()
    {
        var dir = CampaignDir;
        var refs = ReferenceTable.Load();
        var lines = new List<string>
        {
            "                         first solved          |   published Concorde time, s         |     this machine, s     |  APEX exact time ÷                        | gap vs reference",
            "instance          n      year  CPU time       |  1998 Alpha  1999 Alpha  2003 Xeon   | Concorde   APEX exact   | Conc.here  2003   1999   1998   first    | APEX scal.  LKH-3",
        };
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
        static string T(double? s) => s is not { } v || double.IsNaN(v) ? "-" : v < 100 ? v.ToString("0.00", CultureInfo.InvariantCulture) : v.ToString("0", CultureInfo.InvariantCulture);
        static string Time(JsonElement? e) => e is null ? "-" : Proven(e) ? T(Num(e, "wall_s")) : Str(e, "status") == "TIME_LIMIT" ? "limit" : Str(e, "status").ToLowerInvariant();
        static string R(double? a, double? b) => a is { } x && b is { } y && y > 0 && !double.IsNaN(x) ? (x / y).ToString(x / y >= 10 ? "0" : "0.0#", CultureInfo.InvariantCulture) + "x" : "-";
        string VsRef(JsonElement? e, long refv) => e is null ? "-" : double.IsNaN(Num(e, "ub")) ? Str(e, "status").ToLowerInvariant() : (100 * (Num(e, "ub") - refv) / refv).ToString("+0.000;-0.000", CultureInfo.InvariantCulture) + "%";
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
            var running = _campaignActive && _current is { } cur && cur.Instance == name ? " ◀" : "";
            lines.Add(string.Create(CultureInfo.InvariantCulture,
                $"{name,-14}{n,6}  {(rr?.Year is { Length: > 0 } y ? y : "-"),6}  {(rr?.FirstSolveStated is { Length: > 0 } fs ? fs.Replace(" seconds", " s").Replace(" years", " y") : "-"),-13} |  {(rr?.Abcc98Stated is { Length: > 0 } a98 ? a98.Replace(" seconds", "") : "-"),10}  {T(p99),10}  {T(p03),9}   | {Time(co),8}  {Time(ex),10}   | {R(apex, conc),8} {R(apex, p03),6} {R(apex, p99),6} {R(apex, p98),6} {R(apex, pFirst),8}  | {VsRef(sc, refv),9}  {VsRef(lk, refv),9}{running}"));
        }
        _campTable.ItemsSource = lines;
        static string G(List<double> v) => v.Count == 0 ? "-" : Math.Exp(v.Average(Math.Log)).ToString("0.##", CultureInfo.InvariantCulture) + "x";
        _campSummary.Text =
            "Times in seconds, proven optimum only (\"limit\" = time box reached). Ratio = APEX exact time ÷ the other time: above 1x APEX is slower, below 1x faster.\n" +
            $"Geometric mean — APEX ÷ Concorde on this machine: {G(rHere)} ({rHere.Count} instances) · APEX ÷ Concorde 2003 published: {G(r03)} ({r03.Count}) · " +
            $"APEX ÷ Concorde 1999 published: {G(r99)} ({r99.Count}) · APEX ÷ Concorde 1998 paper: {G(r98)} ({r98.Count}) · APEX ÷ first solution: {G(rFirst)} ({rFirst.Count}) · " +
            $"Concorde here ÷ Concorde 2003 (hardware + version): {G(c03)} ({c03.Count}).\n" +
            "First solved: date and solution time of the first optimality proof — National and VLSI instances from their Waterloo 'Log of Computation' pages (Concorde, 2001 Athlon 1.33 GHz / 2002 Athlon 1900+); " +
            "TSPLIB: usa13509, d15112, pla85900 (published CPU years). 1998 Alpha: the 1977-1992 TSPLIB records re-solved by Applegate, Bixby, Chvátal and Cook (Documenta Math. ICM 1998, 400 MHz AlphaServer); " +
            "the original 1977-1991 TSPLIB run times are only in the original papers. Hardware differs per row: ratios compare this machine with the published one.";
        if (!_campaignActive)
            _campStatus.Text = $"{inst.Count} instances (n <= {(int)(_campMaxN.Value ?? 0)}), {total} results in {dir}";
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
