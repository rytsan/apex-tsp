using System.Globalization;
using ApexTsp.Ipc;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Media;

namespace ApexTsp.Gui;

internal static class Draw
{
    public static FormattedText Text(string s, double size, IBrush brush, bool bold = false) =>
        new(s, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
            new Typeface(FontFamily.Default, FontStyle.Normal, bold ? FontWeight.SemiBold : FontWeight.Normal), size, brush);

    public static string Num(double v) => double.IsNaN(v) ? "-" : v.ToString(Math.Abs(v) >= 1000 ? "0" : "0.###", CultureInfo.InvariantCulture);

    public static void Triangle(DrawingContext ctx, IBrush b, Point p, bool up, double r = 5)
    {
        var g = new StreamGeometry();
        using (var c = g.Open())
        {
            var s = up ? -1 : 1;
            c.BeginFigure(new Point(p.X, p.Y + s * r), true);
            c.LineTo(new Point(p.X - r, p.Y - s * r * 0.8));
            c.LineTo(new Point(p.X + r, p.Y - s * r * 0.8));
            c.EndFigure(true);
        }
        ctx.DrawGeometry(b, null, g);
    }
}

// UB / LB over time with one marker per solver event (shape = kind, color = source) and the gap
// below (linear or log). Hover a marker for its value and source.
public sealed class BoundsChart : Control
{
    public IReadOnlyList<BoundSample> Samples { get; set; } = [];
    public IReadOnlyList<SolverEvent> Events { get; set; } = [];
    public Palette Pal { get; set; } = Palette.Light;
    public bool LogGap { get; set; } = true;

    private readonly List<(Point P, string Tip)> _marks = [];

    public BoundsChart()
    {
        ClipToBounds = true;
    }

    public override void Render(DrawingContext ctx)
    {
        _marks.Clear();
        double w = Bounds.Width, h = Bounds.Height;
        ctx.FillRectangle(new SolidColorBrush(Pal.Background), new Rect(0, 0, w, h));
        var muted = new SolidColorBrush(Pal.Muted);
        var s = Samples;
        var ev = Events;
        if (s.Count < 1 && ev.Count < 1)
        {
            ctx.DrawText(Draw.Text("bounds appear here while the solver runs", 11, muted), new Point(8, 8));
            return;
        }
        double t0 = double.MaxValue, t1 = double.MinValue;
        foreach (var x in s) { t0 = Math.Min(t0, x.Seconds); t1 = Math.Max(t1, x.Seconds); }
        foreach (var e in ev) { t0 = Math.Min(t0, e.Seconds); t1 = Math.Max(t1, e.Seconds); }
        if (t1 - t0 < 1e-6) t1 = t0 + 1;

        // y range: every UB, and LBs not far below the best UB (early Farley bounds can be tiny).
        double ubMin = double.MaxValue, hi = double.MinValue;
        foreach (var x in s) if (!double.IsNaN(x.Ub)) { ubMin = Math.Min(ubMin, x.Ub); hi = Math.Max(hi, x.Ub); }
        foreach (var e in ev) if (e.Kind == EventKind.UbImproved) { ubMin = Math.Min(ubMin, e.Value); hi = Math.Max(hi, e.Value); }
        if (ubMin == double.MaxValue) { ubMin = 0; hi = 1; }
        var floor = ubMin - Math.Max(1e-9, 0.25 * Math.Abs(ubMin));
        var lo = ubMin;
        foreach (var x in s) if (!double.IsNaN(x.Lb) && x.Lb >= floor) lo = Math.Min(lo, x.Lb);
        foreach (var e in ev) if (e.Kind == EventKind.LbImproved && e.Value >= floor) lo = Math.Min(lo, e.Value);
        // Focus on the band where UB and LB meet: early (construction) UBs far above clamp to the top.
        var lbBest = s.Where(x => !double.IsNaN(x.Lb)).Select(x => x.Lb).DefaultIfEmpty(double.NaN).Max();
        if (!double.IsNaN(lbBest) && lbBest < ubMin) hi = Math.Min(hi, ubMin + 3 * (ubMin - Math.Max(lo, lbBest)));
        if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
        var pad = (hi - lo) * 0.06;
        lo -= pad;
        hi += pad;

        const double left = 64, right = 8, top = 8;
        var plotH = (h - 24) * 0.66;
        var gapTop = top + plotH + 14;
        var gapH = h - gapTop - 14;
        double X(double t) => left + (w - left - right) * (t - t0) / (t1 - t0);
        double Y(double v) => top + plotH * (1 - (Math.Clamp(v, lo, hi) - lo) / (hi - lo));
        var grid = new Pen(new SolidColorBrush(Pal.Border), 1);
        ctx.DrawRectangle(null, grid, new Rect(left, top, w - left - right, plotH));
        ctx.DrawText(Draw.Text(Draw.Num(hi), 10, muted), new Point(4, top));
        ctx.DrawText(Draw.Text(Draw.Num(lo), 10, muted), new Point(4, top + plotH - 12));
        ctx.DrawText(Draw.Text($"{t1 - t0:0.#} s", 10, muted), new Point(w - right - 40, h - 13));

        // phases: dashed verticals with labels
        var phasePen = new Pen(muted, 1, new DashStyle([3, 3], 0));
        var labelEnd = double.MinValue;
        foreach (var e in ev.Where(e => e.Kind == EventKind.Phase))
        {
            var x = X(e.Seconds);
            ctx.DrawLine(phasePen, new Point(x, top), new Point(x, top + plotH));
            var t = Draw.Text(e.Detail, 9, muted);
            if (x + 2 < labelEnd) continue;  // labels never overlap; the line stays
            ctx.DrawText(t, new Point(x + 2, top + 1));
            labelEnd = x + 6 + t.Width;
        }

        void Series(Func<BoundSample, double> f, Color c)
        {
            var g = new StreamGeometry();
            using (var gc = g.Open())
            {
                var open = false;
                double last = double.NaN;
                foreach (var x in s)
                {
                    var v = f(x);
                    if (double.IsNaN(v) || double.IsInfinity(v)) continue;
                    var p = new Point(X(x.Seconds), Y(v));
                    if (!open) { gc.BeginFigure(p, false); open = true; }
                    else gc.LineTo(new Point(p.X, Y(last)));  // step: value holds until it changes
                    gc.LineTo(p);
                    last = v;
                }
                if (open) gc.EndFigure(false);
            }
            ctx.DrawGeometry(null, new Pen(new SolidColorBrush(c), 1.6), g);
        }
        var lbColor = ev.LastOrDefault(e => e.Kind == EventKind.LbImproved)?.Detail == "lagrangian" ? Pal.Lagrangian : Pal.Lp;
        Series(x => x.Ub, Pal.Heuristic);
        Series(x => x.Lb, lbColor);

        foreach (var e in ev)
        {
            switch (e.Kind)
            {
                case EventKind.UbImproved:
                {
                    var p = new Point(X(e.Seconds), Y(e.Value));
                    Draw.Triangle(ctx, new SolidColorBrush(e.Heuristic ? Pal.Heuristic : Pal.Lp), p, up: false);
                    _marks.Add((p, $"UB {Draw.Num(e.Value)} at {e.Seconds:0.###} s\nsource: {e.Detail} ({(e.Heuristic ? "heuristic" : "LP integral")})"));
                    break;
                }
                case EventKind.LbImproved:
                {
                    var p = new Point(X(e.Seconds), Y(e.Value));
                    Draw.Triangle(ctx, new SolidColorBrush(e.Detail == "lagrangian" ? Pal.Lagrangian : Pal.Lp), p, up: true, r: 3.5);
                    _marks.Add((p, $"LB {Draw.Num(e.Value)} at {e.Seconds:0.###} s\nsource: {(e.Detail == "lagrangian" ? "Lagrangian (Held-Karp)" : "LP (Farley / tree)")}"));
                    break;
                }
                case EventKind.Proven:
                {
                    var p = new Point(X(e.Seconds), Y(e.Value));
                    ctx.DrawEllipse(new SolidColorBrush(Pal.Proven), null, p, 6, 6);
                    _marks.Add((p, $"OPTIMAL PROVEN {Draw.Num(e.Value)} at {e.Seconds:0.###} s"));
                    break;
                }
            }
        }

        // gap %
        ctx.DrawRectangle(null, grid, new Rect(left, gapTop, w - left - right, gapH));
        var gaps = s.Where(x => !double.IsNaN(x.GapPct) && x.GapPct >= 0).ToList();
        ctx.DrawText(Draw.Text("gap %" + (LogGap ? " (log)" : ""), 10, muted), new Point(4, gapTop));
        if (gaps.Count > 0)
        {
            double G(double g) => LogGap ? Math.Log10(Math.Max(g, 1e-4)) : g;
            double gHi = gaps.Max(x => G(x.GapPct)), gLo = LogGap ? Math.Min(gaps.Min(x => G(x.GapPct)), -1) : 0;
            if (gHi - gLo < 1e-9) gHi = gLo + 1;
            var geo = new StreamGeometry();
            using (var gc = geo.Open())
            {
                gc.BeginFigure(new Point(X(gaps[0].Seconds), gapTop + gapH * (1 - (G(gaps[0].GapPct) - gLo) / (gHi - gLo))), false);
                foreach (var x in gaps.Skip(1)) gc.LineTo(new Point(X(x.Seconds), gapTop + gapH * (1 - (G(x.GapPct) - gLo) / (gHi - gLo))));
                gc.EndFigure(false);
            }
            ctx.DrawGeometry(null, new Pen(new SolidColorBrush(Pal.Accent), 1.4), geo);
            ctx.DrawText(Draw.Text($"{gaps[^1].GapPct:0.####}%", 10, new SolidColorBrush(Pal.Text), true), new Point(w - right - 70, gapTop + 2));
        }
    }

    protected override void OnPointerMoved(PointerEventArgs e)
    {
        var p = e.GetPosition(this);
        var best = _marks.Where(m => Math.Abs(m.P.X - p.X) < 8 && Math.Abs(m.P.Y - p.Y) < 8)
                         .OrderBy(m => Math.Abs(m.P.X - p.X) + Math.Abs(m.P.Y - p.Y)).Select(m => m.Tip).FirstOrDefault();
        ToolTip.SetTip(this, best);
        ToolTip.SetIsOpen(this, best is not null);
    }

    protected override void OnPointerExited(PointerEventArgs e) => ToolTip.SetIsOpen(this, false);
}

// Cuts per separation round: stacked bars by family + LP value line; red tick = no violated cut.
public sealed class CutsChart : Control
{
    public IReadOnlyList<CutRow> Rounds { get; set; } = [];
    public Palette Pal { get; set; } = Palette.Light;
    public int Window { get; set; } = 80;

    public static readonly Color Sec = Color.Parse("#129EAF"), Blossom = Color.Parse("#D56E9B"),
                                 Component = Color.Parse("#7A8B99"), Pool = Color.Parse("#B8C2CC");

    public CutsChart()
    {
        ClipToBounds = true;
    }

    public override void Render(DrawingContext ctx)
    {
        double w = Bounds.Width, h = Bounds.Height;
        ctx.FillRectangle(new SolidColorBrush(Pal.Background), new Rect(0, 0, w, h));
        var muted = new SolidColorBrush(Pal.Muted);
        var legend = new (string, Color)[] { ("SEC", Sec), ("blossom", Blossom), ("component", Component), ("pool", Pool) };
        var lx = 6.0;
        foreach (var (name, c) in legend)
        {
            ctx.FillRectangle(new SolidColorBrush(c), new Rect(lx, 6, 9, 9));
            var t = Draw.Text(name, 10, muted);
            ctx.DrawText(t, new Point(lx + 12, 3));
            lx += 18 + t.Width;
        }
        var r = Rounds;
        if (r.Count == 0)
        {
            ctx.DrawText(Draw.Text("no separation rounds yet", 11, muted), new Point(6, 24));
            return;
        }
        var view = r.Skip(Math.Max(0, r.Count - Window)).ToList();
        const double top = 22, bottom = 16, left = 6, right = 6;
        var plotH = h - top - bottom;
        var bw = (w - left - right) / Math.Max(Window, view.Count);
        var max = Math.Max(1, view.Max(x => (double)x.Found + x.Pool));
        var red = new SolidColorBrush(Pal.Danger);
        for (var i = 0; i < view.Count; ++i)
        {
            var x = left + i * bw;
            var y = top + plotH;
            foreach (var (v, c) in new[] { (view[i].Sec, Sec), (view[i].Blossom, Blossom), (view[i].Component, Component), (view[i].Pool, Pool) })
            {
                if (v == 0) continue;
                var hh = plotH * v / max;
                y -= hh;
                ctx.FillRectangle(new SolidColorBrush(c), new Rect(x + 0.5, y, Math.Max(1, bw - 1), hh));
            }
            if (view[i].Found + view[i].Pool == 0) ctx.FillRectangle(red, new Rect(x + 0.5, top + plotH - 3, Math.Max(1, bw - 1), 3));
        }
        // LP value (own scale)
        double lo = view.Min(x => x.Lp), hi = view.Max(x => x.Lp);
        if (hi - lo < 1e-9) { lo -= 1; hi += 1; }
        var g = new StreamGeometry();
        using (var gc = g.Open())
        {
            for (var i = 0; i < view.Count; ++i)
            {
                var p = new Point(left + (i + 0.5) * bw, top + plotH * (1 - (view[i].Lp - lo) / (hi - lo)) * 0.9 + plotH * 0.05);
                if (i == 0) gc.BeginFigure(p, false);
                else gc.LineTo(p);
            }
            gc.EndFigure(false);
        }
        ctx.DrawGeometry(null, new Pen(new SolidColorBrush(Pal.Lp), 1.4), g);
        ctx.DrawText(Draw.Text($"rounds {view[0].Round}..{view[^1].Round}   max {max:0} cuts/round   LP {Draw.Num(lo)} .. {Draw.Num(hi)}", 10, muted), new Point(left, h - 14));
    }
}

// Branch-and-cut tree: depth down, leaves left to right in DFS order, color by state.
public sealed class TreePanel : Control
{
    public IReadOnlyDictionary<uint, TreeNodePacket> Nodes { get; set; } = new Dictionary<uint, TreeNodePacket>();
    public Palette Pal { get; set; } = Palette.Light;
    public event Action<TreeNodePacket>? NodeClicked;

    private readonly Dictionary<uint, Point> _pos = [];

    public TreePanel()
    {
        ClipToBounds = true;
    }

    public Color StateColor(TreeNodeState s) => s switch
    {
        TreeNodeState.Branched => Pal.Lp,
        TreeNodeState.Integral => Pal.Proven,
        TreeNodeState.Infeasible => Pal.Danger,
        TreeNodeState.Reopened => Pal.Heuristic,
        TreeNodeState.Pruned => Pal.Muted,
        _ => Pal.Text,  // open
    };

    public override void Render(DrawingContext ctx)
    {
        double w = Bounds.Width, h = Bounds.Height;
        ctx.FillRectangle(new SolidColorBrush(Pal.Background), new Rect(0, 0, w, h));
        var muted = new SolidColorBrush(Pal.Muted);
        _pos.Clear();
        var nodes = Nodes;
        if (nodes.Count == 0)
        {
            ctx.DrawText(Draw.Text("the branch-and-cut tree appears here (exact mode)", 11, muted), new Point(6, 6));
            return;
        }
        var kids = new Dictionary<uint, List<uint>>();
        foreach (var n in nodes.Values)
            if (n.Parent != uint.MaxValue)
            {
                if (!kids.TryGetValue(n.Parent, out var l)) kids[n.Parent] = l = [];
                l.Add(n.Id);
            }
        foreach (var l in kids.Values) l.Sort();
        var roots = nodes.Values.Where(n => n.Parent == uint.MaxValue || !nodes.ContainsKey(n.Parent)).Select(n => n.Id).OrderBy(x => x).ToList();
        // leaf order (iterative DFS), internal x = mean of children
        var x = new Dictionary<uint, double>();
        var depthMax = 0u;
        var leaf = 0;
        var stack = new Stack<(uint Id, bool Done)>();
        foreach (var r in Enumerable.Reverse(roots)) stack.Push((r, false));
        while (stack.Count > 0)
        {
            var (id, done) = stack.Pop();
            depthMax = Math.Max(depthMax, nodes[id].Depth);
            if (!kids.TryGetValue(id, out var ch) || ch.Count == 0) { x[id] = leaf++; continue; }
            if (done) { x[id] = ch.Average(c => x[c]); continue; }
            stack.Push((id, true));
            for (var i = ch.Count - 1; i >= 0; --i) stack.Push((ch[i], false));
        }
        const double pad = 10;
        var sx = (w - 2 * pad) / Math.Max(1, leaf - 1);
        var sy = (h - 2 * pad - 14) / Math.Max(1, depthMax);
        foreach (var (id, v) in x) _pos[id] = new Point(pad + (leaf == 1 ? (w - 2 * pad) / 2 : v * sx), pad + nodes[id].Depth * sy);
        var edge = new Pen(new SolidColorBrush(Pal.Border), 1);
        foreach (var n in nodes.Values)
            if (n.Parent != uint.MaxValue && _pos.TryGetValue(n.Parent, out var pp) && _pos.TryGetValue(n.Id, out var pc))
                ctx.DrawLine(edge, pp, pc);
        var r0 = nodes.Count > 3000 ? 1.5 : nodes.Count > 500 ? 2.5 : 4;
        foreach (var n in nodes.Values)
        {
            if (!_pos.TryGetValue(n.Id, out var p)) continue;
            var st = (TreeNodeState)n.State;
            var c = new SolidColorBrush(StateColor(st));
            if (st == TreeNodeState.Open) ctx.DrawEllipse(null, new Pen(c, 1.2), p, r0, r0);
            else ctx.DrawEllipse(c, null, p, r0, r0);
        }
        var counts = nodes.Values.GroupBy(n => (TreeNodeState)n.State).OrderBy(g => g.Key).Select(g => $"{g.Key.ToString().ToLowerInvariant()} {g.Count()}");
        ctx.DrawText(Draw.Text($"{nodes.Count} nodes · depth {depthMax} · " + string.Join(" · ", counts), 10, muted), new Point(6, h - 14));
    }

    protected override void OnPointerPressed(PointerPressedEventArgs e)
    {
        var p = e.GetPosition(this);
        var hit = _pos.OrderBy(kv => Math.Abs(kv.Value.X - p.X) + Math.Abs(kv.Value.Y - p.Y)).FirstOrDefault();
        if (_pos.Count > 0 && Math.Abs(hit.Value.X - p.X) + Math.Abs(hit.Value.Y - p.Y) < 12 && Nodes.TryGetValue(hit.Key, out var n))
            NodeClicked?.Invoke(n);
    }

    public static string Describe(TreeNodePacket n)
    {
        var where = n.Parent == uint.MaxValue ? " root" : FormattableString.Invariant($" parent {n.Parent}, branch x[{n.BranchU},{n.BranchV}] = {n.BranchDir}");
        return FormattableString.Invariant($"node {n.Id} ({((TreeNodeState)n.State).ToString().ToLowerInvariant()}) depth {n.Depth}{where}\nbound {Draw.Num(n.Bound)}  LP {Draw.Num(n.LpValue)}  cuts SEC {n.CutsSec} blossom {n.CutsBlossom} component {n.CutsComponent}");
    }
}
