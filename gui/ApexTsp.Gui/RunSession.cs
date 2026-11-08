using System.Diagnostics;
using System.Text;
using ApexTsp.Ipc;

namespace ApexTsp.Gui;

// One engine run driven by the app: launches apex_tsp (or attaches to a running one), connects,
// forwards client events, owns the shared-tour reader and the run directory (report, tour, log).
// Events fire on the reader thread; the window marshals them to the UI thread.
public sealed class RunSession : IDisposable
{
    public string Endpoint { get; }
    public string SharedTour { get; }
    public string RunDir { get; }
    public string ReportPath => Path.Combine(RunDir, "report.json");
    public string TourPath => Path.Combine(RunDir, "tour.tour");
    public string LogPath => Path.Combine(RunDir, "engine.log");
    public EngineClient? Client { get; private set; }
    public Hello? EngineHello => Client?.EngineHello;
    public DateTimeOffset Started { get; } = DateTimeOffset.Now;

    public event Action<EngineClient>? Connected;   // subscribe client events here (before Start)
    public event Action<string>? Failed;

    private Process? _engine;
    private StreamWriter? _log;
    private SharedTourReader? _reader;
    private readonly Dictionary<ulong, MsgType> _sent = [];
    public event Action<MsgType>? Acked;

    private RunSession(string runDir, string? endpoint, string? sharedTour)
    {
        RunDir = runDir;
        var tag = $"apex_tsp_gui_{Environment.ProcessId}_{Path.GetFileName(runDir)}";
        Endpoint = endpoint ?? (OperatingSystem.IsWindows() ? tag : Path.Combine(Path.GetTempPath(), tag + ".sock"));
        SharedTour = sharedTour ?? Path.Combine(Path.GetTempPath(), tag + ".tour");
    }

    public static RunSession Launch(string engine, string instance, RunSettings settings, string runDir, IEnumerable<string>? extraArgs = null)
    {
        var s = new RunSession(runDir, null, null);
        var psi = new ProcessStartInfo(engine) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true };
        foreach (var a in settings.ToEngineArgs(instance, s.Endpoint, s.SharedTour, s.ReportPath, s.TourPath)) psi.ArgumentList.Add(a);
        foreach (var a in extraArgs ?? []) psi.ArgumentList.Add(a);
        s._log = new StreamWriter(s.LogPath, false, Encoding.UTF8) { AutoFlush = true };
        s._engine = Process.Start(psi) ?? throw new IOException("cannot start " + engine);
        s._engine.OutputDataReceived += (_, e) => s.LogLine(e.Data);
        s._engine.ErrorDataReceived += (_, e) => s.LogLine(e.Data);
        s._engine.BeginOutputReadLine();
        s._engine.BeginErrorReadLine();
        return s;
    }

    public static RunSession Attach(string endpoint, string sharedTour, string runDir) => new(runDir, endpoint, sharedTour);

    private void LogLine(string? line)
    {
        var log = _log;
        if (line is null || log is null) return;
        try { lock (log) log.WriteLine(line); }
        catch (ObjectDisposedException) { }  // output after the session closed
    }

    // Connects in the background; Connected fires with the client before its reader starts.
    public void ConnectAsync(TimeSpan timeout) => Task.Run(() =>
    {
        try
        {
            var c = EngineClient.Connect(Endpoint, timeout);
            c.Ack += seq => { if (TakeSent(seq) is { } t) Acked?.Invoke(t); };
            Client = c;
            Connected?.Invoke(c);
            c.Start();
        }
        catch (Exception e) when (e is IOException or TimeoutException or System.Net.Sockets.SocketException)
        {
            Failed?.Invoke(e.Message);
        }
    });

    private MsgType? TakeSent(ulong seq)
    {
        lock (_sent) return _sent.Remove(seq, out var t) ? t : null;
    }

    public bool Send(MsgType t)
    {
        if (Client is null) return false;
        try
        {
            var seq = Client.Send(t);
            lock (_sent) _sent[seq] = t;
            return true;
        }
        catch (IOException) { return false; }
    }

    private bool _disposed;
    public bool Running => !_disposed && (_engine is { HasExited: false } || (Client is not null && _engine is null));

    // Latest tour from the shared mapping (seqlock copy; never blocks the engine).
    public bool TryReadTour(out uint[] tour, out int count)
    {
        tour = [];
        count = 0;
        try
        {
            _reader ??= new SharedTourReader(SharedTour);
            tour = new uint[_reader.Capacity];
            return _reader.TryRead(ref tour, out count, out _);
        }
        catch (IOException) { return false; }
    }

    public void Dispose()
    {
        if (_disposed) return;
        _disposed = true;
        try
        {
            if (_engine is { HasExited: false }) Send(MsgType.Shutdown);
            _engine?.WaitForExit(5000);
            if (_engine is { HasExited: false }) _engine.Kill();
        }
        catch (InvalidOperationException) { }
        Client?.Dispose();
        _reader?.Dispose();
        _engine?.Dispose();
        if (_log is not null) lock (_log) _log.Dispose();
        _log = null;
        if (!OperatingSystem.IsWindows() && _engine is not null)
        {
            try { File.Delete(SharedTour); } catch (IOException) { }
        }
    }
}
