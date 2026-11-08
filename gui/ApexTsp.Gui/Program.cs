using Avalonia;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Styling;
using Avalonia.Themes.Fluent;

namespace ApexTsp.Gui;

public sealed class GuiOptions
{
    public string? Engine;          // apex_tsp binary: the GUI launches it
    public string? Attach;          // endpoint of an engine already running
    public string Instance = "";    // .tsp (coordinates for the map)
    public string? Endpoint;
    public string? SharedTour;
    public string? DemoOut;         // scripted pan/zoom + screenshots + frame stats, exits after FINISHED
    public List<string> EngineArgs = [];

    public const string Usage =
        "usage: ApexTsp.Gui --instance <file.tsp> (--engine <apex_tsp> [-- <engine args>] | --attach <endpoint> --shared-tour <file>)\n" +
        "                   [--endpoint <name>] [--demo-out <dir>]";

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
                case "--": o.EngineArgs.AddRange(args[(i + 1)..]); i = args.Length; break;
                default: throw new ArgumentException("unknown argument " + args[i]);
            }
        }
        if (o.Instance.Length == 0) throw new ArgumentException("--instance is required");
        if ((o.Engine is null) == (o.Attach is null)) throw new ArgumentException("give exactly one of --engine / --attach");
        if (o.Attach is not null && o.SharedTour is null) throw new ArgumentException("--attach needs --shared-tour");
        return o;
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
        RequestedThemeVariant = ThemeVariant.Light;
    }

    public override void OnFrameworkInitializationCompleted()
    {
        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
            desktop.MainWindow = new MainWindow(Program.Options);
        base.OnFrameworkInitializationCompleted();
    }
}
