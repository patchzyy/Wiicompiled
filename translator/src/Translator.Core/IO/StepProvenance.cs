using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace Translator.Core.IO;

/// <summary>What identifies one invocation of a build step apart from the files it touches.</summary>
public sealed record StepInvocation(
    string Step,
    string? TranslatorIdentity,
    string Runtime,
    string WorkingDirectory,
    IReadOnlyList<string> Arguments,
    int? EffectiveThreads,
    int ProcessorCount)
{
    public static StepInvocation ForCurrentProcess(string step, IReadOnlyList<string> arguments, int? effectiveThreads) => new(
        step,
        StepProvenance.ComputeTranslatorIdentity(),
        RuntimeInformation.FrameworkDescription,
        Directory.GetCurrentDirectory(),
        arguments.ToArray(),
        effectiveThreads,
        Environment.ProcessorCount);
}

/// <summary>
/// On-disk record of a successful step: the invocation plus every file it read (with its
/// SHA-256), every path it probed, every directory listing it took, and every output it left
/// (with its size). A later run of the same step skips only when all of it still holds.
/// </summary>
public sealed class StepProvenanceRecord
{
    public int SchemaVersion { get; set; }
    public string Step { get; set; } = string.Empty;
    public string? TranslatorIdentity { get; set; }
    public string Runtime { get; set; } = string.Empty;
    public string WorkingDirectory { get; set; } = string.Empty;
    public List<string> Arguments { get; set; } = [];
    public int? EffectiveThreads { get; set; }
    public int ProcessorCount { get; set; }
    public List<EnvironmentExpansionEntry> EnvironmentExpansions { get; set; } = [];
    public List<InputEntry> Inputs { get; set; } = [];
    public List<ProbeEntry> Probes { get; set; } = [];
    public List<ScanEntry> Scans { get; set; } = [];
    public List<OutputEntry> Outputs { get; set; } = [];

    public sealed class EnvironmentExpansionEntry
    {
        public string Value { get; set; } = string.Empty;
        public string Expanded { get; set; } = string.Empty;
    }

    public sealed class InputEntry
    {
        public string Path { get; set; } = string.Empty;
        public string Sha256 { get; set; } = string.Empty;
    }

    public sealed class ProbeEntry
    {
        public string Path { get; set; } = string.Empty;
        public bool IsDirectory { get; set; }
        public bool Exists { get; set; }
    }

    public sealed class ScanEntry
    {
        public string Directory { get; set; } = string.Empty;
        public string Pattern { get; set; } = "*";
        public bool Recursive { get; set; }
        public bool Directories { get; set; }
        public bool DirectoryExists { get; set; }
        public List<string> Entries { get; set; } = [];

        public FileScanQuery ToQuery() => new(
            Directory,
            Pattern,
            Recursive ? SearchOption.AllDirectories : SearchOption.TopDirectoryOnly,
            Directories ? FileScanKind.Directories : FileScanKind.Files);
    }

    public sealed class OutputEntry
    {
        public string Path { get; set; } = string.Empty;
        public long Size { get; set; }
        public long LastWriteTicks { get; set; }
    }
}

public static class StepProvenance
{
    /// <summary>Bump whenever the record's meaning changes; any other version is a miss.</summary>
    public const int SchemaVersion = 2;

    /// <summary>Test hook: when set, a command records every access and writes it here as a provenance record.</summary>
    public const string AccessLogEnvironmentVariable = "WIICOMPILED_TRANSLATOR_ACCESS_LOG";

    private static readonly JsonSerializerOptions JsonOptions = new()
    {
        WriteIndented = true,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
    };

    /// <summary>
    /// SHA-256 over the running translator's binaries: the single-file app host itself, or,
    /// when the app runs from a framework-dependent layout (app host or <c>dotnet</c>), every
    /// assembly and runtime manifest beside it. Null when it cannot be determined; a record
    /// with a null identity never matches.
    /// </summary>
    public static string? ComputeTranslatorIdentity()
    {
        try
        {
            var files = new SortedDictionary<string, string>(StringComparer.Ordinal);
            var entryName = System.Reflection.Assembly.GetEntryAssembly()?.GetName().Name;
            var processPath = Environment.ProcessPath;
            var baseDirectory = AppContext.BaseDirectory;
            if (string.IsNullOrEmpty(entryName) || string.IsNullOrEmpty(baseDirectory)) return null;

            // The entry assembly sits on disk unless it is bundled into a single-file app host.
            var frameworkDependent = File.Exists(Path.Combine(baseDirectory, entryName + ".dll"));
            if (!frameworkDependent)
            {
                // Single-file: the host is the whole translator, whatever it was renamed to
                // (the AppImage ships it as translator-cli).
                if (processPath is null || !File.Exists(processPath)) return null;
                files["host:" + Path.GetFileName(processPath)] = Sha256OfFile(processPath);
            }
            else
            {
                // An app host next to the assemblies (not the shared dotnet muxer) is part of the translator too.
                if (processPath is not null && File.Exists(processPath) &&
                    string.Equals(Path.GetFileNameWithoutExtension(processPath), entryName, StringComparison.OrdinalIgnoreCase))
                {
                    files["host:" + Path.GetFileName(processPath)] = Sha256OfFile(processPath);
                }

                foreach (var file in Directory.EnumerateFiles(baseDirectory, "*", SearchOption.TopDirectoryOnly)
                             .Where(static file => file.EndsWith(".dll", StringComparison.OrdinalIgnoreCase) ||
                                                   file.EndsWith(".deps.json", StringComparison.OrdinalIgnoreCase) ||
                                                   file.EndsWith(".runtimeconfig.json", StringComparison.OrdinalIgnoreCase)))
                {
                    files["lib:" + Path.GetFileName(file)] = Sha256OfFile(file);
                }
            }

            if (files.Count == 0) return null;
            var manifest = new StringBuilder();
            foreach (var (name, hash) in files) manifest.Append(hash).Append("  ").Append(name).Append('\n');
            return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(manifest.ToString()))).ToLowerInvariant();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    public static StepProvenanceRecord CreateRecord(StepInvocation invocation, FileAccessSnapshot snapshot) => new()
    {
        SchemaVersion = SchemaVersion,
        Step = invocation.Step,
        TranslatorIdentity = invocation.TranslatorIdentity,
        Runtime = invocation.Runtime,
        WorkingDirectory = invocation.WorkingDirectory,
        Arguments = invocation.Arguments.ToList(),
        EffectiveThreads = invocation.EffectiveThreads,
        ProcessorCount = invocation.ProcessorCount,
        EnvironmentExpansions = snapshot.EnvironmentExpansions
            .Select(static item => new StepProvenanceRecord.EnvironmentExpansionEntry { Value = item.Value, Expanded = item.Expanded })
            .ToList(),
        Inputs = snapshot.Inputs
            .Select(static item => new StepProvenanceRecord.InputEntry { Path = item.Path, Sha256 = item.Sha256 })
            .ToList(),
        Probes = snapshot.Probes
            .Select(static item => new StepProvenanceRecord.ProbeEntry { Path = item.Path, IsDirectory = item.IsDirectory, Exists = item.Exists })
            .ToList(),
        Scans = snapshot.Scans
            .Select(static item => new StepProvenanceRecord.ScanEntry
            {
                Directory = item.Query.Directory,
                Pattern = item.Query.Pattern,
                Recursive = item.Query.Option == SearchOption.AllDirectories,
                Directories = item.Query.Kind == FileScanKind.Directories,
                DirectoryExists = Directory.Exists(item.Query.Directory),
                Entries = item.Entries.ToList(),
            })
            .ToList(),
        Outputs = snapshot.Outputs
            .Select(static item => new StepProvenanceRecord.OutputEntry
            {
                Path = item.Path,
                Size = item.Size,
                LastWriteTicks = File.GetLastWriteTimeUtc(item.Path).Ticks,
            })
            .ToList(),
    };

    /// <summary>The record at <paramref name="path"/>, or null with the reason when it is missing or unusable.</summary>
    public static StepProvenanceRecord? TryRead(string path, out string reason)
    {
        try
        {
            if (!File.Exists(path))
            {
                reason = "no provenance record";
                return null;
            }
            var record = JsonSerializer.Deserialize<StepProvenanceRecord>(File.ReadAllBytes(path), JsonOptions);
            if (record is null)
            {
                reason = "empty provenance record";
                return null;
            }
            if (record.SchemaVersion != SchemaVersion)
            {
                reason = $"provenance schema {record.SchemaVersion} is not {SchemaVersion}";
                return null;
            }
            if (!IsComplete(record))
            {
                reason = "incomplete provenance record";
                return null;
            }
            reason = string.Empty;
            return record;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or NotSupportedException)
        {
            reason = $"unreadable provenance record: {ex.Message}";
            return null;
        }
    }

    /// <summary>
    /// JSON nulls deserialize into the record, so a hand-edited or damaged one is rejected here as a
    /// miss instead of throwing while it is checked, before the step could delete it.
    /// </summary>
    private static bool IsComplete(StepProvenanceRecord record) =>
        record.Arguments is not null && !record.Arguments.Contains(null!) &&
        record.EnvironmentExpansions is not null &&
        record.EnvironmentExpansions.All(entry => entry is { Value: not null, Expanded: not null }) &&
        record.Inputs is not null && record.Inputs.All(entry => entry is { Path: not null, Sha256: not null }) &&
        record.Probes is not null && record.Probes.All(entry => entry is { Path: not null }) &&
        record.Scans is not null &&
        record.Scans.All(entry => entry is { Directory: not null, Pattern: not null, Entries: not null } &&
                                  !entry.Entries.Contains(null!)) &&
        record.Outputs is not null && record.Outputs.All(entry => entry is { Path: not null });

    /// <summary>Null when <paramref name="record"/> describes exactly this invocation, otherwise why not.</summary>
    public static string? CompareInvocation(StepProvenanceRecord record, StepInvocation invocation)
    {
        if (invocation.TranslatorIdentity is null) return "the running translator's identity is unknown";
        if (!string.Equals(record.Step, invocation.Step, StringComparison.Ordinal)) return "the record belongs to another step";
        if (!string.Equals(record.TranslatorIdentity, invocation.TranslatorIdentity, StringComparison.Ordinal))
            return "the translator changed";
        if (!string.Equals(record.Runtime, invocation.Runtime, StringComparison.Ordinal)) return "the .NET runtime changed";
        if (!string.Equals(record.WorkingDirectory, invocation.WorkingDirectory, StringComparison.Ordinal))
            return "the working directory changed";
        if (!record.Arguments.SequenceEqual(invocation.Arguments, StringComparer.Ordinal)) return "the arguments changed";
        if (record.EffectiveThreads != invocation.EffectiveThreads) return "the thread count changed";
        if (record.ProcessorCount != invocation.ProcessorCount) return "the processor count changed";
        return null;
    }

    /// <summary>Null when every recorded file-system fact still holds, otherwise the first that does not.</summary>
    public static string? CheckFileSystem(StepProvenanceRecord record)
    {
        try
        {
            foreach (var expansion in record.EnvironmentExpansions)
            {
                if (!string.Equals(Environment.ExpandEnvironmentVariables(expansion.Value), expansion.Expanded, StringComparison.Ordinal))
                    return $"environment expansion of '{expansion.Value}' changed";
            }
            foreach (var probe in record.Probes)
            {
                var exists = probe.IsDirectory ? Directory.Exists(probe.Path) : File.Exists(probe.Path);
                if (exists != probe.Exists)
                    return $"{(probe.IsDirectory ? "directory" : "file")} {probe.Path} {(probe.Exists ? "no longer exists" : "now exists")}";
            }
            foreach (var scan in record.Scans)
            {
                var query = scan.ToQuery();
                var exists = Directory.Exists(query.Directory);
                if (exists != scan.DirectoryExists) return $"directory {query.Directory} appeared or disappeared";
                var listing = exists ? query.ListNow() : [];
                if (!listing.SequenceEqual(scan.Entries, StringComparer.Ordinal))
                    return $"directory listing of {query.Directory} ({query.Pattern}) changed";
            }
            foreach (var output in record.Outputs)
            {
                if (!File.Exists(output.Path)) return $"output {output.Path} is missing";
                var file = new FileInfo(output.Path);
                if (file.Length != output.Size) return $"output {output.Path} changed size";
                if (file.LastWriteTimeUtc.Ticks != output.LastWriteTicks) return $"output {output.Path} was modified";
            }
            foreach (var input in record.Inputs)
            {
                if (!File.Exists(input.Path)) return $"input {input.Path} is missing";
                if (!string.Equals(Sha256OfFile(input.Path), input.Sha256, StringComparison.OrdinalIgnoreCase))
                    return $"input {input.Path} changed";
            }
            return null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException or NotSupportedException)
        {
            return $"could not verify the record: {ex.Message}";
        }
    }

    /// <summary>Null when the record at <paramref name="path"/> proves this invocation is current, otherwise why not.</summary>
    public static string? Evaluate(string path, StepInvocation invocation)
    {
        var record = TryRead(path, out var reason);
        if (record is null) return reason;
        return CompareInvocation(record, invocation) ?? CheckFileSystem(record);
    }

    /// <summary>Temp file in the same directory, then an overwriting rename, so a reader never sees half a record.</summary>
    public static void WriteAtomically(string path, StepProvenanceRecord record)
    {
        var fullPath = Path.GetFullPath(path);
        var directory = Path.GetDirectoryName(fullPath)!;
        Directory.CreateDirectory(directory);
        var temporary = Path.Combine(directory, $".{Path.GetFileName(fullPath)}.{Guid.NewGuid():N}.tmp");
        try
        {
            File.WriteAllBytes(temporary, JsonSerializer.SerializeToUtf8Bytes(record, JsonOptions));
            File.Move(temporary, fullPath, overwrite: true);
        }
        finally
        {
            if (File.Exists(temporary)) File.Delete(temporary);
        }
    }

    public static string? AccessLogPathFromEnvironment() =>
        Environment.GetEnvironmentVariable(AccessLogEnvironmentVariable) is { Length: > 0 } value ? value : null;

    private static string Sha256OfFile(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}

/// <summary>
/// One command run under a provenance gate and/or the access-log hook. <see cref="Start"/>
/// either proves the step current (the caller skips it) or deletes the old record before
/// anything else happens and starts recording; <see cref="Finish"/> writes the new record
/// only after the step succeeded.
/// </summary>
public sealed class StepProvenanceSession
{
    private readonly string? _recordPath;
    private readonly string? _accessLogPath;
    private readonly StepInvocation _invocation;
    private readonly FileAccessRecorder? _recorder;
    private readonly TextWriter _diagnostics;

    private StepProvenanceSession(
        string? recordPath, string? accessLogPath, StepInvocation invocation,
        FileAccessRecorder? recorder, TextWriter diagnostics, bool isCurrent)
    {
        _recordPath = recordPath;
        _accessLogPath = accessLogPath;
        _invocation = invocation;
        _recorder = recorder;
        _diagnostics = diagnostics;
        IsCurrent = isCurrent;
    }

    /// <summary>True when the existing record proves the step current; nothing was recorded or deleted.</summary>
    public bool IsCurrent { get; }

    public static StepProvenanceSession? Start(
        string? recordPath, string? accessLogPath, StepInvocation invocation, TextWriter diagnostics)
    {
        if (recordPath is null && accessLogPath is null) return null;
        string? fullRecordPath = null;
        if (recordPath is not null)
        {
            fullRecordPath = Path.GetFullPath(recordPath);
            var miss = StepProvenance.Evaluate(fullRecordPath, invocation);
            if (miss is null)
                return new StepProvenanceSession(fullRecordPath, null, invocation, null, diagnostics, isCurrent: true);
            diagnostics.WriteLine($"[translator] {invocation.Step} must run: {miss}.");
            // From here until a successful finish there is no record, so a crash can never leave one claiming "current".
            if (!TryDelete(fullRecordPath, diagnostics)) fullRecordPath = null;
        }

        var recorder = new FileAccessRecorder();
        TrackedFs.Install(recorder);
        return new StepProvenanceSession(
            fullRecordPath, accessLogPath is null ? null : Path.GetFullPath(accessLogPath),
            invocation, recorder, diagnostics, isCurrent: false);
    }

    public void Finish(int exitCode)
    {
        if (_recorder is null) return;
        TrackedFs.Install(null);
        // A failed step only ever loses its record; nothing is inspected that could fail in turn.
        if ((_recordPath is null || exitCode != 0) && _accessLogPath is null) return;
        // Recording is an optimisation: failing to write it must never fail a step that succeeded.
        try
        {
            Record(exitCode);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            _diagnostics.WriteLine($"[translator] could not write the {_invocation.Step} provenance record: {ex.Message}");
            if (_recordPath is not null) TryDelete(_recordPath, _diagnostics);
        }
    }

    private static bool TryDelete(string path, TextWriter diagnostics)
    {
        try
        {
            if (File.Exists(path)) File.Delete(path);
            return true;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            diagnostics.WriteLine($"[translator] could not delete {path}; no provenance record is kept: {ex.Message}");
            return false;
        }
    }

    private void Record(int exitCode)
    {
        var snapshot = _recorder!.Snapshot();
        var record = StepProvenance.CreateRecord(_invocation, snapshot);
        if (_accessLogPath is not null) StepProvenance.WriteAtomically(_accessLogPath, record);
        if (_recordPath is null || exitCode != 0) return;
        if (snapshot.UntrackedInputs.Count != 0)
        {
            _diagnostics.WriteLine(
                $"[translator] {_invocation.Step} read {string.Join(", ", snapshot.UntrackedInputs)}; no provenance record is kept.");
            return;
        }
        if (_invocation.TranslatorIdentity is null)
        {
            _diagnostics.WriteLine($"[translator] the running translator could not be identified; no provenance record is kept.");
            return;
        }
        StepProvenance.WriteAtomically(_recordPath, record);
    }
}
