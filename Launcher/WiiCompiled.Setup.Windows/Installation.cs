using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>Provenance written by the bundled build script next to every product it produces.</summary>
internal sealed class LocalBuildProvenance
{
    public int SchemaVersion { get; set; }
    public string Profile { get; set; } = "";
    public string DolSha256 { get; set; } = "";
    public string RelSha256 { get; set; } = "";
    public string? CodePulSha256 { get; set; }
    public string? RetroWfcPayloadMode { get; set; }
    public string? RetroWfcPayloadSha256 { get; set; }
    public long? RetroWfcPayloadLength { get; set; }
    public string Compiler { get; set; } = "";

    public const string FileName = "local-build.json";
}

/// <summary>
/// The typed product status of the <c>--check-products</c> contract. These are the only nine answers
/// the backend gives; see docs/WHEELWIZARD_CONTRACT.md. A frontend branches on the status alone.
/// </summary>
internal enum ProductStatus
{
    Absent,

    /// <summary>The binary matches the installed toolkit and the canonical compile inputs.</summary>
    Current,

    /// <summary>The toolkit that produced the binary is no longer the installed toolkit.</summary>
    ToolkitChanged,

    /// <summary>The canonical Code.pul is not the one this binary statically embeds.</summary>
    CodePulChanged,

    /// <summary>Another translator-consumed Retro Rewind input changed.</summary>
    CompileInputsChanged,

    /// <summary>Only the cached Retro-WFC payload needs repair; no compilation.</summary>
    PayloadChanged,

    /// <summary>
    /// The canonical Retro Rewind installation is missing or structurally invalid, so the product
    /// can neither be trusted nor rebuilt until the frontend supplies it.
    /// </summary>
    InputsMissing,

    /// <summary>
    /// The product exists, but the installation cannot prove its provenance or provide the required
    /// toolkit. A caller must repair the installation before launching it.
    /// </summary>
    Blocked,

    /// <summary>The binary or the support files it needs are missing or unusable.</summary>
    Broken
}

/// <summary>
/// The result of a non-mutating product inspection. Status and detail are exactly what the wire
/// carries: nothing is derived that is not also reported.
/// </summary>
internal sealed record ProductState(ProductStatus Status, string Detail)
{
    /// <summary>The stable machine-readable status string of the command-line contract.</summary>
    public string Reason => Status switch
    {
        ProductStatus.Absent => "absent",
        ProductStatus.Current => "current",
        ProductStatus.ToolkitChanged => "toolkit-changed",
        ProductStatus.CodePulChanged => "code-pul-changed",
        ProductStatus.CompileInputsChanged => "compile-inputs-changed",
        ProductStatus.PayloadChanged => "payload-changed",
        ProductStatus.InputsMissing => "inputs-missing",
        ProductStatus.Blocked => "blocked",
        _ => "broken"
    };

    /// <summary>Whether a caller must do something before this product may be launched.</summary>
    public bool ActionRequired => Status is not ProductStatus.Current and not ProductStatus.Absent;
}

/// <summary>
/// The on-disk shape of an installation, plus the cheap checks a launch performs before starting a
/// binary. Everything here is O(1), a single file hash, or the compile-input identity of the
/// canonical Retro Rewind installation; the multi-gigabyte asset tree is never walked.
/// </summary>
internal sealed class Installation
{
    public Installation(string root) => Root = FileSystemUtilities.NormalizePath(root);

    public string Root { get; }
    public string ToolkitDirectory => InstalledLayout.Toolkit(Root);
    public string WorkspaceDirectory => InstalledLayout.Workspace(Root);
    public string WorkspaceAssetsDirectory => Path.Combine(WorkspaceDirectory, "Assets");
    public string WorkspaceRetroWfcPayload => Path.Combine(WorkspaceAssetsDirectory, "OfflinePayload");
    public string BaseDirectory => Path.Combine(Root, "Base");
    public string BaseExecutable => Path.Combine(BaseDirectory, "WiiCompiled.exe");
    public string RetroDirectory => Path.Combine(Root, "RetroRewind");
    public string RetroExecutable => Path.Combine(RetroDirectory, "RetroRewind.exe");
    public string GameDataDirectory => Path.Combine(Root, "GameAssets", "DATA");
    public string InstallStatePath => Path.Combine(Root, InstalledLayout.InstallStateFileName);
    public string ToolkitStatePath => Path.Combine(Root, ToolkitState.FileName);

    /// <summary>The portable root this installation lives in, or null for an ordinary installation.</summary>
    public string? PortableRootDirectory => PortableRoot.TryFind(Root);

    /// <summary>
    /// The runtime configuration this installation reads: <c>&lt;portable root&gt;\UserData\Config.toml</c>
    /// when portable, otherwise the shared per-user file.
    /// </summary>
    public string ConfigPath => RuntimeConfiguration.ResolveConfigPath(Root);

    /// <summary>The install directory copy of setup that acts as the CLI launcher.</summary>
    public string SetupCopyPath => Path.Combine(Root, ProductInfo.SetupCopyName);

    private const string Toolkit = InstalledLayout.ToolkitDirectoryName + "\\";
    private const string Workspace = InstalledLayout.WorkspaceDirectoryName + "\\";

    private static readonly string[] RequiredToolkitFiles =
    [
        Toolkit + @"Translator\Translator.Cli.exe",
        Toolkit + @"CMake\bin\cmake.exe",
        Toolkit + @"Ninja\ninja.exe",
        Toolkit + @"llvm-mingw\bin\clang-22.exe",
        Toolkit + @"llvm-mingw\bin\ld.lld.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-clang.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-clang++.exe",
        Toolkit + @"llvm-mingw\bin\x86_64-w64-mingw32-windres.exe",
        Workspace + "LocalBuild.ps1",
        Workspace + "NativeBuildFlags.ps1",
        Workspace + @"projects\mkwii\recomp.yml",
        Workspace + @"projects\mkwii-ntsc-u\recomp.yml",
        Workspace + @"projects\mkwii-ntsc-j\recomp.yml",
        Workspace + @"runtime\CMakeLists.txt",
        Workspace + @"runtime\assets\dsp\dsp_coef.bin"
    ];

    // These are the source trees and pinned dependency roots LocalBuild.ps1 consumes during every
    // native configure/build. Presence is intentionally a cheap structural test, not another full
    // toolkit hash on every status check. The dependency names come from the one shipped list, so a
    // dependency added to the payload cannot be missed here.
    private static readonly string[] RequiredNonEmptyToolkitDirectories =
    [
        Workspace + @"runtime\src",
        Workspace + @"runtime\assets\wii",
        Workspace + "aurora-main",
        .. InstalledLayout.DependencyNames.Select(name => Workspace + @"Dependencies\" + name)
    ];

    public bool Exists => File.Exists(InstallStatePath);

    /// <summary>
    /// Whether this directory holds an installation at all. A missing or empty directory is not a
    /// broken installation, it is simply nothing - a frontend probing a path it has not installed to
    /// yet must get "absent", not an error.
    /// </summary>
    public bool IsPresent => Directory.Exists(Root) &&
                             (Exists || File.Exists(BaseExecutable) || File.Exists(RetroExecutable));
    /// <summary>
    /// A cheap structural check for the programs and workspace inputs LocalBuild.ps1 actually
    /// invokes. Product inspection intentionally does not rehash the full toolkit every time, but
    /// it must not claim a product current when its compiler, linker, or native runtime is gone.
    /// </summary>
    public bool HasToolkit => RequiredToolkitFiles.All(relative => File.Exists(Path.Combine(Root, relative))) &&
                              RequiredNonEmptyToolkitDirectories.All(relative =>
                                  IsNonEmptyRegularDirectory(Path.Combine(Root, relative)));
    public bool HasRetroProduct => File.Exists(RetroExecutable);

    public InstallState? ReadInstallState() => JsonState.TryRead<InstallState>(InstallStatePath);

    /// <summary>The game id of the installed disc, which selects its region's Retro-WFC payload.</summary>
    public string ReadGameId()
    {
        try { return ReadDiscGameId(GameDataDirectory); }
        catch (IOException ex)
        {
            throw new InvalidDataException(
                "The installed game data has no readable disc header. Apply setup again with the disc image.", ex);
        }
    }

    /// <summary>The six-character game id in an extracted disc's <c>sys/boot.bin</c>.</summary>
    internal static string ReadDiscGameId(string dataRoot)
    {
        using var stream = File.OpenRead(Path.Combine(dataRoot, "sys", "boot.bin"));
        var id = new byte[6];
        stream.ReadExactly(id);
        return System.Text.Encoding.ASCII.GetString(id);
    }

    internal ToolkitState? ReadToolkitState() => JsonState.TryRead<ToolkitState>(ToolkitStatePath);

    /// <summary>
    /// The identity of the installed toolkit. Inspection never writes or adopts state: a missing
    /// toolkit provenance record must not silently make an existing product look trustworthy.
    /// </summary>
    public string ResolveToolkitFingerprint()
    {
        var state = ReadToolkitState();
        if (state is { SchemaVersion: 2 } && !string.IsNullOrEmpty(state.ToolkitFingerprint) &&
            !string.IsNullOrEmpty(state.ToolkitPackageFingerprint) && HasToolkit)
            return state.ToolkitFingerprint;

        // Do not derive a replacement identity from whatever happens to be on disk. A missing or
        // unsupported toolkit-state.json has no authoritative chain to the installed products and
        // is therefore blocked until setup repairs it.
        return "";
    }

    /// <summary>The recorded package identity of the installed Toolkit directory, under the same
    /// fail-closed rules as <see cref="ResolveToolkitFingerprint"/>.</summary>
    public string ResolveToolkitPackageFingerprint()
    {
        var state = ReadToolkitState();
        if (state is { SchemaVersion: 2 } && !string.IsNullOrEmpty(state.ToolkitFingerprint) &&
            !string.IsNullOrEmpty(state.ToolkitPackageFingerprint) && HasToolkit)
            return state.ToolkitPackageFingerprint;

        return "";
    }

    public void WriteToolkitState(string fingerprint, string packageFingerprint, string releaseTag,
        string runtimeAssetsFingerprint = "", string translationFingerprint = "",
        string nativeToolchainFingerprint = "") =>
        JsonState.Write(ToolkitStatePath, new ToolkitState
        {
            ToolkitFingerprint = fingerprint,
            ToolkitPackageFingerprint = packageFingerprint,
            RuntimeAssetsFingerprint = runtimeAssetsFingerprint,
            TranslationFingerprint = translationFingerprint,
            NativeToolchainFingerprint = nativeToolchainFingerprint,
            ToolkitReleaseTag = releaseTag
        });

    /// <summary>
    /// Reads the complete product provenance. A product without the current fingerprint schema is
    /// intentionally untrusted rather than guessed from an older, partial provenance document.
    /// </summary>
    public ProductFingerprint? ReadProductFingerprint(string productDirectory, string toolkitFingerprint)
    {
        var recorded = JsonState.TryRead<ProductFingerprint>(
            Path.Combine(productDirectory, ProductFingerprint.FileName));
        return recorded is { SchemaVersion: 1 } ? recorded : null;
    }

    /// <summary>
    /// The canonical Retro Rewind install to compare against: explicit <c>--retro-dir</c> if given, else the
    /// recorded <c>retro_rewind_root</c>. Reads only compile inputs, so it stays cheap for every status check and launch.
    /// </summary>
    internal RetroRewindCompileInputs? ResolveCanonicalCompileInputs(string? explicitDirectory,
        out string? error, CancellationToken cancellationToken = default)
    {
        error = null;
        var selected = string.IsNullOrWhiteSpace(explicitDirectory)
            ? ConfiguredRetroRewindRoot
            : explicitDirectory;
        if (string.IsNullOrWhiteSpace(selected))
        {
            if (HasRetroProduct || ReadInstallState()?.RetroRewindInstalled == true)
                error = "No canonical Retro Rewind folder is recorded for this installation. " +
                        "Repair it with Wheel Wizard's current Retro Rewind folder.";
            return null;
        }

        try
        {
            return CompileInputsFingerprint.Compute(selected, cancellationToken);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException
                                   or ArgumentException)
        {
            error = "The canonical Retro Rewind folder cannot be read: " + ex.Message;
            return null;
        }
    }

    /// <summary>The canonical Retro Rewind root the runtime reads its assets from, if one is set.</summary>
    public string? ConfiguredRetroRewindRoot => RuntimeConfiguration.GetRetroRewindRoot(ConfigPath);

    public ProductState CheckBase(string toolkitFingerprint)
    {
        var state = CheckBaseCore(toolkitFingerprint);
        if (state.Status != ProductStatus.Current) return state;
        var runtimeAssetsError = ValidateCopiedRuntimeAssets(BaseDirectory, "base", toolkitFingerprint);
        return runtimeAssetsError is null ? state : new ProductState(ProductStatus.Broken, runtimeAssetsError);
    }

    /// <summary>
    /// Everything about the base product except its copied support files. Repair inspects those
    /// separately because they are republished from the installed workspace without recompiling.
    /// </summary>
    internal ProductState CheckBaseCore(string toolkitFingerprint)
    {
        if (!IsUsableExecutable(BaseExecutable))
            return new ProductState(ProductStatus.Broken, "The base recomp executable is missing.");
        if (!HasUsableToolkit(toolkitFingerprint))
            return MissingToolkit();
        var installState = ReadCurrentInstallState();
        if (installState is null)
            return new ProductState(ProductStatus.Blocked,
                "The installation state is missing, unsupported, or belongs to another directory.");
        var fingerprint = ReadProductFingerprint(BaseDirectory, toolkitFingerprint);
        if (fingerprint is null)
            return new ProductState(ProductStatus.Blocked,
                "The base recomp has no current build provenance. Repair it with the current setup.");
        if (!fingerprint.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal))
            return new ProductState(ProductStatus.ToolkitChanged,
                "The base recomp was produced by a different recompilation toolkit.");
        var provenanceError = ValidateProductProvenance(BaseDirectory, "base", fingerprint, installState);
        return provenanceError is null
            ? new ProductState(ProductStatus.Current, "")
            : new ProductState(ProductStatus.Blocked, provenanceError);
    }

    /// <summary>
    /// Classifies the Retro Rewind product against the canonical installation's compile inputs. The
    /// asset tree is never inspected: the runtime reads it live, so an asset-only Retro Rewind
    /// update leaves this product current.
    /// </summary>
    public ProductState CheckRetroRewind(string toolkitFingerprint,
        RetroRewindCompileInputs? canonical, string? canonicalError = null,
        bool? cachedRetroWfcPayloadMatches = null)
    {
        var state = CheckRetroRewindCore(toolkitFingerprint, canonical, canonicalError,
            cachedRetroWfcPayloadMatches);
        if (state.Status != ProductStatus.Current) return state;
        var runtimeAssetsError = ValidateCopiedRuntimeAssets(RetroDirectory, "retro-rewind",
            toolkitFingerprint);
        return runtimeAssetsError is null ? state : new ProductState(ProductStatus.Broken, runtimeAssetsError);
    }

    internal ProductState CheckRetroRewindCore(string toolkitFingerprint,
        RetroRewindCompileInputs? canonical, string? canonicalError = null,
        bool? cachedRetroWfcPayloadMatches = null)
    {
        var installState = ReadCurrentInstallState();
        var recordedAsInstalled = installState?.RetroRewindInstalled == true;
        if ((HasRetroProduct || recordedAsInstalled) && !HasUsableToolkit(toolkitFingerprint))
            return MissingToolkit();

        if (!HasRetroProduct)
        {
            return recordedAsInstalled
                ? new ProductState(ProductStatus.Broken,
                    "Retro Rewind is recorded as installed, but its executable is missing.")
                : new ProductState(ProductStatus.Absent, "Retro Rewind is not installed.");
        }

        if (canonicalError is not null)
            return new ProductState(ProductStatus.InputsMissing, canonicalError);

        var fingerprint = ReadProductFingerprint(RetroDirectory, toolkitFingerprint);
        if (fingerprint is null)
            return new ProductState(ProductStatus.Blocked,
                "The Retro Rewind product has no current build provenance. " +
                "Repair it with Wheel Wizard's current Retro Rewind folder.");
        if (!fingerprint.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal))
            return new ProductState(ProductStatus.ToolkitChanged,
                "Retro Rewind was produced by a different recompilation toolkit.");
        if (installState is null)
            return new ProductState(ProductStatus.Blocked,
                "The installation state is missing, unsupported, or belongs to another directory.");

        var provenanceError = ValidateProductProvenance(RetroDirectory, "retro-rewind", fingerprint,
            installState);
        if (provenanceError is not null)
            return new ProductState(ProductStatus.Blocked, provenanceError);

        if (string.IsNullOrWhiteSpace(fingerprint.CodePulSha256) ||
            string.IsNullOrWhiteSpace(fingerprint.RetroRewindCompileInputsSha256) ||
            string.IsNullOrWhiteSpace(installState.RetroRewindCodePulSha256) ||
            string.IsNullOrWhiteSpace(installState.RetroRewindCompileInputsSha256) ||
            !Same(installState.RetroRewindCodePulSha256, fingerprint.CodePulSha256) ||
            !Same(installState.RetroRewindCompileInputsSha256,
                fingerprint.RetroRewindCompileInputsSha256))
            return new ProductState(ProductStatus.Blocked,
                "Retro Rewind's compile-input provenance is incomplete or inconsistent. " +
                "Repair it with Wheel Wizard's current Retro Rewind folder.");

        if (canonical is not null)
        {
            if (!Same(canonical.CodePulSha256, fingerprint.CodePulSha256))
                return new ProductState(ProductStatus.CodePulChanged,
                    "The canonical Retro Rewind Code.pul changed since WiiCompiled was translated.");
            if (!Same(canonical.CompileInputsSha256, fingerprint.RetroRewindCompileInputsSha256))
                return new ProductState(ProductStatus.CompileInputsChanged,
                    "Translator-consumed Retro Rewind inputs changed since WiiCompiled was built.");
        }

        if (fingerprint.RetroWfcPayloadMode == "downloaded" &&
            CheckRetroWfcPayloadCache(fingerprint, installState, cachedRetroWfcPayloadMatches)
                is { } payloadError)
            return payloadError;

        return new ProductState(ProductStatus.Current, "");
    }

    /// <summary>
    /// Compares the workspace payload cache with the identity this product embeds.
    /// <paramref name="cachedMatches"/> carries an observation the operation already paid for:
    /// true/false skip the hash, null means nothing is known yet.
    /// </summary>
    private ProductState? CheckRetroWfcPayloadCache(ProductFingerprint fingerprint, InstallState installState,
        bool? cachedMatches)
    {
        if (string.IsNullOrWhiteSpace(fingerprint.RetroWfcPayloadSha256) ||
            fingerprint.RetroWfcPayloadLength <= 0 ||
            string.IsNullOrWhiteSpace(installState.RetroWfcPayloadSha256) ||
            installState.RetroWfcPayloadLength <= 0)
            return new ProductState(ProductStatus.Blocked,
                "The Retro-WFC payload provenance is incomplete.");
        if (cachedMatches == false)
            return new ProductState(ProductStatus.PayloadChanged,
                "The cached Retro-WFC payload is missing or does not match the current snapshot.");
        if (cachedMatches == true) return null;

        try
        {
            var payload = InputValidation.ResolveRetroWfcPayloadFile(WorkspaceRetroWfcPayload, ReadGameId());
            if (!InputValidation.Sha256File(payload).Equals(fingerprint.RetroWfcPayloadSha256,
                    StringComparison.OrdinalIgnoreCase) ||
                new FileInfo(payload).Length != fingerprint.RetroWfcPayloadLength)
                return new ProductState(ProductStatus.PayloadChanged,
                    "The cached Retro-WFC payload is not the one this product was built with.");
            return null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return new ProductState(ProductStatus.PayloadChanged,
                "The cached Retro-WFC payload is missing or invalid: " + ex.Message);
        }
    }

    private static bool Same(string left, string right) =>
        left.Equals(right, StringComparison.OrdinalIgnoreCase);

    internal bool ProductUsesGameInputs(string productDirectory, string toolkitFingerprint,
        string expectedDolSha256, string expectedRelSha256)
    {
        var fingerprint = ReadProductFingerprint(productDirectory, toolkitFingerprint);
        return fingerprint is not null &&
               fingerprint.DolSha256.Equals(expectedDolSha256, StringComparison.OrdinalIgnoreCase) &&
               fingerprint.RelSha256.Equals(expectedRelSha256, StringComparison.OrdinalIgnoreCase);
    }

    private bool HasUsableToolkit(string toolkitFingerprint) =>
        HasToolkit && !string.IsNullOrWhiteSpace(toolkitFingerprint);

    private static ProductState MissingToolkit() =>
        new(ProductStatus.Blocked,
            "The recompilation toolkit is missing or incomplete. Repair the setup before launching or updating products.");

    private InstallState? ReadCurrentInstallState()
    {
        var state = ReadInstallState();
        if (state is not { SchemaVersion: 1 } || string.IsNullOrWhiteSpace(state.InstallDir)) return null;
        try
        {
            return FileSystemUtilities.PathsEqual(state.InstallDir, Root) ? state : null;
        }
        catch { return null; }
    }

    private string? ValidateProductProvenance(string productDirectory, string expectedProfile,
        ProductFingerprint fingerprint, InstallState state)
    {
        if (!fingerprint.Profile.Equals(expectedProfile, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(fingerprint.DolSha256) ||
            string.IsNullOrWhiteSpace(fingerprint.RelSha256) ||
            string.IsNullOrWhiteSpace(fingerprint.ExecutableSha256))
            return $"The {expectedProfile} product provenance is incomplete.";

        var executable = expectedProfile == "base" ? BaseExecutable : RetroExecutable;
        if (!IsUsableExecutable(executable))
            return $"The {expectedProfile} executable is missing or incomplete.";
        try
        {
            if (!InputValidation.Sha256File(executable).Equals(fingerprint.ExecutableSha256,
                    StringComparison.OrdinalIgnoreCase))
                return $"The {expectedProfile} executable no longer matches its recorded build identity.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return $"The {expectedProfile} executable cannot be verified: {ex.Message}";
        }

        var dol = Path.Combine(GameDataDirectory, "sys", "main.dol");
        var rel = Path.Combine(GameDataDirectory, "files", "rel", "StaticR.rel");
        var fst = Path.Combine(GameDataDirectory, "sys", "fst.bin");
        if (!File.Exists(dol) || !File.Exists(rel) || !File.Exists(fst))
            return "The installed game assets are incomplete. Apply setup again with the disc image.";
        try
        {
            var dolSha = InputValidation.Sha256File(dol);
            var relSha = InputValidation.Sha256File(rel);
            if (!dolSha.Equals(fingerprint.DolSha256, StringComparison.OrdinalIgnoreCase) ||
                !relSha.Equals(fingerprint.RelSha256, StringComparison.OrdinalIgnoreCase) ||
                !dolSha.Equals(state.DolSha256, StringComparison.OrdinalIgnoreCase) ||
                !relSha.Equals(state.RelSha256, StringComparison.OrdinalIgnoreCase))
                return "The installed game inputs do not match the product provenance.";
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return "The installed game inputs cannot be verified: " + ex.Message;
        }

        var local = JsonState.TryRead<LocalBuildProvenance>(
            Path.Combine(productDirectory, LocalBuildProvenance.FileName));
        if (local is not { SchemaVersion: 1 } ||
            !local.Profile.Equals(expectedProfile, StringComparison.Ordinal) ||
            !local.DolSha256.Equals(fingerprint.DolSha256, StringComparison.OrdinalIgnoreCase) ||
            !local.RelSha256.Equals(fingerprint.RelSha256, StringComparison.OrdinalIgnoreCase))
            return $"The {expectedProfile} executable has invalid local-build provenance.";
        if (expectedProfile == "retro-rewind" &&
            (!string.Equals(local.RetroWfcPayloadMode, fingerprint.RetroWfcPayloadMode,
                 StringComparison.Ordinal) ||
             !string.Equals(local.RetroWfcPayloadSha256 ?? "", fingerprint.RetroWfcPayloadSha256,
                 StringComparison.OrdinalIgnoreCase) ||
             (local.RetroWfcPayloadLength ?? 0) != fingerprint.RetroWfcPayloadLength))
        {
            return "The Retro Rewind executable has invalid Retro-WFC payload provenance.";
        }

        return null;
    }

    /// <summary>
    /// Runtime support files are copied beside products rather than embedded in the executable.
    /// The installed toolkit-state is their authoritative release identity; products with missing
    /// or altered copies cannot be declared launchable merely because their .exe still hashes.
    /// </summary>
    internal string? ValidateCopiedRuntimeAssets(string productDirectory, string expectedProfile,
        string toolkitFingerprint)
    {
        var toolkitState = ReadToolkitState();
        if (toolkitState is not { SchemaVersion: 2 } ||
            !string.Equals(toolkitState.ToolkitFingerprint, toolkitFingerprint, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(toolkitState.RuntimeAssetsFingerprint))
        {
            return "The installed runtime-asset provenance is missing or does not belong to the current toolkit.";
        }

        string? failure = null;
        if (!ToolkitFingerprint.ProductRuntimeAssetsMatch(productDirectory,
                toolkitState.RuntimeAssetsFingerprint, diagnostic: line => failure = line))
        {
            // An access-denied or half-deleted tree must not masquerade as an ordinary hash mismatch.
            return failure ??
                   $"The {expectedProfile} product's copied runtime assets are missing, corrupt, or stale.";
        }
        return null;
    }

    private static bool IsUsableExecutable(string path)
    {
        try { return File.Exists(path) && new FileInfo(path).Length >= 64 * 1024; }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return false; }
    }

    private static bool IsNonEmptyRegularDirectory(string path)
    {
        try
        {
            if (!Directory.Exists(path)) return false;
            var directory = new DirectoryInfo(path);
            if ((directory.Attributes & FileAttributes.ReparsePoint) != 0) return false;
            using var entries = Directory.EnumerateFileSystemEntries(path).GetEnumerator();
            return entries.MoveNext();
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>The Retro-WFC payload choice this installation was built with.</summary>
    public RetroWfcPayloadMode ResolveRetroWfcPayloadMode(string toolkitFingerprint)
    {
        var recorded = ReadProductFingerprint(RetroDirectory, toolkitFingerprint)?.RetroWfcPayloadMode;
        recorded = string.IsNullOrEmpty(recorded) ? ReadInstallState()?.RetroWfcPayloadMode : recorded;
        return recorded switch
        {
            "downloaded" => RetroWfcPayloadMode.Online,
            "skipped" => RetroWfcPayloadMode.Skipped,
            _ => RetroWfcPayloadMode.NotApplicable
        };
    }

    public bool MatchesRetroWfcPayloadSnapshot(string toolkitFingerprint,
        RetroWfcPayloadSnapshot snapshot)
    {
        var fingerprint = ReadProductFingerprint(RetroDirectory, toolkitFingerprint);
        var state = ReadCurrentInstallState();
        return fingerprint is { RetroWfcPayloadMode: "downloaded" } &&
               state is { RetroWfcPayloadMode: "downloaded" } &&
               string.Equals(fingerprint.RetroWfcPayloadSha256, snapshot.Sha256, StringComparison.Ordinal) &&
               string.Equals(state.RetroWfcPayloadSha256, snapshot.Sha256, StringComparison.Ordinal);
    }

    public bool CachedRetroWfcPayloadMatches(RetroWfcPayloadSnapshot snapshot)
    {
        try
        {
            var payload = InputValidation.ResolveRetroWfcPayloadFile(WorkspaceRetroWfcPayload, ReadGameId());
            return new FileInfo(payload).Length == snapshot.ByteLength &&
                   InputValidation.Sha256File(payload).Equals(snapshot.Sha256, StringComparison.Ordinal);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            return false;
        }
    }
}
