using System.Buffers.Binary;
using System.Runtime.InteropServices;

namespace ApexTsp.Ipc;

// Mirror of engine/src/ipc/protocol.hpp (Sprint 5 M5.1). Little-endian wire format.
public static class Protocol
{
    public const ushort Version = 3;  // v3: typed solver events (Sprint 6 M6.1)
    public const ushort VersionMin = 2;
    public const int HeaderBytes = 8;          // u32 length + u16 version + u8 type + u8 flags
    public const uint MaxFrameBytes = 64u << 20;
    public const byte FlagCrc = 0x01;
    public const int HelloBytes = 17;
    public const ulong TelemetryMagic = (0x41545350UL << 32) | 2;  // "ATSP", packet layout 2
    public const uint MaxLpEdges = 1u << 20;

    // Lowest protocol version that knows the type (same as first_version in the engine).
    public static ushort FirstVersion(MsgType t) =>
        t is >= MsgType.LpSolution and <= MsgType.Event or MsgType.Command ? (ushort)3 : (ushort)2;

    public static bool KnownType(byte t) => Enum.IsDefined((MsgType)t);
    public static bool IsControl(MsgType t) => t >= MsgType.Pause && t <= MsgType.Shutdown;

    // Same bounds as payload_size_ok in the engine.
    public static bool PayloadSizeOk(MsgType t, long n) => t switch
    {
        MsgType.Hello => n == HelloBytes,
        MsgType.Pause or MsgType.Resume or MsgType.Cancel or MsgType.Checkpoint or MsgType.Shutdown or MsgType.Ack => n == 8,
        MsgType.Telemetry => n == Marshal.SizeOf<TelemetryPacket>(),
        MsgType.TourUpdate => n == 12,
        MsgType.Close => n <= 4096,
        MsgType.Log => n <= 65536,
        MsgType.ClusterMap or MsgType.Finished => n <= MaxFrameBytes - 8,
        MsgType.LpSolution => n >= 12 && (n - 12) % 12 == 0 && (n - 12) / 12 <= MaxLpEdges,
        MsgType.CutRound => n == Marshal.SizeOf<CutRoundPacket>(),
        MsgType.TreeNode => n == Marshal.SizeOf<TreeNodePacket>(),
        MsgType.Event => n >= Marshal.SizeOf<EventHeader>() && n <= Marshal.SizeOf<EventHeader>() + 256,
        MsgType.Command => n is >= 8 and <= 8 + 4096,
        _ => false,
    };

    public static byte[] Encode(MsgType type, ReadOnlySpan<byte> payload, bool withCrc = false, ushort version = Version)
    {
        var len = 4 + payload.Length + (withCrc ? 4 : 0);
        var b = new byte[4 + len];
        BinaryPrimitives.WriteUInt32LittleEndian(b, (uint)len);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(4), version);
        b[6] = (byte)type;
        b[7] = withCrc ? FlagCrc : (byte)0;
        payload.CopyTo(b.AsSpan(HeaderBytes));
        if (withCrc) BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(HeaderBytes + payload.Length), Crc32.Compute(payload));
        return b;
    }

    public static byte[] PackU64(ulong v)
    {
        var b = new byte[8];
        BinaryPrimitives.WriteUInt64LittleEndian(b, v);
        return b;
    }

    public static byte[] Pack(in Hello h)
    {
        var b = new byte[HelloBytes];
        BinaryPrimitives.WriteUInt16LittleEndian(b, h.Version);
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(2), h.ProtocolMin);
        b[4] = h.Mode;
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(5), h.MemLimitMb);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(9), h.Threads);
        BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(13), h.GuiCapabilities);
        return b;
    }

    public static Hello? UnpackHello(ReadOnlySpan<byte> p)
    {
        if (p.Length != HelloBytes) return null;
        var h = new Hello
        {
            Version = BinaryPrimitives.ReadUInt16LittleEndian(p),
            ProtocolMin = BinaryPrimitives.ReadUInt16LittleEndian(p[2..]),
            Mode = p[4],
            MemLimitMb = BinaryPrimitives.ReadUInt32LittleEndian(p[5..]),
            Threads = BinaryPrimitives.ReadUInt32LittleEndian(p[9..]),
            GuiCapabilities = BinaryPrimitives.ReadUInt32LittleEndian(p[13..]),
        };
        return h.ProtocolMin > h.Version ? null : h;
    }

    public static ushort Negotiate(in Hello a, in Hello b)
    {
        var hi = Math.Min(a.Version, b.Version);
        var lo = Math.Max(a.ProtocolMin, b.ProtocolMin);
        return hi >= lo ? hi : (ushort)0;
    }

    public static TelemetryPacket? UnpackTelemetry(ReadOnlySpan<byte> p)
    {
        if (p.Length != Marshal.SizeOf<TelemetryPacket>()) return null;
        var t = MemoryMarshal.Read<TelemetryPacket>(p);
        return t.MagicVersion == TelemetryMagic ? t : null;
    }
}

public enum MsgType : byte
{
    Hello = 0x01,
    Close = 0x02,
    Pause = 0x10,
    Resume = 0x11,
    Cancel = 0x12,
    Checkpoint = 0x13,
    Shutdown = 0x14,
    Ack = 0x1F,
    Telemetry = 0x20,
    TourUpdate = 0x21,
    ClusterMap = 0x22,
    Log = 0x23,
    Finished = 0x24,
    LpSolution = 0x25,
    CutRound = 0x26,
    TreeNode = 0x27,
    Event = 0x28,
    Command = 0x15,
}

public enum TreeNodeState : byte { Open, Pruned, Infeasible, Integral, Branched, Reopened }

public enum EventKind : byte { UbImproved, LbImproved, Phase, NoViolatedCuts, TailingOff, Rcf, Proven }

public readonly record struct LpEdge(uint U, uint V, float X);

public sealed record SolverEvent(EventKind Kind, double Seconds, double Value, bool Heuristic, string Detail);

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct CutRoundPacket
{
    public ulong TimestampUs;
    public uint Round;
    public uint Rows;
    public double LpValue;
    public uint Sec, Blossom, Component, Pool;
    public uint Aged;
    public uint LpIterations;
    public float LpS, SepS;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct TreeNodePacket
{
    public ulong TimestampUs;
    public uint Id, Parent, Depth;
    public byte State;
    public byte BranchDir;
    public uint BranchU, BranchV;
    public double Bound;
    public double LpValue;
    public uint CutsSec, CutsBlossom, CutsComponent;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct EventHeader
{
    public ulong TimestampUs;
    public double Value;
    public byte Kind;
    public byte Heuristic;
}

public sealed record Frame(ushort Version, MsgType Type, byte Flags, byte[] Payload);

public struct Hello
{
    public ushort Version;
    public ushort ProtocolMin;
    public byte Mode;
    public uint MemLimitMb;
    public uint Threads;
    public uint GuiCapabilities;

    public Hello() { Version = Protocol.Version; ProtocolMin = Protocol.VersionMin; }
}

// MASTER v2 struct; the engine static_asserts 120 bytes, T_gui_struct_sizes_match_engine checks here.
[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct TelemetryPacket
{
    public ulong MagicVersion;
    public ulong Seq;
    public ulong TimestampUs;
    public uint Mode;
    public uint LbSource;
    public uint Phase;
    public uint OpenNodes;
    public uint ActiveEdges;
    public uint PrunedEdgesRcf;
    public uint CutPoolSize;
    public double UpperBound;
    public double LowerBound;
    public double GapPct;
    public ulong SimplexIterations;
    public ulong Refactorizations;
    public double IterPerSec;
    public ulong RamBytes;
    public ulong ColdBytes;
    public uint ThreadsLp;
}

[StructLayout(LayoutKind.Sequential, Pack = 1)]
public struct SharedTourHeader
{
    public ulong Epoch;
    public uint Count;
    public uint Capacity;
}

public static class Crc32
{
    private static readonly uint[] Table = BuildTable();

    private static uint[] BuildTable()
    {
        var t = new uint[256];
        for (uint i = 0; i < 256; ++i)
        {
            var c = i;
            for (var k = 0; k < 8; ++k) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }

    public static uint Compute(ReadOnlySpan<byte> data, uint crc = 0)
    {
        crc = ~crc;
        foreach (var b in data) crc = Table[(crc ^ b) & 0xFF] ^ (crc >> 8);
        return ~crc;
    }
}

// Streaming decoder with byte-wise resync, same acceptance rules as the engine's FrameDecoder.
public sealed class FrameDecoder
{
    private byte[] _buf = new byte[1 << 16];
    private int _start, _end;
    public long DroppedBytes { get; private set; }
    public long CrcFailures { get; private set; }

    public void Feed(ReadOnlySpan<byte> bytes)
    {
        if (_end + bytes.Length > _buf.Length)
        {
            var live = _end - _start;
            var cap = _buf.Length;
            while (live + bytes.Length > cap) cap *= 2;
            var nb = cap == _buf.Length ? _buf : new byte[cap];
            Buffer.BlockCopy(_buf, _start, nb, 0, live);
            _buf = nb;
            _start = 0;
            _end = live;
        }
        bytes.CopyTo(_buf.AsSpan(_end));
        _end += bytes.Length;
    }

    public Frame? Next()
    {
        while (_end - _start >= Protocol.HeaderBytes)
        {
            var h = _buf.AsSpan(_start);
            var len = BinaryPrimitives.ReadUInt32LittleEndian(h);
            var ver = BinaryPrimitives.ReadUInt16LittleEndian(h[4..]);
            var type = h[6];
            var flags = h[7];
            var minLen = 4u + ((flags & Protocol.FlagCrc) != 0 ? 4u : 0u);
            if (len < minLen || len > Protocol.MaxFrameBytes || ver < Protocol.VersionMin || ver > Protocol.Version ||
                !Protocol.KnownType(type) || (flags & ~Protocol.FlagCrc) != 0 ||
                !Protocol.PayloadSizeOk((MsgType)type, len - minLen))
            {
                ++_start;
                ++DroppedBytes;
                continue;
            }
            if (_end - _start < 4 + (long)len) return null;
            var plen = (int)(len - minLen);
            var payload = h.Slice(Protocol.HeaderBytes, plen);
            if ((flags & Protocol.FlagCrc) != 0 &&
                Crc32.Compute(payload) != BinaryPrimitives.ReadUInt32LittleEndian(h[(Protocol.HeaderBytes + plen)..]))
            {
                ++CrcFailures;
                ++_start;
                ++DroppedBytes;
                continue;
            }
            var f = new Frame(ver, (MsgType)type, flags, payload.ToArray());
            _start += 4 + (int)len;
            if (_start == _end) _start = _end = 0;
            return f;
        }
        return null;
    }
}
