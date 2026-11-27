using System.Security.Cryptography;

namespace Translator.Core.IO;

public enum FileScanKind
{
    Files,
    Directories,
}

/// <summary>One directory listing exactly as a command asked for it.</summary>
public sealed record FileScanQuery(string Directory, string Pattern, SearchOption Option, FileScanKind Kind)
{
    public FileScanQuery Normalized() => this with { Directory = Path.GetFullPath(Directory) };

    /// <summary>The entries the query yields right now, relative to its directory, '/'-separated, ordinal order.</summary>
    public string[] ListNow()
    {
        var root = Path.GetFullPath(Directory);
        var entries = Kind == FileScanKind.Files
            ? System.IO.Directory.EnumerateFiles(root, Pattern, Option)
            : System.IO.Directory.EnumerateDirectories(root, Pattern, Option);
        return Relative(root, entries);
    }

    public static string[] Relative(string root, IEnumerable<string> entries) =>
        entries.Select(entry => Path.GetRelativePath(root, Path.GetFullPath(entry)).Replace('\\', '/'))
            .Order(StringComparer.Ordinal)
            .ToArray();
}

/// <summary>Everything one command run touched, as <see cref="FileAccessRecorder.Snapshot"/> reports it.</summary>
public sealed record FileAccessSnapshot(
    IReadOnlyList<(string Path, string Sha256)> Inputs,
    IReadOnlyList<(string Path, bool IsDirectory, bool Exists)> Probes,
    IReadOnlyList<(FileScanQuery Query, string[] Entries)> Scans,
    IReadOnlyList<(string Path, long Size)> Outputs,
    IReadOnlyList<(string Value, string Expanded)> EnvironmentExpansions,
    IReadOnlyList<string> CurrentDirectories,
    IReadOnlyList<string> UntrackedInputs);

/// <summary>
/// Collects what a command reads (with the content hash it saw), which paths it probed,
/// which directories it listed, and what it wrote. Paths the command itself created during
/// the run (staging trees, temporaries, its own outputs read back) are not inputs.
/// First observation wins: a file read and later rewritten keeps the hash it was read
/// with, so the record cannot match until a run reads what it leaves behind (fail closed).
/// Thread-safe; translation reads from many workers at once.
/// </summary>
public sealed class FileAccessRecorder
{
    public static readonly StringComparer PathComparer =
        OperatingSystem.IsWindows() || OperatingSystem.IsMacOS() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal;

    private readonly object _gate = new();
    private readonly Dictionary<string, string> _inputs = new(PathComparer);
    private readonly Dictionary<string, (bool IsDirectory, bool Exists)> _probes = new(PathComparer);
    private readonly Dictionary<FileScanQuery, string[]> _scans = new(new QueryComparer());
    private readonly HashSet<FileScanQuery> _outputScans = new(new QueryComparer());
    private readonly HashSet<string> _producedFiles = new(PathComparer);
    private readonly HashSet<string> _producedDirectories = new(PathComparer);
    private readonly HashSet<string> _outputTrees = new(PathComparer);
    private readonly HashSet<string> _deletedFiles = new(PathComparer);
    private readonly HashSet<string> _deletedDirectories = new(PathComparer);
    private readonly Dictionary<string, string> _expansions = new(StringComparer.Ordinal);
    private readonly HashSet<string> _currentDirectories = new(StringComparer.Ordinal);
    private readonly List<string> _untracked = new();

    /// <summary>Whether a read of <paramref name="path"/> still needs its content hashed.</summary>
    public bool WantsContent(string path)
    {
        var full = Path.GetFullPath(path);
        lock (_gate) return !_inputs.ContainsKey(full) && !IsProducedLocked(full);
    }

    public void OnRead(string path, ReadOnlySpan<byte> content)
    {
        if (!WantsContent(path)) return;
        OnReadHash(path, SHA256.HashData(content));
    }

    public void OnReadHash(string path, byte[] sha256)
    {
        var full = Path.GetFullPath(path);
        lock (_gate)
        {
            if (IsProducedLocked(full)) return;
            _inputs.TryAdd(full, Convert.ToHexString(sha256).ToLowerInvariant());
        }
    }

    public void OnFileProbe(string path, bool exists) => OnProbe(path, isDirectory: false, exists);

    public void OnDirectoryProbe(string path, bool exists) => OnProbe(path, isDirectory: true, exists);

    private void OnProbe(string path, bool isDirectory, bool exists)
    {
        var full = Path.GetFullPath(path);
        lock (_gate)
        {
            if (IsProducedLocked(full)) return;
            _probes.TryAdd(ProbeKey(full, isDirectory), (isDirectory, exists));
        }
    }

    public void OnScan(FileScanQuery query, IEnumerable<string> entries)
    {
        var normalized = query.Normalized();
        var listing = FileScanQuery.Relative(normalized.Directory, entries);
        lock (_gate)
        {
            if (IsProducedLocked(normalized.Directory)) return;
            _scans.TryAdd(normalized, listing);
        }
    }

    public void OnOutputScan(FileScanQuery query)
    {
        var normalized = query.Normalized();
        lock (_gate)
        {
            if (IsProducedLocked(normalized.Directory)) return;
            _outputScans.Add(normalized);
        }
    }

    public void OnWrite(string path)
    {
        var full = Path.GetFullPath(path);
        lock (_gate) _producedFiles.Add(full);
    }

    public void OnDeleteFile(string path)
    {
        var full = Path.GetFullPath(path);
        lock (_gate)
        {
            if (!_producedFiles.Remove(full) && !IsProducedLocked(full))
                _deletedFiles.Add(full);
        }
    }

    public void OnCreateDirectory(string path)
    {
        var full = Path.GetFullPath(path);
        lock (_gate) _producedDirectories.Add(full);
    }

    public void OnDeleteDirectory(string path)
    {
        var full = Path.GetFullPath(path);
        lock (_gate)
        {
            if (!IsProducedLocked(full)) _deletedDirectories.Add(full);
            _outputTrees.RemoveWhere(tree => IsSameOrUnder(tree, full));
        }
    }

    public void OnMoveDirectory(string source, string destination)
    {
        var fullSource = Path.GetFullPath(source);
        var fullDestination = Path.GetFullPath(destination);
        lock (_gate)
        {
            var sourceWasProduced = IsProducedLocked(fullSource);
            if (!sourceWasProduced) _deletedDirectories.Add(fullSource);
            _outputTrees.RemoveWhere(tree => IsSameOrUnder(tree, fullSource));
            _producedDirectories.Add(fullDestination);
            // A tree the command generated and then published is its output; a pre-existing
            // tree it only moved aside (a backup) is not.
            if (sourceWasProduced) _outputTrees.Add(fullDestination);
        }
    }

    public void OnEnvironmentExpansion(string value, string expanded)
    {
        if (string.Equals(value, expanded, StringComparison.Ordinal) && !value.Contains('%')) return;
        lock (_gate) _expansions.TryAdd(value, expanded);
    }

    public void OnCurrentDirectory(string directory)
    {
        lock (_gate) _currentDirectories.Add(directory);
    }

    public void OnUntrackedInput(string description)
    {
        lock (_gate) _untracked.Add(description);
    }

    /// <summary>
    /// The run as a whole, judged by the file system as it stands now: outputs are the files
    /// the command produced that still exist (and every file of a tree it published), output
    /// listings are taken now, and a pre-existing path it deleted must stay deleted.
    /// </summary>
    public FileAccessSnapshot Snapshot()
    {
        lock (_gate)
        {
            var probes = new Dictionary<string, (bool IsDirectory, bool Exists)>(_probes, PathComparer);
            foreach (var deleted in _deletedFiles.Where(path => !File.Exists(path) && !Directory.Exists(path)))
                probes.TryAdd(ProbeKey(deleted, isDirectory: false), (false, false));
            foreach (var deleted in _deletedDirectories.Where(path => !File.Exists(path) && !Directory.Exists(path)))
                probes.TryAdd(ProbeKey(deleted, isDirectory: true), (true, false));

            var scans = new Dictionary<FileScanQuery, string[]>(_scans, new QueryComparer());
            foreach (var query in _outputScans)
                scans[query] = Directory.Exists(query.Directory) ? query.ListNow() : [];

            var outputs = new Dictionary<string, long>(PathComparer);
            foreach (var path in _producedFiles.Where(File.Exists))
                outputs[path] = new FileInfo(path).Length;
            foreach (var tree in _outputTrees.Where(Directory.Exists))
            {
                var query = new FileScanQuery(tree, "*", SearchOption.AllDirectories, FileScanKind.Files);
                scans[query] = query.ListNow();
                foreach (var file in Directory.EnumerateFiles(tree, "*", SearchOption.AllDirectories))
                    outputs[Path.GetFullPath(file)] = new FileInfo(file).Length;
            }

            return new FileAccessSnapshot(
                _inputs.OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                    .Select(static pair => (pair.Key, pair.Value)).ToArray(),
                probes.Select(static pair => (Path: pair.Key[2..], pair.Value.IsDirectory, pair.Value.Exists))
                    .OrderBy(static item => item.Path, StringComparer.Ordinal)
                    .ThenBy(static item => item.IsDirectory)
                    .ToArray(),
                scans.OrderBy(static pair => pair.Key.Directory, StringComparer.Ordinal)
                    .ThenBy(static pair => pair.Key.Pattern, StringComparer.Ordinal)
                    .ThenBy(static pair => pair.Key.Option)
                    .ThenBy(static pair => pair.Key.Kind)
                    .Select(static pair => (pair.Key, pair.Value)).ToArray(),
                outputs.OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                    .Select(static pair => (pair.Key, pair.Value)).ToArray(),
                _expansions.OrderBy(static pair => pair.Key, StringComparer.Ordinal)
                    .Select(static pair => (pair.Key, pair.Value)).ToArray(),
                _currentDirectories.Order(StringComparer.Ordinal).ToArray(),
                _untracked.ToArray());
        }
    }

    private static string ProbeKey(string fullPath, bool isDirectory) => (isDirectory ? "D:" : "F:") + fullPath;

    private bool IsProducedLocked(string fullPath)
    {
        if (_producedFiles.Contains(fullPath)) return true;
        for (var current = fullPath; !string.IsNullOrEmpty(current); current = Path.GetDirectoryName(current))
        {
            if (_producedDirectories.Contains(current)) return true;
        }
        return false;
    }

    private static bool IsSameOrUnder(string path, string root)
    {
        if (PathComparer.Equals(path, root)) return true;
        var prefix = Path.TrimEndingDirectorySeparator(root) + Path.DirectorySeparatorChar;
        return path.StartsWith(prefix, OperatingSystem.IsWindows() || OperatingSystem.IsMacOS()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal);
    }

    private sealed class QueryComparer : IEqualityComparer<FileScanQuery>
    {
        public bool Equals(FileScanQuery? x, FileScanQuery? y) =>
            ReferenceEquals(x, y) ||
            (x is not null && y is not null &&
             PathComparer.Equals(x.Directory, y.Directory) &&
             string.Equals(x.Pattern, y.Pattern, StringComparison.Ordinal) &&
             x.Option == y.Option && x.Kind == y.Kind);

        public int GetHashCode(FileScanQuery obj) =>
            HashCode.Combine(PathComparer.GetHashCode(obj.Directory), obj.Pattern, obj.Option, obj.Kind);
    }
}
