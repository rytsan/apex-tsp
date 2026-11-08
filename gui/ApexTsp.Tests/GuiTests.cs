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
    }
}

public class ViewModelTests
{
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
