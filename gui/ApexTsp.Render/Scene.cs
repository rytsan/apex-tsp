using System.Globalization;

namespace ApexTsp.Render;

// Node coordinates (file order = engine node index) + current tour + cluster overlay.
public sealed class Scene
{
    public float[] X { get; }
    public float[] Y { get; }
    public int Count => X.Length;
    public float MinX { get; }
    public float MinY { get; }
    public float MaxX { get; }
    public float MaxY { get; }

    // Original coordinates (double) and metric, for tour lengths: the engine's EUC_2D / CEIL_2D
    // rules (exact sqrt on doubles, nint / ceil).
    public double[]? XD { get; init; }
    public double[]? YD { get; init; }
    public bool Ceil2D { get; init; }

    public long TourLength(ReadOnlySpan<uint> t)
    {
        long sum = 0;
        for (var i = 0; i < t.Length; ++i)
        {
            uint a = t[i], b = t[(i + 1) % t.Length];
            double dx = XD is null ? X[a] - X[b] : XD[a] - XD[b], dy = YD is null ? Y[a] - Y[b] : YD[a] - YD[b];
            var d = Math.Sqrt(dx * dx + dy * dy);
            sum += Ceil2D ? (long)Math.Ceiling(d) : (long)Math.Floor(d + 0.5);
        }
        return sum;
    }

    public uint[] Tour { get; private set; } = [];
    public int TourCount { get; private set; }
    public IReadOnlyList<ClusterBox> Clusters => _clusters;

    // Tour history overlays (M6.3b): earlier tours drawn in their own colors; Removed / Added are
    // the edges the last improvement cut out of / brought into the tour.
    public List<TourLayer> Layers { get; } = [];
    public (uint A, uint B)[] Removed { get; private set; } = [];
    public (uint A, uint B)[] Added { get; private set; } = [];

    public void SetDiff(uint[] previous, int prevCount, uint[] current, int count)
    {
        var a = EdgeSet(previous, prevCount);
        var b = EdgeSet(current, count);
        Removed = [.. a.Where(e => !b.Contains(e)).Select(Split)];
        Added = [.. b.Where(e => !a.Contains(e)).Select(Split)];
    }

    public void ClearDiff()
    {
        Removed = [];
        Added = [];
    }

    private static HashSet<ulong> EdgeSet(uint[] t, int m)
    {
        var h = new HashSet<ulong>(m);
        for (var i = 0; i < m; ++i) h.Add(Key(t[i], t[(i + 1) % m]));
        return h;
    }

    private static (uint, uint) Split(ulong k) => ((uint)(k >> 32), (uint)k);

    // Current LP support (exact mode, M6.3): edges with x > 0; New marks edges absent from the
    // previous LP (flashed briefly so the change between rounds is visible).
    public LpSegment[] Lp { get; private set; } = [];
    public bool[] LpNew { get; private set; } = [];
    public bool LpFlash { get; set; }
    public int LpVersion { get; private set; }

    public void SetLp(LpSegment[] lp)
    {
        var prev = new HashSet<ulong>(Lp.Length);
        foreach (var e in Lp) prev.Add(Key(e.U, e.V));
        var fresh = new bool[lp.Length];
        for (var i = 0; i < lp.Length; ++i) fresh[i] = Lp.Length > 0 && !prev.Contains(Key(lp[i].U, lp[i].V));
        Lp = lp;
        LpNew = fresh;
        ++LpVersion;
    }

    private static ulong Key(uint a, uint b) => a < b ? (ulong)a << 32 | b : (ulong)b << 32 | a;
    private readonly List<ClusterBox> _clusters = [];

    public Scene(float[] x, float[] y)
    {
        if (x.Length != y.Length) throw new ArgumentException("x/y length mismatch");
        X = x;
        Y = y;
        MinX = MinY = float.MaxValue;
        MaxX = MaxY = float.MinValue;
        for (var i = 0; i < x.Length; ++i)
        {
            MinX = Math.Min(MinX, x[i]); MaxX = Math.Max(MaxX, x[i]);
            MinY = Math.Min(MinY, y[i]); MaxY = Math.Max(MaxY, y[i]);
        }
    }

    // tour: node indices (count valid entries); indices >= Count are ignored when drawing.
    public void SetTour(uint[] tour, int count)
    {
        Tour = tour;
        TourCount = count;
    }

    public void SetCluster(uint id, float x0, float y0, float x1, float y1)
    {
        foreach (var c in _clusters)
            if (c.Id == id) { c.X0 = x0; c.Y0 = y0; c.X1 = x1; c.Y1 = y1; return; }
        _clusters.Add(new ClusterBox { Id = id, X0 = x0, Y0 = y0, X1 = x1, Y1 = y1 });
    }

    public void SetClusterState(uint id, byte state)
    {
        foreach (var c in _clusters)
            if (c.Id == id) { c.State = state; return; }
    }

    // NODE_COORD_SECTION of a TSPLIB file (any 2D metric).
    public static Scene LoadTsplib(string path)
    {
        var xs = new List<double>();
        var ys = new List<double>();
        var ceil = false;
        var inCoords = false;
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            if (!inCoords)
            {
                if (line.StartsWith("EDGE_WEIGHT_TYPE", StringComparison.Ordinal)) ceil = line.Contains("CEIL_2D", StringComparison.Ordinal);
                if (line.StartsWith("NODE_COORD_SECTION", StringComparison.Ordinal)) inCoords = true;
                continue;
            }
            if (line == "EOF") break;
            var parts = line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 3) break;
            xs.Add(double.Parse(parts[1], NumberStyles.Float, CultureInfo.InvariantCulture));
            ys.Add(double.Parse(parts[2], NumberStyles.Float, CultureInfo.InvariantCulture));
        }
        if (xs.Count == 0) throw new InvalidDataException(path + ": no NODE_COORD_SECTION");
        return new Scene([.. xs.Select(v => (float)v)], [.. ys.Select(v => (float)v)]) { XD = [.. xs], YD = [.. ys], Ceil2D = ceil };
    }
}

public readonly record struct LpSegment(uint U, uint V, float X);

public sealed record TourLayer(uint[] Tour, int Count, SkiaSharp.SKColor Color);

public sealed class ClusterBox
{
    public uint Id;
    public float X0, Y0, X1, Y1;
    public byte State;  // 0 idle, 1 solving, 2 patching
}

// World -> screen: screen = (world - center) * scale + size / 2, y flipped (TSPLIB y grows up).
public struct Viewport
{
    public double CenterX, CenterY, Scale;
    public int Width, Height;

    public static Viewport Fit(Scene s, int width, int height, double margin = 0.03)
    {
        var w = Math.Max(1e-9, s.MaxX - s.MinX);
        var h = Math.Max(1e-9, s.MaxY - s.MinY);
        var scale = Math.Min(width / w, height / h) * (1 - 2 * margin);
        return new Viewport { CenterX = (s.MinX + s.MaxX) / 2.0, CenterY = (s.MinY + s.MaxY) / 2.0, Scale = scale, Width = width, Height = height };
    }

    public readonly float Sx(float x) => (float)((x - CenterX) * Scale + Width / 2.0);
    public readonly float Sy(float y) => (float)(Height / 2.0 - (y - CenterY) * Scale);

    public void Pan(double dxPixels, double dyPixels)
    {
        CenterX -= dxPixels / Scale;
        CenterY += dyPixels / Scale;
    }

    // Zoom by factor around a screen point (the point stays under the cursor).
    public void Zoom(double factor, double px, double py)
    {
        var wx = (px - Width / 2.0) / Scale + CenterX;
        var wy = (Height / 2.0 - py) / Scale + CenterY;
        Scale *= factor;
        CenterX = wx - (px - Width / 2.0) / Scale;
        CenterY = wy - (Height / 2.0 - py) / Scale;
    }
}
