using System.Globalization;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace WiiCompiled.Setup.Linux;

/// <summary>
/// The provenance record written after a fully successful Linux install: every input that decides
/// what the installed products contain, and the content of every file the build published.
/// Kept at ~/.local/share/WiiCompiled/product-provenance.json.
/// </summary>
internal sealed class ProductProvenanceRecord
{
    public int SchemaVersion { get; set; }
    public string Kind { get; set; } = "";
    public Dictionary<string, string>? Inputs { get; set; }
    public List<ProductOutputRecord>? Outputs { get; set; }
}

internal sealed class ProductOutputRecord
{
    public string Path { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
}

/// <summary>Size and whole-second modification time, as stat's %s / %Y report them.</summary>
internal readonly record struct FileFacts(long Size, long UnixMtime);

internal sealed record SyncStateEntry(string Path, string Sha256, long Size, long Mtime);

/// <summary>A parsed .mkw-sync-state written by build-cache.sh's mkw_sync_tree.</summary>
internal sealed record SyncState(long Written, IReadOnlyList<string> Directories, IReadOnlyList<SyncStateEntry> Entries);

internal sealed record ManifestEntry(string Sha256, long Size);

/// <summary>
/// Pure decision logic for skipping a rebuild when the installed products are provably current.
/// Every function fails closed: anything missing, malformed or different means "not current".
/// </summary>
internal static class ProductCurrency
{
    public const int SchemaVersion = 1;
    public const string Kind = "wiicompiled-linux-product-provenance";
    public const string None = "<none>";
    public const string SyncStateHeader = "mkw-sync-state-v1";
    public const string ManifestHeader = "mkw-sync-manifest-v1";

    /// <summary>Exactly the same keys with exactly the same values.</summary>
    public static bool InputsMatch(IReadOnlyDictionary<string, string>? recorded, IReadOnlyDictionary<string, string>? current)
    {
        if (recorded is null || current is null || current.Count == 0 || recorded.Count != current.Count) return false;
        foreach (var (key, value) in current)
        {
            if (!recorded.TryGetValue(key, out var recordedValue) ||
                !string.Equals(recordedValue, value, StringComparison.Ordinal))
                return false;
        }
        return true;
    }

    /// <summary>
    /// True only when the record has this schema, its inputs equal <paramref name="currentInputs"/>,
    /// it covers every <paramref name="requiredOutputs"/>, and every recorded output still has its
    /// recorded size and SHA-256 according to <paramref name="observe"/>.
    /// </summary>
    public static bool IsCurrent(ProductProvenanceRecord? recorded, IReadOnlyDictionary<string, string>? currentInputs,
        IEnumerable<string> requiredOutputs, Func<string, (long Size, string Sha256)?> observe)
    {
        if (recorded is null || recorded.SchemaVersion != SchemaVersion ||
            !string.Equals(recorded.Kind, Kind, StringComparison.Ordinal))
            return false;
        if (!InputsMatch(recorded.Inputs, currentInputs)) return false;
        if (recorded.Outputs is null || recorded.Outputs.Count == 0) return false;

        var seen = new HashSet<string>(StringComparer.Ordinal);
        foreach (var output in recorded.Outputs)
        {
            if (output is null || string.IsNullOrEmpty(output.Path) || !output.Path.StartsWith('/') ||
                output.Size < 0 || !IsSha256(output.Sha256) || !seen.Add(output.Path))
                return false;
        }
        foreach (var required in requiredOutputs)
        {
            if (!seen.Contains(required)) return false;
        }
        foreach (var output in recorded.Outputs)
        {
            var actual = observe(output.Path);
            if (actual is null || actual.Value.Size != output.Size ||
                !string.Equals(actual.Value.Sha256, output.Sha256, StringComparison.Ordinal))
                return false;
        }
        return true;
    }

    public static ProductProvenanceRecord CreateRecord(IReadOnlyDictionary<string, string> inputs,
        IEnumerable<ProductOutputRecord> outputs) => new()
    {
        SchemaVersion = SchemaVersion,
        Kind = Kind,
        Inputs = new Dictionary<string, string>(inputs, StringComparer.Ordinal),
        Outputs = outputs.ToList(),
    };

    /// <summary>Parses a sync state with the exact rules build-cache.sh's awk applies, or null.</summary>
    public static SyncState? ParseSyncState(string text)
    {
        if (!text.EndsWith('\n')) return null;
        var lines = text[..^1].Split('\n');
        if (lines.Length < 4 || lines[0] != SyncStateHeader) return null;

        var written = lines[1].Split('\t');
        if (written.Length != 2 || written[0] != "written" || !TryCount(written[1], out var writtenAt)) return null;

        var dirsLine = lines[2].Split('\t');
        if (dirsLine.Length < 2 || dirsLine[0] != "dirs") return null;
        var directories = dirsLine[1..];
        for (var i = 0; i < directories.Length; i++)
        {
            if (!IsSafeRelativePath(directories[i])) return null;
            for (var j = 0; j < directories.Length; j++)
            {
                // Overlapping directories would let one file count twice.
                if (i != j && (directories[i] == directories[j] ||
                               directories[j].StartsWith(directories[i] + "/", StringComparison.Ordinal)))
                    return null;
            }
        }

        var end = lines[^1].Split('\t');
        if (end.Length != 2 || end[0] != "end" || !TryCount(end[1], out var count)) return null;
        var body = lines[3..^1];
        if (body.Length != count) return null;

        var entries = new List<SyncStateEntry>(body.Length);
        var paths = new HashSet<string>(StringComparer.Ordinal);
        foreach (var line in body)
        {
            var fields = line.Split('\t', 4);
            if (fields.Length != 4 || !IsSha256(fields[0]) || !TryCount(fields[1], out var size) ||
                !TryCount(fields[2], out var mtime) || !IsSafeRelativePath(fields[3]) ||
                !directories.Any(directory => fields[3].StartsWith(directory + "/", StringComparison.Ordinal)) ||
                !paths.Add(fields[3]))
                return null;
            entries.Add(new SyncStateEntry(fields[3], fields[0], size, mtime));
        }
        return new SyncState(writtenAt, directories, entries);
    }

    /// <summary>Parses a .mkw-sync-manifest / .mkw-toolkit-manifest, or null.</summary>
    public static IReadOnlyDictionary<string, ManifestEntry>? ParseManifest(string text)
    {
        if (!text.EndsWith('\n')) return null;
        var lines = text[..^1].Split('\n');
        if (lines.Length < 2 || lines[0] != ManifestHeader) return null;
        var end = lines[^1].Split('\t');
        if (end.Length != 2 || end[0] != "end" || !TryCount(end[1], out var count)) return null;
        var body = lines[1..^1];
        if (body.Length != count) return null;

        var entries = new Dictionary<string, ManifestEntry>(StringComparer.Ordinal);
        foreach (var line in body)
        {
            var fields = line.Split('\t', 3);
            if (fields.Length != 3 || !IsSha256(fields[0]) || !TryCount(fields[1], out var size) ||
                !IsSafeRelativePath(fields[2]) || !entries.TryAdd(fields[2], new ManifestEntry(fields[0], size)))
                return null;
        }
        return entries;
    }

    /// <summary>
    /// The content identity of a synced workspace: the hashes its sync state recorded, but only if
    /// every recorded file still has its recorded size and mtime and the synced directories hold
    /// exactly the recorded files. A file whose mtime is not older than the record itself could
    /// have been rewritten within that same second, so it must also still hash to the recorded
    /// value. <paramref name="facts"/> returns null for a missing or non-regular file;
    /// <paramref name="hashOf"/> returns its SHA-256 or null; <paramref name="listFiles"/> returns
    /// every non-directory entry under a synced directory as a workspace-relative path, or null
    /// when that cannot be listed or holds a link.
    /// </summary>
    public static string? WorkspaceIdentity(SyncState state, Func<string, FileFacts?> facts,
        Func<string, string?> hashOf, Func<string, IReadOnlyCollection<string>?> listFiles)
    {
        foreach (var entry in state.Entries)
        {
            var current = facts(entry.Path);
            if (current is null || current.Value.Size != entry.Size || current.Value.UnixMtime != entry.Mtime) return null;
            if (entry.Mtime >= state.Written &&
                !string.Equals(hashOf(entry.Path), entry.Sha256, StringComparison.Ordinal))
                return null;
        }

        var expected = new HashSet<string>(state.Entries.Select(entry => entry.Path), StringComparer.Ordinal);
        var listed = 0;
        foreach (var directory in state.Directories)
        {
            var files = listFiles(directory);
            if (files is null) return null;
            foreach (var file in files)
            {
                if (!expected.Contains(file)) return null;
                listed++;
            }
        }
        if (listed != expected.Count) return null;

        var identity = new StringBuilder("mkw-workspace-content-v1\n");
        identity.Append("dirs\t").Append(string.Join('\t', state.Directories)).Append('\n');
        foreach (var entry in state.Entries.OrderBy(entry => entry.Path, StringComparer.Ordinal))
            identity.Append(entry.Sha256).Append('\t').Append(entry.Size.ToString(CultureInfo.InvariantCulture))
                .Append('\t').Append(entry.Path).Append('\n');
        return Sha256Hex(Encoding.UTF8.GetBytes(identity.ToString()));
    }

    /// <summary>
    /// The toolkit identity (the manifest's own hash), but only if the manifest is well formed,
    /// every listed file still has its listed size, and it lists every required file.
    /// </summary>
    public static string? ToolkitIdentity(byte[] manifestBytes, IEnumerable<string> requiredFiles, Func<string, long?> sizeOf)
    {
        string text;
        try { text = new UTF8Encoding(false, true).GetString(manifestBytes); }
        catch (ArgumentException) { return null; }
        var manifest = ParseManifest(text);
        if (manifest is null) return null;
        foreach (var required in requiredFiles)
        {
            if (!manifest.ContainsKey(required)) return null;
        }
        foreach (var (path, entry) in manifest)
        {
            if (sizeOf(path) != entry.Size) return null;
        }
        return Sha256Hex(manifestBytes);
    }

    /// <summary>The path of <paramref name="resolved"/> inside <paramref name="root"/>, or null.</summary>
    public static string? RelativeInside(string root, string resolved)
    {
        var prefix = root.EndsWith('/') ? root : root + "/";
        if (!resolved.StartsWith(prefix, StringComparison.Ordinal) || resolved.Length == prefix.Length) return null;
        var relative = resolved[prefix.Length..];
        return IsSafeRelativePath(relative) ? relative : null;
    }

    public static bool IsSafeRelativePath(string path) =>
        path.Length > 0 && path[0] != '/' && path.IndexOfAny(['\\', '\0', '\t', '\n', '\r']) < 0 &&
        path.Split('/').All(segment => segment.Length > 0 && segment != "." && segment != "..");

    public static bool IsSha256(string? value) =>
        value is { Length: 64 } && value.All(c => c is >= '0' and <= '9' or >= 'a' and <= 'f');

    public static string Sha256Hex(byte[] bytes) => Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant();

    private static bool TryCount(string text, out long value)
    {
        value = 0;
        return text.Length > 0 && text.All(char.IsAsciiDigit) &&
               long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out value);
    }
}

/// <summary>Every input the currency check reads, as InstallAsync resolved them.</summary>
internal sealed record ProductCurrencyOptions(
    string Workspace,
    string AssetsDirectory,
    string Profile,
    IReadOnlyList<(string Directory, string Executable)> Products,
    string InstallDirectory,
    string? BaseInstallDirectory,
    string? RetroDirectory,
    string? RetroWfcOfflineDirectory,
    bool SkipRetroWfcPayload,
    string? TranslatorBin,
    string? CcBin,
    string? CxxBin,
    string? FuseLd,
    string? CmakeBin,
    string? NinjaBin,
    string? NativePrebuiltDir,
    string? Sysroot);

/// <summary>
/// File-system side of the currency check. Only a Linux run from a toolkit that ships
/// .mkw-toolkit-manifest (the AppImage) qualifies; everything else always builds.
/// </summary>
internal static class ProductCurrencyProbe
{
    public const string ToolkitManifestName = ".mkw-toolkit-manifest";
    public const string SyncStateName = ".mkw-sync-state";

    // Published beside the executable by local-build.sh's publish_built_product.
    private static readonly string[] CompanionFiles =
        ["dsp_coef.bin", "initial_pipeline_cache.db", "cacert.pem", "local-build.json"];

    // The mbed TLS path cannot reach any server without the CA bundle beside the executable.
    private const string CaBundle = "cacert.pem";

    public static IEnumerable<string> RequiredOutputs(ProductCurrencyOptions options) =>
        options.Products.SelectMany(product => new[]
        {
            Path.GetFullPath(Path.Combine(product.Directory, product.Executable)),
            Path.GetFullPath(Path.Combine(product.Directory, CaBundle)),
        });

    /// <summary>True when every product executable can still be run by its owner.</summary>
    public static bool ExecutablesRunnable(ProductCurrencyOptions options)
    {
        if (!OperatingSystem.IsLinux()) return false;
        try
        {
            return options.Products.All(product =>
                (File.GetUnixFileMode(Path.Combine(product.Directory, product.Executable)) & UnixFileMode.UserExecute) != 0);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return false;
        }
    }

    /// <summary>The current inputs, or null (with a reason) when they cannot all be pinned down.</summary>
    public static Dictionary<string, string>? TryCollectInputs(ProductCurrencyOptions options, out string reason)
    {
        try
        {
            return CollectInputs(options, out reason);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException
                                       or ArgumentException or NotSupportedException or PlatformNotSupportedException)
        {
            reason = ex.Message;
            return null;
        }
    }

    private static Dictionary<string, string>? CollectInputs(ProductCurrencyOptions options, out string reason)
    {
        if (!OperatingSystem.IsLinux()) { reason = "only Linux installs are checked"; return null; }
        if (options.Sysroot is not null) { reason = "--sysroot contents are not tracked"; return null; }

        // The AppImage runs this from <AppDir>/usr/bin, and its manifest sits at <AppDir>.
        var resolvedRoot = RealPath(Path.Combine(AppContext.BaseDirectory, "..", ".."));
        if (resolvedRoot is null) { reason = "no toolkit manifest"; return null; }
        string toolkitRoot = resolvedRoot;
        var manifestPath = Path.Combine(toolkitRoot, ToolkitManifestName);
        if (!File.Exists(manifestPath)) { reason = "no toolkit manifest"; return null; }

        var inputs = new Dictionary<string, string>(StringComparer.Ordinal);
        var required = new List<string>();
        string? ToolkitFile(string key, string? path)
        {
            // The build resolves a relative tool from the workspace, not from setup's directory.
            if (string.IsNullOrEmpty(path) || !Path.IsPathRooted(path)) return null;
            var resolved = RealPath(path);
            var relative = resolved is null || !File.Exists(resolved) ? null : ProductCurrency.RelativeInside(toolkitRoot, resolved);
            if (relative is null) return null;
            required.Add(relative);
            inputs[key] = relative;
            return relative;
        }

        if (ToolkitFile("setup.executable", Environment.ProcessPath) is null ||
            ToolkitFile("tool.translator", options.TranslatorBin) is null ||
            ToolkitFile("tool.cc", options.CcBin) is null ||
            ToolkitFile("tool.cxx", options.CxxBin) is null ||
            ToolkitFile("tool.cmake", options.CmakeBin) is null ||
            ToolkitFile("tool.ninja", options.NinjaBin) is null)
        {
            reason = "a build tool is not part of the toolkit";
            return null;
        }

        if (string.IsNullOrEmpty(options.FuseLd))
            inputs["tool.fuse-ld"] = ProductCurrency.None;
        else if (!options.FuseLd.Contains('/'))
            inputs["tool.fuse-ld"] = "name:" + options.FuseLd;
        else if (ToolkitFile("tool.fuse-ld", options.FuseLd) is { } linker)
            inputs["tool.fuse-ld"] = "path:" + linker;
        else { reason = "the linker is not part of the toolkit"; return null; }

        if (string.IsNullOrEmpty(options.NativePrebuiltDir))
            inputs["tool.native-prebuilt"] = ProductCurrency.None;
        else
        {
            var resolved = RealPath(options.NativePrebuiltDir);
            var relative = resolved is null || !Directory.Exists(resolved) ? null : ProductCurrency.RelativeInside(toolkitRoot, resolved);
            if (relative is null) { reason = "the native prebuilt package is not part of the toolkit"; return null; }
            inputs["tool.native-prebuilt"] = relative;
        }

        var toolkit = ProductCurrency.ToolkitIdentity(File.ReadAllBytes(manifestPath), required,
            relative => SizeOf(Path.Combine(toolkitRoot, relative)));
        if (toolkit is null) { reason = "the toolkit does not match its manifest"; return null; }
        inputs["toolkit.content"] = toolkit;

        var workspace = Path.GetFullPath(options.Workspace);
        var stateText = ReadIfExists(Path.Combine(workspace, SyncStateName));
        var state = stateText is null ? null : ProductCurrency.ParseSyncState(stateText);
        var workspaceIdentity = state is null ? null : ProductCurrency.WorkspaceIdentity(state,
            relative => Facts(Path.Combine(workspace, relative)),
            relative => Observe(Path.Combine(workspace, relative))?.Sha256,
            directory => ListFiles(workspace, directory));
        if (workspaceIdentity is null) { reason = "the workspace no longer matches its last sync"; return null; }
        inputs["workspace.path"] = workspace;
        inputs["workspace.content"] = workspaceIdentity;

        inputs["setup.version"] = ProductInfo.Version;
        inputs["host.rid"] = RuntimeInformation.RuntimeIdentifier;
        inputs["host.architecture"] = RuntimeInformation.OSArchitecture.ToString();
        inputs["host.x86-cpu-profile"] = HostCpuRequirement.SelectBuildProfile().ToString();

        inputs["profile"] = options.Profile;
        inputs["install.dir"] = Path.GetFullPath(options.InstallDirectory);
        inputs["install.base-dir"] = options.BaseInstallDirectory is null ? ProductCurrency.None : Path.GetFullPath(options.BaseInstallDirectory);
        inputs["assets.main-dol.sha256"] = Sha256Of(Path.Combine(options.AssetsDirectory, "main.dol"));
        inputs["assets.static-rel.sha256"] = Sha256Of(Path.Combine(options.AssetsDirectory, "StaticR.rel"));

        if (string.IsNullOrEmpty(options.RetroDirectory))
        {
            inputs["retro.dir"] = ProductCurrency.None;
        }
        else
        {
            var retro = Path.GetFullPath(options.RetroDirectory);
            inputs["retro.dir"] = retro;
            inputs["retro.code-pul.sha256"] = Sha256Of(Path.Combine(retro, "Binaries", "Code.pul"));
            // The translator mounts the pack root and its files/ folder as DVD overlays.
            inputs["retro.files-dir"] = Directory.Exists(Path.Combine(retro, "files")) ? "present" : "absent";
            if (options.SkipRetroWfcPayload)
            {
                inputs["retro.wfc"] = "skip";
            }
            else
            {
                if (string.IsNullOrEmpty(options.RetroWfcOfflineDirectory)) { reason = "no Retro-WFC payload"; return null; }
                var payload = DirectoryDigest(options.RetroWfcOfflineDirectory);
                if (payload is null) { reason = "the Retro-WFC payload cannot be read"; return null; }
                inputs["retro.wfc"] = "payload:" + payload;
            }
        }

        reason = "";
        return inputs;
    }

    /// <summary>Every file the build published for <paramref name="options"/>'s products, or null.</summary>
    public static List<ProductOutputRecord>? TryCollectOutputs(ProductCurrencyOptions options)
    {
        try
        {
            var outputs = new List<ProductOutputRecord>();
            foreach (var (directory, executable) in options.Products)
            {
                if (!AddOutput(outputs, Path.Combine(directory, executable))) return null;
                foreach (var companion in CompanionFiles)
                {
                    var path = Path.Combine(directory, companion);
                    if (!File.Exists(path) && companion == CaBundle) return null;
                    if (File.Exists(path) && !AddOutput(outputs, path)) return null;
                }
                var bootstrap = Path.Combine(directory, "wii_bootstrap");
                if (Directory.Exists(bootstrap))
                {
                    var files = ListFiles(directory, "wii_bootstrap");
                    if (files is null) return null;
                    foreach (var file in files.Order(StringComparer.Ordinal))
                    {
                        if (!AddOutput(outputs, Path.Combine(directory, file))) return null;
                    }
                }
            }
            return outputs;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Size and SHA-256 of a regular, non-link file, or null.</summary>
    public static (long Size, string Sha256)? Observe(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.LinkTarget is not null) return null;
            using var stream = File.OpenRead(path);
            var sha = Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
            return (stream.Length, sha);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException
                                       or NotSupportedException)
        {
            return null;
        }
    }

    /// <summary>Removes the record before a build, so a failed or interrupted build never leaves one.</summary>
    public static void DeleteRecord(string recordPath, IInstallReporter? reporter)
    {
        try
        {
            if (File.Exists(recordPath)) File.Delete(recordPath);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            reporter?.Diagnostic($"Could not remove the product provenance record ({ex.Message.TrimEnd('.')}).");
        }
    }

    /// <summary>
    /// Records the products just built, but only when the inputs read again now still equal the
    /// ones read before the build. Never fails the install; without a record the next run builds.
    /// </summary>
    public static void TryRecord(string recordPath, ProductCurrencyOptions options,
        IReadOnlyDictionary<string, string> inputsBeforeBuild, IInstallReporter reporter)
    {
        try
        {
            var inputsAfterBuild = TryCollectInputs(options, out _);
            if (!ProductCurrency.InputsMatch(inputsBeforeBuild, inputsAfterBuild)) return;
            var outputs = TryCollectOutputs(options);
            if (outputs is null || outputs.Count == 0) return;
            JsonState.Write(recordPath, ProductCurrency.CreateRecord(inputsBeforeBuild, outputs));
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidOperationException
                                       or ArgumentException or NotSupportedException)
        {
            reporter.Diagnostic($"Could not record the product provenance ({ex.Message.TrimEnd('.')}).");
        }
    }

    /// <summary>
    /// realpath(3) in managed code: resolves every symbolic link component. Null when the path does
    /// not exist or cannot be resolved.
    /// </summary>
    public static string? RealPath(string path)
    {
        try
        {
            var full = Path.IsPathRooted(path) ? path : Path.Combine(Directory.GetCurrentDirectory(), path);
            full = full.Replace('\\', '/');
            if (!full.StartsWith('/')) return null;
            var remaining = new LinkedList<string>(full.Split('/', StringSplitOptions.RemoveEmptyEntries));
            var current = "/";
            var links = 0;
            while (remaining.First is { } first)
            {
                var part = first.Value;
                remaining.RemoveFirst();
                if (part == ".") continue;
                if (part == "..")
                {
                    current = Path.GetDirectoryName(current)?.Replace('\\', '/') is { Length: > 0 } parent ? parent : "/";
                    continue;
                }
                var candidate = current == "/" ? "/" + part : current + "/" + part;
                var target = new FileInfo(candidate).LinkTarget;
                if (target is not null)
                {
                    if (++links > 40) return null;
                    var targetParts = target.Replace('\\', '/').Split('/', StringSplitOptions.RemoveEmptyEntries);
                    for (var i = targetParts.Length - 1; i >= 0; i--) remaining.AddFirst(targetParts[i]);
                    if (target.StartsWith('/')) current = "/";
                    continue;
                }
                if (!File.Exists(candidate) && !Directory.Exists(candidate)) return null;
                current = candidate;
            }
            return current;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }

    /// <summary>Size and whole-second mtime of a regular, non-link file, or null.</summary>
    public static FileFacts? Facts(string path)
    {
        try
        {
            var info = new FileInfo(path);
            if (!info.Exists || info.LinkTarget is not null) return null;
            return new FileFacts(info.Length, new DateTimeOffset(info.LastWriteTimeUtc).ToUnixTimeSeconds());
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }

    /// <summary>
    /// Every non-directory entry under <paramref name="root"/>/<paramref name="directory"/>, as
    /// '/'-separated paths relative to <paramref name="root"/>. Empty for a missing directory;
    /// null when anything is a link or cannot be read.
    /// </summary>
    public static IReadOnlyCollection<string>? ListFiles(string root, string directory)
    {
        try
        {
            var top = new DirectoryInfo(Path.Combine(root, directory));
            if (top.LinkTarget is not null) return null;
            if (!top.Exists) return File.Exists(top.FullName) ? null : Array.Empty<string>();
            var files = new List<string>();
            var options = new EnumerationOptions
            {
                RecurseSubdirectories = true,
                AttributesToSkip = 0,
                IgnoreInaccessible = false,
                ReturnSpecialDirectories = false,
            };
            foreach (var entry in top.EnumerateFileSystemInfos("*", options))
            {
                if (entry.LinkTarget is not null) return null;
                if (entry is DirectoryInfo) continue;
                files.Add(Path.GetRelativePath(root, entry.FullName).Replace('\\', '/'));
            }
            return files;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }

    /// <summary>Digest of every file (path, size, SHA-256) under a directory, or null.</summary>
    public static string? DirectoryDigest(string directory)
    {
        var full = Path.GetFullPath(directory);
        var files = ListFiles(Path.GetDirectoryName(full) ?? full, Path.GetFileName(full));
        if (files is null || !Directory.Exists(full)) return null;
        var digest = new StringBuilder("mkw-directory-v1\n");
        var parent = Path.GetDirectoryName(full) ?? full;
        foreach (var file in files.Order(StringComparer.Ordinal))
        {
            var observed = Observe(Path.Combine(parent, file));
            if (observed is null) return null;
            digest.Append(observed.Value.Sha256).Append('\t')
                .Append(observed.Value.Size.ToString(CultureInfo.InvariantCulture)).Append('\t')
                .Append(file).Append('\n');
        }
        return ProductCurrency.Sha256Hex(Encoding.UTF8.GetBytes(digest.ToString()));
    }

    private static bool AddOutput(List<ProductOutputRecord> outputs, string path)
    {
        var full = Path.GetFullPath(path);
        var observed = Observe(full);
        if (observed is null) return false;
        outputs.Add(new ProductOutputRecord { Path = full, Size = observed.Value.Size, Sha256 = observed.Value.Sha256 });
        return true;
    }

    private static long? SizeOf(string path)
    {
        try
        {
            var info = new FileInfo(path);
            return info.Exists && info.LinkTarget is null ? info.Length : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }

    private static string? ReadIfExists(string path)
    {
        try
        {
            return File.Exists(path) ? File.ReadAllText(path, new UTF8Encoding(false, true)) : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return null;
        }
    }

    private static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }
}
