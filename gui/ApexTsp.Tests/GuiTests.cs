using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text.RegularExpressions;
using ApexTsp.Gui;
using ApexTsp.Ipc;
using ApexTsp.Render;
using SkiaSharp;
using Xunit;
using Xunit.Abstractions;

namespace ApexTsp.Tests;

internal static class Repo
{
    public static string Root
    {
        get
        {
            var d = new DirectoryInfo(AppContext.BaseDirectory);
            while (d is not null && !File.Exists(Path.Combine(d.FullName, "engine", "src", "ipc", "protocol.hpp"))) d = d.Parent;
            return d?.FullName ?? throw new InvalidOperationException("repository root not found");
        }
    }

    public static string EngineBinary()
    {
        var env = Environment.GetEnvironmentVariable("APEX_BIN");
        if (!string.IsNullOrEmpty(env)) return env;
        var exe = OperatingSystem.IsWindows() ? "apex_tsp.exe" : "apex_tsp";
        foreach (var preset in new[] { "dev-gcc", "dev-msvc", "benchmark", "dev-clang" })
        {
            var p = Path.Combine(Root, "build", preset, exe);
            if (File.Exists(p)) return p;
        }
        throw new FileNotFoundException("apex_tsp not built (set APEX_BIN)");
    }
}

public class ProtocolTests
{
    // Sizes come from the engine header itself, so the check follows any engine change.
    [Fact]
    public void T_gui_struct_sizes_match_engine()
    {
        var hpp = File.ReadAllText(Path.Combine(Repo.Root, "engine", "src", "ipc", "protocol.hpp"));
        int Grab(string pattern) => int.Parse(Regex.Match(hpp, pattern).Groups[1].Value);
        Assert.Equal(Grab(@"sizeof\(TelemetryPacket\) == (\d+)"), Marshal.SizeOf<TelemetryPacket>());
        Assert.Equal(Grab(@"sizeof\(SharedTourHeader\) == (\d+)"), Marshal.SizeOf<SharedTourHeader>());
        Assert.Equal(Grab(@"sizeof\(CutRoundPacket\) == (\d+)"), Marshal.SizeOf<CutRoundPacket>());
        Assert.Equal(Grab(@"sizeof\(TreeNodePacket\) == (\d+)"), Marshal.SizeOf<TreeNodePacket>());
        Assert.Equal(Grab(@"sizeof\(EventHeader\) == (\d+)"), Marshal.SizeOf<EventHeader>());
        Assert.Equal(12, Grab(@"sizeof\(LpEdgeWire\) == (\d+)"));
        Assert.Equal(Protocol.HelloBytes, Grab(@"kHelloBytes = (\d+)"));
        Assert.Equal(Protocol.HeaderBytes, Grab(@"kHeaderBytes = (\d+)"));
        Assert.Equal(Protocol.Version, Grab(@"kProtocolVersion = (\d+)"));
        Assert.Equal(Protocol.VersionMin, Grab(@"kProtocolMin = (\d+)"));
        foreach (Match m in Regex.Matches(hpp, @"^\s+(\w+) = 0x([0-9A-Fa-f]{2}),", RegexOptions.Multiline))
            Assert.Equal(Convert.ToByte(m.Groups[2].Value, 16), (byte)Enum.Parse<MsgType>(m.Groups[1].Value));
    }

    [Fact]
    public void T_crc32_known_vectors_and_framing()
    {
        Assert.Equal(0xCBF43926u, Crc32.Compute("123456789"u8));
        Assert.Equal(0x414FA339u, Crc32.Compute("The quick brown fox jumps over the lazy dog"u8));
        var d = new FrameDecoder();
        var rng = new Random(2026);
        var junk = new byte[1 << 20];
        rng.NextBytes(junk);
        d.Feed(junk);
        while (d.Next() is not null) { }
        var ok = Protocol.Encode(MsgType.Ack, Protocol.PackU64(77), withCrc: true);
        var bad = (byte[])ok.Clone();
        bad[9] ^= 1;
        d.Feed(bad);
        Frame? got = null;
        for (var rep = 0; rep < 8 && got is null; ++rep)
        {
            d.Feed(ok);
            while (d.Next() is { } f) if (f.Type == MsgType.Ack) got = f;
        }
        Assert.NotNull(got);
        Assert.Equal(77UL, BitConverter.ToUInt64(got!.Payload));
        Assert.True(d.CrcFailures >= 1);
    }

    // Cross-language: this client against the real engine; FINISHED equals the --report bytes.
    [Fact]
    public async Task T_client_against_engine()
    {
        var dir = Directory.CreateTempSubdirectory("apex_gui_client_");
        var endpoint = OperatingSystem.IsWindows() ? "apex_gui_client_" + Environment.ProcessId : Path.Combine(dir.FullName, "e.sock");
        var shared = Path.Combine(dir.FullName, "tour.shm");
        var report = Path.Combine(dir.FullName, "r.json");
        var psi = new ProcessStartInfo(Repo.EngineBinary()) { UseShellExecute = false, RedirectStandardOutput = true };
        foreach (var a in new[] { "--instance", Path.Combine(Repo.Root, "data", "a280.tsp"), "--mode", "exact", "--ipc", endpoint, "--shared-tour", shared, "--report", report })
            psi.ArgumentList.Add(a);
        using var engine = Process.Start(psi)!;
        engine.OutputDataReceived += (_, _) => { };
        engine.BeginOutputReadLine();
        using var c = EngineClient.Connect(endpoint, TimeSpan.FromSeconds(20));
        Assert.Equal(0, c.EngineHello.Mode);
        var finished = new TaskCompletionSource<string>();
        var telemetry = 0;
        c.Telemetry += _ => Interlocked.Increment(ref telemetry);
        c.Finished += j => finished.TrySetResult(j);
        c.Start();
        var json = await finished.Task.WaitAsync(TimeSpan.FromSeconds(60));
        await engine.WaitForExitAsync(new CancellationTokenSource(10000).Token);
        Assert.Equal(File.ReadAllText(report), json);
        Assert.Contains("\"status\":\"OPTIMAL_PROVEN\"", json);
        Assert.True(telemetry >= 1);
        using var reader = new SharedTourReader(shared);
        var buf = Array.Empty<uint>();
        Assert.True(reader.TryRead(ref buf, out var n, out var epoch));
        Assert.Equal(280, n);
        Assert.Equal(280, buf.Take(n).Distinct().Count());
        Assert.True(epoch % 2 == 0 && epoch > 0);
        dir.Delete(true);
    }

    // v3 solver view against the real engine: tree, cuts, LP support and provenance events.
    [Fact]
    public async Task T_client_v3_solver_events()
    {
        var dir = Directory.CreateTempSubdirectory("apex_gui_v3_");
        var endpoint = OperatingSystem.IsWindows() ? "apex_gui_v3_" + Environment.ProcessId : Path.Combine(dir.FullName, "e.sock");
        var psi = new ProcessStartInfo(Repo.EngineBinary()) { UseShellExecute = false, RedirectStandardOutput = true };
        foreach (var a in new[] { "--instance", Path.Combine(Repo.Root, "data", "a280.tsp"), "--mode", "exact", "--ipc", endpoint })
            psi.ArgumentList.Add(a);
        using var engine = Process.Start(psi)!;
        engine.OutputDataReceived += (_, _) => { };
        engine.BeginOutputReadLine();
        using var c = EngineClient.Connect(endpoint, TimeSpan.FromSeconds(20));
        Assert.Equal(3, c.NegotiatedVersion);
        var nodes = new List<TreeNodePacket>();
        var cuts = new List<CutRoundPacket>();
        var events = new List<SolverEvent>();
        var lpViews = 0;
        var lpOk = true;
        c.TreeNode += n => { lock (nodes) nodes.Add(n); };
        c.CutRound += r => { lock (cuts) cuts.Add(r); };
        c.SolverEvent += e => { lock (events) events.Add(e); };
        c.LpSolution += (_, e) =>
        {
            Interlocked.Increment(ref lpViews);
            if (e.Length == 0 || e.Any(x => x.U >= 280 || x.V >= 280 || x.X <= 0 || x.X > 1.0001f)) lpOk = false;
        };
        var finished = new TaskCompletionSource<string>();
        c.Finished += j => finished.TrySetResult(j);
        c.Start();
        await finished.Task.WaitAsync(TimeSpan.FromSeconds(60));
        await engine.WaitForExitAsync(new CancellationTokenSource(10000).Token);
        Assert.Contains(nodes, n => n.Id == 0 && n.Parent == uint.MaxValue);
        Assert.NotEmpty(cuts);
        Assert.Contains(cuts, r => r.Sec + r.Blossom + r.Component > 0);
        Assert.True(lpViews >= 1 && lpOk);
        Assert.Contains(events, e => e.Kind == EventKind.UbImproved && e.Heuristic);
        Assert.Contains(events, e => e.Kind == EventKind.LbImproved && e.Detail == "lp");
        Assert.Contains(events, e => e.Kind == EventKind.Proven && e.Value == 2579);
        Assert.Contains(events, e => e.Kind == EventKind.Phase && e.Detail == "bnc");
        dir.Delete(true);
    }
}

// Frame times are measured: run alone, not beside the engine test.
[CollectionDefinition("perf", DisableParallelization = true)]
public class PerfCollection;

[Collection("perf")]
public class RenderTests(ITestOutputHelper output)
{
    private static Scene Synthetic(int n, int seed)
    {
        var rng = new Random(seed);
        var x = new float[n];
        var y = new float[n];
        for (var i = 0; i < n; ++i)  // clustered, like real instances
        {
            var c = i % 40;
            x[i] = (float)(c % 8 * 1000 + rng.NextDouble() * 600);
            y[i] = (float)(c / 8 * 1000 + rng.NextDouble() * 600);
        }
        var s = new Scene(x, y);
        // Short-edge tour like a real one: clusters in order, inside each a serpentine over 6-unit strips.
        var tour = Enumerable.Range(0, n).Select(i => (uint)i)
            .OrderBy(i => i % 40)
            .ThenBy(i => (int)(x[i] % 1000 / 6))
            .ThenBy(i => (int)(x[i] % 1000 / 6) % 2 == 0 ? y[i] : -y[i]).ToArray();
        s.SetTour(tour, n);
        s.SetCluster(0, 0, 0, 2000, 2000);
        s.SetClusterState(0, 1);
        return s;
    }

    [Fact]
    public void T_lod_path_executes()
    {
        var s = Synthetic(100_000, 5);
        var r = new TourRenderer();
        RenderStats Draw(int w, int h, double zoom)
        {
            using var surface = SKSurface.Create(new SKImageInfo(w, h, SKColorType.Bgra8888, SKAlphaType.Premul));
            var v = Viewport.Fit(s, w, h);
            v.Zoom(zoom, w / 2.0, h / 2.0);
            return r.Render(surface.Canvas, s, v);
        }
        var full = Draw(1280, 800, 1);
        var close = Draw(1280, 800, 12);
        var small = Draw(320, 200, 1);
        var tiny = Draw(96, 64, 1);
        output.WriteLine($"full {full}\nclose {close}\nsmall {small}\ntiny {tiny}");
        Assert.Equal(100_000, full.Visible);
        Assert.Equal(LodLevel.Decimated, full.Level);   // > 50k on screen: aggregated
        Assert.True(full.Segments <= r.RawLimit && full.Points <= 1280 * 800);
        Assert.Equal(LodLevel.Raw, close.Level);         // zoomed in: detail back
        Assert.True(close.Visible <= r.RawLimit);
        Assert.Equal(LodLevel.Clusters, small.Level);
        Assert.Equal(LodLevel.Heatmap, tiny.Level);

        // Frame-time distribution over a pan/zoom sequence (headless, 1280x800, 100k nodes).
        using var surf = SKSurface.Create(new SKImageInfo(1280, 800, SKColorType.Bgra8888, SKAlphaType.Premul));
        var view = Viewport.Fit(s, 1280, 800);
        var ms = new List<double>();
        var levels = new Dictionary<LodLevel, int>();
        for (var i = 0; i < 400; ++i)
        {
            if (i % 200 < 80) view.Zoom(1.04, 640, 400);
            else if (i % 200 < 120) view.Pan(9, 5);
            else view.Zoom(1 / 1.04, 640, 400);
            var st = r.Render(surf.Canvas, s, view);
            if (i >= 10) ms.Add(st.Ms);  // first frames: JIT and buffer allocation
            levels[st.Level] = levels.GetValueOrDefault(st.Level) + 1;
        }
        output.WriteLine("render ms: " + FrameStats.Describe(ms));
        output.WriteLine("levels: " + string.Join(", ", levels.OrderBy(k => k.Key).Select(k => $"{k.Key}={k.Value}")));
        Assert.True(levels.ContainsKey(LodLevel.Raw) && levels.ContainsKey(LodLevel.Decimated));
        Assert.True(FrameStats.Percentile(ms, 0.95) <= 16, "p95 render time above 16 ms");

        // LP overlay at the largest exact-mode scale (usa13509 / d18512 class: 20000 nodes): tour
        // edges at x = 1, a short fractional edge per 10 nodes, then a second LP with 10% new edges
        // (flashed), every layer on. LP edges are candidate-graph edges, so short.
        const int m = 20_000;
        var ls = Synthetic(m, 9);
        var t = ls.Tour;
        var pos = new int[m];
        for (var i = 0; i < m; ++i) pos[t[i]] = i;
        var lp = new List<LpSegment>();
        for (var i = 0; i < m; ++i) lp.Add(new LpSegment(t[i], t[(i + 1) % m], i % 10 == 0 ? 0.5f : 1f));
        for (var i = 0; i + 7 < m; i += 10) lp.Add(new LpSegment(t[i], t[i + 7], 0.5f));
        ls.SetLp([.. lp]);
        ls.SetLp([.. lp.Select((e, i) => i % 10 == 0 ? new LpSegment(e.U, t[(pos[e.U] + 2) % m], e.X) : e)]);
        ls.LpFlash = true;
        Assert.InRange(ls.LpNew.Count(x => x), lp.Count / 12, lp.Count / 10 + 1);
        view = Viewport.Fit(ls, 1280, 800);
        ms.Clear();
        for (var i = 0; i < 400; ++i)
        {
            if (i % 200 < 80) view.Zoom(1.04, 640, 400);
            else if (i % 200 < 120) view.Pan(9, 5);
            else view.Zoom(1 / 1.04, 640, 400);
            var st = r.Render(surf.Canvas, ls, view);
            if (i >= 10) ms.Add(st.Ms);
        }
        output.WriteLine($"render ms, {m} nodes with LP overlay ({lp.Count} edges): " + FrameStats.Describe(ms));
        Assert.True(FrameStats.Percentile(ms, 0.95) <= 16, "p95 render time with the LP overlay above 16 ms");
    }
}

public class ViewModelTests
{
    [Fact]
    public void T_vm_solver_view_provenance()
    {
        var vm = new MainViewModel();
        Assert.Equal("NO TOUR YET", vm.UbBadge.Text);
        vm.OnTelemetry(Packet(1, 2700, double.NaN, 2));
        vm.OnSolverEvent(new SolverEvent(EventKind.UbImproved, 0.1, 2700, true, "construct"));
        Assert.Equal(("UB · HEURISTIC (construct)", "heuristic"), vm.UbBadge);
        Assert.Equal("NO BOUND YET", vm.LbBadge.Text);
        vm.OnTelemetry(Packet(2, 2600, 2500, 6));
        vm.OnSolverEvent(new SolverEvent(EventKind.LbImproved, 0.2, 2500, false, "lp"));
        Assert.Equal(("LB · LP BOUND", "lp"), vm.LbBadge);
        vm.OnCutRound(new CutRoundPacket { Round = 1, Sec = 5, Blossom = 2, LpValue = 2500 });
        Assert.Equal("separating", vm.CutState);
        vm.OnSolverEvent(new SolverEvent(EventKind.NoViolatedCuts, 0.3, 2510, false, ""));
        Assert.Equal("no violated cuts", vm.CutState);
        vm.OnCutRound(new CutRoundPacket { Round = 2, Component = 1 });
        Assert.Equal(1UL, vm.TotalComponent);
        Assert.Equal(5UL, vm.TotalSec);
        vm.OnTreeNode(new TreeNodePacket { Id = 0, Parent = uint.MaxValue, State = (byte)TreeNodeState.Branched });
        vm.OnTreeNode(new TreeNodePacket { Id = 1, Parent = 0, Depth = 1, State = (byte)TreeNodeState.Open });
        vm.OnTreeNode(new TreeNodePacket { Id = 2, Parent = 0, Depth = 1, State = (byte)TreeNodeState.Open });
        vm.OnTreeNode(new TreeNodePacket { Id = 1, Parent = 0, Depth = 1, State = (byte)TreeNodeState.Pruned });  // update in place
        Assert.Equal(3, vm.Tree.Count);
        Assert.Equal(1, vm.TreeOpen);
        vm.OnSolverEvent(new SolverEvent(EventKind.UbImproved, 0.5, 2579, false, "lp_integral"));
        Assert.Equal(("UB · LP (lp_integral)", "lp"), vm.UbBadge);
        vm.OnSolverEvent(new SolverEvent(EventKind.Proven, 0.6, 2579, false, ""));
        Assert.Equal(("OPTIMAL PROVEN", "proven"), vm.LbBadge);
        vm.OnTelemetry(Packet(3, 2579, 141_000_000, 4));
        Assert.Equal("lagrangian", vm.LbSource);   // packet field (scalable runs) ...
        Assert.Equal("OPTIMAL PROVEN", vm.LbBadge.Text);  // ... never hides a proof
        Assert.Equal(LogCategory.Cuts, LogLine.Classify("cut_round round=3 lp=1"));
        Assert.Equal(LogCategory.Heuristic, LogLine.Classify("polish heuristic=true ub=5"));
        Assert.Equal(LogCategory.Lp, LogLine.Classify("pricing round=1 global_lb=3"));
        Assert.Equal(LogCategory.Tree, LogLine.Classify("tree_progress nodes=1000"));
        Assert.Equal(LogCategory.System, LogLine.Classify("[app] run started"));
        vm.Reset();
        Assert.Empty(vm.Tree);
        Assert.Empty(vm.Events);
        Assert.Equal("-", vm.CutState);
    }

    private static TelemetryPacket Packet(ulong seq, double ub, double lb, uint phase = 5) => new()
    {
        MagicVersion = Protocol.TelemetryMagic,
        Seq = seq,
        TimestampUs = seq * 1_000_000,
        Mode = 1,
        LbSource = 1,
        Phase = phase,
        UpperBound = ub,
        LowerBound = lb,
        GapPct = double.IsNaN(lb) ? double.NaN : 100 * (ub - lb) / ub,
        RamBytes = 80UL << 20,
    };

    [Fact]
    public void T_vm_gap_chart_updates()
    {
        var vm = new MainViewModel { RamCeilingBytes = 4096UL << 20 };
        var history = 0;
        vm.HistoryChanged += () => ++history;
        vm.OnTelemetry(Packet(1, 150_000_000, double.NaN, 2));
        Assert.Equal("-", vm.LbSource);  // no LB yet: no badge source
        for (ulong s = 2; s <= 1000; ++s) vm.OnTelemetry(Packet(s, 150_000_000 - s * 1000, 141_000_000 + s * 10));
        vm.OnTelemetry(Packet(500, 1, 1));   // reordered: dropped
        vm.OnTelemetry(Packet(1000, 1, 1));  // duplicate: dropped
        Assert.Equal(1000, history);
        Assert.Equal(2UL, vm.DroppedPackets);
        Assert.Equal(vm.HistoryCapacity, vm.History.Count);  // rolling window
        Assert.Equal(1000, vm.History[^1].Seconds);
        Assert.Equal(149_000_000, vm.UpperBound);
        Assert.Equal("lagrangian", vm.LbSource);
        Assert.Equal("polish", vm.Phase);
        Assert.InRange(vm.GapPct, 5.36, 5.37);
        Assert.InRange(vm.RamFraction, 0.0195, 0.0196);
        Assert.True(vm.History.Zip(vm.History.Skip(1)).All(p => p.Second.Ub < p.First.Ub));

        var boxes = 0;
        vm.ClusterBox += (id, x0, _, x1, _) => { ++boxes; Assert.Equal(7u, id); Assert.True(x1 > x0); };
        vm.OnLog("cluster_box id=7 x0=1.5 y0=2.0 x1=30.0 y1=40.0");
        Assert.Equal(1, boxes);
        vm.OnFinished("{\"instance\":\"x\",\"mode\":\"scalable\",\"status\":\"CANCELLED\"}");
        Assert.Equal("CANCELLED", vm.Status);
        Assert.True(vm.Finished);
    }
}

public class AppModelTests
{
    [Fact]
    public void T_tour_diff_cut_and_added_edges()
    {
        var s = new Scene(new float[8], new float[8]);
        uint[] a = [0, 1, 2, 3, 4, 5, 6, 7];
        uint[] b = [0, 1, 5, 4, 3, 2, 6, 7];  // 2-opt: cut (1,2) and (5,6), add (1,5) and (2,6)
        s.SetDiff(a, 8, b, 8);
        Assert.Equal(2, s.Removed.Length);
        Assert.Equal(2, s.Added.Length);
        Assert.Contains((1u, 2u), s.Removed);
        Assert.Contains((5u, 6u), s.Removed);
        Assert.Contains((1u, 5u), s.Added);
        Assert.Contains((2u, 6u), s.Added);
        s.SetDiff(b, 8, [7, 6, 2, 3, 4, 5, 1, 0], 8);  // same cycle reversed and rotated: nothing changes
        Assert.Empty(s.Removed);
        Assert.Empty(s.Added);
        s.ClearDiff();
        Assert.Empty(s.Removed);
    }

    [Fact]
    public void T_settings_to_engine_args()
    {
        var s = new RunSettings { Mode = "scalable", TimeLimitS = 90.5, Threads = 3, Seed = 42, MemLimitMb = 2048, CheckpointDir = "/tmp/ck", Resume = true };
        var a = s.ToEngineArgs("x.tsp", "e.sock", "t.shm", "r.json", "o.tour");
        string After(string flag) => a[a.IndexOf(flag) + 1];
        Assert.Equal("scalable", After("--mode"));
        Assert.Equal("90.5", After("--time-limit"));
        Assert.Equal("3", After("--threads"));
        Assert.Equal("42", After("--seed"));
        Assert.Equal("2048", After("--mem-limit-mb"));
        Assert.Equal("/tmp/ck", After("--checkpoint"));
        Assert.Equal("e.sock", After("--ipc"));
        Assert.Equal("r.json", After("--report"));
        Assert.Equal("o.tour", After("--out"));
        Assert.Contains("--resume", a);
        Assert.DoesNotContain("--checkpoint", new RunSettings().ToEngineArgs("x", "e", "t", "r", "o"));
        Assert.NotNull(new RunSettings { Resume = true }.Validate());
        Assert.NotNull(new RunSettings { Threads = 0 }.Validate());
        Assert.NotNull(new RunSettings { Mode = "fast" }.Validate());
        Assert.Throws<ArgumentException>(() => new RunSettings { TimeLimitS = 0 }.ToEngineArgs("x", "e", "t", "r", "o"));
    }

    [Fact]
    public void T_history_round_trip()
    {
        var home = Directory.CreateTempSubdirectory("apex_gui_home_");
        try
        {
            var store = new AppStore(home.FullName);
            var settings = store.LoadSettings();  // missing file: defaults
            settings.Touch("/a.tsp");
            settings.Touch("/b.tsp");
            settings.Touch("/a.tsp");
            settings.Theme = "dark";
            settings.Run.Threads = 7;
            store.SaveSettings(settings);
            var back = store.LoadSettings();
            Assert.Equal(["/a.tsp", "/b.tsp"], back.Recent);
            Assert.Equal("dark", back.Theme);
            Assert.Equal(7, back.Run.Threads);

            var inst = Path.Combine(Repo.Root, "data", "a280.tsp");
            var vm = new MainViewModel();
            vm.OnTelemetry(new TelemetryPacket { MagicVersion = Protocol.TelemetryMagic, Seq = 1, TimestampUs = 1000, UpperBound = 2600, LowerBound = double.NaN });
            vm.OnTelemetry(new TelemetryPacket { MagicVersion = Protocol.TelemetryMagic, Seq = 2, TimestampUs = 2000, UpperBound = 2579, LowerBound = 2579 });
            vm.OnFinished("{\"instance\":\"a280\",\"mode\":\"exact\",\"status\":\"OPTIMAL_PROVEN\",\"ub\":2579,\"lb\":2579,\"lb_source\":\"lp\",\"gap_pct\":0,\"wall_s\":0.4}");
            var first = store.NewRunDir(inst, out var id1);
            var second = store.NewRunDir(inst, out var id2);
            Assert.NotEqual(id1, id2);
            AppStore.SaveRecord(first, vm.ToRecord(id1, inst, "a280", DateTimeOffset.Now.AddMinutes(-1), new RunSettings { Threads = 2 }));
            AppStore.SaveRecord(second, vm.ToRecord(id2, inst, "a280", DateTimeOffset.Now, new RunSettings { Threads = 4 }));
            var runs = store.ListRuns(inst);
            Assert.Equal(2, runs.Count);
            Assert.Equal(second, runs[0].Dir);  // newest first
            var r = runs[0].Run;
            Assert.Equal("OPTIMAL_PROVEN", r.Status);
            Assert.Equal(2579, r.Ub);
            Assert.Equal(0.4, r.WallS);
            Assert.Equal("lp", r.LbSource);
            Assert.Equal(4, r.Settings.Threads);
            Assert.Equal(2, r.Bounds.Count);
            Assert.True(double.IsNaN(r.Bounds[0].Lb));  // NaN survives JSON
            Assert.Empty(store.ListRuns(Path.Combine(Repo.Root, "data", "eil51.tsp")));
            Assert.Equal(2, store.ListRuns().Count);

            var vm2 = new MainViewModel();
            vm2.LoadRecord(r, "");
            Assert.Equal(2579, vm2.UpperBound);
            Assert.Equal(2, vm2.History.Count);
            Assert.True(vm2.Finished);
        }
        finally { home.Delete(true); }
    }

    [Fact]
    public void T_tour_file_round_trip_and_instance_info()
    {
        var dir = Directory.CreateTempSubdirectory("apex_gui_tour_");
        try
        {
            var inst = InstanceInfo.Read(Path.Combine(Repo.Root, "data", "a280.tsp"));
            Assert.Equal("a280", inst.Name);
            Assert.Equal(280, inst.Dimension);
            Assert.Equal("EUC_2D", inst.EdgeWeightType);
            var opt = TourFiles.Read(Path.Combine(Repo.Root, "data", "a280.opt.tour"));
            Assert.Equal(280, opt.Length);
            Assert.Equal(280, opt.Distinct().Count());
            var p = Path.Combine(dir.FullName, "t.tour");
            TourFiles.Write(p, "a280", opt, "test");
            Assert.Equal(opt, TourFiles.Read(p));
            var scene = Scene.LoadTsplib(Path.Combine(Repo.Root, "data", "a280.tsp"));
            var svg = Path.Combine(dir.FullName, "t.svg");
            TourFiles.WriteSvg(svg, scene, opt);
            var text = File.ReadAllText(svg);
            Assert.StartsWith("<svg", text);
            Assert.Equal(280, text.Split("points=\"")[1].Split('"')[0].Split(' ', StringSplitOptions.RemoveEmptyEntries).Length);
        }
        finally { dir.Delete(true); }
    }
}

public class MetricTests
{
    // Same lengths as the engine: a280 / pr1002 optimal tours (EUC_2D) from the TSPLIB files.
    [Theory]
    [InlineData("a280", 2579)]
    [InlineData("pr1002", 259045)]
    [InlineData("berlin52", 7542)]
    public void T_tour_length_matches_known_optimum(string name, long opt)
    {
        var s = Scene.LoadTsplib(Path.Combine(Repo.Root, "data", name + ".tsp"));
        Assert.Equal(opt, s.TourLength(TourFiles.Read(Path.Combine(Repo.Root, "data", name + ".opt.tour"))));
    }
}

public class LabAndEditorTests
{
    private static Process StartEngine(params string[] args)
    {
        var psi = new ProcessStartInfo(Repo.EngineBinary()) { UseShellExecute = false, RedirectStandardOutput = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        var p = Process.Start(psi)!;
        p.OutputDataReceived += (_, _) => { };
        p.BeginOutputReadLine();
        return p;
    }

    [Fact]
    public async Task T_lab_commands_against_engine()
    {
        var dir = Directory.CreateTempSubdirectory("apex_gui_lab_");
        var endpoint = OperatingSystem.IsWindows() ? "apex_gui_lab_" + Environment.ProcessId : Path.Combine(dir.FullName, "e.sock");
        var shared = Path.Combine(dir.FullName, "tour.shm");
        using var engine = StartEngine("--instance", Path.Combine(Repo.Root, "data", "pr1002.tsp"), "--mode", "lab", "--ipc", endpoint, "--shared-tour", shared);
        using var c = EngineClient.Connect(endpoint, TimeSpan.FromSeconds(20));
        Assert.Equal(2, c.EngineHello.Mode);
        var steps = new System.Collections.Concurrent.BlockingCollection<string>();
        c.Log += l => { if (l.StartsWith("lab_step ", StringComparison.Ordinal) || l.StartsWith("lab_error", StringComparison.Ordinal)) steps.Add(l); };
        var finished = new TaskCompletionSource<string>();
        c.Finished += j => finished.TrySetResult(j);
        c.Start();
        string Next() => steps.TryTake(out var l, TimeSpan.FromSeconds(30)) ? l : throw new TimeoutException("no lab reply");
        c.SendCommand("heuristic nn 3");
        var nn = LabStepRow.Parse(Next(), 1)!;
        Assert.Equal("nn", nn.Name);
        long prev = nn.After;
        foreach (var cmd in new[] { "two_opt", "two_opt_oropt", "lk", "ils 0.5" })
        {
            c.SendCommand("heuristic " + cmd);
            var r = LabStepRow.Parse(Next(), 2)!;
            Assert.Equal(cmd.Split(' ')[0], r.Name);
            Assert.Equal(prev, r.Before);
            Assert.True(r.After <= r.Before);
            prev = r.After;
        }
        c.SendCommand("heuristic bogus");  // rejected by the server before the queue: answered as a log line
        c.SendCommand("heuristic quit");
        var json = await finished.Task.WaitAsync(TimeSpan.FromSeconds(30));
        await engine.WaitForExitAsync(new CancellationTokenSource(10000).Token);
        Assert.Contains("\"mode\":\"lab\"", json);
        Assert.Contains($"\"ub\":{prev},", json);
        using var reader = new SharedTourReader(shared);
        var buf = Array.Empty<uint>();
        Assert.True(reader.TryRead(ref buf, out var n, out _));
        Assert.Equal(1002, buf.Take(n).Distinct().Count());
        var scene = Scene.LoadTsplib(Path.Combine(Repo.Root, "data", "pr1002.tsp"));
        Assert.Equal(prev, scene.TourLength(buf.AsSpan(0, n)));  // GUI metric == engine metric
        dir.Delete(true);
    }

    [Fact]
    public async Task T_editor_instance_solves_to_proven_optimum()
    {
        var dir = Directory.CreateTempSubdirectory("apex_gui_edit_");
        var e = new EditableInstance { Name = "edit60" };
        e.Generate("clustered", 60, 7);
        Assert.Equal(60, e.Points.Distinct().Count());
        e.Points.Add((500, 500));  // a point added by hand
        e.Points.RemoveAt(0);      // and one deleted
        var path = Path.Combine(dir.FullName, "edit60.tsp");
        e.Save(path);
        Assert.Equal(60, Scene.LoadTsplib(path).Count);
        Assert.Equal(60, InstanceInfo.Read(path).Dimension);
        var report = Path.Combine(dir.FullName, "r.json");
        using var engine = StartEngine("--instance", path, "--mode", "exact", "--time-limit", "60", "--report", report);
        await engine.WaitForExitAsync(new CancellationTokenSource(90000).Token);
        Assert.Equal(0, engine.ExitCode);
        Assert.Contains("\"status\":\"OPTIMAL_PROVEN\"", File.ReadAllText(report));
        foreach (var kind in EditableInstance.Kinds)
        {
            var g = new EditableInstance();
            g.Generate(kind, 250, 3);
            Assert.Equal(250, g.Points.Count);
            Assert.Equal(250, g.Points.Distinct().Count());
        }
        Assert.Throws<InvalidOperationException>(() => new EditableInstance().Save(Path.Combine(dir.FullName, "empty.tsp")));
        dir.Delete(true);
    }
}

public class ReferenceTests
{
    [Fact]
    public void T_reference_table_loads()
    {
        var path = Path.Combine(Repo.Root, "engine", "tools", "history", "reference.csv");
        var rows = ReferenceTable.Load(path);
        Assert.True(rows.Count >= 50);
        Assert.Equal(259045, rows["pr1002"].Value);
        Assert.Equal("optimal", rows["pr1002"].Kind);
        Assert.Equal("1987", rows["pr2392"].Year);
        Assert.Contains("Padberg", rows["pr2392"].SolvedBy);
        Assert.Equal("best_known_tour", rows["mona-lisa100K"].Kind);
        Assert.Contains("not proven optimal", rows["mona-lisa100K"].Summary());
        Assert.DoesNotContain("optimum", rows["mona-lisa100K"].Summary().Split('\n')[0]);
        Assert.Equal(["a", "b,c", "d\"e", ""], ReferenceTable.SplitCsv("a,\"b,c\",\"d\"\"e\","));
    }
}
