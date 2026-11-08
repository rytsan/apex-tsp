using System.Diagnostics;
using SkiaSharp;

namespace ApexTsp.Render;

// LOD levels, from close to far: every node and tour edge; one point per pixel and every k-th
// tour vertex; grid cells aggregated; density heatmap.
public enum LodLevel { Raw, Decimated, Clusters, Heatmap }

public readonly record struct RenderStats(LodLevel Level, int Visible, int Points, int Segments, double Ms);

// SkiaSharp renderer of a Scene (M5.3). Raw drawing only while at most RawLimit nodes are on
// screen; beyond that the level follows the on-screen density (visible nodes per pixel).
public sealed class TourRenderer
{
    public int RawLimit { get; set; } = 50_000;
    public double DecimatedMaxDensity { get; set; } = 0.25;
    public double ClustersMaxDensity { get; set; } = 2.0;
    public int ClusterCellPx { get; set; } = 6;
    public int HeatCellPx { get; set; } = 12;
    public bool ClusterOverlay { get; set; } = true;   // CLUSTER_MAP heatmap overlay
    public LodLevel? Force { get; set; }
    // Theme colors (the app switches them with the light / dark theme).
    public SKColor Background { get; set; } = SKColors.White;
    public SKColor TourColor { get => _tourPaint.Color; set => _tourPaint.Color = value; }
    public SKColor NodeColor { get => _nodePaint.Color; set => _nodePaint.Color = value; }
    // Layers (M6.3).
    public bool ShowTour { get; set; } = true;
    public bool ShowNodes { get; set; } = true;
    public bool ShowLp { get; set; } = true;
    public bool ShowDiff { get; set; } = true;
    public bool ShowLabels { get; set; } = true;   // node ids while at most LabelLimit nodes are visible
    public int LabelLimit { get; set; } = 400;
    private readonly SKPaint _labelPaint = new() { TextSize = 10, IsAntialias = true };
    public SKColor RemovedColor { get; set; } = new(0xD9, 0x30, 0x25);
    public SKColor AddedColor { get; set; } = new(0x1E, 0x8E, 0x3E);
    private readonly SKPaint _layerPaint = new() { Style = SKPaintStyle.Stroke, StrokeWidth = 0, IsAntialias = false };
    public SKColor LpIntegralColor { get; set; } = new(0x1A, 0x73, 0xE8);
    public SKColor LpFractionalColor { get; set; } = new(0xD9, 0x30, 0x25);
    public SKColor LpNewColor { get; set; } = new(0xF9, 0xAB, 0x00);
    private readonly SKPaint _lpPaint = new() { Style = SKPaintStyle.Stroke, IsAntialias = true, StrokeCap = SKStrokeCap.Round };

    private float[] _sx = [], _sy = [];
    private bool[] _vis = [];
    private SKPoint[] _pts = [];
    private int[] _cells = [];
    private readonly SKPaint _tourPaint = new() { Color = new SKColor(0x2B, 0x7B, 0xE4), StrokeWidth = 0, IsAntialias = false, Style = SKPaintStyle.Stroke };
    private readonly SKPaint _nodePaint = new() { Color = new SKColor(0x20, 0x20, 0x20), StrokeWidth = 2, IsAntialias = false };
    private readonly SKPaint _fill = new() { Style = SKPaintStyle.Fill, IsAntialias = false };
    private readonly SKPaint _boxPaint = new() { Style = SKPaintStyle.Stroke, StrokeWidth = 0, IsAntialias = false };

    public LodLevel Choose(int visible, int width, int height)
    {
        if (Force is { } f) return f;
        if (visible <= RawLimit) return LodLevel.Raw;
        var density = visible / Math.Max(1.0, (double)width * height);
        if (density <= DecimatedMaxDensity) return LodLevel.Decimated;
        return density <= ClustersMaxDensity ? LodLevel.Clusters : LodLevel.Heatmap;
    }

    public RenderStats Render(SKCanvas canvas, Scene s, in Viewport v)
    {
        var sw = Stopwatch.StartNew();
        canvas.Clear(Background);
        var n = s.Count;
        if (_sx.Length < n) { _sx = new float[n]; _sy = new float[n]; _vis = new bool[n]; _pts = new SKPoint[n]; }
        int W = v.Width, H = v.Height, visible = 0;
        for (var i = 0; i < n; ++i)
        {
            var x = v.Sx(s.X[i]);
            var y = v.Sy(s.Y[i]);
            _sx[i] = x;
            _sy[i] = y;
            var on = x >= 0 && y >= 0 && x < W && y < H;
            _vis[i] = on;
            if (on) ++visible;
        }
        var level = Choose(visible, W, H);
        int points = 0, segments = 0;
        switch (level)
        {
            case LodLevel.Raw:
                foreach (var layer in s.Layers) { _layerPaint.Color = layer.Color; segments += DrawTour(canvas, layer.Tour, layer.Count, s.Count, 1, W, H, _layerPaint); }
                if (ShowTour) segments += DrawTour(canvas, s, 1, W, H);
                if (ShowDiff) segments += DrawDiff(canvas, s, W, H, detail: true);
                if (ShowLp) segments += DrawLp(canvas, s, W, H, fractionalOnly: false);
                if (ShowNodes)
                {
                    for (var i = 0; i < n; ++i)
                        if (_vis[i]) _pts[points++] = new SKPoint(_sx[i], _sy[i]);
                    canvas.DrawPoints(SKPointMode.Points, _pts.AsSpan(0, points).ToArray(), _nodePaint);
                }
                if (ShowLabels && visible <= LabelLimit)
                {
                    _labelPaint.Color = NodeColor.WithAlpha(170);
                    for (var i = 0; i < n; ++i)
                        if (_vis[i]) canvas.DrawText((i + 1).ToString(System.Globalization.CultureInfo.InvariantCulture), _sx[i] + 3, _sy[i] - 3, _labelPaint);
                }
                break;
            case LodLevel.Decimated:
            {
                var k = (int)Math.Ceiling((double)visible / RawLimit);
                foreach (var layer in s.Layers) { _layerPaint.Color = layer.Color; segments += DrawTour(canvas, layer.Tour, layer.Count, s.Count, k, W, H, _layerPaint); }
                if (ShowTour) segments += DrawTour(canvas, s, k, W, H);
                if (ShowDiff) segments += DrawDiff(canvas, s, W, H, detail: false);
                if (ShowLp) segments += DrawLp(canvas, s, W, H, fractionalOnly: true);
                if (!ShowNodes) break;
                var occ = Cells(W * H);
                for (var i = 0; i < n; ++i)
                {
                    if (!_vis[i]) continue;
                    var c = (int)_sy[i] * W + (int)_sx[i];
                    if (occ[c]++ == 0) _pts[points++] = new SKPoint(_sx[i], _sy[i]);
                }
                canvas.DrawPoints(SKPointMode.Points, _pts.AsSpan(0, points).ToArray(), _nodePaint);
                break;
            }
            case LodLevel.Clusters:
                points = DrawCells(canvas, n, ClusterCellPx, W, H, heat: false);
                segments = DrawCellTour(canvas, s, ClusterCellPx, W, H);
                break;
            case LodLevel.Heatmap:
                points = DrawCells(canvas, n, HeatCellPx, W, H, heat: true);
                break;
        }
        if (ClusterOverlay) DrawClusterBoxes(canvas, s, v);
        canvas.Flush();
        return new RenderStats(level, visible, points, segments, sw.Elapsed.TotalMilliseconds);
    }

    // LP support: x = 1 hairline in the integral color; 0 < x < 1 in four buckets (opacity, and
    // width while few, grow with x); edges new since the previous LP drawn on top while LpFlash is
    // set. Wide and antialiased strokes only while the visible count stays small (frame budget).
    public int LpDetailLimit { get; set; } = 3000;
    private readonly List<SKPoint>[] _lpBuckets = [.. Enumerable.Range(0, 6).Select(_ => new List<SKPoint>())];

    private int DrawLp(SKCanvas canvas, Scene s, int W, int H, bool fractionalOnly)
    {
        var lp = s.Lp;
        if (lp.Length == 0) return 0;
        // buckets: 0 = x = 1, 1..4 = fractional by x quarter, 5 = new since the previous LP
        foreach (var l in _lpBuckets) l.Clear();
        for (var i = 0; i < lp.Length; ++i)
        {
            var e = lp[i];
            if (e.U >= s.Count || e.V >= s.Count || !SegmentVisible(e.U, e.V, W, H)) continue;
            var integral = e.X >= 0.999f;
            var isNew = s.LpFlash && i < s.LpNew.Length && s.LpNew[i];
            if (integral && fractionalOnly && !isNew) continue;
            var b = isNew ? 5 : integral ? 0 : 1 + Math.Clamp((int)(e.X * 4), 0, 3);
            _lpBuckets[b].Add(new SKPoint(_sx[e.U], _sy[e.U]));
            _lpBuckets[b].Add(new SKPoint(_sx[e.V], _sy[e.V]));
        }
        var frac = _lpBuckets[1].Count + _lpBuckets[2].Count + _lpBuckets[3].Count + _lpBuckets[4].Count;
        void Stroke(int b, SKColor c, float wide, int count)
        {
            var pts = _lpBuckets[b];
            if (pts.Count == 0) return;
            var detail = count / 2 <= LpDetailLimit;
            _lpPaint.IsAntialias = detail;
            _lpPaint.StrokeWidth = detail ? wide : 0;
            _lpPaint.Color = c;
            canvas.DrawPoints(SKPointMode.Lines, [.. pts], _lpPaint);
        }
        Stroke(0, LpIntegralColor, 1.2f, _lpBuckets[0].Count);
        for (var q = 0; q < 4; ++q) Stroke(1 + q, LpFractionalColor.WithAlpha((byte)(110 + 45 * q)), 1.2f + q * 0.7f, frac);
        Stroke(5, LpNewColor, 3f, _lpBuckets[5].Count);
        return _lpBuckets.Sum(l => l.Count) / 2;
    }

    private int[] Cells(int size)
    {
        if (_cells.Length < size) _cells = new int[size];
        else Array.Clear(_cells, 0, size);
        return _cells;
    }

    // Edges cut out of the tour by the last improvement (red, dashed) and brought in (green).
    private readonly SKPaint _diffPaint = new() { Style = SKPaintStyle.Stroke, IsAntialias = true, StrokeCap = SKStrokeCap.Round };

    private int DrawDiff(SKCanvas canvas, Scene s, int W, int H, bool detail)
    {
        var drawn = 0;
        void Set((uint A, uint B)[] edges, SKColor c, bool dashed)
        {
            if (edges.Length == 0) return;
            var pts = new List<SKPoint>(edges.Length * 2);
            foreach (var (a, b) in edges)
            {
                if (a >= s.Count || b >= s.Count || !SegmentVisible(a, b, W, H)) continue;
                pts.Add(new SKPoint(_sx[a], _sy[a]));
                pts.Add(new SKPoint(_sx[b], _sy[b]));
            }
            var wide = detail && pts.Count / 2 <= LpDetailLimit;
            _diffPaint.StrokeWidth = wide ? 2.4f : 0;
            _diffPaint.IsAntialias = wide;
            _diffPaint.Color = c;
            using var dash = dashed && wide ? SKPathEffect.CreateDash([6, 4], 0) : null;
            _diffPaint.PathEffect = dash;
            canvas.DrawPoints(SKPointMode.Lines, [.. pts], _diffPaint);
            _diffPaint.PathEffect = null;
            drawn += pts.Count / 2;
        }
        Set(s.Removed, RemovedColor, dashed: true);
        Set(s.Added, AddedColor, dashed: false);
        return drawn;
    }

    // Tour as one path through every k-th vertex; segments entirely off screen are skipped.
    private int DrawTour(SKCanvas canvas, Scene s, int k, int W, int H) =>
        DrawTour(canvas, s.Tour, s.TourCount, s.Count, k, W, H, _tourPaint);

    private int DrawTour(SKCanvas canvas, uint[] tour, int count, int nodes, int k, int W, int H, SKPaint paint)
    {
        var m = Math.Min(count, tour.Length);
        if (m < 2) return 0;
        using var path = new SKPath();
        var segments = 0;
        uint prev = uint.MaxValue;
        var pen = false;
        for (var j = 0; j <= m; j += k)
        {
            var node = tour[j == m ? 0 : j];
            if (node >= nodes) { pen = false; continue; }
            if (prev != uint.MaxValue && SegmentVisible(prev, node, W, H))
            {
                if (!pen) path.MoveTo(_sx[prev], _sy[prev]);
                path.LineTo(_sx[node], _sy[node]);
                pen = true;
                ++segments;
            }
            else pen = false;
            prev = node;
            if (j < m && j + k > m) j = m - k;  // close the cycle through the first vertex
        }
        canvas.DrawPath(path, paint);
        return segments;
    }

    private bool SegmentVisible(uint a, uint b, int W, int H)
    {
        if (_vis[a] || _vis[b]) return true;
        float x0 = _sx[a], x1 = _sx[b], y0 = _sy[a], y1 = _sy[b];
        return !((x0 < 0 && x1 < 0) || (y0 < 0 && y1 < 0) || (x0 >= W && x1 >= W) || (y0 >= H && y1 >= H));
    }

    // Aggregated grid: square per non-empty cell, opacity (clusters) or color (heatmap) by count.
    private int DrawCells(SKCanvas canvas, int n, int cell, int W, int H, bool heat)
    {
        int cw = (W + cell - 1) / cell, ch = (H + cell - 1) / cell;
        var counts = Cells(cw * ch);
        var max = 1;
        for (var i = 0; i < n; ++i)
        {
            if (!_vis[i]) continue;
            var c = (int)_sy[i] / cell * cw + (int)_sx[i] / cell;
            max = Math.Max(max, ++counts[c]);
        }
        var logMax = Math.Log(1 + max);
        var drawn = 0;
        for (var c = 0; c < cw * ch; ++c)
        {
            if (counts[c] == 0) continue;
            var t = Math.Log(1 + counts[c]) / logMax;
            _fill.Color = heat ? Ramp(t) : NodeColor.WithAlpha((byte)(60 + 195 * t));
            canvas.DrawRect(c % cw * cell, c / cw * cell, cell, cell, _fill);
            ++drawn;
        }
        return drawn;
    }

    // Tour through the sequence of grid cells it visits (consecutive repeats merged).
    private int DrawCellTour(SKCanvas canvas, Scene s, int cell, int W, int H)
    {
        var m = Math.Min(s.TourCount, s.Tour.Length);
        if (m < 2) return 0;
        using var path = new SKPath();
        int last = -1, segments = 0;
        var pen = false;
        int cw = (W + cell - 1) / cell;
        for (var j = 0; j <= m; ++j)
        {
            var node = s.Tour[j == m ? 0 : j];
            if (node >= s.Count || !_vis[node]) { pen = false; last = -1; continue; }
            var c = (int)_sy[node] / cell * cw + (int)_sx[node] / cell;
            if (c == last) continue;
            float x = c % cw * cell + cell / 2f, y = c / cw * cell + cell / 2f;
            if (!pen) { path.MoveTo(x, y); pen = true; }
            else { path.LineTo(x, y); ++segments; }
            last = c;
        }
        canvas.DrawPath(path, _tourPaint);
        return segments;
    }

    private void DrawClusterBoxes(SKCanvas canvas, Scene s, in Viewport v)
    {
        foreach (var b in s.Clusters)
        {
            if (b.State == 0) continue;
            float x0 = v.Sx(b.X0), x1 = v.Sx(b.X1), y0 = v.Sy(b.Y1), y1 = v.Sy(b.Y0);
            var col = b.State == 1 ? new SKColor(0xFF, 0x98, 0x00) : new SKColor(0xE5, 0x39, 0x35);
            _fill.Color = col.WithAlpha(50);
            canvas.DrawRect(x0, y0, x1 - x0, y1 - y0, _fill);
            _boxPaint.Color = col;
            canvas.DrawRect(x0, y0, x1 - x0, y1 - y0, _boxPaint);
        }
    }

    private static SKColor Ramp(double t)  // blue -> yellow -> red
    {
        t = Math.Clamp(t, 0, 1);
        return t < 0.5
            ? new SKColor((byte)(510 * t), (byte)(510 * t), (byte)(255 * (1 - 2 * t)))
            : new SKColor(255, (byte)(255 * (2 - 2 * t)), 0);
    }
}
