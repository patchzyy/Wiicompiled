using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// Reconciles installed products against the canonical Retro Rewind install Wheel Wizard owns: the
/// toolkit compiles, the source only contributes inputs and its path (<c>retro_rewind_root</c>).
/// </summary>
internal sealed class ProductRepairService
{
    internal enum RetroWfcPayloadAction { None, PublishCacheOnly, RebuildRetro }

    /// <summary>
    /// Inputs every reconciliation entry point shares. Everything here is optional context the
    /// caller already owns under the operation lock; the entry point decides what work is possible.
    /// </summary>
    internal sealed record ReconcileOptions
    {
        /// <summary>Operation scratch the caller owns, or null to own one for this call.</summary>
        public string? ScratchRoot { get; init; }

        /// <summary>The release's authoritative clean-disc inputs, supplied together or not at all.</summary>
        public string? ExpectedDolSha256 { get; init; }
        public string? ExpectedRelSha256 { get; init; }

        /// <summary>
        /// The frontend's own canonical <c>RetroRewind6</c> directory, recorded as
        /// <c>retro_rewind_root</c>. Required by every Retro Rewind operation: the compile-input
        /// snapshot lives in operation scratch, so it can never be the durable value.
        /// </summary>
        public string? CanonicalRetroRewindRoot { get; init; }
    }

    internal sealed record ReconciliationResult(
        RetroRewindCompileInputs? CompileInputs,
        bool PublicationCommitted,
        /// <summary>
        /// true when this operation proved or published the online payload cache, null when it made
        /// no observation and the next inspection must check for itself. false is never produced
        /// here; product inspection reserves it for a cache already proven stale.
        /// </summary>
        bool? CachedRetroWfcPayloadMatches);

    private readonly Installation _installation;
    private readonly IInstallReporter _reporter;

    public ProductRepairService(Installation installation, IInstallReporter reporter)
    {
        _installation = installation;
        _reporter = reporter;
    }

    /// <summary>
    /// Repairs the base product and any copy-only drift the installation can fix from its own
    /// authoritative workspace. The caller must hold <see cref="InstallOperationLock"/>.
    /// </summary>
    public Task<ReconciliationResult> RepairBaseAsync(ReconcileOptions options,
        CancellationToken cancellationToken = default) =>
        ReconcileAsync(null, RetroWfcPayloadMode.NotApplicable, options, cancellationToken);

    /// <summary>
    /// Reconciles Retro Rewind against one immutable compile-input snapshot of the canonical
    /// installation, recompiling only what those inputs actually change.
    /// </summary>
    public Task<ReconciliationResult> RepairRetroAsync(RetroRewindCompileInputs snapshot,
        RetroWfcPayloadMode payloadMode, ReconcileOptions options,
        CancellationToken cancellationToken = default)
    {
        ArgumentNullException.ThrowIfNull(snapshot);
        if (payloadMode == RetroWfcPayloadMode.NotApplicable)
            throw new InvalidOperationException(
                "A Retro Rewind operation requires a Retro-WFC payload mode.");
        return ReconcileAsync(snapshot, payloadMode, options, cancellationToken);
    }

    private async Task<ReconciliationResult> ReconcileAsync(RetroRewindCompileInputs? snapshot,
        RetroWfcPayloadMode requestedPayloadMode, ReconcileOptions options,
        CancellationToken cancellationToken)
    {
        if (!_installation.IsPresent)
            throw new InvalidOperationException("WiiCompiled is not installed here.");
        if (!_installation.HasToolkit)
            throw new InvalidOperationException(
                "The installed recompilation toolkit is missing. Apply the current setup release before repairing products.");
        RunningProductGuard.EnsureProductsNotRunning(_installation.Root);
        var expectedDolSha256 = options.ExpectedDolSha256;
        var expectedRelSha256 = options.ExpectedRelSha256;
        if (string.IsNullOrWhiteSpace(expectedDolSha256) != string.IsNullOrWhiteSpace(expectedRelSha256))
            throw new ArgumentException("Both authoritative game-input hashes must be supplied together.");

        var toolkitFingerprint = _installation.ResolveToolkitFingerprint();
        if (string.IsNullOrWhiteSpace(toolkitFingerprint))
            throw new InvalidDataException("The installed recompilation toolkit cannot be identified safely.");

        using var ownedScratch = options.ScratchRoot is null
            ? InstallScratchSpace.CreateInsideInstall(_installation.Root, _reporter)
            : null;
        var scratchRoot = options.ScratchRoot ?? ownedScratch!.Root;
        Directory.CreateDirectory(scratchRoot);

        RetroWfcPayloadSnapshot? payloadSnapshot = null;
        if (requestedPayloadMode == RetroWfcPayloadMode.Online)
        {
            _reporter.Progress(InstallStages.PrepareRetro,
                "Downloading the current shared Retro-WFC payload snapshot...", 3);
            var payloadScratch = Path.Combine(scratchRoot, "retro-wfc-payload");
            try
            {
                payloadSnapshot = await InputValidation.DownloadRetroWfcPayloadAsync(
                    InputValidation.CurrentRetroWfcPayloadUri, payloadScratch, cancellationToken);
            }
            catch (Exception ex) when (!cancellationToken.IsCancellationRequested &&
                                       ex is HttpRequestException or TimeoutException or IOException)
            {
                payloadSnapshot = RecoverInstalledRetroWfcPayload(toolkitFingerprint,
                    Path.Combine(scratchRoot, "retro-wfc-payload-recovered"), ex, cancellationToken);
            }
        }


        _reporter.Progress(InstallStages.PrepareRetro,
            "Checking which installed products are still current...", 3);
        var downloadedPayloadMatchesProduct = payloadSnapshot is not null &&
                                              _installation.MatchesRetroWfcPayloadSnapshot(
                                                  toolkitFingerprint, payloadSnapshot);
        var cachedPayloadMatchesSnapshot = payloadSnapshot is null
            ? (bool?)null
            : _installation.CachedRetroWfcPayloadMatches(payloadSnapshot);
        var payloadAction = payloadSnapshot is null
            ? RetroWfcPayloadAction.None
            : PlanRetroWfcPayloadReconciliation(downloadedPayloadMatchesProduct,
                cachedPayloadMatchesSnapshot == true);

        // The canonical root the runtime must read assets from. A base-only operation never changes
        // it, so its recorded value stays exactly as the last Retro Rewind operation left it.
        var canonicalRoot = snapshot is null
            ? null
            : Path.GetFullPath(options.CanonicalRetroRewindRoot ?? throw new InvalidOperationException(
                "A Retro Rewind operation must record the canonical Retro Rewind folder."));
        var canonicalRootChanged = canonicalRoot is not null && !RecordedCanonicalRootMatches(canonicalRoot);

        var baseState = _installation.CheckBaseCore(toolkitFingerprint);
        var retroState = _installation.CheckRetroRewindCore(toolkitFingerprint, snapshot,
            canonicalError: null, cachedPayloadMatchesSnapshot);
        var desiredInputDrift = FindDesiredInputDrift(_installation, toolkitFingerprint,
            expectedDolSha256, expectedRelSha256);
        var rebuildBase = RequiresCompilation(baseState.Status) || desiredInputDrift.Base;
        var rebuildRetro = false;
        var syncRetroWfcPayload = false;

        if (snapshot is null)
        {
            if (desiredInputDrift.Retro)
                throw new InvalidOperationException(
                    "The release changes the clean game inputs used by the installed Retro Rewind product. " +
                    "Supply Wheel Wizard's current Retro Rewind folder explicitly so it can be rebuilt safely.");
            if (retroState.ActionRequired)
                throw new InvalidOperationException(
                    "Retro Rewind needs repair, but Wheel Wizard's current Retro Rewind folder was not supplied. " +
                    retroState.Detail);
        }
        else
        {
            rebuildRetro = desiredInputDrift.Retro || RequiresCompilation(retroState.Status) ||
                           retroState.Status == ProductStatus.Absent;
            syncRetroWfcPayload = retroState.Status == ProductStatus.PayloadChanged;

            if (payloadAction == RetroWfcPayloadAction.RebuildRetro)
            {
                rebuildRetro = true;
                syncRetroWfcPayload = true;
            }
            else if (payloadAction == RetroWfcPayloadAction.PublishCacheOnly)
            {
                syncRetroWfcPayload = true;
            }

            var installedMode = _installation.ResolveRetroWfcPayloadMode(toolkitFingerprint);
            if (_installation.HasRetroProduct && installedMode != RetroWfcPayloadMode.NotApplicable &&
                installedMode != requestedPayloadMode)
            {
                rebuildRetro = true;
                syncRetroWfcPayload = requestedPayloadMode == RetroWfcPayloadMode.Online;
            }
            if (rebuildRetro && requestedPayloadMode == RetroWfcPayloadMode.Online)
                syncRetroWfcPayload = true;
        }

        // Copied support files are republished from the installed workspace, never recompiled. A
        // rebuild already stages fresh copies, so only a product that is not being rebuilt needs it.
        var syncBaseRuntimeAssets = !rebuildBase && File.Exists(_installation.BaseExecutable) &&
                                    _installation.ValidateCopiedRuntimeAssets(_installation.BaseDirectory,
                                        "base", toolkitFingerprint) is not null;
        var syncRetroRuntimeAssets = !rebuildRetro && _installation.HasRetroProduct &&
                                     _installation.ValidateCopiedRuntimeAssets(_installation.RetroDirectory,
                                         "retro-rewind", toolkitFingerprint) is not null;

        if (!rebuildBase && !rebuildRetro && !syncRetroWfcPayload && !syncBaseRuntimeAssets &&
            !syncRetroRuntimeAssets && !canonicalRootChanged)
        {
            _reporter.Progress(InstallStages.Publish, "The installed products are already current.", 99);
            return new ReconciliationResult(snapshot, PublicationCommitted: false,
                VerifiedOnlinePayloadCache(toolkitFingerprint));
        }

        cancellationToken.ThrowIfCancellationRequested();
        var builder = new LocalBuildService(_reporter);
        string? baseOutput = null;
        string? retroOutput = null;

        if (rebuildBase || rebuildRetro)
        {
            EnsureSufficientRepairDiskSpace();
            _reporter.Progress(InstallStages.PrepareRetro,
                "Verifying the installed recompilation toolkit...", 4);
            var toolkitComponents = VerifyToolkitForCompilation(toolkitFingerprint, cancellationToken);
            RefreshTranslationAssets(expectedDolSha256, expectedRelSha256, cancellationToken);

            // A broken or blocked product may mean the caches themselves are damaged, so nothing
            // recorded on disk is allowed to contribute to the repaired product.
            var forceCleanBuild =
                (rebuildBase && baseState.Status is ProductStatus.Broken or ProductStatus.Blocked) ||
                (rebuildRetro && retroState.Status is ProductStatus.Broken or ProductStatus.Blocked);
            var (dolSha, relSha) = TranslationInputHashes();

            if (rebuildBase && rebuildRetro)
            {
                // One retro-aware translation, one build graph, both products - the same reason the
                // install path uses BuildProfile.Both: sequential legs would re-emit every shared
                // base shard with retro-aware content and recompile all of them twice.
                var compileInputs = snapshot ?? throw new InvalidOperationException(
                    "Retro Rewind recompilation requires a compile-input snapshot.");
                baseOutput = Path.Combine(scratchRoot, "base-output");
                retroOutput = Path.Combine(scratchRoot, "retro-output");
                _reporter.Progress(InstallStages.BuildBase,
                    "Recompiling Mario Kart Wii and Retro Rewind with the installed toolkit...", 5);
                await BuildWithCleanRetryAsync(forceCleanBuild, clean =>
                    builder.BuildAsync(_installation.Root, BuildProfile.Both, retroOutput,
                        requestedPayloadMode, payloadSnapshot?.Directory, cancellationToken,
                        toolkitComponents, clean,
                        progress: new BuildProgressWindow(_reporter, InstallStages.BuildBase, 5, 92),
                        retroRewindPackageDirectory: compileInputs.RetroRewindRoot,
                        baseOutputDirectory: baseOutput));
                LocalBuildService.WriteFingerprint(baseOutput, BuildProfile.Base, toolkitFingerprint,
                    dolSha, relSha, "", RetroWfcPayloadMode.NotApplicable);
                LocalBuildService.WriteFingerprint(retroOutput, BuildProfile.RetroRewind,
                    toolkitFingerprint, dolSha, relSha, compileInputs.CodePulSha256,
                    requestedPayloadMode, compileInputs.CompileInputsSha256,
                    payloadSnapshot?.Sha256 ?? "", payloadSnapshot?.ByteLength ?? 0);
            }
            else if (rebuildBase)
            {
                baseOutput = Path.Combine(scratchRoot, "base-output");
                _reporter.Progress(InstallStages.BuildBase,
                    "Recompiling Mario Kart Wii with the installed toolkit...", 5);
                await BuildWithCleanRetryAsync(forceCleanBuild, clean =>
                    builder.BuildAsync(_installation.Root, BuildProfile.Base, baseOutput,
                        RetroWfcPayloadMode.NotApplicable, null, cancellationToken,
                        toolkitComponents, clean,
                        progress: new BuildProgressWindow(_reporter, InstallStages.BuildBase, 5, 92)));
                LocalBuildService.WriteFingerprint(baseOutput, BuildProfile.Base, toolkitFingerprint,
                    dolSha, relSha, "", RetroWfcPayloadMode.NotApplicable);
            }
            else
            {
                var compileInputs = snapshot ?? throw new InvalidOperationException(
                    "Retro Rewind recompilation requires a compile-input snapshot.");
                retroOutput = Path.Combine(scratchRoot, "retro-output");
                _reporter.Progress(InstallStages.BuildRetro,
                    "Recompiling Retro Rewind for the canonical Code.pul...", 5);
                await BuildWithCleanRetryAsync(forceCleanBuild, clean =>
                    builder.BuildAsync(_installation.Root, BuildProfile.RetroRewind, retroOutput,
                        requestedPayloadMode, payloadSnapshot?.Directory, cancellationToken,
                        toolkitComponents, clean,
                        progress: new BuildProgressWindow(_reporter, InstallStages.BuildRetro, 5, 92),
                        retroRewindPackageDirectory: compileInputs.RetroRewindRoot));
                LocalBuildService.WriteFingerprint(retroOutput, BuildProfile.RetroRewind,
                    toolkitFingerprint, dolSha, relSha, compileInputs.CodePulSha256,
                    requestedPayloadMode, compileInputs.CompileInputsSha256,
                    payloadSnapshot?.Sha256 ?? "", payloadSnapshot?.ByteLength ?? 0);
            }
        }

        if (retroOutput is not null)
            PreserveProductConfig(_installation.RetroDirectory, retroOutput);
        if (baseOutput is not null)
            PreserveProductConfig(_installation.BaseDirectory, baseOutput);

        cancellationToken.ThrowIfCancellationRequested();
        PublishProducts(new PublicationPlan
        {
            BaseOutput = baseOutput,
            RetroOutput = retroOutput,
            CompileInputs = snapshot,
            CanonicalRetroRewindRoot = canonicalRoot,
            SyncBaseRuntimeAssets = syncBaseRuntimeAssets,
            SyncRetroRuntimeAssets = syncRetroRuntimeAssets,
            SyncRetroWfcPayload = syncRetroWfcPayload,
            ToolkitFingerprint = toolkitFingerprint,
            ScratchRoot = scratchRoot,
            RequestedPayloadMode = requestedPayloadMode,
            PayloadSnapshot = payloadSnapshot,
            ExpectedDolSha256 = expectedDolSha256,
            ExpectedRelSha256 = expectedRelSha256
        }, cancellationToken);
        _reporter.Progress(InstallStages.Publish, "Product repair complete.", 99);
        return new ReconciliationResult(snapshot, PublicationCommitted: true,
            VerifiedOnlinePayloadCache(toolkitFingerprint));
    }


    private RetroWfcPayloadSnapshot RecoverInstalledRetroWfcPayload(string toolkitFingerprint,
        string destinationDirectory, Exception downloadFailure, CancellationToken cancellationToken)
    {
        InvalidOperationException Unavailable(string reason) => new(
            "The Retro-WFC payload service is unavailable" +
            $" ({downloadFailure.Message.TrimEnd('.')}), and {reason}" +
            " Try again once the service is reachable.", downloadFailure);

        var fingerprint = _installation.ReadProductFingerprint(_installation.RetroDirectory,
            toolkitFingerprint);
        var state = _installation.ReadInstallState();
        if (fingerprint is not { RetroWfcPayloadMode: "downloaded" } ||
            state is not { RetroWfcPayloadMode: "downloaded" } ||
            string.IsNullOrWhiteSpace(fingerprint.RetroWfcPayloadSha256) ||
            !fingerprint.RetroWfcPayloadSha256.Equals(state.RetroWfcPayloadSha256,
                StringComparison.Ordinal) ||
            fingerprint.RetroWfcPayloadLength <= 0)
        {
            throw Unavailable("this installation has no verified payload snapshot to fall back to.");
        }

        string cachedFile;
        try
        {
            cachedFile = InputValidation.ResolveRetroWfcPayloadFile(
                _installation.WorkspaceRetroWfcPayload);
            cancellationToken.ThrowIfCancellationRequested();
            if (new FileInfo(cachedFile).Length != fingerprint.RetroWfcPayloadLength ||
                !InputValidation.Sha256File(cachedFile).Equals(fingerprint.RetroWfcPayloadSha256,
                    StringComparison.Ordinal))
            {
                throw Unavailable("the installed payload snapshot no longer matches its recorded identity.");
            }

            var destination = Path.Combine(Path.GetFullPath(destinationDirectory), "binary",
                "payload.RMCPD00.bin");
            Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
            File.Copy(cachedFile, destination, overwrite: true);
            var root = InputValidation.ValidateStagedRetroWfcPayloadDirectory(destinationDirectory);
            _reporter.Diagnostic(
                "The Retro-WFC payload service is unavailable; reusing the installation's verified " +
                $"payload snapshot (sha256 {fingerprint.RetroWfcPayloadSha256[..12]}..., " +
                $"{fingerprint.RetroWfcPayloadLength} bytes).");
            return new RetroWfcPayloadSnapshot(root, fingerprint.RetroWfcPayloadSha256,
                fingerprint.RetroWfcPayloadLength);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or InvalidDataException)
        {
            throw Unavailable($"the installed payload snapshot could not be reused ({ex.Message.TrimEnd('.')}).");
        }
    }

    /// <summary>The statuses that can only be resolved by producing the product again.</summary>
    private static bool RequiresCompilation(ProductStatus status) =>
        status is ProductStatus.ToolkitChanged or ProductStatus.CodePulChanged or
            ProductStatus.CompileInputsChanged or ProductStatus.InputsMissing or
            ProductStatus.Blocked or ProductStatus.Broken;

    private bool? VerifiedOnlinePayloadCache(string toolkitFingerprint) =>
        _installation.HasRetroProduct &&
        _installation.ResolveRetroWfcPayloadMode(toolkitFingerprint) == RetroWfcPayloadMode.Online
            ? true
            : null;

    internal static RetroWfcPayloadAction PlanRetroWfcPayloadReconciliation(bool productMatchesSnapshot,
        bool cacheMatchesSnapshot) =>
        !productMatchesSnapshot ? RetroWfcPayloadAction.RebuildRetro
        : cacheMatchesSnapshot ? RetroWfcPayloadAction.None
        : RetroWfcPayloadAction.PublishCacheOnly;

    private bool RecordedCanonicalRootMatches(string canonicalRoot)
    {
        var configured = _installation.ConfiguredRetroRewindRoot;
        var recorded = _installation.ReadInstallState()?.RetroRewindRoot;
        return configured is not null && recorded is not null &&
               SamePath(configured, canonicalRoot) && SamePath(recorded, canonicalRoot);
    }

    private static bool SamePath(string left, string right)
    {
        try { return FileSystemUtilities.PathsEqual(left, right); }
        catch { return false; }
    }

    /// <summary>Everything one reconciliation decided to publish as a single transaction.</summary>
    private sealed record PublicationPlan
    {
        public string? BaseOutput { get; init; }
        public string? RetroOutput { get; init; }
        public RetroRewindCompileInputs? CompileInputs { get; init; }
        public string? CanonicalRetroRewindRoot { get; init; }
        public bool SyncBaseRuntimeAssets { get; init; }
        public bool SyncRetroRuntimeAssets { get; init; }
        public bool SyncRetroWfcPayload { get; init; }

        public required string ToolkitFingerprint { get; init; }
        public required string ScratchRoot { get; init; }
        public RetroWfcPayloadMode RequestedPayloadMode { get; init; }
        public RetroWfcPayloadSnapshot? PayloadSnapshot { get; init; }
        public string? ExpectedDolSha256 { get; init; }
        public string? ExpectedRelSha256 { get; init; }
    }

    private void PublishProducts(PublicationPlan plan, CancellationToken cancellationToken)
    {
        var entries = new List<InstallTransactionEntry>();
        if (plan.BaseOutput is not null)
            entries.Add(InstallTransactionEntry.Directory(plan.BaseOutput, _installation.BaseDirectory));
        if (plan.RetroOutput is not null)
            entries.Add(InstallTransactionEntry.Directory(plan.RetroOutput, _installation.RetroDirectory));

        if (plan.SyncBaseRuntimeAssets || plan.SyncRetroRuntimeAssets)
            AddRuntimeAssetPublicationEntries(entries, plan.ScratchRoot, plan.SyncBaseRuntimeAssets,
                plan.SyncRetroRuntimeAssets, plan.ToolkitFingerprint, cancellationToken);

        if (plan.SyncRetroWfcPayload)
        {
            if (plan.PayloadSnapshot is null)
                throw new InvalidDataException("The downloaded Retro-WFC payload was not staged.");
            entries.Add(InstallTransactionEntry.Directory(plan.PayloadSnapshot.Directory,
                _installation.WorkspaceRetroWfcPayload));
        }
        if (plan.RetroOutput is not null && plan.RequestedPayloadMode == RetroWfcPayloadMode.Skipped)
        {
            var emptyPayload = Path.Combine(plan.ScratchRoot, "retro-wfc-payload-removed");
            Directory.CreateDirectory(emptyPayload);
            entries.Add(InstallTransactionEntry.Directory(emptyPayload,
                _installation.WorkspaceRetroWfcPayload));
        }

        // Copied-runtime repair does not change product or input provenance. Publish only the
        // authoritative support files in that case, leaving install-state byte-for-byte intact.
        var publishState = plan.BaseOutput is not null || plan.RetroOutput is not null ||
                           plan.CanonicalRetroRewindRoot is not null;
        if (publishState)
        {
            var statePath = Path.Combine(plan.ScratchRoot, InstalledLayout.InstallStateFileName);
            JsonState.Write(statePath, BuildUpdatedInstallState(plan.ToolkitFingerprint,
                plan.CompileInputs, plan.CanonicalRetroRewindRoot,
                plan.RetroOutput is null ? null : plan.RequestedPayloadMode,
                plan.RetroOutput is null ? null : plan.PayloadSnapshot, plan.ExpectedDolSha256,
                plan.ExpectedRelSha256));
            entries.Add(InstallTransactionEntry.File(statePath, _installation.InstallStatePath));
        }

        cancellationToken.ThrowIfCancellationRequested();
        // The game may have been started during a long compile; publishing renames its directory.
        RunningProductGuard.EnsureProductsNotRunning(_installation.Root);
        var configPath = RuntimeConfiguration.ResolveConfigPath(_installation.Root);
        var configSnapshot = RuntimeConfiguration.Capture(configPath);
        using var transaction = InstallTransaction.Begin(_installation.Root, _reporter, entries.ToArray());
        transaction.Publish();
        if (plan.CanonicalRetroRewindRoot is not null)
        {
            transaction.RecordRuntimeConfigurationMutation(configSnapshot);
            RuntimeConfiguration.SetRetroRewindRoot(configPath, plan.CanonicalRetroRewindRoot);
        }
        transaction.Commit();
    }

    private async Task BuildWithCleanRetryAsync(bool forceCleanBuild, Func<bool, Task> build)
    {
        try
        {
            await build(forceCleanBuild);
        }
        catch (InvalidOperationException ex) when (!forceCleanBuild)
        {
            _reporter.Diagnostic(
                $"Incremental recompilation failed ({ex.Message}); retrying with a clean build.");
            await build(true);
        }
    }

    private void EnsureSufficientRepairDiskSpace()
    {
        const long localBuildAllowance = 14L * 1024 * 1024 * 1024;
        const long safetyAllowance = 2L * 1024 * 1024 * 1024;
        FileSystemUtilities.EnsureFreeSpace(_installation.Root,
            localBuildAllowance + safetyAllowance, "Local recompilation");
    }

    internal ToolkitFingerprintComponents VerifyToolkitForCompilation(string authoritativeFingerprint,
        CancellationToken cancellationToken = default)
    {
        var state = _installation.ReadToolkitState();
        if (state is not { SchemaVersion: 2 } ||
            string.IsNullOrWhiteSpace(state.ToolkitFingerprint) ||
            !state.ToolkitFingerprint.Equals(authoritativeFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed toolkit provenance is missing or does not match this repair operation.");
        }

        var components = ToolkitFingerprint.ComputeComponents(_installation.Root, cancellationToken);
        if (!components.Compile.Equals(authoritativeFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed recompilation toolkit was modified or is incomplete. Apply the current setup release before compiling products.");
        }
        return components;
    }

    private void AddRuntimeAssetPublicationEntries(List<InstallTransactionEntry> entries,
        string scratchRoot, bool includeBase, bool includeRetro, string toolkitFingerprint,
        CancellationToken cancellationToken)
    {
        var state = _installation.ReadToolkitState();
        if (state is not { SchemaVersion: 2 } ||
            !state.ToolkitFingerprint.Equals(toolkitFingerprint, StringComparison.Ordinal) ||
            string.IsNullOrWhiteSpace(state.RuntimeAssetsFingerprint))
        {
            throw new InvalidDataException(
                "The installed runtime-asset provenance is missing or does not belong to the current toolkit.");
        }

        var expectedFingerprint = state.RuntimeAssetsFingerprint;
        var sourceFingerprint = ToolkitFingerprint.ComputeRuntimeAssets(_installation.Root, cancellationToken);
        if (!sourceFingerprint.Equals(expectedFingerprint, StringComparison.Ordinal))
        {
            throw new InvalidDataException(
                "The installed BuildWorkspace runtime assets do not match their authoritative toolkit provenance.");
        }

        var products = new[]
            {
                (Include: includeBase, Name: "Base", Destination: _installation.BaseDirectory),
                (Include: includeRetro, Name: "RetroRewind", Destination: _installation.RetroDirectory)
            }
            .Where(product => product.Include)
            .Select(product => (product.Name, product.Destination));
        RuntimeAssetPublication.AddEntries(entries,
            Path.Combine(_installation.WorkspaceDirectory, "runtime", "assets"),
            Path.Combine(scratchRoot, "runtime-asset-repair"), products, expectedFingerprint,
            "repair", _reporter.Diagnostic, cancellationToken);

        cancellationToken.ThrowIfCancellationRequested();
        var sourceAfter = ToolkitFingerprint.ComputeRuntimeAssets(_installation.Root, cancellationToken);
        if (!sourceAfter.Equals(sourceFingerprint, StringComparison.Ordinal))
            throw new IOException("The installed BuildWorkspace runtime assets changed while repair was being prepared.");
    }

    private InstallState BuildUpdatedInstallState(string toolkitFingerprint,
        RetroRewindCompileInputs? compileInputs, string? canonicalRetroRewindRoot,
        RetroWfcPayloadMode? payloadMode = null,
        RetroWfcPayloadSnapshot? retroWfcPayloadSnapshot = null, string? expectedDolSha256 = null,
        string? expectedRelSha256 = null)
    {
        var state = _installation.ReadInstallState() ?? new InstallState { InstallDir = _installation.Root };
        state.InstallDir = _installation.Root;
        state.SchemaVersion = 1;

        if (string.IsNullOrWhiteSpace(state.SetupVersion)) state.SetupVersion = ProductInfo.Version;
        if (string.IsNullOrWhiteSpace(state.ProductVersion)) state.ProductVersion = state.SetupVersion;
        if (!string.IsNullOrWhiteSpace(expectedDolSha256)) state.DolSha256 = expectedDolSha256;
        if (!string.IsNullOrWhiteSpace(expectedRelSha256)) state.RelSha256 = expectedRelSha256;
        state.RetroRewindInstalled = compileInputs is not null || _installation.HasRetroProduct;
        if (compileInputs is not null)
        {
            state.RetroRewindCodePulSha256 = compileInputs.CodePulSha256;
            state.RetroRewindCompileInputsSha256 = compileInputs.CompileInputsSha256;
        }
        if (canonicalRetroRewindRoot is not null)
            state.RetroRewindRoot = Path.GetFullPath(canonicalRetroRewindRoot);
        if (payloadMode is not null)
        {
            state.RetroWfcPayloadMode = payloadMode == RetroWfcPayloadMode.Online
                ? "downloaded"
                : "skipped";
            state.RetroWfcPayloadSha256 = payloadMode == RetroWfcPayloadMode.Online
                ? retroWfcPayloadSnapshot?.Sha256 ?? throw new InvalidDataException(
                    "The downloaded Retro-WFC payload snapshot is missing.")
                : "";
            state.RetroWfcPayloadLength = payloadMode == RetroWfcPayloadMode.Online
                ? retroWfcPayloadSnapshot!.ByteLength
                : 0;
        }
        else
        {
            var retro = _installation.ReadProductFingerprint(_installation.RetroDirectory,
                toolkitFingerprint);
            if (retro is not null && !string.IsNullOrWhiteSpace(retro.RetroWfcPayloadMode))
            {
                state.RetroWfcPayloadMode = retro.RetroWfcPayloadMode;
                state.RetroWfcPayloadSha256 = retro.RetroWfcPayloadSha256;
                state.RetroWfcPayloadLength = retro.RetroWfcPayloadLength;
            }
        }
        return state;
    }

    internal static (bool Base, bool Retro) FindDesiredInputDrift(Installation installation,
        string toolkitFingerprint, string? expectedDolSha256, string? expectedRelSha256)
    {
        if (string.IsNullOrWhiteSpace(expectedDolSha256) || string.IsNullOrWhiteSpace(expectedRelSha256))
            return (false, false);
        return (
            File.Exists(installation.BaseExecutable) &&
            !installation.ProductUsesGameInputs(installation.BaseDirectory, toolkitFingerprint,
                expectedDolSha256, expectedRelSha256),
            installation.HasRetroProduct &&
            !installation.ProductUsesGameInputs(installation.RetroDirectory, toolkitFingerprint,
                expectedDolSha256, expectedRelSha256));
    }


    private void RefreshTranslationAssets(string? expectedDolSha256, string? expectedRelSha256,
        CancellationToken cancellationToken)
    {
        if (string.IsNullOrWhiteSpace(expectedDolSha256) && string.IsNullOrWhiteSpace(expectedRelSha256))
        {
            var state = _installation.ReadInstallState();
            expectedDolSha256 = state is { SchemaVersion: 1 } ? state.DolSha256 : null;
            expectedRelSha256 = state is { SchemaVersion: 1 } ? state.RelSha256 : null;
        }
        if (string.IsNullOrWhiteSpace(expectedDolSha256) || string.IsNullOrWhiteSpace(expectedRelSha256))
            throw new InvalidDataException(
                "The installed game-input provenance is missing or unsupported. Apply the current setup with the disc image.");

        Directory.CreateDirectory(_installation.WorkspaceAssetsDirectory);
        foreach (var (name, source, expectedSha256) in new[]
                 {
                     ("main.dol", Path.Combine(_installation.GameDataDirectory, "sys", "main.dol"), expectedDolSha256),
                     ("StaticR.rel", Path.Combine(_installation.GameDataDirectory, "files", "rel", "StaticR.rel"),
                         expectedRelSha256)
                 })
        {
            cancellationToken.ThrowIfCancellationRequested();
            if (!File.Exists(source))
                throw new InvalidDataException(
                    $"The installed Mario Kart Wii {name} is missing. Apply setup again with the disc image.");
            if (!InputValidation.Sha256File(source).Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException(
                    $"The installed Mario Kart Wii {name} does not match its recorded clean-disc identity. " +
                    "Apply the current setup with the disc image.");

            var workspaceInput = Path.Combine(_installation.WorkspaceAssetsDirectory, name);
            if (File.Exists(workspaceInput) &&
                InputValidation.Sha256File(workspaceInput)
                    .Equals(expectedSha256, StringComparison.OrdinalIgnoreCase))
                continue;
            File.Copy(source, workspaceInput, overwrite: true);
        }
    }

    private (string DolSha, string RelSha) TranslationInputHashes() =>
        (InputValidation.Sha256File(Path.Combine(_installation.WorkspaceAssetsDirectory, "main.dol")),
            InputValidation.Sha256File(Path.Combine(_installation.WorkspaceAssetsDirectory, "StaticR.rel")));

    private static void PreserveProductConfig(string currentProduct, string preparedProduct)
    {
        var config = Path.Combine(currentProduct, "config");
        if (Directory.Exists(config))
            FileSystemUtilities.CopyDirectory(config, Path.Combine(preparedProduct, "config"));
    }
}
