using System.ComponentModel;
using System.Globalization;
using System.Text.Json;
using ApexTsp.Ipc;

namespace ApexTsp.Gui;

public readonly record struct BoundSample(double Seconds, double Ub, double Lb, double GapPct);

// Engine state for the panels. Plain C# (no Avalonia types): fed from the IPC client, read by
// the views; T_vm_gap_chart_updates drives it with a synthetic stream. Call from one thread.
public sealed class MainViewModel : INotifyPropertyChanged
{
    public static readonly string[] PhaseNames = ["load", "candidate_graph", "construct", "two_opt", "held_karp", "polish", "bnc", "done"];

    public int HistoryCapacity { get; init; } = 600;
    private readonly List<BoundSample> _history = [];
    private ulong _lastSeq;

    public event PropertyChangedEventHandler? PropertyChanged;
    public event Action? HistoryChanged;
    public event Action<uint, float, float, float, float>? ClusterBox;  // id, x0, y0, x1, y1

    public IReadOnlyList<BoundSample> History => _history;
    public double UpperBound { get; private set; } = double.NaN;
    public double LowerBound { get; private set; } = double.NaN;
    public double GapPct { get; private set; } = double.NaN;
    public string Phase { get; private set; } = "-";
    public string Mode { get; private set; } = "-";
    public string LbSource { get; private set; } = "-";      // badge: "lp" (exact) | "lagrangian" (scalable)
    public uint CutPoolSize { get; private set; }
    public uint OpenNodes { get; private set; }
    public double IterPerSec { get; private set; }
    public ulong RamBytes { get; private set; }
    public ulong RamCeilingBytes { get; set; }
    public double RamFraction => RamCeilingBytes == 0 ? 0 : Math.Min(1.0, (double)RamBytes / RamCeilingBytes);
    public ulong DroppedPackets { get; private set; }         // duplicates / reordered seq
    public string Status { get; private set; } = "connecting";
    public string ReportJson { get; private set; } = "";
    public bool Finished { get; private set; }
    public List<string> LogTail { get; } = [];

    public void OnTelemetry(in TelemetryPacket p)
    {
        if (p.Seq <= _lastSeq) { ++DroppedPackets; return; }
        _lastSeq = p.Seq;
        UpperBound = p.UpperBound;
        LowerBound = p.LowerBound;
        GapPct = p.GapPct;
        Phase = p.Phase < PhaseNames.Length ? PhaseNames[p.Phase] : "-";
        Mode = p.Mode == 0 ? "exact" : "scalable";
        LbSource = double.IsNaN(p.LowerBound) ? "-" : p.LbSource == 0 ? "lp" : "lagrangian";
        CutPoolSize = p.CutPoolSize;
        OpenNodes = p.OpenNodes;
        IterPerSec = p.IterPerSec;
        RamBytes = p.RamBytes;
        if (Status == "connecting") Status = "running";
        _history.Add(new BoundSample(p.TimestampUs / 1e6, p.UpperBound, p.LowerBound, p.GapPct));
        if (_history.Count > HistoryCapacity) _history.RemoveRange(0, _history.Count - HistoryCapacity);
        Changed();
        HistoryChanged?.Invoke();
    }

    public void OnLog(string line)
    {
        LogTail.Add(line);
        if (LogTail.Count > 200) LogTail.RemoveRange(0, LogTail.Count - 200);
        if (line.StartsWith("cluster_box ", StringComparison.Ordinal))
        {
            var kv = ParseFields(line);
            if (kv.TryGetValue("id", out var id) && kv.TryGetValue("x0", out var x0) && kv.TryGetValue("y0", out var y0) &&
                kv.TryGetValue("x1", out var x1) && kv.TryGetValue("y1", out var y1))
                ClusterBox?.Invoke((uint)id, (float)x0, (float)y0, (float)x1, (float)y1);
        }
        else if (line.StartsWith("paused", StringComparison.Ordinal)) { Status = "paused"; Changed(); }
    }

    public void OnAck(MsgType sent)
    {
        Status = sent switch
        {
            MsgType.Pause => "pausing",
            MsgType.Resume => "running",
            MsgType.Cancel => "cancelling",
            MsgType.Checkpoint => Status,
            _ => Status,
        };
        Changed();
    }

    public void OnFinished(string json)
    {
        ReportJson = json;
        Finished = true;
        try
        {
            using var doc = JsonDocument.Parse(json);
            Status = doc.RootElement.GetProperty("status").GetString() ?? "done";
        }
        catch (JsonException) { Status = "done (bad report)"; }
        Phase = "done";
        Changed();
    }

    public void OnClosed(string reason)
    {
        if (!Finished) Status = "disconnected: " + reason;
        Changed();
    }

    private void Changed() => PropertyChanged?.Invoke(this, new PropertyChangedEventArgs(null));

    private static Dictionary<string, double> ParseFields(string line)
    {
        var d = new Dictionary<string, double>();
        foreach (var tok in line.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            var k = tok.IndexOf('=');
            if (k > 0 && double.TryParse(tok.AsSpan(k + 1), NumberStyles.Float, CultureInfo.InvariantCulture, out var v))
                d[tok[..k]] = v;
        }
        return d;
    }

    public static string Fmt(double v) => double.IsNaN(v) ? "-" : v.ToString("0.###", CultureInfo.InvariantCulture);
}
