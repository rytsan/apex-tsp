using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace ApexTsp.Gui;

// Run settings chosen in the window, turned into engine arguments (M6.2). Plain C#, tested.
public sealed class RunSettings
{
    public string Mode { get; set; } = "exact";        // exact | scalable
    public double TimeLimitS { get; set; } = 600;
    public int Threads { get; set; } = Math.Max(1, Environment.ProcessorCount / 2);
    public ulong Seed { get; set; } = 1;
    public int MemLimitMb { get; set; } = 4096;
    public string CheckpointDir { get; set; } = "";     // empty: no checkpoints
    public bool Resume { get; set; }

    public static readonly string[] Modes = ["exact", "scalable"];

    // Null when valid, else the first problem in words.
    public string? Validate()
    {
        if (!Modes.Contains(Mode)) return "mode must be exact or scalable";
        if (!(TimeLimitS > 0)) return "time limit must be > 0 s";
        if (Threads < 1 || Threads > 256) return "threads must be 1..256";
        if (MemLimitMb < 64) return "memory ceiling must be >= 64 MB";
        if (Resume && CheckpointDir.Length == 0) return "resume needs a checkpoint directory";
        return null;
    }

    public List<string> ToEngineArgs(string instance, string endpoint, string sharedTour, string reportPath, string tourPath)
    {
        if (Validate() is { } why) throw new ArgumentException(why);
        var a = new List<string>
        {
            "--instance", instance, "--mode", Mode,
            "--time-limit", TimeLimitS.ToString("0.###", CultureInfo.InvariantCulture),
            "--threads", Threads.ToString(CultureInfo.InvariantCulture),
            "--seed", Seed.ToString(CultureInfo.InvariantCulture),
            "--mem-limit-mb", MemLimitMb.ToString(CultureInfo.InvariantCulture),
            "--ipc", endpoint, "--shared-tour", sharedTour, "--report", reportPath, "--out", tourPath,
        };
        if (CheckpointDir.Length > 0) a.AddRange(["--checkpoint", CheckpointDir]);
        if (Resume) a.Add("--resume");
        return a;
    }

    public RunSettings Clone() => (RunSettings)MemberwiseClone();
}

// TSPLIB header + size, for the instance summary.
public sealed record InstanceInfo(string Path, string Name, int Dimension, string EdgeWeightType, string Comment)
{
    public static InstanceInfo Read(string path)
    {
        string name = System.IO.Path.GetFileNameWithoutExtension(path), ewt = "", comment = "";
        var dim = 0;
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (line.StartsWith("NODE_COORD_SECTION", StringComparison.Ordinal) || line == "EOF") break;
            var k = line.IndexOf(':');
            if (k < 0) continue;
            var key = line[..k].Trim();
            var val = line[(k + 1)..].Trim();
            switch (key)
            {
                case "NAME": name = val; break;
                case "DIMENSION": int.TryParse(val, NumberStyles.Integer, CultureInfo.InvariantCulture, out dim); break;
                case "EDGE_WEIGHT_TYPE": ewt = val; break;
                case "COMMENT": comment = comment.Length == 0 ? val : comment + " " + val; break;
            }
        }
        return new InstanceInfo(path, name, dim, ewt, comment);
    }
}

// TSPLIB .tour writer / reader (1-based node ids, -1 terminator).
public static class TourFiles
{
    public static void Write(string path, string name, ReadOnlySpan<uint> tour, string comment)
    {
        var sb = new StringBuilder();
        sb.Append("NAME : ").Append(name).Append(".tour\n");
        sb.Append("COMMENT : ").Append(comment).Append('\n');
        sb.Append("TYPE : TOUR\n");
        sb.Append("DIMENSION : ").Append(tour.Length.ToString(CultureInfo.InvariantCulture)).Append('\n');
        sb.Append("TOUR_SECTION\n");
        foreach (var v in tour) sb.Append((v + 1).ToString(CultureInfo.InvariantCulture)).Append('\n');
        sb.Append("-1\nEOF\n");
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, sb.ToString());
        File.Move(tmp, path, overwrite: true);
    }

    public static uint[] Read(string path)
    {
        var t = new List<uint>();
        var inTour = false;
        foreach (var raw in File.ReadLines(path))
        {
            var line = raw.Trim();
            if (!inTour) { inTour = line.StartsWith("TOUR_SECTION", StringComparison.Ordinal); continue; }
            foreach (var tok in line.Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries))
            {
                if (tok == "-1" || tok == "EOF") return [.. t];
                t.Add(uint.Parse(tok, CultureInfo.InvariantCulture) - 1);
            }
        }
        return [.. t];
    }

    // Tour as an SVG polyline (y flipped like the map).
    public static void WriteSvg(string path, ApexTsp.Render.Scene s, ReadOnlySpan<uint> tour, string stroke = "#1A73E8")
    {
        var w = Math.Max(1e-9, s.MaxX - s.MinX);
        var h = Math.Max(1e-9, s.MaxY - s.MinY);
        var scale = 1000 / Math.Max(w, h);
        var sb = new StringBuilder();
        var ci = CultureInfo.InvariantCulture;
        sb.Append(ci, $"<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 {w * scale:0.#} {h * scale:0.#}\">\n");
        sb.Append(ci, $"<polygon fill=\"none\" stroke=\"{stroke}\" stroke-width=\"0.6\" points=\"");
        foreach (var v in tour)
            if (v < s.Count) sb.Append(ci, $"{(s.X[v] - s.MinX) * scale:0.##},{(s.MaxY - s.Y[v]) * scale:0.##} ");
        sb.Append("\"/>\n</svg>\n");
        File.WriteAllText(path, sb.ToString());
    }
}

// Persistent app state: engine path, recent instances, theme, last settings.
public sealed class AppSettings
{
    public string? EnginePath { get; set; }
    public List<string> Recent { get; set; } = [];
    public string Theme { get; set; } = "system";   // system | light | dark
    public RunSettings Run { get; set; } = new();

    public void Touch(string instance)
    {
        Recent.Remove(instance);
        Recent.Insert(0, instance);
        if (Recent.Count > 10) Recent.RemoveRange(10, Recent.Count - 10);
    }
}

// One finished (or interrupted) run, as stored under the history directory.
public sealed class RunRecord
{
    public string Id { get; set; } = "";
    public string Instance { get; set; } = "";
    public string InstanceName { get; set; } = "";
    public DateTimeOffset Started { get; set; }
    public RunSettings Settings { get; set; } = new();
    public string Status { get; set; } = "";
    public double Ub { get; set; } = double.NaN;
    public double Lb { get; set; } = double.NaN;
    public double GapPct { get; set; } = double.NaN;
    public double WallS { get; set; }
    public string LbSource { get; set; } = "";
    public List<BoundSample> Bounds { get; set; } = [];
}

[JsonSourceGenerationOptions(WriteIndented = true, NumberHandling = JsonNumberHandling.AllowNamedFloatingPointLiterals)]
[JsonSerializable(typeof(AppSettings))]
[JsonSerializable(typeof(RunRecord))]
internal sealed partial class AppJson : JsonSerializerContext;

// <home>/settings.json and <home>/history/<instance key>/<run id>/{run.json, report.json, tour.tour}.
// home = $APEX_TSP_HOME or the user's local app data folder.
public sealed class AppStore
{
    public string Home { get; }
    public AppStore(string? home = null)
    {
        var env = Environment.GetEnvironmentVariable("APEX_TSP_HOME");
        Home = home ?? (string.IsNullOrEmpty(env)
            ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "apex-tsp")
            : env);
        Directory.CreateDirectory(Home);
    }

    private string SettingsPath => Path.Combine(Home, "settings.json");

    public AppSettings LoadSettings()
    {
        try { return JsonSerializer.Deserialize(File.ReadAllText(SettingsPath), AppJson.Default.AppSettings) ?? new(); }
        catch (Exception e) when (e is IOException or JsonException) { return new(); }
    }

    public void SaveSettings(AppSettings s) => WriteAtomic(SettingsPath, JsonSerializer.Serialize(s, AppJson.Default.AppSettings));

    public static string InstanceKey(string instancePath)
    {
        var full = Path.GetFullPath(instancePath);
        var hash = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(full)))[..8].ToLowerInvariant();
        return Path.GetFileNameWithoutExtension(full) + "-" + hash;
    }

    // New run directory (id = UTC timestamp, unique).
    public string NewRunDir(string instancePath, out string id)
    {
        var root = Path.Combine(Home, "history", InstanceKey(instancePath));
        id = DateTime.UtcNow.ToString("yyyyMMdd-HHmmss-fff", CultureInfo.InvariantCulture);
        var dir = Path.Combine(root, id);
        for (var k = 1; Directory.Exists(dir); ++k) dir = Path.Combine(root, id + "-" + k);
        id = Path.GetFileName(dir);
        Directory.CreateDirectory(dir);
        return dir;
    }

    public static void SaveRecord(string runDir, RunRecord r) =>
        WriteAtomic(Path.Combine(runDir, "run.json"), JsonSerializer.Serialize(r, AppJson.Default.RunRecord));

    public static RunRecord? LoadRecord(string runDir)
    {
        try { return JsonSerializer.Deserialize(File.ReadAllText(Path.Combine(runDir, "run.json")), AppJson.Default.RunRecord); }
        catch (Exception e) when (e is IOException or JsonException) { return null; }
    }

    // Runs of one instance (or all), newest first: (directory, record).
    public List<(string Dir, RunRecord Run)> ListRuns(string? instancePath = null)
    {
        var root = Path.Combine(Home, "history");
        var dirs = instancePath is null
            ? (Directory.Exists(root) ? Directory.GetDirectories(root) : [])
            : [Path.Combine(root, InstanceKey(instancePath))];
        var list = new List<(string, RunRecord)>();
        foreach (var d in dirs.Where(Directory.Exists))
            foreach (var run in Directory.GetDirectories(d))
                if (LoadRecord(run) is { } r) list.Add((run, r));
        return [.. list.OrderByDescending(x => x.Item2.Started)];
    }

    private static void WriteAtomic(string path, string text)
    {
        var tmp = path + ".tmp";
        File.WriteAllText(tmp, text);
        File.Move(tmp, path, overwrite: true);
    }
}
