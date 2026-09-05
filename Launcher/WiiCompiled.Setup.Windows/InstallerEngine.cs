using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

internal sealed class InstallerEngine
{
    private readonly IInstallReporter _reporter;

    public InstallerEngine(IInstallReporter reporter) => _reporter = reporter;

    public async Task InstallAsync(InstallOptions options, CancellationToken cancellationToken = default)
    {
        var installDirectory = Path.GetFullPath(options.InstallDirectory);
        ValidateInstallDirectory(installDirectory);

        InputValidation.ValidateExtension(options.GamePath);
        var parent = Directory.GetParent(installDirectory)?.FullName
                     ?? throw new InvalidOperationException("The installation directory has no parent.");
        Directory.CreateDirectory(parent);

        using var operationLock = InstallOperationLock.Acquire(installDirectory, _reporter);
        var existing = new Installation(installDirectory);

        PortableInstallHealing.HealMovedInstall(existing, _reporter);
        // Capture this before publishing anything. An existing installation may be repaired or
        // updated by this invocation, but those operations must not recreate the user's shortcuts.
        var firstInstall = !existing.IsPresent;
        var previousState = existing.ReadInstallState();
        using var scratch = Directory.Exists(installDirectory)
            ? InstallScratchSpace.CreateInsideInstall(installDirectory, _reporter)
            : InstallScratchSpace.CreateSibling(installDirectory, _reporter);
        var staging = scratch.Root;


        const long payloadStagingAllowance = 2L * 1024 * 1024 * 1024;
        FileSystemUtilities.EnsureFreeSpace(installDirectory, payloadStagingAllowance, "Setup");

        using var payload = PayloadArchive.OpenCurrent();
        var manifest = payload.ReadManifest();
        var toolkit = InstalledLayout.Toolkit(staging);
        var workspace = InstalledLayout.Workspace(staging);

        var candidateToolkitFingerprint = manifest.ToolkitFingerprint;
        var candidateToolkitPackageFingerprint = manifest.ToolkitPackageFingerprint;
        var candidateRuntimeAssetsFingerprint = manifest.RuntimeAssetsFingerprint;
        var installedToolkitFingerprint = existing.IsPresent
            ? existing.ResolveToolkitFingerprint()
            : "";
        var installedToolkitPackageFingerprint = existing.IsPresent
            ? existing.ResolveToolkitPackageFingerprint()
            : "";
        var reusableGameAssets = FindReusableGameAssets(existing, manifest);

        var sameToolkit = existing.IsPresent && reusableGameAssets is not null &&
                          installedToolkitFingerprint.Equals(candidateToolkitFingerprint,
                              StringComparison.Ordinal);
        var samePackageContent = existing.IsPresent &&
                                 installedToolkitPackageFingerprint.Equals(
                                     candidateToolkitPackageFingerprint, StringComparison.Ordinal);
        var sameToolkitPackage = sameToolkit && samePackageContent;
        var hadRetro = existing.HasRetroProduct || previousState?.RetroRewindInstalled == true;

        var runtimeAssetsCurrent = sameToolkit && RuntimeAssetsAreCurrent(existing,
            candidateRuntimeAssetsFingerprint, cancellationToken);
        var installedNodTool = Path.Combine(existing.ToolkitDirectory, "nodtool.exe");
        var extractToolkit = MustRefreshToolkit(sameToolkit, samePackageContent,
            File.Exists(installedNodTool));
        var extractWorkspace = !sameToolkit || !runtimeAssetsCurrent;

        _reporter.Progress(InstallStages.ExtractToolkit,
            "Inspecting the release's local recompilation toolkit...", 1);
        if (extractToolkit) payload.ExtractDirectory(InstalledLayout.ToolkitDirectoryName, toolkit);
        if (extractWorkspace)
        {
            payload.ExtractDirectory(InstalledLayout.WorkspaceDirectoryName, workspace);
            WorkspaceTimestamps.MarkChangedFiles(existing.WorkspaceDirectory, workspace,
                _reporter.Diagnostic, cancellationToken);
        }
        payload.ExtractEntry("host/WiiCompiled-Setup.exe",
            Path.Combine(staging, ProductInfo.SetupCopyName));
        payload.ExtractDirectory("licenses", Path.Combine(staging, "licenses"));
        payload.ExtractEntry(InstalledLayout.PayloadManifestFileName,
            Path.Combine(staging, InstalledLayout.PayloadManifestFileName));

        var nodTool = extractToolkit ? Path.Combine(toolkit, "nodtool.exe") : installedNodTool;
        _reporter.Progress(InstallStages.Validate, "Checking the Wii disc image...", 2);
        var header = await InputValidation.ReadDiscHeaderAsync(nodTool, options.GamePath,
            cancellationToken);
        InputValidation.EnsureCompatibleDisc(header, manifest);
        var canonicalRetroRoot = options.RetroDirectoryPath is null
            ? null
            : RetroRewindSource.ResolveRetroRewind6(options.RetroDirectoryPath);
        ValidateRetroOptions(canonicalRetroRoot, options.RetroWfcPayloadMode, manifest);

        var retroCompileInputs = canonicalRetroRoot is null
            ? null
            : SnapshotRetroRewindCompileInputs(canonicalRetroRoot,
                Path.Combine(staging, "compile-inputs"), cancellationToken);
        if (options.Portable)
        {
            var portableRoot = PortableRoot.Create(parent);
            _reporter.Diagnostic($"Installing as a portable installation under {portableRoot}.");
        }

        if (sameToolkit)
        {
            var desiredInputDrift = ProductRepairService.FindDesiredInputDrift(existing,
                candidateToolkitFingerprint, manifest.ExpectedDolSha256, manifest.ExpectedRelSha256);
            if (desiredInputDrift.Retro && retroCompileInputs is null)
                throw new InvalidOperationException(
                    "This release changes the clean game inputs used by the installed Retro Rewind product. " +
                    "Supply Wheel Wizard's current Retro Rewind folder explicitly so it can be rebuilt safely.");

            var reconciliation = await ReconcileSameToolkitAsync(existing, retroCompileInputs,
                canonicalRetroRoot, options.RetroWfcPayloadMode, Path.Combine(staging, "repair"),
                manifest.ExpectedDolSha256, manifest.ExpectedRelSha256, cancellationToken);
            var remainingCancellation = reconciliation.PublicationCommitted
                ? CancellationToken.None
                : cancellationToken;


            if (!runtimeAssetsCurrent)
                runtimeAssetsCurrent = RuntimeAssetsAreCurrent(existing,
                    candidateRuntimeAssetsFingerprint, remainingCancellation);

            remainingCancellation.ThrowIfCancellationRequested();
            EnsureReconciliationCurrent(existing, candidateToolkitFingerprint, retroCompileInputs,
                manifest.ExpectedDolSha256, manifest.ExpectedRelSha256,
                reconciliation.CachedRetroWfcPayloadMatches);

            var updatedState = BuildInstallState(existing.ReadInstallState(), installDirectory, manifest,
                retroCompileInputs, canonicalRetroRoot, options.RetroWfcPayloadMode);
            PrepareMetadata(staging, manifest, updatedState);
            remainingCancellation.ThrowIfCancellationRequested();

            var releaseEntries = new List<InstallTransactionEntry>();
            if (!sameToolkitPackage)
                AddComponent(releaseEntries, staging, installDirectory,
                    InstalledLayout.ToolkitDirectoryName);
            if (!runtimeAssetsCurrent)
                AddRuntimeAssetPublicationEntries(releaseEntries, staging, installDirectory,
                    updatedState.RetroRewindInstalled, candidateRuntimeAssetsFingerprint,
                    remainingCancellation);
            Publish(staging, installDirectory, canonicalRetroRoot, updatedState,
                releaseEntries, remainingCancellation, createShortcuts: firstInstall);
            return;
        }

        if (hadRetro && retroCompileInputs is null)
        {
            throw new InvalidOperationException(
                "The installed Retro Rewind product must be produced again. Supply Wheel Wizard's " +
                "current Retro Rewind folder explicitly so it can be rebuilt safely.");
        }

        EnsureSufficientRemainingDiskSpace(installDirectory,
            needsExtractedDisc: reusableGameAssets is null, needsLocalBuild: true);

        if (reusableGameAssets is null)
        {
            await ExtractGameAssetsAsync(nodTool, options.GamePath,
                Path.Combine(staging, "GameAssets"), manifest, cancellationToken);
        }

        await PublishToolkitAndReconcileProductsAsync(existing, staging, workspace, manifest,
            previousState, options, canonicalRetroRoot, retroCompileInputs,
            publishGameAssets: reusableGameAssets is null, createShortcuts: firstInstall,
            cancellationToken: cancellationToken);
    }


    internal static bool MustRefreshToolkit(bool sameToolkit, bool samePackageContent,
        bool nodToolPresent) =>
        !sameToolkit || !samePackageContent || !nodToolPresent;

    private static void AddComponent(List<InstallTransactionEntry> entries, string staging,
        string installDirectory, string name) =>
        entries.Add(InstallTransactionEntry.Directory(Path.Combine(staging, name),
            Path.Combine(installDirectory, name)));

    /// <summary>
    /// Build-produced workspace directories (not shipped in a release) grafted from the installed workspace into
    /// the staged one, carrying the base translation, native build dir, verified inputs, and Retro-WFC payload cache
    /// across a toolkit update. Each is re-validated by the build, so stale caches degrade to a clean rebuild, never reuse.
    /// </summary>
    private static readonly string[] WorkspaceCacheDirectories =
        ["generated", "build", "native-build", "Assets", "PulsarPacks"];

    private async Task PublishToolkitAndReconcileProductsAsync(Installation existing, string staging,
        string stagedWorkspace, PayloadManifest manifest, InstallState? previousState,
        InstallOptions options, string? canonicalRetroRoot,
        RetroRewindCompileInputs? retroCompileInputs,
        bool publishGameAssets, bool createShortcuts, CancellationToken cancellationToken)
    {
        var installDirectory = existing.Root;


        foreach (var cacheDirectory in WorkspaceCacheDirectories)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var source = Path.Combine(existing.WorkspaceDirectory, cacheDirectory);
            if (!Directory.Exists(source)) continue;
            var destination = Path.Combine(stagedWorkspace, cacheDirectory);
            if (Directory.Exists(destination)) Directory.Delete(destination, recursive: true);
            Directory.Move(source, destination);
        }


        var state = BuildInstallState(previousState, installDirectory, manifest,
            compileInputs: null, canonicalRetroRoot, options.RetroWfcPayloadMode);
        PrepareMetadata(staging, manifest, state);

        cancellationToken.ThrowIfCancellationRequested();
        var entries = new List<InstallTransactionEntry>();

        AddComponent(entries, staging, installDirectory, InstalledLayout.ToolkitDirectoryName);
        AddComponent(entries, staging, installDirectory, InstalledLayout.WorkspaceDirectoryName);
        if (publishGameAssets) AddComponent(entries, staging, installDirectory, "GameAssets");

        Publish(staging, installDirectory, canonicalRetroRoot, state,
            entries, cancellationToken, progressPercent: 8, completionPercent: 10,
            createShortcuts: createShortcuts);

        _reporter.Progress(InstallStages.BuildBase,
            "Producing the installed products with the published toolkit...", 11);

        var reconciliation = await ReconcileSameToolkitAsync(existing, retroCompileInputs,
            canonicalRetroRoot, options.RetroWfcPayloadMode, repairScratch: null,
            manifest.ExpectedDolSha256, manifest.ExpectedRelSha256, cancellationToken);
        EnsureReconciliationCurrent(existing, manifest.ToolkitFingerprint, retroCompileInputs,
            manifest.ExpectedDolSha256, manifest.ExpectedRelSha256,
            reconciliation.CachedRetroWfcPayloadMatches);
    }

    private Task<ProductRepairService.ReconciliationResult> ReconcileSameToolkitAsync(Installation existing,
        RetroRewindCompileInputs? retroCompileInputs, string? canonicalRetroRoot,
        RetroWfcPayloadMode payloadMode, string? repairScratch, string expectedDolSha256,
        string expectedRelSha256, CancellationToken cancellationToken)
    {
        var repair = new ProductRepairService(existing, _reporter);
        var options = new ProductRepairService.ReconcileOptions
        {
            ScratchRoot = repairScratch,
            ExpectedDolSha256 = expectedDolSha256,
            ExpectedRelSha256 = expectedRelSha256,
            CanonicalRetroRewindRoot = canonicalRetroRoot
        };

        return retroCompileInputs is null
            ? repair.RepairBaseAsync(options, cancellationToken)
            : repair.RepairRetroAsync(retroCompileInputs, payloadMode, options, cancellationToken);
    }

    private static void EnsureReconciliationCurrent(Installation installation, string toolkitFingerprint,
        RetroRewindCompileInputs? retroCompileInputs, string expectedDolSha256, string expectedRelSha256,
        bool? cachedRetroWfcPayloadMatches)
    {
        var baseState = installation.CheckBase(toolkitFingerprint);
        var retroState = installation.CheckRetroRewind(toolkitFingerprint, retroCompileInputs,
            canonicalError: null, cachedRetroWfcPayloadMatches);
        var desiredInputsCurrent = installation.ProductUsesGameInputs(installation.BaseDirectory,
                                       toolkitFingerprint, expectedDolSha256, expectedRelSha256) &&
                                   (!installation.HasRetroProduct ||
                                    installation.ProductUsesGameInputs(installation.RetroDirectory,
                                        toolkitFingerprint, expectedDolSha256, expectedRelSha256));
        if (baseState.Status == ProductStatus.Current && !retroState.ActionRequired && desiredInputsCurrent) return;

        var details = string.Join(" ", new[] { baseState, retroState }
            .Where(state => state.ActionRequired)
            .Select(state => state.Detail)
            .Where(detail => !string.IsNullOrWhiteSpace(detail)));
        throw new InvalidOperationException(
            "Product reconciliation did not produce a current installation with the release's game inputs; " +
            "release metadata was not advanced." +
            (string.IsNullOrWhiteSpace(details) ? "" : " " + details));
    }

    /// <summary>
    /// Publishes the caller's component entries together with the release metadata every install and
    /// update advances, then applies the runtime configuration this installation owns.
    /// </summary>
    private void Publish(string staging, string installDirectory,
        string? canonicalRetroRoot, InstallState state,
        List<InstallTransactionEntry> entries, CancellationToken cancellationToken,
        int progressPercent = 95, int completionPercent = 99, bool createShortcuts = false)
    {
        entries.Add(InstallTransactionEntry.Directory(Path.Combine(staging, "licenses"),
            Path.Combine(installDirectory, "licenses")));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, ProductInfo.SetupCopyName),
            Path.Combine(installDirectory, ProductInfo.SetupCopyName)));
        entries.Add(InstallTransactionEntry.File(
            Path.Combine(staging, InstalledLayout.PayloadManifestFileName),
            Path.Combine(installDirectory, InstalledLayout.PayloadManifestFileName)));
        entries.Add(InstallTransactionEntry.File(Path.Combine(staging, ToolkitState.FileName),
            Path.Combine(installDirectory, ToolkitState.FileName)));
        entries.Add(InstallTransactionEntry.File(
            Path.Combine(staging, InstalledLayout.InstallStateFileName),
            Path.Combine(installDirectory, InstalledLayout.InstallStateFileName)));

        cancellationToken.ThrowIfCancellationRequested();
        RunningProductGuard.EnsureProductsNotRunning(installDirectory);
        _reporter.Progress(InstallStages.Publish, "Publishing the completed installation...", progressPercent);
        var configPath = RuntimeConfiguration.ResolveConfigPath(installDirectory);
        var configSnapshot = RuntimeConfiguration.Capture(configPath);
        using var transaction = InstallTransaction.Begin(installDirectory, _reporter, entries.ToArray());
        transaction.Publish();
        transaction.RecordRuntimeConfigurationMutation(configSnapshot);
        RuntimeConfiguration.SetDvdRoot(configPath, Path.Combine(installDirectory, "GameAssets", "DATA"));
        // The runtime scans this directory live for its asset overlay, so it is recorded on every
        // operation that receives one and is never rewritten by a base-only operation.
        if (canonicalRetroRoot is not null)
            RuntimeConfiguration.SetRetroRewindRoot(configPath, canonicalRetroRoot);
        transaction.Commit();

        try
        {
            // A portable installation is owned by the folder it lives in, not by this machine. Adding
            // a machine-wide uninstall entry for it would outlive the folder and, worse, would
            // overwrite the entry of a normal installation on the same account.
            if (PortableRoot.TryFind(installDirectory) is not null)
            {
                _reporter.Diagnostic(
                    "Portable installation: no Windows uninstall entry was registered. Remove the folder, " +
                    "or run the copied setup with --uninstall, to uninstall it.");
            }
            else
            {
                ShellIntegration.RegisterUninstaller(installDirectory, state.RetroRewindInstalled);
            }
            if (createShortcuts)
                ShellIntegration.CreateShortcuts(installDirectory);
        }
        catch (Exception ex)
        {
            _reporter.Diagnostic("The installation succeeded, but Windows shell integration failed: " +
                                 ex.Message);
        }

        _reporter.Progress(InstallStages.Publish, "Installation complete.", completionPercent);
    }

    private static void PrepareMetadata(string staging, PayloadManifest manifest, InstallState state)
    {
        JsonState.Write(Path.Combine(staging, ToolkitState.FileName), new ToolkitState
        {
            ToolkitFingerprint = manifest.ToolkitFingerprint,
            ToolkitPackageFingerprint = manifest.ToolkitPackageFingerprint,
            RuntimeAssetsFingerprint = manifest.RuntimeAssetsFingerprint,
            TranslationFingerprint = manifest.TranslationFingerprint,
            NativeToolchainFingerprint = manifest.NativeToolchainFingerprint,
            ToolkitReleaseTag = manifest.ToolkitReleaseTag
        });
        JsonState.Write(Path.Combine(staging, InstalledLayout.InstallStateFileName), state);
    }

    private static InstallState BuildInstallState(InstallState? previousState, string installDirectory,
        PayloadManifest manifest, RetroRewindCompileInputs? compileInputs, string? canonicalRetroRoot,
        RetroWfcPayloadMode payloadMode,
        RetroWfcPayloadSnapshot? retroWfcPayloadSnapshot = null)
    {
        // Preserve the pre-update object for runtime-configuration rollback/ownership decisions.
        var state = new InstallState
        {
            SchemaVersion = 1,
            SetupVersion = ProductInfo.Version,
            ProductVersion = manifest.ProductVersion,
            InstallDir = installDirectory,
            InstalledUtc = previousState?.InstalledUtc ?? DateTime.UtcNow,
            RetroRewindInstalled = previousState?.RetroRewindInstalled ?? false,
            ToolkitReleaseTag = manifest.ToolkitReleaseTag,
            DolSha256 = manifest.ExpectedDolSha256,
            RelSha256 = manifest.ExpectedRelSha256,
            RetroRewindCodePulSha256 = previousState?.RetroRewindCodePulSha256 ?? "",
            RetroRewindCompileInputsSha256 = previousState?.RetroRewindCompileInputsSha256 ?? "",
            RetroWfcPayloadMode = previousState?.RetroWfcPayloadMode ?? "",
            RetroWfcPayloadSha256 = previousState?.RetroWfcPayloadSha256 ?? "",
            RetroWfcPayloadLength = previousState?.RetroWfcPayloadLength ?? 0,
            RetroRewindRoot = canonicalRetroRoot is null
                ? previousState?.RetroRewindRoot ?? ""
                : Path.GetFullPath(canonicalRetroRoot)
        };
        if (compileInputs is not null)
        {
            state.RetroRewindInstalled = true;
            state.RetroRewindCodePulSha256 = compileInputs.CodePulSha256;
            state.RetroRewindCompileInputsSha256 = compileInputs.CompileInputsSha256;
            state.RetroWfcPayloadMode = payloadMode == RetroWfcPayloadMode.Online
                ? "downloaded"
                : "skipped";
            state.RetroWfcPayloadSha256 = payloadMode == RetroWfcPayloadMode.Online
                ? retroWfcPayloadSnapshot?.Sha256 ?? state.RetroWfcPayloadSha256
                : "";
            state.RetroWfcPayloadLength = payloadMode == RetroWfcPayloadMode.Online
                ? retroWfcPayloadSnapshot?.ByteLength ?? state.RetroWfcPayloadLength
                : 0;
            if (payloadMode == RetroWfcPayloadMode.Online &&
                (string.IsNullOrWhiteSpace(state.RetroWfcPayloadSha256) || state.RetroWfcPayloadLength <= 0))
                throw new InvalidDataException("The downloaded Retro-WFC payload snapshot is missing.");
        }
        return state;
    }

    /// <summary>
    /// Captures the canonical installation's compile inputs into operation-owned staging. Only
    /// Code.pul and the bundled Pulsar sources are copied; the asset tree stays where Wheel Wizard
    /// owns it and is read live by the runtime through <c>retro_rewind_root</c>.
    /// </summary>
    private RetroRewindCompileInputs SnapshotRetroRewindCompileInputs(string canonicalRetroRoot,
        string destination, CancellationToken cancellationToken)
    {
        _reporter.Progress(InstallStages.Validate, "Snapshotting the Retro Rewind compile inputs...", 3);
        return CompileInputsFingerprint.Snapshot(canonicalRetroRoot, destination, cancellationToken);
    }

    private bool RuntimeAssetsAreCurrent(Installation installation, string expectedFingerprint,
        CancellationToken cancellationToken)
    {
        var installedSource = ToolkitFingerprint.TryComputeRuntimeAssets(installation.Root, cancellationToken,
            _reporter.Diagnostic);
        if (!expectedFingerprint.Equals(installedSource, StringComparison.Ordinal)) return false;
        if (File.Exists(installation.BaseExecutable) &&
            !ToolkitFingerprint.ProductRuntimeAssetsMatch(installation.BaseDirectory, expectedFingerprint,
                cancellationToken, _reporter.Diagnostic))
            return false;
        return !installation.HasRetroProduct ||
               ToolkitFingerprint.ProductRuntimeAssetsMatch(installation.RetroDirectory,
                   expectedFingerprint, cancellationToken, _reporter.Diagnostic);
    }

    private void AddRuntimeAssetPublicationEntries(List<InstallTransactionEntry> entries,
        string staging, string installDirectory, bool includeRetro, string expectedFingerprint,
        CancellationToken cancellationToken)
    {
        // The identity was computed from this exact operation-owned payload extraction, so the
        // staged assets are published as they are; only repair has an installed source to re-verify.
        var sourceAssets = Path.Combine(InstalledLayout.Workspace(staging), "runtime", "assets");
        var products = (includeRetro ? new[] { "Base", "RetroRewind" } : new[] { "Base" })
            .Select(product => (product, Path.Combine(installDirectory, product)));
        RuntimeAssetPublication.AddEntries(entries, sourceAssets,
            Path.Combine(staging, "runtime-asset-publication"), products, expectedFingerprint,
            "publication", _reporter.Diagnostic, cancellationToken);

        entries.Add(InstallTransactionEntry.Directory(sourceAssets,
            Path.Combine(InstalledLayout.Workspace(installDirectory), "runtime", "assets")));
    }

    private static void ValidateRetroOptions(string? canonicalRetroRoot, RetroWfcPayloadMode mode,
        PayloadManifest manifest)
    {
        if (canonicalRetroRoot is null && mode != RetroWfcPayloadMode.NotApplicable)
            throw new InvalidOperationException(
                "A Retro-WFC payload option requires the canonical Retro Rewind folder.");
        if (canonicalRetroRoot is not null && mode == RetroWfcPayloadMode.NotApplicable)
            throw new InvalidOperationException(
                "Download the Retro-WFC payload, or explicitly skip the optional payload.");
        if (canonicalRetroRoot is not null && mode == RetroWfcPayloadMode.Online)
            InputValidation.ValidateRetroWfcPayloadUri(manifest.RetroWfcPayloadUri);
    }

    private string? FindReusableGameAssets(Installation existing, PayloadManifest manifest)
    {
        var dataRoot = existing.GameDataDirectory;
        // No fst.bin simply means nothing was extracted here before; anything past that gate is an
        // existing extraction that fails reuse, which the log must say before the expensive
        // re-extraction quietly repairs it.
        if (!File.Exists(Path.Combine(dataRoot, "sys", "fst.bin"))) return null;
        try
        {
            ValidateExtractedGame(dataRoot, manifest);
            return dataRoot;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            _reporter.Diagnostic(
                "The installed game assets cannot be reused; the disc will be extracted again: " + ex.Message);
            return null;
        }
    }

    private async Task ExtractGameAssetsAsync(string nodTool, string gamePath, string destination,
        PayloadManifest manifest, CancellationToken cancellationToken)
    {
        _reporter.Progress(InstallStages.ExtractDisc,
            "Extracting the game disc. This is the longest preparation step...", 6);
        // Extracted straight into a "DATA" subfolder so the on-disk layout matches what
        // Installation.GameDataDirectory and every other reader of it already expect - nodtool
        // itself has no such wrapper (it extracts sys/+files/ directly to whatever <outdir> is
        // given), so this is purely destination-side, not a nodtool convention.
        var dataRoot = Path.Combine(destination, "DATA");
        var extraction = await ProcessRunner.RunAsync(nodTool,
            ["extract", Path.GetFullPath(gamePath), dataRoot, "-q"],
            line => { if (!string.IsNullOrWhiteSpace(line)) _reporter.Diagnostic(line); },
            cancellationToken);
        if (extraction.ExitCode != 0)
            throw new InvalidDataException("Game extraction failed. " + extraction.CombinedOutput.Trim());
        ValidateExtractedGame(dataRoot, manifest);
    }

    private static void ValidateExtractedGame(string dataRoot, PayloadManifest manifest)
    {
        var dol = Path.Combine(dataRoot, "sys", "main.dol");
        var rel = Path.Combine(dataRoot, "files", "rel", "StaticR.rel");
        var fst = Path.Combine(dataRoot, "sys", "fst.bin");
        if (!File.Exists(dol) || !File.Exists(rel) || !File.Exists(fst))
            throw new InvalidDataException("The disc extraction is missing required Mario Kart Wii files.");
        var dolHash = InputValidation.Sha256File(dol);
        var relHash = InputValidation.Sha256File(rel);
        if (!dolHash.Equals(manifest.ExpectedDolSha256, StringComparison.OrdinalIgnoreCase) ||
            !relHash.Equals(manifest.ExpectedRelSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidDataException(
                "The disc is RMCP01 but does not match the supported clean PAL revision. " +
                "Patched or otherwise modified game code cannot be installed safely.");
        }
    }

    private static void ValidateInstallDirectory(string path) =>
        FileSystemUtilities.EnsureUsableLocation(path, "The installation folder");

    private static void EnsureSufficientRemainingDiskSpace(string installDirectory,
        bool needsExtractedDisc, bool needsLocalBuild)
    {
        const long extractedDiscAllowance = 5L * 1024 * 1024 * 1024;
        const long localBuildAllowance = 14L * 1024 * 1024 * 1024;
        const long safetyAllowance = 2L * 1024 * 1024 * 1024;
        // This preflight runs after the release payload and the compile-input snapshot have already
        // been staged. Count only work that can still allocate space from this point.
        var required = checked((needsExtractedDisc ? extractedDiscAllowance : 0) +
                               (needsLocalBuild ? localBuildAllowance : 0) + safetyAllowance);

        FileSystemUtilities.EnsureFreeSpace(installDirectory, required, "Setup");
    }
}
