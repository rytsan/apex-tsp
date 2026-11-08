using System.Globalization;
using ApexTsp.Ipc;
using ApexTsp.Render;
using Avalonia;
using Avalonia.Media.Imaging;
using Avalonia.Threading;

namespace ApexTsp.Gui;

// Scripted runs that leave evidence files (gates). They drive the same methods as the toolbar.
public sealed partial class MainWindow
{
    private DispatcherTimer? _demo;
    private int _demoStep;
    private bool _liveShot;

    // ---- G5.3: pan/zoom cycles through the input path, LOD levels, screenshots, frame stats.
    private void StartDemo()
    {
        Directory.CreateDirectory(_opt.DemoOut!);
        _demo = new DispatcherTimer(TimeSpan.FromMilliseconds(33), DispatcherPriority.Background, (_, _) => DemoTick());
        _demo.Start();
    }

    private void DemoTick()
    {
        if (_map.Scene is not { TourCount: > 0 } scene) return;
        var cx = _map.Bounds.Width / 2;
        var cy = _map.Bounds.Height / 2;
        var i = _demoStep++ % 300;
        var first = _demoStep <= 300;
        var shot = (string name) => { if (first) _map.SavePng(Path.Combine(_opt.DemoOut!, name)); };
        switch (i)
        {
            case 0: _map.Fit(); break;
            case 1: shot("map_fit.png"); break;
            case < 100: _map.ZoomAt(1.06, cx, cy); break;
            case 100: shot("map_zoomed.png"); break;
            case < 160: _map.PanBy(8, 4); break;
            case < 250: _map.ZoomAt(1 / 1.06, cx, cy); break;
            case 250: _map.Renderer.Force = LodLevel.Clusters; _map.Fit(); break;
            case 252: shot("map_clusters.png"); break;
            case 260: _map.Renderer.Force = LodLevel.Heatmap; _map.Invalidate(); break;
            case 262: shot("map_heatmap.png"); break;
            case 270: _map.Renderer.Force = null; _map.Invalidate(); break;
            default: _map.PanBy(i % 2 == 0 ? 3 : -3, 0); break;
        }
        if (!_liveShot && _vm.Tree.Count >= 10 && LpFrames > 0 && _vm.CutRounds.Count > 0)
        {
            _liveShot = true;
            _map.Fit();
            RefreshSolverViews();
            DispatcherTimer.RunOnce(() => { SaveWindowPng(Path.Combine(_opt.DemoOut!, "window_live.png")); _map.SavePng(Path.Combine(_opt.DemoOut!, "map_live_lp.png")); }, TimeSpan.FromMilliseconds(150));
        }
        if (first && scene.Clusters.Any(c => c.State != 0) && !File.Exists(Path.Combine(_opt.DemoOut!, "map_cluster_overlay.png")))
            _map.SavePng(Path.Combine(_opt.DemoOut!, "map_cluster_overlay.png"));
    }

    private void FinishDemo()
    {
        _demo?.Stop();
        var dir = _opt.DemoOut!;
        _map.Renderer.Force = null;
        _map.Fit();
        _map.SavePng(Path.Combine(dir, "map_final.png"));
        SaveWindowPng(Path.Combine(dir, "window_final.png"));
        File.WriteAllText(Path.Combine(dir, "report.json"), _vm.ReportJson);
        if (_run is not null && File.Exists(_run.LogPath)) File.Copy(_run.LogPath, Path.Combine(dir, "engine.log"), true);
        var r = _map.Renderer;
        File.WriteAllText(Path.Combine(dir, "frame_stats.txt"), FrameStats.Describe(_map, _map.Scene!.Count) +
            $"layers: tour={r.ShowTour} nodes={r.ShowNodes} lp={r.ShowLp} clusters={r.ClusterOverlay} flash={_flashLp.IsChecked}\n" +
            $"solver view: lp_solutions={LpFrames} lp_edges_last={_map.Scene.Lp.Length} cut_rounds={_vm.CutRounds.Count} tree_nodes={_vm.Tree.Count} events={_vm.Events.Count}\n" +
            $"events by kind: {string.Join(", ", _vm.Events.GroupBy(e => e.Kind).OrderBy(g => g.Key).Select(g => $"{g.Key}={g.Count()}"))}\n" +
            $"badges: {_vm.UbBadge.Text} | {_vm.LbBadge.Text} | cuts {_vm.CutState}\n");
        Close();
    }

    public void SaveWindowPng(string path)
    {
        var size = new PixelSize((int)Bounds.Width, (int)Bounds.Height);
        using var rtb = new RenderTargetBitmap(size, new Vector(96, 96));
        rtb.Render(this);
        rtb.Save(path);
    }

    // ---- G6.2: open -> configure -> run -> pause / resume -> save tour -> reload from history.
    private StreamWriter? _appLog;
    private int _appStage;
    private DateTime _appT0, _appStageT;
    private bool _appFinished;
    private string _appRunDir = "";

    private void AppDemoLog(string text)
    {
        if (_appLog is null) return;
        _appLog.WriteLine(string.Create(CultureInfo.InvariantCulture, $"t={(DateTime.UtcNow - _appT0).TotalSeconds:0.000} {text}"));
        _appLog.Flush();
    }

    private void StartAppDemo()
    {
        Directory.CreateDirectory(_opt.DemoApp!);
        _appLog = new StreamWriter(Path.Combine(_opt.DemoApp!, "app_demo.log"));
        _appT0 = DateTime.UtcNow;
        AppDemoLog($"opened {_inst?.Path} n={_inst?.Dimension} type={_inst?.EdgeWeightType}");
        _demo = new DispatcherTimer(TimeSpan.FromMilliseconds(250), DispatcherPriority.Background, (_, _) => AppDemoTick());
        _demo.Start();
    }

    private void DemoAppOnFinished()
    {
        if (_appLog is null) return;
        _appFinished = true;
        _appRunDir = _run?.RunDir ?? "";
        AppDemoLog($"finished status={_vm.Status} ub={MainViewModel.Fmt(_vm.UpperBound)} lb={MainViewModel.Fmt(_vm.LowerBound)} wall_s={_vm.WallS}");
    }

    private void Stage(int next)
    {
        _appStage = next;
        _appStageT = DateTime.UtcNow;
    }

    private double StageSeconds => (DateTime.UtcNow - _appStageT).TotalSeconds;

    private void Check(bool ok, string what)
    {
        AppDemoLog((ok ? "PASS " : "FAIL ") + what);
        if (!ok) _appFailed = true;
    }

    private bool _appFailed;

    private void AppDemoTick()
    {
        var dir = _opt.DemoApp!;
        switch (_appStage)
        {
            case 0:  // configure in the form (as a user would), then start
                LoadSettingsIntoForm(new RunSettings { Mode = "exact", TimeLimitS = 45, Threads = 2, Seed = 1, MemLimitMb = 2048 });
                ApplyEngineArgs(_opt.EngineArgs);
                var s = ReadForm();
                AppDemoLog(string.Create(CultureInfo.InvariantCulture, $"configured mode={s.Mode} time_limit={s.TimeLimitS} threads={s.Threads} seed={s.Seed} mem_mb={s.MemLimitMb}"));
                SaveWindowPng(Path.Combine(dir, "1_configured.png"));
                StartRun();
                Stage(1);
                break;
            case 1:  // running with bounds: pause
                if (_vm.Phase == "bnc" && _vm.History.Count >= 2 && StageSeconds > 3)
                {
                    AppDemoLog($"pause requested at ub={MainViewModel.Fmt(_vm.UpperBound)} lb={MainViewModel.Fmt(_vm.LowerBound)}");
                    Send(MsgType.Pause);
                    Stage(2);
                }
                else if (StageSeconds > 120) { Check(false, "solver reached branch-and-cut"); Stage(9); }
                break;
            case 2:
                if (StageSeconds > 2)
                {
                    Check(_vm.Status == "paused", $"status after pause = {_vm.Status}");
                    SaveWindowPng(Path.Combine(dir, "2_paused.png"));
                    Send(MsgType.Resume);
                    Stage(3);
                }
                break;
            case 3:
                if (StageSeconds > 1 && _vm.Status != "paused") { Check(true, $"resumed, status = {_vm.Status}"); Stage(4); }
                break;
            case 4:  // wait for the end of the run
                if (_appFinished)
                {
                    SaveWindowPng(Path.Combine(dir, "3_finished.png"));
                    var saved = Path.Combine(dir, "saved.tour");
                    SaveTour(saved);
                    var t = TourFiles.Read(saved);
                    Check(t.Length == _inst!.Dimension && t.Distinct().Count() == t.Length, $"saved tour is a permutation of {t.Length} nodes");
                    Stage(5);
                }
                break;
            case 5:  // history tab
                _tabs.SelectedIndex = 1;
                RefreshRuns();
                _runs.SelectedIndex = 0;
                Check(_runList.Count >= 1 && _runList[0].Dir == _appRunDir, $"history lists the run first ({_runList.Count} runs)");
                Stage(51);
                break;
            case 51:  // clear the view, reload the run from disk
                if (StageSeconds < 0.5) break;
                SaveWindowPng(Path.Combine(dir, "4_history.png"));
                var ub = _vm.UpperBound;
                _vm.Reset();
                _map.Scene!.SetTour([], 0);
                Check(LoadRun(_appRunDir), "run reloaded from history");
                Check(_map.Scene.TourCount == _inst!.Dimension, $"reloaded tour has {_map.Scene.TourCount} nodes");
                Check(_vm.UpperBound == ub && _vm.History.Count > 0, $"reloaded UB {MainViewModel.Fmt(_vm.UpperBound)} == {MainViewModel.Fmt(ub)}, {_vm.History.Count} bound samples");
                Stage(6);
                break;
            case 6:
                if (StageSeconds > 0.5)
                {
                    SaveWindowPng(Path.Combine(dir, "5_reloaded.png"));
                    _theme.SelectedItem = "dark";
                    Stage(7);
                }
                break;
            case 7:
                if (StageSeconds > 0.5)
                {
                    Check(ActualThemeVariant == Avalonia.Styling.ThemeVariant.Dark, "dark theme applied");
                    SaveWindowPng(Path.Combine(dir, "6_dark.png"));
                    _theme.SelectedItem = "system";
                    Stage(9);
                }
                break;
            case 9:
                _demo?.Stop();
                AppDemoLog(_appFailed ? "RESULT FAIL" : "RESULT PASS");
                _appLog?.Dispose();
                _appLog = null;
                Close();
                break;
        }
    }
}
