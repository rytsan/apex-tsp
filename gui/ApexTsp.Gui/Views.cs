using System.Diagnostics;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Media.Imaging;
using Avalonia.Platform;
using SkiaSharp;

namespace ApexTsp.Gui;

// Map canvas. CPU render path (MASTER §3 fallback): TourRenderer (SkiaSharp) draws into a
// WriteableBitmap, so the same code runs with or without GPU and under NativeAOT.
public sealed class MapView : Control
{
    public Scene? Scene { get; set; }
    public Viewport View;
    public TourRenderer Renderer { get; } = new();
    public RenderStats Last { get; private set; }
    public List<double> FrameMs { get; } = [];
    public List<double> InputLatencyMs { get; } = [];
    public Dictionary<LodLevel, int> LevelFrames { get; } = [];

    private WriteableBitmap? _bmp;
    private bool _dirty = true, _fitted;
    private long _inputTicks;          // first input event not rendered yet (Stopwatch ticks)
    private Point? _drag;

    public MapView()
    {
        ClipToBounds = true;
        Focusable = true;
    }

    public void Invalidate()
    {
        _dirty = true;
        InvalidateVisual();
    }

    public void Fit()
    {
        if (Scene is null || Bounds.Width < 2) return;
        View = Viewport.Fit(Scene, (int)Bounds.Width, (int)Bounds.Height);
        _fitted = true;
        Invalidate();
    }

    // Input entry points (pointer handlers and the demo script use the same path).
    public void PanBy(double dx, double dy)
    {
        MarkInput();
        View.Pan(dx, dy);
        Invalidate();
    }

    public void ZoomAt(double factor, double x, double y)
    {
        MarkInput();
        View.Zoom(factor, x, y);
        Invalidate();
    }

    private void MarkInput()
    {
        if (_inputTicks == 0) _inputTicks = Stopwatch.GetTimestamp();
    }

    public override void Render(DrawingContext context)
    {
        var w = Math.Max(1, (int)Bounds.Width);
        var h = Math.Max(1, (int)Bounds.Height);
        if (Scene is null) return;
        if (!_fitted) { View = Viewport.Fit(Scene, w, h); _fitted = true; }
        if (_bmp is null || _bmp.PixelSize.Width != w || _bmp.PixelSize.Height != h)
        {
            _bmp?.Dispose();
            _bmp = new WriteableBitmap(new PixelSize(w, h), new Vector(96, 96), PixelFormat.Bgra8888, AlphaFormat.Premul);
            _dirty = true;
        }
        View.Width = w;
        View.Height = h;
        if (_dirty)
        {
            using (var fb = _bmp.Lock())
            {
                var info = new SKImageInfo(w, h, SKColorType.Bgra8888, SKAlphaType.Premul);
                using var surface = SKSurface.Create(info, fb.Address, fb.RowBytes);
                Last = Renderer.Render(surface.Canvas, Scene, View);
            }
            FrameMs.Add(Last.Ms);
            LevelFrames[Last.Level] = LevelFrames.GetValueOrDefault(Last.Level) + 1;
            if (_inputTicks != 0)
            {
                InputLatencyMs.Add(Stopwatch.GetElapsedTime(_inputTicks).TotalMilliseconds);
                _inputTicks = 0;
            }
            _dirty = false;
        }
        context.DrawImage(_bmp, new Rect(0, 0, w, h));
        if (HoverText is { } t)
        {
            var ft = new FormattedText(t, System.Globalization.CultureInfo.InvariantCulture, FlowDirection.LeftToRight, Typeface.Default, 11, Brushes.White);
            context.FillRectangle(new SolidColorBrush(Color.FromArgb(200, 0x20, 0x23, 0x28)), new Rect(6, 6, ft.Width + 12, ft.Height + 8), 4);
            context.DrawText(ft, new Point(12, 10));
            if (_hoverPt is { } hp) context.DrawEllipse(null, new Pen(Brushes.OrangeRed, 2), hp, 6, 6);
        }
    }

    // Hover: nearest node within 8 px -> id (TSPLIB, 1-based), coordinates, tour neighbours and
    // the lengths of its two tour edges.
    public string? HoverText { get; private set; }
    private Point? _hoverPt;

    private void UpdateHover(Point p)
    {
        string? text = null;
        _hoverPt = null;
        if (Scene is { } s && s.Count > 0)
        {
            var best = -1;
            var bd = 64.0;
            for (var i = 0; i < s.Count; ++i)
            {
                double dx = View.Sx(s.X[i]) - p.X, dy = View.Sy(s.Y[i]) - p.Y, d = dx * dx + dy * dy;
                if (d < bd) { bd = d; best = i; }
            }
            if (best >= 0)
            {
                var u = (uint)best;
                var ci = System.Globalization.CultureInfo.InvariantCulture;
                double x = s.XD?[u] ?? s.X[u], y = s.YD?[u] ?? s.Y[u];
                text = string.Create(ci, $"node {u + 1}  ({x:0.###}, {y:0.###})");
                if (s.TourNeighbors(u) is var (a, b))
                    text += string.Create(ci, $"\ntour: {a + 1} —{s.Dist(a, u)}— {u + 1} —{s.Dist(u, b)}— {b + 1}");
                _hoverPt = new Point(View.Sx(s.X[u]), View.Sy(s.Y[u]));
            }
        }
        if (text != HoverText) { HoverText = text; InvalidateVisual(); }
    }

    protected override void OnPointerExited(PointerEventArgs e)
    {
        HoverText = null;
        InvalidateVisual();
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        if (e.ClickCount == 2) { Fit(); return; }
        _drag = e.GetPosition(this);
        e.Pointer.Capture(this);
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        if (_drag is not { } d) { UpdateHover(e.GetPosition(this)); return; }
        var p = e.GetPosition(this);
        PanBy(p.X - d.X, p.Y - d.Y);
        _drag = p;
    }

    protected override void OnPointerReleased(PointerReleasedEventArgs e)
    {
        _drag = null;
        e.Pointer.Capture(null);
    }

    protected override void OnPointerWheelChanged(PointerWheelEventArgs e)
    {
        var p = e.GetPosition(this);
        ZoomAt(Math.Pow(1.2, e.Delta.Y), p.X, p.Y);
    }

    // Offscreen copy of the current view (screenshots).
    public void SavePng(string path)
    {
        if (Scene is null) return;
        using var bmp = new SKBitmap(View.Width, View.Height);
        using (var canvas = new SKCanvas(bmp)) Renderer.Render(canvas, Scene, View);
        using var data = bmp.Encode(SKEncodedImageFormat.Png, 90);
        using var f = File.Create(path);
        data.SaveTo(f);
    }
}
