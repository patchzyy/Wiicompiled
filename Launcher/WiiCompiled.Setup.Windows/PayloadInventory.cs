using System.Text.Json;
using System.Text.Json.Serialization;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// The per-file identity of every shipped <c>Toolkit/</c> and <c>BuildWorkspace/</c> file, written
/// once at release-build time by <c>--emit-payload-identities</c> beside (never inside) the small
/// <c>payload-manifest.json</c>. It lets an install verify what it streams out of the payload, and
/// reuse an unchanged installed component after a cheap stat check, without re-hashing ~28,000 files.
/// </summary>
internal sealed class PayloadInventory
{
    public const int CurrentSchemaVersion = 1;
    public const string FileName = "payload-inventory.json";

    /// <summary>
    /// Build-produced workspace directories (never shipped) that installs carry across updates.
    /// They are not release content, so integrity checks of the workspace ignore them.
    /// </summary>
    public static readonly string[] WorkspaceCacheDirectories =
        ["generated", "build", "native-build", "Assets", "PulsarPacks"];

    /// <summary>
    /// Extracted toolkit files keep this exact timestamp (see <see cref="PayloadArchive"/>). Nothing
    /// a build runs writes into the toolkit, so any other timestamp means the file was replaced.
    /// </summary>
    public static readonly DateTime NormalizedTimestampUtc = new(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);

    // FAT32/exFAT record local time at two-second resolution: a portable install on such a volume
    // reads back the normalized stamp shifted by the machine's UTC offset. Anything a program wrote
    // since is decades away from 2000-01-01, so this tolerance cannot hide a replaced file.
    private static readonly TimeSpan TimestampTolerance = TimeSpan.FromHours(15);

    public int SchemaVersion { get; set; }

    /// <summary>The compile identity this inventory was computed together with.</summary>
    public string ToolkitFingerprint { get; set; } = "";
    public string ToolkitPackageFingerprint { get; set; } = "";
    public List<PayloadInventoryFile> Files { get; set; } = [];

    /// <summary>
    /// Payload-relative files each translation project reads, as the shipped translator's
    /// <c>list-translation-inputs</c> reported them at release-build time.
    /// </summary>
    public Dictionary<string, List<string>> TranslationInputs { get; set; } = new(StringComparer.Ordinal);

    private Dictionary<string, PayloadInventoryFile>? _byPath;

    [JsonIgnore]
    public IReadOnlyDictionary<string, PayloadInventoryFile> ByPath =>
        _byPath ??= Files.ToDictionary(file => file.Path, StringComparer.Ordinal);

    public IEnumerable<PayloadInventoryFile> Under(string prefix) =>
        Files.Where(file => file.Path.StartsWith(prefix, StringComparison.Ordinal));

    public IReadOnlyDictionary<string, string> Hashes() =>
        Files.ToDictionary(file => file.Path, file => file.Sha256, StringComparer.Ordinal);

    public IReadOnlyDictionary<string, IReadOnlyList<string>> TranslationInputLists() =>
        TranslationInputs.ToDictionary(pair => pair.Key, pair => (IReadOnlyList<string>)pair.Value,
            StringComparer.Ordinal);

    public static PayloadInventory Parse(Stream stream)
    {
        var inventory = JsonSerializer.Deserialize<PayloadInventory>(stream,
                            new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
                        ?? throw new InvalidDataException("The payload inventory is invalid.");
        inventory.Validate();
        return inventory;
    }

    public void Validate()
    {
        if (SchemaVersion != CurrentSchemaVersion)
            throw new InvalidDataException($"Unsupported payload inventory schema {SchemaVersion}.");
        if (Files is not { Count: > 0 })
            throw new InvalidDataException("The payload inventory lists no files.");
        if (ToolkitFingerprint is null || ToolkitPackageFingerprint is null)
            throw new InvalidDataException("The payload inventory has no toolkit identity.");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var file in Files)
        {
            if (file is null || file.Sha256 is null)
                throw new InvalidDataException("The payload inventory has an incomplete entry.");
            if (string.IsNullOrEmpty(file.Path) || file.Path.Contains('\\') || file.Path.StartsWith('/') ||
                file.Path.Split('/').Any(part => part is "" or "." or "..") ||
                !(file.Path.StartsWith(InstalledLayout.ToolkitEntryPrefix, StringComparison.Ordinal) ||
                  file.Path.StartsWith(InstalledLayout.WorkspaceEntryPrefix, StringComparison.Ordinal)) ||
                file.Size < 0 || file.Sha256.Length != 64 || !file.Sha256.All(char.IsAsciiHexDigit))
                throw new InvalidDataException($"The payload inventory has an invalid entry '{file.Path}'.");
            if (!seen.Add(file.Path))
                throw new InvalidDataException($"The payload inventory lists '{file.Path}' twice.");
        }
        TranslationInputs ??= new Dictionary<string, List<string>>(StringComparer.Ordinal);
        if (TranslationInputs.Any(pair => pair.Value is null || pair.Value.Any(path => path is null)))
            throw new InvalidDataException("The payload inventory has an incomplete translation input list.");
    }

    /// <summary>Compact on purpose: it lists every shipped file and is read once per install.</summary>
    public void Write(string path) => File.WriteAllText(path, JsonSerializer.Serialize(this));

    /// <summary>
    /// Compares the files under <paramref name="prefix"/> in the installation at <paramref name="root"/>
    /// with the inventory, without reading any content: every expected file must exist as a regular
    /// file with its exact byte size (and, for the toolkit, the normalized payload timestamp), and
    /// nothing else may be there. Paths under <paramref name="excludedPrefixes"/> are skipped on both
    /// sides; the workspace's build caches are always skipped. Returns the first few discrepancies;
    /// an empty list means the component may be reused as the inventory describes it.
    /// </summary>
    public List<string> StatCheck(string root, string prefix, bool requireNormalizedTimestamp,
        IReadOnlyCollection<string>? excludedPrefixes = null, CancellationToken cancellationToken = default,
        int maxReported = 10)
    {
        excludedPrefixes ??= [];
        var problems = new List<string>();
        bool Excluded(string relative) =>
            excludedPrefixes.Any(excluded => relative.StartsWith(excluded, StringComparison.Ordinal)) ||
            IsWorkspaceCache(relative);

        var expected = Under(prefix).Where(file => !Excluded(file.Path))
            .ToDictionary(file => file.Path, StringComparer.OrdinalIgnoreCase);
        var directory = Path.Combine(root, prefix.TrimEnd('/').Replace('/', Path.DirectorySeparatorChar));
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        void Report(string problem)
        {
            if (problems.Count < maxReported) problems.Add(problem);
            else if (problems.Count == maxReported) problems.Add("...");
        }

        try
        {
            if (!Directory.Exists(directory))
            {
                if (expected.Count > 0) Report($"{prefix} is missing");
                return problems;
            }
            if ((File.GetAttributes(directory) & FileAttributes.ReparsePoint) != 0)
            {
                Report($"{prefix} is a link, not shipped content");
                return problems;
            }
            var pending = new Stack<DirectoryInfo>();
            pending.Push(new DirectoryInfo(directory));
            while (pending.Count > 0)
            {
                cancellationToken.ThrowIfCancellationRequested();
                foreach (var entry in pending.Pop().EnumerateFileSystemInfos())
                {
                    var relative = Path.GetRelativePath(root, entry.FullName).Replace('\\', '/');
                    if (Excluded(relative)) continue;
                    if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
                    {
                        Report($"{relative} is a link, not shipped content");
                        continue;
                    }
                    if (entry is DirectoryInfo subdirectory)
                    {
                        pending.Push(subdirectory);
                        continue;
                    }
                    var file = (FileInfo)entry;
                    if (!expected.TryGetValue(relative, out var shipped))
                    {
                        Report($"{relative} is not part of this release");
                        continue;
                    }
                    seen.Add(relative);
                    if (!shipped.Path.Equals(relative, StringComparison.Ordinal))
                        Report($"{relative} differs in case from {shipped.Path}");
                    else if (file.Length != shipped.Size)
                        Report($"{relative} is {file.Length} bytes, expected {shipped.Size}");
                    else if (requireNormalizedTimestamp &&
                             (file.LastWriteTimeUtc - NormalizedTimestampUtc).Duration() > TimestampTolerance)
                        Report($"{relative} was modified after it was installed");
                }
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            Report($"{prefix} cannot be inspected: {ex.Message}");
            return problems;
        }

        foreach (var missing in expected.Keys.Where(path => !seen.Contains(path)).Order(StringComparer.Ordinal))
            Report($"{missing} is missing");
        return problems;
    }

    private static bool IsWorkspaceCache(string relative)
    {
        if (!relative.StartsWith(InstalledLayout.WorkspaceEntryPrefix, StringComparison.Ordinal)) return false;
        var top = relative[InstalledLayout.WorkspaceEntryPrefix.Length..].Split('/')[0];
        return WorkspaceCacheDirectories.Contains(top, StringComparer.OrdinalIgnoreCase);
    }

    /// <summary>
    /// Throws unless the files streamed out of the payload under <paramref name="prefix"/> are
    /// exactly what the inventory lists, byte size and SHA-256 included.
    /// </summary>
    public void VerifyExtracted(string prefix, IReadOnlyDictionary<string, PayloadFileIdentity> extracted)
    {
        var expected = Under(prefix).ToList();
        var problems = new List<string>();
        foreach (var file in expected)
        {
            if (!extracted.TryGetValue(file.Path, out var actual))
                problems.Add($"{file.Path} is missing from the payload");
            else if (actual.Size != file.Size ||
                     !actual.Sha256.Equals(file.Sha256, StringComparison.OrdinalIgnoreCase))
                problems.Add($"{file.Path} does not match its recorded identity");
            if (problems.Count >= 10) break;
        }
        foreach (var path in extracted.Keys)
        {
            if (problems.Count >= 10) break;
            if (path.StartsWith(prefix, StringComparison.Ordinal) && !ByPath.ContainsKey(path))
                problems.Add($"{path} is not listed in the payload inventory");
        }
        if (problems.Count > 0)
            throw new InvalidDataException("The installation payload is damaged: " +
                                           string.Join("; ", problems) + ". Download the setup again.");
    }
}

internal sealed class PayloadInventoryFile
{
    public string Path { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
}

/// <summary>The identity of one file as it was streamed out of the payload.</summary>
internal sealed record PayloadFileIdentity(long Size, string Sha256);

/// <summary>How much of the shipped toolkit an operation extracts.</summary>
internal enum ToolkitRefresh
{
    /// <summary>The installed toolkit is the release's, verified by the stat check.</summary>
    None,

    /// <summary>Only <c>Toolkit/Translator</c> changed; the installed toolchain is reused.</summary>
    TranslatorOnly,

    /// <summary>The whole toolkit is extracted and published.</summary>
    Full
}

/// <summary>How much of the shipped build workspace an operation extracts.</summary>
internal enum WorkspaceExtraction { None, RuntimeAssetsOnly, Full }
