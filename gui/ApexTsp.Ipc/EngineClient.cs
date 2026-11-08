using System.Buffers.Binary;
using System.IO.MemoryMappedFiles;
using System.IO.Pipes;
using System.Net.Sockets;
using System.Runtime.InteropServices;

namespace ApexTsp.Ipc;

public sealed record ClusterEntry(uint Id, byte State);  // 0 idle, 1 solving, 2 patching

// Client side of the engine IPC (UDS on Linux, named pipe on Windows). A reader thread decodes
// frames and raises events on that thread; consumers marshal to their UI thread.
public sealed class EngineClient : IDisposable
{
    private readonly Stream _stream;
    private readonly IDisposable? _owner;
    private readonly Thread _reader;
    private readonly object _writeLock = new();
    private ulong _seq;
    private volatile bool _closed;

    public Hello EngineHello { get; }
    public event Action<TelemetryPacket>? Telemetry;
    public event Action<ulong, uint>? TourUpdate;           // epoch, count
    public event Action<IReadOnlyList<ClusterEntry>>? ClusterMap;
    public event Action<string>? Log;
    public event Action<string>? Finished;                  // report JSON
    public event Action<ulong>? Ack;
    public event Action<string>? Closed;                    // reason
    // v3 (negotiated version >= 3)
    public event Action<ulong, LpEdge[]>? LpSolution;       // seq, support edges
    public event Action<CutRoundPacket>? CutRound;
    public event Action<TreeNodePacket>? TreeNode;
    public event Action<SolverEvent>? SolverEvent;
    public ushort NegotiatedVersion { get; private set; }

    private EngineClient(Stream stream, IDisposable? owner, Hello engine)
    {
        _stream = stream;
        _owner = owner;
        EngineHello = engine;
        _reader = new Thread(ReadLoop) { IsBackground = true, Name = "apex-ipc-reader" };
    }

    // Connects and exchanges Hello; Start() begins reading. Throws on timeout or version mismatch.
    public static EngineClient Connect(string endpoint, TimeSpan timeout, uint guiCapabilities = 1)
    {
        var (stream, owner) = OpenStream(endpoint, timeout);
        var dec = new FrameDecoder();
        var buf = new byte[4096];
        var deadline = DateTime.UtcNow + timeout;
        Hello? engine = null;
        stream.ReadTimeout = (int)Math.Max(100, timeout.TotalMilliseconds);
        while (engine is null)
        {
            if (DateTime.UtcNow > deadline) throw new TimeoutException("no Hello from the engine");
            var n = stream.Read(buf, 0, buf.Length);
            if (n <= 0) throw new IOException("engine closed during handshake");
            dec.Feed(buf.AsSpan(0, n));
            while (dec.Next() is { } f)
            {
                if (f.Type == MsgType.Close) throw new IOException("engine closed: " + System.Text.Encoding.UTF8.GetString(f.Payload));
                if (f.Type != MsgType.Hello) continue;
                engine = Protocol.UnpackHello(f.Payload) ?? throw new IOException("malformed engine Hello");
                break;
            }
        }
        var mine = new Hello { GuiCapabilities = guiCapabilities };
        var negotiated = Protocol.Negotiate(engine.Value, mine);
        if (negotiated == 0)
            throw new IOException($"version mismatch: engine {engine.Value.ProtocolMin}..{engine.Value.Version}, gui {mine.ProtocolMin}..{mine.Version}");
        // Hello carries the lowest version in its header, so any supported engine decodes it.
        var w = Protocol.Encode(MsgType.Hello, Protocol.Pack(mine), version: Protocol.VersionMin);
        stream.Write(w, 0, w.Length);
        stream.Flush();
        var c = new EngineClient(stream, owner, engine.Value) { NegotiatedVersion = negotiated };
        c._pending = dec;
        return c;
    }

    // Starts the reader thread. Call after subscribing to the events: frames the engine queued
    // before the connection (early phases, first UB) are dispatched first and would be lost otherwise.
    public void Start() => _reader.Start();

    private FrameDecoder _pending = new();

    private static (Stream, IDisposable?) OpenStream(string endpoint, TimeSpan timeout)
    {
        if (OperatingSystem.IsWindows())
        {
            var name = endpoint.StartsWith(@"\\.\pipe\", StringComparison.Ordinal) ? endpoint[9..] : endpoint;
            var p = new NamedPipeClientStream(".", name, PipeDirection.InOut);
            p.Connect((int)timeout.TotalMilliseconds);
            return (p, null);
        }
        var deadline = DateTime.UtcNow + timeout;
        for (;;)
        {
            var s = new Socket(AddressFamily.Unix, SocketType.Stream, ProtocolType.Unspecified);
            try
            {
                s.Connect(new UnixDomainSocketEndPoint(endpoint));
                return (new NetworkStream(s, ownsSocket: true), null);
            }
            catch (SocketException) when (DateTime.UtcNow < deadline)
            {
                s.Dispose();
                Thread.Sleep(20);
            }
        }
    }

    // Control messages: idempotent on the engine side, answered with Ack(seq). Returns seq.
    public ulong Send(MsgType control)
    {
        if (!Protocol.IsControl(control)) throw new ArgumentException("not a control message", nameof(control));
        var seq = Interlocked.Increment(ref _seq);
        Write(Protocol.Encode(control, Protocol.PackU64(seq), withCrc: true, version: NegotiatedVersion));
        return seq;
    }

    // v3 command line (e.g. "heuristic two_opt"); answered with Ack(seq) and a Log line. Returns seq.
    public ulong SendCommand(string command)
    {
        if (NegotiatedVersion < 3) throw new InvalidOperationException("engine speaks protocol " + NegotiatedVersion + ", commands need 3");
        var seq = Interlocked.Increment(ref _seq);
        var text = System.Text.Encoding.UTF8.GetBytes(command);
        var payload = new byte[8 + text.Length];
        BinaryPrimitives.WriteUInt64LittleEndian(payload, seq);
        text.CopyTo(payload, 8);
        Write(Protocol.Encode(MsgType.Command, payload, withCrc: true, version: NegotiatedVersion));
        return seq;
    }

    private void Write(byte[] w)
    {
        lock (_writeLock)
        {
            _stream.Write(w, 0, w.Length);
            _stream.Flush();
        }
    }

    private void ReadLoop()
    {
        var buf = new byte[1 << 16];
        var dec = _pending;
        string reason = "engine closed";
        try
        {
            _stream.ReadTimeout = Timeout.Infinite;
            for (;;)
            {
                while (dec.Next() is { } f) Dispatch(f);
                var n = _stream.Read(buf, 0, buf.Length);
                if (n <= 0) break;
                dec.Feed(buf.AsSpan(0, n));
            }
        }
        catch (Exception e) when (e is IOException or ObjectDisposedException or InvalidOperationException)
        {
            reason = _closed ? "client closed" : e.Message;
        }
        Closed?.Invoke(reason);
    }

    private void Dispatch(Frame f)
    {
        switch (f.Type)
        {
            case MsgType.Telemetry:
                if (Protocol.UnpackTelemetry(f.Payload) is { } t) Telemetry?.Invoke(t);
                break;
            case MsgType.TourUpdate:
                TourUpdate?.Invoke(BinaryPrimitives.ReadUInt64LittleEndian(f.Payload), BinaryPrimitives.ReadUInt32LittleEndian(f.Payload.AsSpan(8)));
                break;
            case MsgType.ClusterMap:
                ClusterMap?.Invoke(ParseClusterMap(f.Payload));
                break;
            case MsgType.Log:
                Log?.Invoke(System.Text.Encoding.UTF8.GetString(f.Payload));
                break;
            case MsgType.Finished:
                Finished?.Invoke(System.Text.Encoding.UTF8.GetString(f.Payload));
                break;
            case MsgType.Ack:
                Ack?.Invoke(BinaryPrimitives.ReadUInt64LittleEndian(f.Payload));
                break;
            case MsgType.Close:
                Closed?.Invoke(System.Text.Encoding.UTF8.GetString(f.Payload));
                break;
            case MsgType.LpSolution:
                LpSolution?.Invoke(BinaryPrimitives.ReadUInt64LittleEndian(f.Payload), ParseLpSolution(f.Payload));
                break;
            case MsgType.CutRound:
                CutRound?.Invoke(MemoryMarshal.Read<CutRoundPacket>(f.Payload));
                break;
            case MsgType.TreeNode:
                TreeNode?.Invoke(MemoryMarshal.Read<TreeNodePacket>(f.Payload));
                break;
            case MsgType.Event:
                SolverEvent?.Invoke(ParseEvent(f.Payload));
                break;
        }
    }

    public static LpEdge[] ParseLpSolution(ReadOnlySpan<byte> p)
    {
        if (p.Length < 12) return [];
        var n = BinaryPrimitives.ReadUInt32LittleEndian(p[8..]);
        if (p.Length != 12 + 12L * n) return [];
        var e = new LpEdge[n];
        for (var i = 0; i < n; ++i)
        {
            var o = 12 + 12 * i;
            e[i] = new LpEdge(BinaryPrimitives.ReadUInt32LittleEndian(p[o..]), BinaryPrimitives.ReadUInt32LittleEndian(p[(o + 4)..]),
                              BinaryPrimitives.ReadSingleLittleEndian(p[(o + 8)..]));
        }
        return e;
    }

    public static SolverEvent ParseEvent(ReadOnlySpan<byte> p)
    {
        var h = MemoryMarshal.Read<EventHeader>(p);
        var detail = System.Text.Encoding.UTF8.GetString(p[Marshal.SizeOf<EventHeader>()..]);
        return new SolverEvent((EventKind)h.Kind, h.TimestampUs / 1e6, h.Value, h.Heuristic != 0, detail);
    }

    public static IReadOnlyList<ClusterEntry> ParseClusterMap(ReadOnlySpan<byte> p)
    {
        if (p.Length < 4) return [];
        var n = BinaryPrimitives.ReadUInt32LittleEndian(p);
        if (p.Length != 4 + 5L * n) return [];
        var list = new ClusterEntry[n];
        for (var i = 0; i < n; ++i)
        {
            var o = 4 + 5 * i;
            list[i] = new ClusterEntry(BinaryPrimitives.ReadUInt32LittleEndian(p[o..]), p[o + 4]);
        }
        return list;
    }

    public void Dispose()
    {
        _closed = true;
        _stream.Dispose();
        _owner?.Dispose();
        if (_reader.IsAlive && Thread.CurrentThread != _reader) _reader.Join(1000);
    }
}

// Reader of the engine's SharedTour file (seqlock, see SharedTourView in protocol.hpp). Never
// blocks the engine: retries while the epoch is odd or changes during the copy.
public sealed unsafe class SharedTourReader : IDisposable
{
    private readonly MemoryMappedFile _mmf;
    private readonly MemoryMappedViewAccessor _view;
    private readonly byte* _base;

    public SharedTourReader(string path)
    {
        using var fs = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        _mmf = MemoryMappedFile.CreateFromFile(fs, null, 0, MemoryMappedFileAccess.Read, HandleInheritability.None, leaveOpen: false);
        _view = _mmf.CreateViewAccessor(0, 0, MemoryMappedFileAccess.Read);
        byte* p = null;
        _view.SafeMemoryMappedViewHandle.AcquirePointer(ref p);
        _base = p + _view.PointerOffset;
    }

    public uint Capacity => Volatile.Read(ref *(uint*)(_base + 12));

    public bool TryRead(ref uint[] tour, out int count, out ulong epoch, int maxRetries = 100000)
    {
        count = 0;
        epoch = 0;
        var cap = Capacity;
        ref var ep = ref *(ulong*)_base;
        var words = (uint*)(_base + 16);
        for (var attempt = 0; attempt <= maxRetries; ++attempt)
        {
            var e1 = Volatile.Read(ref ep);
            if ((e1 & 1) != 0) { Thread.SpinWait(8); continue; }
            if (e1 == 0) return false;
            var n = (int)Math.Min(Volatile.Read(ref *(uint*)(_base + 8)), cap);
            if (tour.Length < n) tour = new uint[n];
            for (var i = 0; i < n; ++i) tour[i] = Volatile.Read(ref words[i]);
            if (Volatile.Read(ref ep) == e1)
            {
                count = n;
                epoch = e1;
                return true;
            }
        }
        return false;
    }

    public void Dispose()
    {
        _view.SafeMemoryMappedViewHandle.ReleasePointer();
        _view.Dispose();
        _mmf.Dispose();
    }
}
