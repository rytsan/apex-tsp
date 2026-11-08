using System.Globalization;
using ApexTsp.Ipc;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;

namespace ApexTsp.Gui;

public sealed record LabStepRow(int Index, string Name, long Before, long After, long Moves, double Ms)
{
    public long Delta => After - Before;

    // "lab_step name=two_opt heuristic=true before=320315 after=278589 delta=-41726 moves=247 ms=0.269 best=278589"
    public static LabStepRow? Parse(string line, int index)
    {
        if (!line.StartsWith("lab_step ", StringComparison.Ordinal)) return null;
        var kv = line.Split(' ', StringSplitOptions.RemoveEmptyEntries).Select(t => t.Split('=', 2)).Where(p => p.Length == 2)
                     .ToDictionary(p => p[0], p => p[1]);
        long L(string k) => kv.TryGetValue(k, out var v) && long.TryParse(v, NumberStyles.Integer, CultureInfo.InvariantCulture, out var x) ? x : 0;
        var ms = kv.TryGetValue("ms", out var m) && double.TryParse(m, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) ? d : 0;
        return kv.TryGetValue("name", out var name) ? new LabStepRow(index, name, L("before"), L("after"), L("moves"), ms) : null;
    }

    public string Line() => string.Create(CultureInfo.InvariantCulture,
        $"#{Index,-3} {Name,-14} {Before,10} -> {After,10}  {Delta,+11} ({(Before > 0 ? 100.0 * Delta / Before : 0),7:+0.00;-0.00}%)  moves {Moves,-7} {Ms,9:0.0} ms");
}

// Heuristics lab tab (M6.4): an engine in --mode lab runs one primal step per button press on
// the current tour; every tour is streamed (shared mapping) and kept in the tour history.
public sealed partial class MainWindow
{
    private readonly MapView _labMap = new();
    private readonly List<LabStepRow> _labSteps = [];
    private readonly ListBox _labList = new() { FontFamily = new FontFamily("monospace"), FontSize = 11 };
    private readonly TextBlock _labStatus = new() { TextWrapping = TextWrapping.Wrap };
    private readonly NumericUpDown _nnStart = new() { Minimum = 0, Value = 0, Increment = 1, FormatString = "0", Width = 110 };
    private readonly NumericUpDown _ilsSeconds = new() { Minimum = 0.1m, Maximum = 86400, Value = 5, Increment = 1, FormatString = "0.#", Width = 110 };
    private readonly TextBox _chain = new() { Text = "greedy two_opt_oropt lk ils 5", MinWidth = 220 };
    private readonly Button _labStart = new() { Content = "Start lab engine" }, _labStop = new() { Content = "Stop" };
    private readonly List<Button> _labButtons = [];
    private readonly Queue<string> _chainQueue = new();
    private bool _labBusy;

    public bool LabActive => _run is { Running: true } && _vm.Mode == "lab" && !_vm.Finished;

    private Control BuildLabTab()
    {
        Button Step(string text, string tip, Func<string> cmd)
        {
            var b = new Button { Content = text, Margin = new Thickness(0, 0, 6, 6), Padding = new Thickness(10, 5) };
            ToolTip.SetTip(b, tip);
            b.Click += (_, _) => LabSend(cmd());
            _labButtons.Add(b);
            return b;
        }
        _labStart.Click += (_, _) => StartLab();
        _labStop.Click += (_, _) => LabSend("quit");
        var runChain = new Button { Content = "Run chain" };
        runChain.Click += (_, _) => _ = RunChain(_chain.Text ?? "");
        _labButtons.Add(runChain);
        var side = new StackPanel { Spacing = 10, Margin = new Thickness(12, 0, 12, 12) };
        side.Children.AddRange([
            Card("Lab engine", new StackPanel { Spacing = 6, Children = { new WrapPanel { Children = { _labStart, new Border { Width = 6 }, _labStop } }, _labStatus } }),
            Card("Construct (replaces the tour)", new StackPanel
            {
                Children =
                {
                    new WrapPanel
                    {
                        Children =
                        {
                            Step("Nearest neighbor", "nearest unvisited node, candidate graph first", () => $"nn {(int)(_nnStart.Value ?? 0)}"),
                            Step("Greedy edge", "shortest edges first, degree <= 2, no cycles; fragments joined", () => "greedy"),
                            Step("Space-filling curve", "Hilbert order", () => "hilbert"),
                            Step("Random", "random permutation (to watch local search repair it)", () => "random"),
                            Step("Decomposition", "density clusters solved and merged (the scalable construction)", () => "decomp"),
                        },
                    },
                    new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { new TextBlock { Text = "NN start node", VerticalAlignment = VerticalAlignment.Center }, _nnStart } },
                },
            }),
            Card("Improve (from the current tour)", new StackPanel
            {
                Children =
                {
                    new WrapPanel
                    {
                        Children =
                        {
                            Step("2-opt", "2-opt on the candidate graph", () => "two_opt"),
                            Step("2-opt + Or-opt", "2-opt and segment moves of 1..3 nodes", () => "two_opt_oropt"),
                            Step("LK (Or-3opt)", "LK-lite chains up to depth 5 + Or-opt", () => "lk"),
                            Step("ILS", "iterated local search: double-bridge kicks + LK repair, for the seconds below", () => string.Create(CultureInfo.InvariantCulture, $"ils {(double)(_ilsSeconds.Value ?? 5)}")),
                        },
                    },
                    new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6, Children = { new TextBlock { Text = "ILS seconds", VerticalAlignment = VerticalAlignment.Center }, _ilsSeconds } },
                },
            }),
            Card("Chain", new StackPanel { Spacing = 6, Children = { _chain, runChain, new TextBlock { Text = "steps separated by spaces; ils takes its seconds", FontSize = 11, Opacity = 0.7 } } }),
        ]);
        var steps = Card("Steps (tour length before -> after)", _labList, fill: true);
        var left = new Grid { RowDefinitions = new RowDefinitions("*,220"), Margin = new Thickness(12, 0, 0, 12), RowSpacing = 10 };
        left.Children.Add(new Border { Child = _labMap, ClipToBounds = true, CornerRadius = new CornerRadius(8) });
        Grid.SetRow(steps, 1);
        left.Children.Add(steps);
        var grid = new Grid { ColumnDefinitions = new ColumnDefinitions("*,420") };
        grid.Children.Add(left);
        var scroll = new ScrollViewer { Content = side };
        Grid.SetColumn(scroll, 1);
        grid.Children.Add(scroll);
        UpdateLabButtons();
        return grid;
    }

    private void StartLab()
    {
        if (_inst is null) { Toast("open an instance first"); return; }
        if (_run is { Running: true } && !_vm.Finished) { Toast("stop the current run first"); return; }
        var s = ReadForm();
        s.Mode = "lab";
        _labSteps.Clear();
        _labList.ItemsSource = null;
        StartRun(s);
    }

    private void LabSend(string cmd)
    {
        if (!LabActive || _run?.Client is not { } c) { Toast("start the lab engine first"); return; }
        try
        {
            c.SendCommand("heuristic " + cmd);
            if (cmd != "quit") { _labBusy = true; _labStatus.Text = "running " + cmd + " …"; }
            UpdateLabButtons();
        }
        catch (Exception e) when (e is IOException or InvalidOperationException) { Toast(e.Message); }
    }

    private int RunChain(string text)
    {
        var toks = text.Split(' ', StringSplitOptions.RemoveEmptyEntries);
        _chainQueue.Clear();
        for (var i = 0; i < toks.Length; ++i)
        {
            var cmd = toks[i];
            if (cmd == "ils" && i + 1 < toks.Length && double.TryParse(toks[i + 1], NumberStyles.Float, CultureInfo.InvariantCulture, out _)) cmd += " " + toks[++i];
            if (cmd == "nn" && i + 1 < toks.Length && uint.TryParse(toks[i + 1], out _)) cmd += " " + toks[++i];
            _chainQueue.Enqueue(cmd);
        }
        var count = _chainQueue.Count;
        if (count > 0) LabSend(_chainQueue.Dequeue());
        return count;
    }

    // Log lines of the lab engine (UI thread).
    private void OnLabLog(string line)
    {
        if (line.StartsWith("lab_ready", StringComparison.Ordinal)) { _labStatus.Text = "ready: pick a construction"; UpdateLabButtons(); return; }
        if (line.StartsWith("lab_error", StringComparison.Ordinal) || line.StartsWith("command_unsupported", StringComparison.Ordinal))
        {
            _labBusy = false;
            _chainQueue.Clear();
            _labStatus.Text = line;
            _labStatus.Foreground = _pal.Brush(_pal.Danger);
            UpdateLabButtons();
            return;
        }
        if (LabStepRow.Parse(line, _labSteps.Count + 1) is not { } row) return;
        _labSteps.Add(row);
        _labList.ItemsSource = _labSteps.Select(r => r.Line()).Reverse().ToList();
        _labBusy = false;
        _labStatus.Foreground = _pal.Brush(_pal.Text);
        _labStatus.Text = string.Create(CultureInfo.InvariantCulture, $"{row.Name}: {row.Before} -> {row.After} ({row.Delta:+0;-0}) in {row.Ms:0.0} ms");
        if (_chainQueue.Count > 0) LabSend(_chainQueue.Dequeue());
        UpdateLabButtons();
    }

    private void UpdateLabButtons()
    {
        var active = LabActive;
        foreach (var b in _labButtons) b.IsEnabled = active && !_labBusy;
        _labStart.IsEnabled = _inst is not null && !(_run is { Running: true } && !_vm.Finished);
        _labStop.IsEnabled = active;
        if (!active && _labStatus.Text is null or "") _labStatus.Text = "the lab engine runs one heuristic per button on the current tour";
    }
}
