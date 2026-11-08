using Avalonia;
using Avalonia.Media;
using Avalonia.Styling;
using SkiaSharp;

namespace ApexTsp.Gui;

// One palette for the app. Provenance colors are fixed meanings everywhere (badges, chart,
// map): heuristic = orange, LP bound = blue, Lagrangian bound = purple, proven = green.
public sealed record Palette(
    Color Background, Color Surface, Color Border, Color Text, Color Muted, Color Accent,
    Color Heuristic, Color Lp, Color Lagrangian, Color Proven, Color Danger, Color MapNode, Color MapTour)
{
    public static readonly Palette Light = new(
        Color.Parse("#F7F8FA"), Color.Parse("#FFFFFF"), Color.Parse("#E1E4EA"), Color.Parse("#1F2329"), Color.Parse("#5F6773"),
        Color.Parse("#1A73E8"), Color.Parse("#E8710A"), Color.Parse("#1A73E8"), Color.Parse("#8E3FD0"), Color.Parse("#1E8E3E"),
        Color.Parse("#D93025"), Color.Parse("#2A2F36"), Color.Parse("#1A73E8"));

    public static readonly Palette Dark = new(
        Color.Parse("#16181C"), Color.Parse("#1F2227"), Color.Parse("#30343B"), Color.Parse("#E6E8EB"), Color.Parse("#9AA1AC"),
        Color.Parse("#8AB4F8"), Color.Parse("#FDAA5C"), Color.Parse("#8AB4F8"), Color.Parse("#C79BF2"), Color.Parse("#81C995"),
        Color.Parse("#F28B82"), Color.Parse("#C9CDD3"), Color.Parse("#8AB4F8"));

    public static Palette For(ThemeVariant v) => v == ThemeVariant.Dark ? Dark : Light;

    public IBrush Brush(Color c) => new SolidColorBrush(c);
    public static SKColor Sk(Color c) => new(c.R, c.G, c.B, c.A);

    // Badge color of a bound / tour source.
    public Color ForSource(string source) => source switch
    {
        "lp" => Lp,
        "lagrangian" => Lagrangian,
        "proven" or "OPTIMAL_PROVEN" => Proven,
        "-" or "" => Muted,
        _ => Heuristic,
    };
}

public static class ThemeChoice
{
    public static readonly string[] Names = ["system", "light", "dark"];

    public static ThemeVariant Variant(string name) => name switch
    {
        "light" => ThemeVariant.Light,
        "dark" => ThemeVariant.Dark,
        _ => ThemeVariant.Default,
    };

    public static ThemeVariant Actual(Application app) => app.ActualThemeVariant == ThemeVariant.Dark ? ThemeVariant.Dark : ThemeVariant.Light;
}
