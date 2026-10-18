using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// Content identity of everything that decides what the locally produced executables contain.
/// First link of toolkit content -&gt; toolkit-state.json -&gt; per-product build-fingerprint.json:
/// unchanged skips retranslation, any change forces a rebuild. Content-based so re-tags are free.
/// </summary>
internal static class ToolkitFingerprint
{
    // v6/native-toolchain v2 forces one clean rebuild past a Ninja stale-object bug.
    // translation v2 added runtime/src, which the translator regex-scans for native overrides;
    // v3 replaced it with the native index extracted from it, whenever that index is current.
    private const string Version = "mkwc-toolkit-compile-v6";
    private const string TranslationVersion = "mkwc-toolkit-translation-v3";
    private const string NativeToolchainVersion = "mkwc-toolkit-native-toolchain-v2";
    private const string PackageVersion = "mkwc-toolkit-package-v1";
    private const string RuntimeAssetsVersion = "mkwc-product-runtime-assets-v3";
    private const string RuntimeAssetsDescription = "The product runtime assets";

    private static readonly string[] SourceExtensions =
        [".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".s", ".asm",
         ".cmake", ".txt", ".yml", ".yaml", ".toml", ".json", ".ps1", ".in", ".def", ".patch"];

    /// <summary>
    /// Inputs that decide translator output: translator, translation project, the native override
    /// registrations/effect contracts it extracts from runtime/src (see
    /// <see cref="AddNativeRegistrations"/>), and the build script's translator command lines.
    /// Unchanged means the completed base translation can be reused instead of re-run.
    /// </summary>
    private static readonly string[] TranslationPrefixes =
    [
        InstalledLayout.ToolkitEntryPrefix + "Translator/",
        InstalledLayout.WorkspaceEntryPrefix + "projects/"
    ];

    private static readonly string[] TranslationFiles =
        [InstalledLayout.WorkspaceEntryPrefix + "LocalBuild.ps1"];

    private const string NativeSourcePrefix = InstalledLayout.WorkspaceEntryPrefix + "runtime/src/";
    private const string NativeIndexEntry = InstalledLayout.WorkspaceEntryPrefix + "runtime/native-index.json";
    // The file set the translator's native index source digest covers.
    private static readonly string[] NativeIndexSourceExtensions = [".cpp", ".h", ".hpp"];

    /// <summary>
    /// Inputs that make compiled objects unsafe to reuse: the compiler/CMake/Ninja toolchain, the
    /// shipped runtime DLLs, and the scripts owning configure flags. Source/dependency changes are
    /// deliberately excluded, since CMake/Ninja already rebuild exactly the affected objects.
    /// </summary>
    private static readonly string[] NativeToolchainPrefixes =
    [
        InstalledLayout.ToolkitEntryPrefix + "llvm-mingw/",
        InstalledLayout.ToolkitEntryPrefix + "CMake/",
        InstalledLayout.ToolkitEntryPrefix + "Ninja/",
        InstalledLayout.ToolkitEntryPrefix + "Redist/"
    ];

    private static readonly string[] NativeToolchainFiles =
    [
        InstalledLayout.WorkspaceEntryPrefix + "LocalBuild.ps1",
        InstalledLayout.WorkspaceEntryPrefix + "NativeBuildFlags.ps1"
    ];

    /// <summary>
    /// Fingerprint of the toolkit at <paramref name="root"/>: the staged payload root at
    /// release-build time, or an install directory when a compile path re-verifies it. The install
    /// path itself never computes this, it compares manifest identity against toolkit-state.json.
    /// </summary>
    public static string Compute(string root, CancellationToken cancellationToken = default) =>
        ComputeComponents(root, cancellationToken).Compile;

    /// <summary>
    /// One walk, three identities: the compile identity plus its translation and native-toolchain
    /// subsets. The subsets never gate correctness, compile identity alone decides if products are
    /// current, they only decide how much completed work carries into the next build.
    /// </summary>
    public static ToolkitFingerprintComponents ComputeComponents(string root,
        CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        var toolkit = InstalledLayout.Toolkit(root);
        var workspace = InstalledLayout.Workspace(root);
        var entries = new SortedDictionary<string, string>(StringComparer.Ordinal);

        // nodtool validates/extracts the user disc but does not influence generated products.
        // Everything else in Toolkit can affect translation, compilation, linking, or copied
        // runtime support and therefore belongs to the compile identity.
        AddDirectory(entries, root, toolkit, null,
            cancellationToken,
            file => !Path.GetFileName(file).Equals("nodtool.exe", StringComparison.OrdinalIgnoreCase));
        AddFile(entries, root, Path.Combine(workspace, "LocalBuild.ps1"), cancellationToken);
        AddFile(entries, root, Path.Combine(workspace, "NativeBuildFlags.ps1"), cancellationToken);
        AddDirectory(entries, root, Path.Combine(workspace, "projects"), null, cancellationToken);
        var runtime = Path.Combine(workspace, "runtime");
        var runtimeAssets = Path.Combine(runtime, "assets");
        // runtime/assets is copied beside the executable; it is deliberately tracked by the
        // independent runtime-assets identity below so changing those bytes does not recompile code.
        AddDirectory(entries, root, runtime, SourceExtensions, cancellationToken,
            file => !FileSystemUtilities.PathContains(runtimeAssets, file));
        AddDirectory(entries, root, Path.Combine(workspace, "aurora-main"), SourceExtensions,
            cancellationToken);
        // The shipped precompiled aurora archives and the pinned Dawn runtime are linked into the
        // product, so a change there changes the binary just as surely as a translator change does.
        // native_prebuilt's provenance.json is excluded: its Contents list already identifies every
        // shipped byte, while its BuiltUtc stamp would make a bit-identical re-harvest look like a
        // toolkit change and force a global user-side rebuild for nothing.
        AddDirectory(entries, root, Path.Combine(workspace, "Dependencies"), null, cancellationToken,
            file => !file.EndsWith(
                Path.Combine("native_prebuilt", "provenance.json"), StringComparison.OrdinalIgnoreCase));

        if (entries.Count == 0)
            throw new InvalidDataException($"No toolkit files were found under {root}; the installation is incomplete.");

        var translation = SelectSubset(entries, TranslationPrefixes, TranslationFiles);
        AddNativeRegistrations(translation, entries, root);
        return new ToolkitFingerprintComponents(
            BuildIdentity(Version, entries),
            BuildIdentity(TranslationVersion, translation),
            BuildIdentity(NativeToolchainVersion,
                SelectSubset(entries, NativeToolchainPrefixes, NativeToolchainFiles)));
    }

    private static SortedDictionary<string, string> SelectSubset(SortedDictionary<string, string> entries,
        string[] prefixes, string[] files)
    {
        var subset = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var (relative, hash) in entries)
        {
            if (prefixes.Any(prefix => relative.StartsWith(prefix, StringComparison.Ordinal)) ||
                files.Any(file => relative.Equals(file, StringComparison.Ordinal)))
                subset[relative] = hash;
        }
        return subset;
    }

    /// <summary>
    /// A native-index.json extracted from exactly the shipped runtime/src stands in for those
    /// sources, minus its source digest, so a runtime change that leaves the registrations alone
    /// keeps the base translation. A missing, unreadable or stale index keeps the sources instead.
    /// </summary>
    private static void AddNativeRegistrations(SortedDictionary<string, string> subset,
        SortedDictionary<string, string> entries, string root)
    {
        var sources = entries.Where(entry => entry.Key.StartsWith(NativeSourcePrefix, StringComparison.Ordinal))
            .ToList();
        var registrations = TryHashCurrentNativeIndex(
            Path.Combine(InstalledLayout.Workspace(root), "runtime", "native-index.json"), sources);
        if (registrations is not null)
        {
            subset[NativeIndexEntry] = registrations;
            return;
        }
        foreach (var (relative, hash) in sources) subset[relative] = hash;
    }

    private static string? TryHashCurrentNativeIndex(string path, List<KeyValuePair<string, string>> sources)
    {
        if (!File.Exists(path)) return null;
        JsonObject? document;
        try { document = JsonNode.Parse(File.ReadAllText(path)) as JsonObject; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException)
        {
            return null;
        }
        if (document?["sourceDigest"] is not JsonValue recorded || !recorded.TryGetValue<string>(out var digest))
            return null;

        // Mirrors the translator's RuntimeNativeIndexFile.ComputeSourceDigest.
        var manifest = new StringBuilder();
        foreach (var (relative, hash) in sources)
        {
            if (!NativeIndexSourceExtensions.Any(extension =>
                    relative.EndsWith(extension, StringComparison.OrdinalIgnoreCase)))
                continue;
            manifest.Append(hash).Append("  ").Append(relative, NativeSourcePrefix.Length,
                relative.Length - NativeSourcePrefix.Length).Append('\n');
        }
        if (!digest.Equals(Sha256Hex(manifest.ToString()), StringComparison.OrdinalIgnoreCase))
            return null;

        document.Remove("sourceDigest");
        return Sha256Hex(document.ToJsonString());
    }

    private static string Sha256Hex(string text) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(text))).ToLowerInvariant();

    /// <summary>
    /// Content identity of the source-owned files copied verbatim beside every product. This is
    /// intentionally independent of <see cref="Compute"/>: an asset-only release is published
    /// transactionally without translating or compiling unchanged code.
    /// </summary>
    public static string ComputeRuntimeAssets(string root, CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        var assets = Path.Combine(InstalledLayout.Workspace(root), "runtime", "assets");
        var entries = new SortedDictionary<string, string>(StringComparer.Ordinal);
        AddMappedDirectory(entries, Path.Combine(assets, ProductRuntimeAssets.SourceBootstrapDirectoryName),
            ProductRuntimeAssets.ProductBootstrapDirectoryName, cancellationToken);
        foreach (var (relativePath, productFileName) in ProductRuntimeAssets.Files)
            AddRequiredMappedFile(entries, ProductRuntimeAssets.SourceFile(assets, relativePath),
                productFileName, cancellationToken);
        return BuildIdentity(RuntimeAssetsVersion, entries);
    }

    public static string? TryComputeRuntimeAssets(string root, CancellationToken cancellationToken = default,
        Action<string>? diagnostic = null)
    {
        try { return ComputeRuntimeAssets(root, cancellationToken); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            diagnostic?.Invoke(
                $"The workspace runtime assets under {root} could not be fingerprinted: {ex.Message}");
            return null;
        }
    }

    /// <summary>Checks the actual copied product files against a source runtime-assets identity.</summary>
    public static bool ProductRuntimeAssetsMatch(string productDirectory, string expectedFingerprint,
        CancellationToken cancellationToken = default, Action<string>? diagnostic = null)
    {
        try
        {
            var entries = new SortedDictionary<string, string>(StringComparer.Ordinal);
            AddMappedDirectory(entries,
                Path.Combine(productDirectory, ProductRuntimeAssets.ProductBootstrapDirectoryName),
                ProductRuntimeAssets.ProductBootstrapDirectoryName, cancellationToken);
            foreach (var (_, productFileName) in ProductRuntimeAssets.Files)
                AddRequiredMappedFile(entries, Path.Combine(productDirectory, productFileName),
                    productFileName, cancellationToken);
            return BuildIdentity(RuntimeAssetsVersion, entries).Equals(expectedFingerprint,
                StringComparison.Ordinal);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            diagnostic?.Invoke(
                $"The copied runtime assets under {productDirectory} could not be verified: {ex.Message}");
            return false;
        }
    }

    /// <summary>Identity of the shipped Toolkit directory, including installer-only tools.</summary>
    public static string ComputePackage(string root, CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        var entries = new SortedDictionary<string, string>(StringComparer.Ordinal);
        AddDirectory(entries, root, InstalledLayout.Toolkit(root), null, cancellationToken);
        if (entries.Count == 0)
            throw new InvalidDataException($"No toolkit package files were found under {root}.");
        return BuildIdentity(PackageVersion, entries);
    }

    /// <summary>
    /// The toolkit is ~26,000 mostly tiny files (llvm-mingw's headers/libraries dominate), so this
    /// walk is bound by per-file open/read overhead, not hashing throughput, minutes rather than
    /// seconds on a cold cache under on-access antivirus. Hashing concurrently keeps a compiling
    /// repair from taking longer to prove toolkit integrity than to translate with it.
    /// </summary>
    private static void AddDirectory(SortedDictionary<string, string> entries, string root, string directory,
        string[]? extensions, CancellationToken cancellationToken = default,
        Func<string, bool>? include = null)
    {
        if (!Directory.Exists(directory)) return;
        var selected = new List<string>();
        foreach (var file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (include is not null && !include(file)) continue;
            if (extensions is not null &&
                !extensions.Contains(Path.GetExtension(file), StringComparer.OrdinalIgnoreCase))
                continue;
            selected.Add(file);
        }

        var hashed = new System.Collections.Concurrent.ConcurrentBag<(string Relative, string Hash)>();
        Parallel.ForEach(selected,
            new ParallelOptions
            {
                CancellationToken = cancellationToken,
                MaxDegreeOfParallelism = Math.Max(2, Environment.ProcessorCount)
            },
            file =>
            {
                if (!File.Exists(file)) return;
                var relative = Path.GetRelativePath(root, file).Replace('\\', '/');
                hashed.Add((relative, InputValidation.Sha256File(file)));
            });
        foreach (var (relative, hash) in hashed) entries[relative] = hash;
    }

    private static void AddFile(SortedDictionary<string, string> entries, string root, string file,
        CancellationToken cancellationToken = default)
    {
        if (!File.Exists(file)) return;
        cancellationToken.ThrowIfCancellationRequested();
        var relative = Path.GetRelativePath(root, file).Replace('\\', '/');
        entries[relative] = InputValidation.Sha256File(file);
    }

    private static void AddMappedDirectory(SortedDictionary<string, string> entries, string directory,
        string identityRoot, CancellationToken cancellationToken)
    {
        if (!Directory.Exists(directory))
            throw new InvalidDataException($"Required product runtime asset directory is missing: {directory}");
        var tree = FileSystemUtilities.EnumerateRegularTree(directory, cancellationToken,
            RuntimeAssetsDescription);
        if (tree.Count == 0)
            throw new InvalidDataException($"Required product runtime asset directory is empty: {directory}");
        foreach (var entry in tree)
        {
            if (entry.IsEmptyDirectory)
                entries[$"empty-directory:{identityRoot}/{entry.RelativePath}"] = "";
            else if (!entry.IsDirectory)
                entries[$"{identityRoot}/{entry.RelativePath}"] = InputValidation.Sha256File(entry.FullPath);
        }
    }

    private static void AddRequiredMappedFile(SortedDictionary<string, string> entries, string file,
        string identityPath, CancellationToken cancellationToken)
    {
        if (!File.Exists(file))
            throw new InvalidDataException($"Required product runtime asset is missing: {file}");
        FileSystemUtilities.RejectReparsePoint(new FileInfo(file), RuntimeAssetsDescription);
        cancellationToken.ThrowIfCancellationRequested();
        entries[identityPath] = InputValidation.Sha256File(file);
    }

    private static string BuildIdentity(string version, SortedDictionary<string, string> entries)
    {
        var builder = new StringBuilder(version).Append('\n');
        foreach (var (relative, hash) in entries)
            builder.Append(relative).Append('|').Append(hash).Append('\n');
        return Sha256Hex(builder.ToString());
    }
}

/// <summary>
/// Compile identity plus the two subsets deciding cache reuse: <see cref="Compile"/> is the
/// authoritative freshness identity, <see cref="Translation"/> decides if the base translation is
/// still current, <see cref="NativeToolchain"/> decides if compiled objects may still be linked.
/// </summary>
internal sealed record ToolkitFingerprintComponents(string Compile, string Translation,
    string NativeToolchain);
