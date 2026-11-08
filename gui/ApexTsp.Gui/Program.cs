using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;

namespace ApexTsp.Gui;

public sealed class GuiOptions
{
    public string? Engine;          // apex_tsp binary (default: settings, then next to the GUI, then the repo build)
    public string? Attach;          // endpoint of an engine already running
    public string? Instance;        // .tsp opened at start
    public string? Endpoint;
    public string? SharedTour;
    public string? DemoOut;         // G5.3 script: start, pan/zoom, screenshots, frame stats, exit after FINISHED
    public string? DemoApp;         // G6.2 script: open, configure, run, pause/resume, save tour, reload from history
    public string? DemoLab;         // G6.4 script: every lab heuristic through the app, then an edited instance solved exact
    public string? Home;            // settings + history folder (default $APEX_TSP_HOME or local app data)
    public List<string> EngineArgs = [];

    public const string Usage =
        "usage: ApexTsp.Gui [<file.tsp>] [--instance <file.tsp>] [--engine <apex_tsp>] [--home <dir>]\n" +
        "                   [--attach <endpoint> --shared-tour <file>] [--demo-out <dir> -- <engine args>] [--demo-app <dir>] [--demo-lab <dir>]";

    public static GuiOptions Parse(string[] args)
    {
        var o = new GuiOptions();
        for (var i = 0; i < args.Length; ++i)
        {
            string Value() => i + 1 < args.Length ? args[++i] : throw new ArgumentException("missing value for " + args[i]);
            switch (args[i])
            {
                case "--engine": o.Engine = Value(); break;
                case "--attach": o.Attach = Value(); break;
                case "--instance": o.Instance = Value(); break;
                case "--endpoint": o.Endpoint = Value(); break;
                case "--shared-tour": o.SharedTour = Value(); break;
                case "--demo-out": o.DemoOut = Value(); break;
                case "--demo-app": o.DemoApp = Value(); break;
                case "--demo-lab": o.DemoLab = Value(); break;
                case "--home": o.Home = Value(); break;
                case "--": o.EngineArgs.AddRange(args[(i + 1)..]); i = args.Length; break;
                default:
                    if (args[i].StartsWith("--", StringComparison.Ordinal) || o.Instance is not null)
                        throw new ArgumentException("unknown argument " + args[i]);
                    o.Instance = args[i];
                    break;
            }
        }
        if (o.Attach is not null && o.SharedTour is null) throw new ArgumentException("--attach needs --shared-tour");
        if ((o.DemoOut ?? o.DemoApp ?? o.DemoLab) is not null && o.Instance is null) throw new ArgumentException("demo scripts need an instance");
        return o;
    }
}

public static class EngineLocator
{
    // First existing candidate: explicit, settings, $APEX_TSP_ENGINE, next to the GUI, repo builds.
    public static string? Find(params string?[] preferred)
    {
        var exe = OperatingSystem.IsWindows() ? "apex_tsp.exe" : "apex_tsp";
        var list = new List<string?>(preferred) { Environment.GetEnvironmentVariable("APEX_TSP_ENGINE"), Path.Combine(AppContext.BaseDirectory, exe) };
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
            foreach (var preset in new[] { "benchmark", "dev-gcc", "dev-msvc", "dev-clang" })
                list.Add(Path.Combine(d.FullName, "build", preset, exe));
        return list.FirstOrDefault(p => !string.IsNullOrEmpty(p) && File.Exists(p));
    }
}

public static class Program
{
    public static GuiOptions Options { get; private set; } = new();

    [STAThread]
    public static int Main(string[] args)
    {
        try { Options = GuiOptions.Parse(args); }
        catch (ArgumentException e)
        {
            Console.Error.WriteLine(e.Message);
            Console.Error.WriteLine(GuiOptions.Usage);
            return 2;
        }
        return BuildAvaloniaApp().StartWithClassicDesktopLifetime([]);
    }

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>().UsePlatformDetect().WithInterFont().LogToTrace();
}

public sealed class App : Application
{
    public override void Initialize()
    {
        Styles.Add(new FluentTheme());
        RequestedThemeVariant = ThemeVariant.Default;  // follows the OS until the user picks one
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow(Program.Options);
        base.OnFrameworkInitializationCompleted();
    }
}
