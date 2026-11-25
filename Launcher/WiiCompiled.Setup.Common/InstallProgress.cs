using System.Text.Json;

namespace WiiCompiled.Setup.Common;

/// <summary>
/// Where an installation reports what it is doing. Progress is coarse and monotonic; raw translator
/// and compiler output is a diagnostic, never progress, because it is unbounded and machine-hostile.
/// </summary>
public interface IInstallReporter
{
    void Progress(string stage, string message, int percent);
    void Diagnostic(string line);
}

/// <summary>Reports to a caller-supplied sink; used by console and maintenance commands.</summary>
public sealed class DelegatingInstallReporter : IInstallReporter
{
    private readonly Action<string> _sink;

    public DelegatingInstallReporter(Action<string> sink) => _sink = sink;

    public void Progress(string stage, string message, int percent) => _sink(message);
    public void Diagnostic(string line) => _sink(line);
}

/// <summary>
/// The <c>--progress-json</c> protocol: one JSON object per line on stdout, nothing else on stdout,
/// diagnostics on stderr. The terminal <c>result</c> line is written exactly once.
/// </summary>
public sealed class NdjsonInstallReporter : IInstallReporter
{
    private static readonly JsonSerializerOptions Options = new() { WriteIndented = false };
    private readonly string _version;
    private readonly Action<string>? _log;
    private readonly object _gate = new();
    private int _lastPercent;
    private bool _finished;

    public NdjsonInstallReporter(string version, Action<string>? log = null)
    {
        _version = version;
        _log = log;
    }

    public void Progress(string stage, string message, int percent)
    {
        lock (_gate)
        {
            if (_finished) return;
            // Percentages are clamped monotonic: a caller's progress bar must never walk backwards
            // because a later stage happened to estimate a lower number.
            _lastPercent = Math.Clamp(Math.Max(percent, _lastPercent), 0, 99);
            WriteLine(new { type = "progress", stage, message, percent = _lastPercent });
        }
        _log?.Invoke(message);
    }

    public void Diagnostic(string line)
    {
        Console.Error.WriteLine(line);
        _log?.Invoke(line);
    }

    public void Success(string installDirectory)
    {
        lock (_gate)
        {
            if (_finished) return;
            _finished = true;
            WriteLine(new { type = "result", success = true, version = _version, installDir = installDirectory });
        }
    }

    public void Failure(string error)
    {
        lock (_gate)
        {
            if (_finished) return;
            _finished = true;
            WriteLine(new { type = "result", success = false, error });
        }
        _log?.Invoke("FAILED: " + error);
    }

    /// <summary>
    /// The terminal result line is the caller's only completion signal, so no exit path may skip it.
    /// Callers invoke this from a finally block; it is a no-op once a result was already written.
    /// </summary>
    public void EnsureFinished(string errorIfUnfinished) => Failure(errorIfUnfinished);

    private static void WriteLine(object value)
    {
        Console.Out.WriteLine(JsonSerializer.Serialize(value, Options));
        Console.Out.Flush();
    }
}

/// <summary>Plain-text console reporting for a run without <c>--progress-json</c>.</summary>
public sealed class ConsoleInstallReporter : IInstallReporter
{
    private readonly Action<string>? _log;

    public ConsoleInstallReporter(Action<string>? log = null) => _log = log;

    public void Progress(string stage, string message, int percent)
    {
        Console.Out.WriteLine($"[{percent,3}%] {message}");
        _log?.Invoke(message);
    }

    public void Diagnostic(string line)
    {
        Console.Out.WriteLine(line);
        _log?.Invoke(line);
    }
}

/// <summary>Build step identifiers from the build scripts' <c>MKWCBUILD:STEP:&lt;id&gt;</c> lines. The
/// id is the contract; LocalBuild.ps1 emits these exactly and Test-PinnedFacts.ps1 fails the release if
/// the two sets disagree, so progress never depends on matching reworded English prose.</summary>
public static class BuildStepIds
{
    public const string ReuseBaseTranslation = "reuse-base-translation";
    public const string RetranslateBase = "retranslate-base";
    public const string TranslateBase = "translate-base";
    public const string EmitBaseManifest = "emit-base-manifest";
    public const string TranslateMod = "translate-mod";
    public const string GenerateDataInit = "generate-data-init";
    public const string EmitBuildShards = "emit-build-shards";
    public const string ConfigureNative = "configure-native";
    public const string Compile = "compile";
}

/// <summary>
/// Maps one local translate-and-compile run onto a slice of the overall percentage. The build
/// scripts announce every step they start with an <c>MKWCBUILD:</c> prefix, so the slice can advance
/// on real events instead of on a timer.
/// </summary>
public sealed class BuildProgressWindow
{
    private const string Marker = "MKWCBUILD:";
    private const string StepMarker = "STEP:";

    /// <summary>The fraction the compile step reaches; beyond it, Ninja's counter drives progress.</summary>
    private const double CompileFraction = 0.58;

    private static readonly (string Id, double Fraction, string Message)[] Steps =
    [
        (BuildStepIds.ReuseBaseTranslation, 0.30, "Reusing the completed base translation"),
        (BuildStepIds.RetranslateBase, 0.05, "The base translation is stale; retranslating it"),
        (BuildStepIds.TranslateBase, 0.08, "Translating Mario Kart Wii"),
        (BuildStepIds.EmitBaseManifest, 0.34, "Creating the translation manifest"),
        (BuildStepIds.TranslateMod, 0.38, "Translating the Retro Rewind Code.pul"),
        (BuildStepIds.GenerateDataInit, 0.44, "Generating game data initialization"),
        (BuildStepIds.EmitBuildShards, 0.48, "Preparing the native build"),
        (BuildStepIds.ConfigureNative, 0.52, "Configuring the compiler"),
        (BuildStepIds.Compile, CompileFraction, "Compiling the game. This is the longest step"),
    ];

    private readonly IInstallReporter _reporter;
    private readonly string _stage;
    private readonly int _start;
    private readonly int _end;
    private double _fraction;
    private string _message = "Preparing the local build";
    private int _reportedPercent = -1;

    public BuildProgressWindow(IInstallReporter reporter, string stage, int start, int end)
    {
        _reporter = reporter;
        _stage = stage;
        _start = start;
        _end = end;
    }

    public void Observe(string line)
    {
        var index = line.IndexOf(Marker, StringComparison.Ordinal);
        if (index >= 0)
        {
            var text = line[(index + Marker.Length)..].Trim();
            if (text.StartsWith(StepMarker, StringComparison.Ordinal))
            {
                var identifier = text[StepMarker.Length..];
                var end = identifier.IndexOf(' ');
                if (end >= 0) identifier = identifier[..end];
                foreach (var (id, fraction, message) in Steps)
                {
                    if (!id.Equals(identifier, StringComparison.Ordinal)) continue;
                    if (fraction > _fraction)
                    {
                        _fraction = fraction;
                        _message = message;
                        Emit();
                    }
                    return;
                }
            }
        }

        // Anything else - an unknown step identifier, a plain MKWCBUILD note, or raw tool output -
        // stays a diagnostic.
        _reporter.Diagnostic(line);
        // Ninja prefixes every edge with [done/total]; how many edges there are depends on how much
        // of the build is already up to date, so follow the counter rather than counting lines.
        if (_fraction >= CompileFraction && TryParseNinjaProgress(line, out var done, out var total))
        {
            _fraction = Math.Max(_fraction, CompileFraction + (0.97 - CompileFraction) * done / total);
            Emit();
        }
    }

    private static bool TryParseNinjaProgress(string line, out int done, out int total)
    {
        done = total = 0;
        if (!line.StartsWith('[')) return false;
        var close = line.IndexOf(']');
        var slash = line.IndexOf('/');
        return close > 0 && slash > 0 && slash < close
            && int.TryParse(line.AsSpan(1, slash - 1), out done)
            && int.TryParse(line.AsSpan(slash + 1, close - slash - 1), out total)
            && total > 0 && done <= total;
    }

    private void Emit()
    {
        var percent = Interpolate(_fraction);
        if (percent == _reportedPercent) return;
        _reportedPercent = percent;
        _reporter.Progress(_stage, _message, percent);
    }

    private int Interpolate(double fraction) =>
        (int)Math.Round(_start + (_end - _start) * Math.Clamp(fraction, 0, 1));
}
