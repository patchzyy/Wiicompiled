using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// Lists the files one translation project reads (<c>Translator.Cli list-translation-inputs</c>).
/// Returns absolute paths; throws when the list cannot be produced.
/// </summary>
internal delegate IReadOnlyList<string> TranslationInputLister(string translatorExecutable, string projectFile,
    CancellationToken cancellationToken);

/// <summary>
/// Content identity of everything that decides what the locally produced executables contain.
/// First link of toolkit content -&gt; toolkit-state.json -&gt; per-product build-fingerprint.json:
/// unchanged skips retranslation, any change forces a rebuild. Content-based so re-tags are free.
/// </summary>
/// <remarks>
/// Every identity is a pure function of a <c>relative path -&gt; SHA-256</c> map
/// (<see cref="ComputeComponentsFromHashes"/>). The disk walk (<see cref="ComputeComponents"/>) and
/// the install path, which reads the same map from the shipped payload inventory it verified while
/// streaming the payload out, therefore cannot disagree about which files they cover.
/// </remarks>
internal static class ToolkitFingerprint
{
    // v6/native-toolchain v2 forces one clean rebuild past a Ninja stale-object bug.
    // compile v8 widened aurora-main and runtime/third_party to every non-dot file, a superset of
    // what the precompiled-package gate (Get-MkwSourceTreeFingerprint) hashes, plus the shipped
    // native configure flags.
    // translation v2 added runtime/src, which the translator regex-scans for native overrides;
    // v3 replaced it with the native index extracted from it, whenever that index is current.
    // native-toolchain v3 keeps only what compiles objects (compiler, CMake + its modules, Ninja,
    // and the fixed configure flags), no longer the build scripts' source text or copied runtimes.
    private const string Version = "mkwc-toolkit-compile-v8";
    private const string TranslationVersion = "mkwc-toolkit-translation-v4";
    private const string TranslationByReleaseVersion = "mkwc-toolkit-translation-release-v1";
    private const string NativeToolchainVersion = "mkwc-toolkit-native-toolchain-v3";
    private const string PackageVersion = "mkwc-toolkit-package-v1";
    private const string ToolchainPackageVersion = "mkwc-toolkit-toolchain-package-v1";
    private const string TranslatorPackageVersion = "mkwc-toolkit-translator-package-v1";
    private const string RuntimeAssetsVersion = "mkwc-product-runtime-assets-v3";
    private const string RuntimeAssetsDescription = "The product runtime assets";

    /// <summary>
    /// The output of <c>Get-MkwNativeFixedConfigureFlags</c>, written by Build-Installer.ps1 beside
    /// LocalBuild.ps1. The native-toolchain identity hashes this rather than the scripts' source
    /// text, and LocalBuild.ps1 refuses to reuse a build directory if it disagrees with the script.
    /// </summary>
    public const string NativeConfigureFlagsFileName = "native-configure-flags.txt";

    private const string T = InstalledLayout.ToolkitEntryPrefix;
    private const string W = InstalledLayout.WorkspaceEntryPrefix;
    public const string TranslatorPrefix = T + "Translator/";

    private static readonly string[] SourceExtensions =
        [".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".inl", ".inc", ".s", ".asm",
         ".cmake", ".txt", ".yml", ".yaml", ".toml", ".json", ".ps1", ".in", ".def", ".patch"];

    private static readonly string[] WorkspaceRootFiles =
        ["LocalBuild.ps1", "NativeBuildFlags.ps1", NativeConfigureFlagsFileName];

    /// <summary>The workspace subtrees the compile identity walks.</summary>
    private static readonly string[] WorkspaceTrees = ["projects", "runtime", "aurora-main", "Dependencies"];

    /// <summary>
    /// Inputs that decide translator output: translator, translation project, the native override
    /// registrations/effect contracts it extracts from runtime/src (see
    /// <see cref="AddNativeRegistrations"/>), and the build script's translator command lines.
    /// Unchanged means the completed base translation can be reused instead of re-run.
    /// </summary>
    private static readonly string[] TranslationPrefixes =
    [
        TranslatorPrefix,
        W + "projects/"
    ];

    private static readonly string[] TranslationFiles =
    [
        // The single source of the base-translation command lines.
        W + "LocalBuild.ps1",
        // Maps the PAL-keyed native index to the installed release's addresses.
        W + "runtime/include/guest_abi/guest_versions.inc"
    ];

    private const string NativeSourcePrefix = W + "runtime/src/";
    private const string NativeIndexEntry = W + "runtime/native-index.json";
    // The file set the translator's native index source digest covers.
    private static readonly string[] NativeIndexSourceExtensions = [".cpp", ".h", ".hpp"];

    /// <summary>
    /// The relative-path rule of the compile identity. nodtool validates/extracts the user disc but
    /// does not influence generated products; everything else in Toolkit can affect translation,
    /// compilation, linking, or copied runtime support. runtime/assets is copied beside the
    /// executable and tracked by the independent runtime-assets identity, so changing those bytes
    /// does not recompile code. native_prebuilt's provenance.json is excluded: its Contents list
    /// already identifies every shipped byte, while its BuiltUtc stamp would make a bit-identical
    /// re-harvest look like a toolkit change and force a global user-side rebuild for nothing.
    /// aurora-main and runtime/third_party cover every non-dot file (plus every source-extension
    /// file, dot paths included), a byte-exact superset of the precompiled-package gate's hash.
    /// </summary>
    internal static bool IsCompileInput(string relative)
    {
        if (relative.StartsWith(T, StringComparison.Ordinal))
            return !FileName(relative).Equals("nodtool.exe", StringComparison.OrdinalIgnoreCase);
        if (!relative.StartsWith(W, StringComparison.Ordinal)) return false;
        var rest = relative[W.Length..];
        if (!rest.Contains('/'))
            return WorkspaceRootFiles.Contains(rest, StringComparer.Ordinal);
        if (rest.StartsWith("projects/", StringComparison.Ordinal)) return true;
        if (rest.StartsWith("runtime/", StringComparison.Ordinal))
        {
            if (rest.StartsWith("runtime/assets/", StringComparison.OrdinalIgnoreCase)) return false;
            if (HasSourceExtension(rest)) return true;
            const string thirdParty = "runtime/third_party/";
            return rest.StartsWith(thirdParty, StringComparison.Ordinal) &&
                   !HasDotSegment(rest[thirdParty.Length..]);
        }
        const string aurora = "aurora-main/";
        if (rest.StartsWith(aurora, StringComparison.Ordinal))
            return HasSourceExtension(rest) || !HasDotSegment(rest[aurora.Length..]);
        if (rest.StartsWith("Dependencies/", StringComparison.Ordinal))
            return !rest.EndsWith("native_prebuilt/provenance.json", StringComparison.OrdinalIgnoreCase);
        return false;
    }

    /// <summary>
    /// Inputs that make compiled objects unsafe to reuse: the compiler, CMake and the modules it
    /// generates build rules from, Ninja, and the fixed configure flags. Source/dependency changes
    /// are deliberately excluded, since CMake/Ninja already rebuild exactly the affected objects;
    /// LocalBuild.ps1 separately records the configure option names and CPU profile it used.
    /// </summary>
    internal static bool IsNativeToolchainInput(string relative)
    {
        if (relative.StartsWith(T + "llvm-mingw/", StringComparison.Ordinal)) return true;
        if (relative.Equals(T + "CMake/bin/cmake.exe", StringComparison.Ordinal)) return true;
        if (relative.Equals(T + "Ninja/ninja.exe", StringComparison.Ordinal)) return true;
        if (relative.Equals(W + NativeConfigureFlagsFileName, StringComparison.Ordinal)) return true;
        const string share = T + "CMake/share/";
        if (!relative.StartsWith(share, StringComparison.Ordinal)) return false;
        var parts = relative[share.Length..].Split('/');
        return parts.Length > 2 && parts[0].StartsWith("cmake-", StringComparison.Ordinal) &&
               parts[1].Equals("Modules", StringComparison.Ordinal);
    }

    private static bool HasSourceExtension(string relative) =>
        SourceExtensions.Contains(Path.GetExtension(relative), StringComparer.OrdinalIgnoreCase);

    private static bool HasDotSegment(string relative) =>
        relative.Split('/').Any(part => part.StartsWith('.'));

    private static string FileName(string relative)
    {
        var slash = relative.LastIndexOf('/');
        return slash < 0 ? relative : relative[(slash + 1)..];
    }

    /// <summary>
    /// Fingerprint of the toolkit at <paramref name="root"/>: the staged payload root at
    /// release-build time, or an install directory when a compile path re-verifies it. The install
    /// path itself never computes this, it compares manifest identity against toolkit-state.json.
    /// </summary>
    public static string Compute(string root, CancellationToken cancellationToken = default) =>
        ComputeComponents(root, cancellationToken, lister: NoTranslationInputs).Compile;

    /// <summary>
    /// One walk, every identity: the compile identity plus its translation and native-toolchain
    /// subsets. The subsets never gate correctness, compile identity alone decides if products are
    /// current, they only decide how much completed work carries into the next build.
    /// <paramref name="lister"/> defaults to running the toolkit's own translator; when it fails the
    /// per-release translation identities are omitted (or, with <paramref name="strictTranslationInputs"/>,
    /// the failure is thrown), which only ever costs a retranslation.
    /// </summary>
    public static ToolkitFingerprintComponents ComputeComponents(string root,
        CancellationToken cancellationToken = default, TranslationInputLister? lister = null,
        bool strictTranslationInputs = false, Action<string>? diagnostic = null)
    {
        root = Path.GetFullPath(root);
        var hashes = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var selected = new List<string>();
        CollectFiles(selected, InstalledLayout.Toolkit(root), cancellationToken);
        var workspace = InstalledLayout.Workspace(root);
        foreach (var name in WorkspaceRootFiles)
        {
            var file = Path.Combine(workspace, name);
            if (File.Exists(file)) selected.Add(file);
        }
        foreach (var tree in WorkspaceTrees)
            CollectFiles(selected, Path.Combine(workspace, tree), cancellationToken);
        HashFiles(hashes, root, selected.Where(file => IsCompileInput(Relative(root, file))),
            cancellationToken);

        if (hashes.Count == 0)
            throw new InvalidDataException($"No toolkit files were found under {root}; the installation is incomplete.");

        var translationInputs = ListTranslationInputs(root, lister ?? RunTranslatorLister,
            strictTranslationInputs, diagnostic, cancellationToken);
        if (translationInputs is not null)
        {
            var listed = translationInputs.Values.SelectMany(files => files)
                .Where(relative => !hashes.ContainsKey(relative) && IsPayloadContent(relative))
                .Distinct(StringComparer.Ordinal)
                .Select(relative => Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)))
                .ToList();
            HashFiles(hashes, root, listed, cancellationToken);
        }

        return ComputeComponentsFromHashes(hashes, relative => ReadText(root, relative), translationInputs);
    }

    /// <summary>
    /// The identities as a pure function of file hashes. <paramref name="hashes"/> may hold more
    /// than the identities consume (a whole payload inventory); only the selection rules above
    /// decide what counts. <paramref name="readText"/> supplies the native index document, and
    /// <paramref name="translationInputs"/> maps each translation project directory name to the
    /// payload-relative files it reads.
    /// </summary>
    public static ToolkitFingerprintComponents ComputeComponentsFromHashes(
        IReadOnlyDictionary<string, string> hashes, Func<string, string?> readText,
        IReadOnlyDictionary<string, IReadOnlyList<string>>? translationInputs)
    {
        var entries = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var (relative, hash) in hashes)
            if (IsCompileInput(relative)) entries[relative] = hash;
        if (entries.Count == 0)
            throw new InvalidDataException("No toolkit files were found; the installation is incomplete.");

        var nativeRegistrations = new SortedDictionary<string, string>(StringComparer.Ordinal);
        AddNativeRegistrations(nativeRegistrations, entries, readText);

        var translation = SelectSubset(entries, TranslationPrefixes, TranslationFiles);
        foreach (var (relative, hash) in nativeRegistrations) translation[relative] = hash;

        var byRelease = new SortedDictionary<string, string>(StringComparer.Ordinal);
        if (translationInputs is not null)
        {
            foreach (var (project, files) in translationInputs)
            {
                var identity = ComputeReleaseTranslation(entries, hashes, nativeRegistrations, files);
                if (identity is not null) byRelease[project] = identity;
            }
        }

        var nativeToolchain = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var (relative, hash) in entries)
            if (IsNativeToolchainInput(relative)) nativeToolchain[relative] = hash;

        return new ToolkitFingerprintComponents(
            BuildIdentity(Version, entries),
            BuildIdentity(TranslationVersion, translation),
            BuildIdentity(NativeToolchainVersion, nativeToolchain))
        {
            TranslationByRelease = byRelease
        };
    }

    /// <summary>
    /// The translation identity of one release: everything the legacy identity covers except the
    /// translation projects, which are narrowed to the files that release's project actually reads.
    /// Null when a listed input is outside the payload or unknown, so the caller falls back to the
    /// whole-projects identity instead of trusting an incomplete list.
    /// </summary>
    private static string? ComputeReleaseTranslation(SortedDictionary<string, string> entries,
        IReadOnlyDictionary<string, string> hashes, SortedDictionary<string, string> nativeRegistrations,
        IReadOnlyList<string> files)
    {
        var subset = SelectSubset(entries, [TranslatorPrefix], TranslationFiles);
        foreach (var (relative, hash) in nativeRegistrations) subset[relative] = hash;
        foreach (var relative in files)
        {
            // Covered (or deliberately replaced) by the native registrations above.
            if (relative.StartsWith(NativeSourcePrefix, StringComparison.Ordinal) ||
                relative.Equals(NativeIndexEntry, StringComparison.Ordinal))
                continue;
            // User-owned inputs staged into the workspace's build caches (a profile's Code.pul under
            // PulsarPacks) are not release content, exactly as for the whole-project identity:
            // LocalBuild.ps1 decides their effect itself through the base translation's
            // modPatchAwareness stamp and check-base-mod-awareness.
            if (IsWorkspaceCacheContent(relative)) continue;
            if (!IsPayloadContent(relative) || !hashes.TryGetValue(relative, out var hash)) return null;
            subset[relative] = hash;
        }
        return BuildIdentity(TranslationByReleaseVersion, subset);
    }

    /// <summary>Whether a payload-relative path names shipped Toolkit/BuildWorkspace content.</summary>
    private static bool IsPayloadContent(string relative) =>
        relative.StartsWith(T, StringComparison.Ordinal) ||
        (relative.StartsWith(W, StringComparison.Ordinal) && !IsWorkspaceCacheContent(relative));

    private static bool IsWorkspaceCacheContent(string relative) =>
        relative.StartsWith(W, StringComparison.Ordinal) &&
        PayloadInventory.WorkspaceCacheDirectories.Contains(relative[W.Length..].Split('/')[0],
            StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// Runs the translator's static input lister for every shipped translation project. Returns
    /// null (or throws when strict) when the list cannot be produced.
    /// </summary>
    internal static SortedDictionary<string, IReadOnlyList<string>>? ListTranslationInputs(string root,
        TranslationInputLister lister, bool strict, Action<string>? diagnostic,
        CancellationToken cancellationToken)
    {
        var projects = Path.Combine(InstalledLayout.Workspace(root), "projects");
        var translator = Path.Combine(InstalledLayout.Toolkit(root), "Translator", "Translator.Cli.exe");
        var result = new SortedDictionary<string, IReadOnlyList<string>>(StringComparer.Ordinal);
        try
        {
            if (!Directory.Exists(projects)) return result;
            foreach (var directory in Directory.EnumerateDirectories(projects).Order(StringComparer.Ordinal))
            {
                cancellationToken.ThrowIfCancellationRequested();
                var project = Path.Combine(directory, "recomp.yml");
                if (!File.Exists(project)) continue;
                var listed = lister(translator, project, cancellationToken);
                var relatives = new SortedSet<string>(StringComparer.Ordinal);
                foreach (var path in listed)
                {
                    var relative = Path.GetRelativePath(root, Path.GetFullPath(path)).Replace('\\', '/');
                    if (Path.IsPathRooted(relative) || relative.StartsWith("../", StringComparison.Ordinal) ||
                        relative == "..")
                        throw new InvalidDataException(
                            $"The translation project {Path.GetFileName(directory)} reads {path}, outside the toolkit.");
                    relatives.Add(relative);
                }
                if (relatives.Count == 0)
                    throw new InvalidDataException(
                        $"The translator listed no inputs for the translation project {Path.GetFileName(directory)}.");
                result[Path.GetFileName(directory)] = relatives.ToList();
            }
            return result;
        }
        catch (Exception ex) when (!strict && ex is not OperationCanceledException)
        {
            diagnostic?.Invoke("The per-release translation identities could not be derived; the " +
                               "whole-project translation identity applies instead: " + ex.Message);
            return null;
        }
    }

    /// <summary>A lister that produces no per-release identities, for callers that only need Compile.</summary>
    internal static readonly TranslationInputLister NoTranslationInputs = (_, _, _) =>
        throw new InvalidOperationException("Per-release translation inputs were not requested.");

    /// <summary>Runs <c>Translator.Cli list-translation-inputs --project &lt;recomp.yml&gt;</c>.</summary>
    internal static IReadOnlyList<string> RunTranslatorLister(string translatorExecutable, string projectFile,
        CancellationToken cancellationToken)
    {
        if (!File.Exists(translatorExecutable))
            throw new FileNotFoundException("The bundled translator is missing.", translatorExecutable);
        var start = new ProcessStartInfo
        {
            FileName = translatorExecutable,
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = Path.GetDirectoryName(projectFile)!
        };
        start.ArgumentList.Add("list-translation-inputs");
        start.ArgumentList.Add("--project");
        start.ArgumentList.Add(projectFile);
        using var process = Process.Start(start)
                            ?? throw new InvalidOperationException("The translator could not be started.");
        var stderr = process.StandardError.ReadToEndAsync(cancellationToken);
        var stdout = process.StandardOutput.ReadToEndAsync(cancellationToken);
        if (!process.WaitForExit(TimeSpan.FromMinutes(2)))
        {
            try { process.Kill(entireProcessTree: true); } catch { }
            throw new TimeoutException("Listing the translation inputs did not finish.");
        }
        process.WaitForExit();
        if (process.ExitCode != 0)
            throw new InvalidDataException(
                $"list-translation-inputs failed with exit code {process.ExitCode}: {stderr.Result.Trim()}");
        return stdout.Result.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
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
        SortedDictionary<string, string> entries, Func<string, string?> readText)
    {
        var sources = entries.Where(entry => entry.Key.StartsWith(NativeSourcePrefix, StringComparison.Ordinal))
            .ToList();
        var registrations = entries.ContainsKey(NativeIndexEntry)
            ? TryHashCurrentNativeIndex(readText(NativeIndexEntry), sources)
            : null;
        if (registrations is not null)
        {
            subset[NativeIndexEntry] = registrations;
            return;
        }
        foreach (var (relative, hash) in sources) subset[relative] = hash;
    }

    private static string? TryHashCurrentNativeIndex(string? text, List<KeyValuePair<string, string>> sources)
    {
        if (text is null) return null;
        JsonObject? document;
        try { document = JsonNode.Parse(text) as JsonObject; }
        catch (JsonException)
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

    private static string? ReadText(string root, string relative)
    {
        try { return File.ReadAllText(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar))); }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return null;
        }
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
    public static string ComputePackage(string root, CancellationToken cancellationToken = default) =>
        ComputePackageComponents(root, cancellationToken).Package;

    /// <summary>
    /// The whole-Toolkit package identity plus its two halves: the toolchain (everything but the
    /// translator) and the translator, so an update that only replaces the translator republishes
    /// just <c>Toolkit/Translator</c>.
    /// </summary>
    public static ToolkitPackageComponents ComputePackageComponents(string root,
        CancellationToken cancellationToken = default)
    {
        root = Path.GetFullPath(root);
        var selected = new List<string>();
        CollectFiles(selected, InstalledLayout.Toolkit(root), cancellationToken);
        var hashes = new SortedDictionary<string, string>(StringComparer.Ordinal);
        HashFiles(hashes, root, selected, cancellationToken);
        if (hashes.Count == 0)
            throw new InvalidDataException($"No toolkit package files were found under {root}.");
        return ComputePackageComponentsFromHashes(hashes);
    }

    public static ToolkitPackageComponents ComputePackageComponentsFromHashes(
        IReadOnlyDictionary<string, string> hashes)
    {
        var package = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var toolchain = new SortedDictionary<string, string>(StringComparer.Ordinal);
        var translator = new SortedDictionary<string, string>(StringComparer.Ordinal);
        foreach (var (relative, hash) in hashes)
        {
            if (!relative.StartsWith(T, StringComparison.Ordinal)) continue;
            package[relative] = hash;
            (relative.StartsWith(TranslatorPrefix, StringComparison.Ordinal) ? translator : toolchain)[relative] = hash;
        }
        if (package.Count == 0)
            throw new InvalidDataException("No toolkit package files were found.");
        return new ToolkitPackageComponents(BuildIdentity(PackageVersion, package),
            BuildIdentity(ToolchainPackageVersion, toolchain),
            BuildIdentity(TranslatorPackageVersion, translator));
    }

    private static void CollectFiles(List<string> selected, string directory, CancellationToken cancellationToken)
    {
        if (!Directory.Exists(directory)) return;
        foreach (var file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            selected.Add(file);
        }
    }

    private static string Relative(string root, string file) =>
        Path.GetRelativePath(root, file).Replace('\\', '/');

    /// <summary>
    /// The toolkit is ~26,000 mostly tiny files (llvm-mingw's headers/libraries dominate), so this
    /// walk is bound by per-file open/read overhead, not hashing throughput, minutes rather than
    /// seconds on a cold cache under on-access antivirus. Hashing concurrently keeps a compiling
    /// repair from taking longer to prove toolkit integrity than to translate with it.
    /// </summary>
    private static void HashFiles(SortedDictionary<string, string> entries, string root, IEnumerable<string> files,
        CancellationToken cancellationToken)
    {
        var hashed = new System.Collections.Concurrent.ConcurrentBag<(string Relative, string Hash)>();
        Parallel.ForEach(files,
            new ParallelOptions
            {
                CancellationToken = cancellationToken,
                MaxDegreeOfParallelism = Math.Max(2, Environment.ProcessorCount)
            },
            file =>
            {
                if (!File.Exists(file)) return;
                hashed.Add((Relative(root, file), InputValidation.Sha256File(file)));
            });
        foreach (var (relative, hash) in hashed) entries[relative] = hash;
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
/// Compile identity plus the subsets deciding cache reuse: <see cref="Compile"/> is the
/// authoritative freshness identity, <see cref="Translation"/> decides if the base translation is
/// still current (<see cref="TranslationByRelease"/> narrows it to the project that release
/// reads), <see cref="NativeToolchain"/> decides if compiled objects may still be linked.
/// </summary>
internal sealed record ToolkitFingerprintComponents(string Compile, string Translation,
    string NativeToolchain)
{
    /// <summary>Translation identity per translation project directory name; may be empty.</summary>
    public IReadOnlyDictionary<string, string> TranslationByRelease { get; init; } =
        new SortedDictionary<string, string>(StringComparer.Ordinal);

    /// <summary>Exact equality including the per-release map (records compare maps by reference).</summary>
    public bool SameIdentities(ToolkitFingerprintComponents other) =>
        Compile == other.Compile && Translation == other.Translation &&
        NativeToolchain == other.NativeToolchain &&
        TranslationByRelease.Count == other.TranslationByRelease.Count &&
        TranslationByRelease.All(pair =>
            other.TranslationByRelease.TryGetValue(pair.Key, out var value) && value == pair.Value);
}

/// <summary>The whole-Toolkit package identity and its toolchain/translator halves.</summary>
internal sealed record ToolkitPackageComponents(string Package, string Toolchain, string Translator);
