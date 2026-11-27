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
        var inventory = payload.TryReadInventory(manifest, _reporter.Diagnostic);
        var toolkit = InstalledLayout.Toolkit(staging);
        var workspace = InstalledLayout.Workspace(staging);

        // The disc decides which release's inputs everything below expects, so it is identified
        // before any reuse decision or toolkit extraction.
        _reporter.Progress(InstallStages.Validate, "Checking the Wii disc image...", 1);
        var nodTool = payload.ExtractDiscTool(Path.Combine(staging, "disc-tool"));
        var header = await InputValidation.ReadDiscHeaderAsync(nodTool, options.GamePath,
            cancellationToken);
        var release = InputValidation.EnsureCompatibleDisc(header, manifest);
        var canonicalRetroRoot = options.RetroDirectoryPath is null
            ? null
            : RetroRewindSource.ResolveRetroRewind6(options.RetroDirectoryPath);
        ValidateRetroOptions(canonicalRetroRoot, options.RetroWfcPayloadMode);

        var retroCompileInputs = canonicalRetroRoot is null
            ? null
            : SnapshotRetroRewindCompileInputs(canonicalRetroRoot,
                Path.Combine(staging, "compile-inputs"), cancellationToken);
        if (options.Portable)
        {
            var portableRoot = PortableRoot.Create(parent);
            _reporter.Diagnostic($"Installing as a portable installation under {portableRoot}.");
        }

        var candidateToolkitFingerprint = manifest.ToolkitFingerprint;
        var candidateToolkitPackageFingerprint = manifest.ToolkitPackageFingerprint;
        var candidateRuntimeAssetsFingerprint = manifest.RuntimeAssetsFingerprint;
        var installedToolkitFingerprint = existing.IsPresent
            ? existing.ResolveToolkitFingerprint()
            : "";
        var installedToolkitPackageFingerprint = existing.IsPresent
            ? existing.ResolveToolkitPackageFingerprint()
            : "";
        var reusableGameAssets = FindReusableGameAssets(existing, release);

        var sameToolkit = existing.IsPresent && reusableGameAssets is not null &&
                          installedToolkitFingerprint.Equals(candidateToolkitFingerprint,
                              StringComparison.Ordinal);
        var samePackageContent = existing.IsPresent &&
                                 installedToolkitPackageFingerprint.Equals(
                                     candidateToolkitPackageFingerprint, StringComparison.Ordinal);
        var hadRetro = existing.HasRetroProduct || previousState?.RetroRewindInstalled == true;

        var runtimeAssetsCurrent = sameToolkit && RuntimeAssetsAreCurrent(existing,
            candidateRuntimeAssetsFingerprint, cancellationToken);
        var installedNodTool = Path.Combine(existing.ToolkitDirectory, "nodtool.exe");

        _reporter.Progress(InstallStages.ExtractToolkit,
            "Inspecting the release's local recompilation toolkit...", 4);
        var toolkitRefresh = DecideToolkitRefresh(existing, manifest, inventory, sameToolkit,
            samePackageContent, File.Exists(installedNodTool), cancellationToken);
        // An unchanged toolkit only ever needs the copied runtime assets refreshed: nothing else in
        // the workspace is published on that path, so nothing else is extracted (or re-stamped).
        var workspaceExtraction = !sameToolkit ? WorkspaceExtraction.Full
            : runtimeAssetsCurrent ? WorkspaceExtraction.None
            : WorkspaceExtraction.RuntimeAssetsOnly;

        var extracted = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        ExtractToolkit(payload, toolkitRefresh, toolkit, extracted);
        ExtractWorkspace(payload, workspaceExtraction, existing, workspace, extracted, cancellationToken);
        VerifyExtraction(inventory, toolkitRefresh, workspaceExtraction, extracted);
        payload.ExtractEntry("host/WiiCompiled-Setup.exe",
            Path.Combine(staging, ProductInfo.SetupCopyName));
        payload.ExtractDirectory("licenses", Path.Combine(staging, "licenses"));
        payload.ExtractEntry(InstalledLayout.PayloadManifestFileName,
            Path.Combine(staging, InstalledLayout.PayloadManifestFileName));

        var install = new InstallContext(existing, staging, payload, manifest, inventory, release,
            previousState, options, canonicalRetroRoot, retroCompileInputs, firstInstall);

        if (sameToolkit)
        {
            var desiredInputDrift = ProductRepairService.FindDesiredInputDrift(existing,
                candidateToolkitFingerprint, release.DolSha256, release.RelSha256);
            if (desiredInputDrift.Retro && retroCompileInputs is null)
                throw new InvalidOperationException(
                    "This release changes the clean game inputs used by the installed Retro Rewind product. " +
                    "Supply Wheel Wizard's current Retro Rewind folder explicitly so it can be rebuilt safely.");

            ProductRepairService.ReconciliationResult reconciliation;
            try
            {
                reconciliation = await ReconcileSameToolkitAsync(existing, retroCompileInputs,
                    canonicalRetroRoot, options.RetroWfcPayloadMode, Path.Combine(staging, "repair"),
                    release.DolSha256, release.RelSha256, cancellationToken);
            }
            catch (InvalidDataException ex) when (ToolkitIntegrity.IsFailure(ex))
            {
                // Without this, re-running setup would find the same "current" toolkit and fail the
                // same way forever. Nothing was published yet; publish the release's files and retry once.
                _reporter.Diagnostic(ex.Message + " Publishing the release's toolkit again and retrying once.");
                await RecoverToolkitAndReconcileAsync(install,
                    toolkitStaged: toolkitRefresh == ToolkitRefresh.Full, cancellationToken);
                return;
            }
            var remainingCancellation = reconciliation.PublicationCommitted
                ? CancellationToken.None
                : cancellationToken;


            if (!runtimeAssetsCurrent)
                runtimeAssetsCurrent = RuntimeAssetsAreCurrent(existing,
                    candidateRuntimeAssetsFingerprint, remainingCancellation);

            remainingCancellation.ThrowIfCancellationRequested();
            EnsureReconciliationCurrent(existing, candidateToolkitFingerprint, retroCompileInputs,
                release.DolSha256, release.RelSha256, reconciliation.CachedRetroWfcPayloadMatches);

            var updatedState = BuildInstallState(existing.ReadInstallState(), installDirectory, manifest,
                release, retroCompileInputs, canonicalRetroRoot, options.RetroWfcPayloadMode);
            PrepareMetadata(staging, manifest, updatedState);
            remainingCancellation.ThrowIfCancellationRequested();

            var releaseEntries = new List<InstallTransactionEntry>();
            // Keyed on what was extracted, so a toolkit refreshed only to restore a missing
            // nodtool.exe is actually published.
            AddToolkitEntries(releaseEntries, toolkitRefresh, staging, installDirectory);
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
                Path.Combine(staging, "GameAssets"), release, cancellationToken);
        }

        await PublishToolkitAndReconcileProductsAsync(install, toolkitRefresh,
            publishGameAssets: reusableGameAssets is null, cancellationToken);
    }

    /// <summary>Everything one install operation shares between its publication paths.</summary>
    internal sealed record InstallContext(Installation Existing, string Staging, PayloadArchive Payload,
        PayloadManifest Manifest, PayloadInventory? Inventory, GameRelease Release, InstallState? PreviousState,
        InstallOptions Options, string? CanonicalRetroRoot, RetroRewindCompileInputs? RetroCompileInputs,
        bool FirstInstall);

    /// <summary>
    /// Re-extract <c>Toolkit/</c> if and only if its package identity changed, nodtool.exe is
    /// missing, or the installed copy fails the stat check against the release inventory; when only
    /// the translator changed and the installed toolchain passes, just <c>Toolkit/Translator</c>.
    /// Without an inventory this is the original rule: refresh unless the whole toolkit is unchanged.
    /// </summary>
    internal static ToolkitRefresh PlanToolkitRefresh(bool inventoryAvailable, bool sameToolkit,
        bool samePackageContent, bool nodToolPresent, Func<bool> toolkitStatOk,
        bool sameToolchainPackage, Func<bool> toolchainStatOk)
    {
        if (!inventoryAvailable)
            return sameToolkit && samePackageContent && nodToolPresent ? ToolkitRefresh.None : ToolkitRefresh.Full;
        if (!nodToolPresent) return ToolkitRefresh.Full;
        if (samePackageContent) return toolkitStatOk() ? ToolkitRefresh.None : ToolkitRefresh.Full;
        if (sameToolchainPackage && toolchainStatOk()) return ToolkitRefresh.TranslatorOnly;
        return ToolkitRefresh.Full;
    }

    private ToolkitRefresh DecideToolkitRefresh(Installation existing, PayloadManifest manifest,
        PayloadInventory? inventory, bool sameToolkit, bool samePackageContent, bool nodToolPresent,
        CancellationToken cancellationToken)
    {
        bool StatOk(string description, IReadOnlyCollection<string>? excluded)
        {
            var problems = inventory!.StatCheck(existing.Root, InstalledLayout.ToolkitEntryPrefix,
                requireNormalizedTimestamp: true, excluded, cancellationToken);
            if (problems.Count == 0) return true;
            _reporter.Diagnostic($"The installed {description} cannot be reused as it is: " +
                                 string.Join("; ", problems));
            return false;
        }

        var state = existing.IsPresent && existing.ResolveToolkitPackageFingerprint().Length > 0
            ? existing.ReadToolkitState()
            : null;
        var sameToolchainPackage = state is not null &&
                                   !string.IsNullOrWhiteSpace(manifest.ToolchainPackageFingerprint) &&
                                   manifest.ToolchainPackageFingerprint.Equals(
                                       state.ToolchainPackageFingerprint ?? "", StringComparison.Ordinal);
        var refresh = PlanToolkitRefresh(inventory is not null, sameToolkit, samePackageContent, nodToolPresent,
            () => StatOk("toolkit", null),
            sameToolchainPackage,
            () => StatOk("toolchain", [ToolkitFingerprint.TranslatorPrefix]));
        _reporter.Diagnostic(refresh switch
        {
            ToolkitRefresh.None => "The installed toolkit is this release's and is reused.",
            ToolkitRefresh.TranslatorOnly => "Only the translator changed; the installed toolchain is reused.",
            _ => "The release's toolkit is extracted."
        });
        return refresh;
    }

    private static void ExtractToolkit(PayloadArchive payload, ToolkitRefresh refresh, string stagedToolkit,
        IDictionary<string, PayloadFileIdentity> extracted)
    {
        if (refresh == ToolkitRefresh.Full)
            payload.ExtractDirectory(InstalledLayout.ToolkitDirectoryName, stagedToolkit, extracted);
        else if (refresh == ToolkitRefresh.TranslatorOnly)
            payload.ExtractDirectory(ToolkitFingerprint.TranslatorPrefix,
                Path.Combine(stagedToolkit, "Translator"), extracted);
    }

    private void ExtractWorkspace(PayloadArchive payload, WorkspaceExtraction extraction, Installation existing,
        string stagedWorkspace, IDictionary<string, PayloadFileIdentity> extracted,
        CancellationToken cancellationToken)
    {
        if (extraction == WorkspaceExtraction.Full)
        {
            payload.ExtractDirectory(InstalledLayout.WorkspaceDirectoryName, stagedWorkspace, extracted);
            WorkspaceTimestamps.MarkChangedFiles(existing.WorkspaceDirectory, stagedWorkspace,
                _reporter.Diagnostic, cancellationToken);
        }
        else if (extraction == WorkspaceExtraction.RuntimeAssetsOnly)
        {
            // Only published as copied runtime support files, never compiled, so their timestamps
            // carry no meaning for the incremental build.
            payload.ExtractDirectory(RuntimeAssetsEntryPrefix,
                Path.Combine(stagedWorkspace, "runtime", "assets"), extracted);
        }
    }

    private const string RuntimeAssetsEntryPrefix = InstalledLayout.WorkspaceEntryPrefix + "runtime/assets/";

    /// <summary>Every byte streamed out of the payload must be exactly what the release inventory lists.</summary>
    private static void VerifyExtraction(PayloadInventory? inventory, ToolkitRefresh toolkit,
        WorkspaceExtraction workspace, IReadOnlyDictionary<string, PayloadFileIdentity> extracted)
    {
        if (inventory is null) return;
        if (toolkit == ToolkitRefresh.Full) inventory.VerifyExtracted(InstalledLayout.ToolkitEntryPrefix, extracted);
        else if (toolkit == ToolkitRefresh.TranslatorOnly)
            inventory.VerifyExtracted(ToolkitFingerprint.TranslatorPrefix, extracted);
        if (workspace == WorkspaceExtraction.Full)
            inventory.VerifyExtracted(InstalledLayout.WorkspaceEntryPrefix, extracted);
        else if (workspace == WorkspaceExtraction.RuntimeAssetsOnly)
            inventory.VerifyExtracted(RuntimeAssetsEntryPrefix, extracted);
    }

    /// <summary>
    /// Never overlaps <c>Toolkit</c> and <c>Toolkit/Translator</c>: a transaction rejects overlapping
    /// destinations, and the translator-only plan staged nothing else.
    /// </summary>
    internal static void AddToolkitEntries(List<InstallTransactionEntry> entries, ToolkitRefresh refresh,
        string staging, string installDirectory)
    {
        if (refresh == ToolkitRefresh.Full)
            AddComponent(entries, staging, installDirectory, InstalledLayout.ToolkitDirectoryName);
        else if (refresh == ToolkitRefresh.TranslatorOnly)
            entries.Add(InstallTransactionEntry.Directory(
                Path.Combine(InstalledLayout.Toolkit(staging), "Translator"),
                Path.Combine(InstalledLayout.Toolkit(installDirectory), "Translator")));
    }

    /// <summary>
    /// The installed toolkit failed integrity verification although it is this release's. Extract
    /// the release's Toolkit and BuildWorkspace afresh (keeping the build caches) and run the normal
    /// publish-then-reconcile path, which itself re-verifies what it published and retries once.
    /// </summary>
    private async Task RecoverToolkitAndReconcileAsync(InstallContext install, bool toolkitStaged,
        CancellationToken cancellationToken)
    {
        var toolkit = InstalledLayout.Toolkit(install.Staging);
        var workspace = InstalledLayout.Workspace(install.Staging);
        // A whole toolkit this operation already extracted (and verified) is complete; anything
        // else staged so far was partial and is replaced.
        if (!toolkitStaged && Directory.Exists(toolkit)) Directory.Delete(toolkit, recursive: true);
        if (Directory.Exists(workspace)) Directory.Delete(workspace, recursive: true);
        _reporter.Progress(InstallStages.ExtractToolkit,
            "Restoring the release's local recompilation toolkit...", 4);
        var extracted = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        var toolkitPlan = toolkitStaged ? ToolkitRefresh.None : ToolkitRefresh.Full;
        ExtractToolkit(install.Payload, toolkitPlan, toolkit, extracted);
        ExtractWorkspace(install.Payload, WorkspaceExtraction.Full, install.Existing, workspace, extracted,
            cancellationToken);
        VerifyExtraction(install.Inventory, toolkitPlan, WorkspaceExtraction.Full, extracted);
        EnsureSufficientRemainingDiskSpace(install.Existing.Root, needsExtractedDisc: false, needsLocalBuild: true);
        await PublishToolkitAndReconcileProductsAsync(install, ToolkitRefresh.Full, publishGameAssets: false,
            cancellationToken);
    }

    private static void AddComponent(List<InstallTransactionEntry> entries, string staging,
        string installDirectory, string name) =>
        entries.Add(InstallTransactionEntry.Directory(Path.Combine(staging, name),
            Path.Combine(installDirectory, name)));

    /// <summary>
    /// Build-produced workspace directories (not shipped in a release) grafted from the installed workspace into
    /// the staged one, carrying the base translation, native build dir, verified inputs, and Retro-WFC payload cache
    /// across a toolkit update. Each is re-validated by the build, so stale caches degrade to a clean rebuild, never reuse.
    /// </summary>
    private static readonly string[] WorkspaceCacheDirectories = PayloadInventory.WorkspaceCacheDirectories;

    private async Task PublishToolkitAndReconcileProductsAsync(InstallContext install,
        ToolkitRefresh toolkitRefresh, bool publishGameAssets, CancellationToken cancellationToken)
    {
        var existing = install.Existing;
        var staging = install.Staging;
        var manifest = install.Manifest;
        var release = install.Release;
        var installDirectory = existing.Root;

        var stagedWorkspace = InstalledLayout.Workspace(staging);
        try
        {
            GraftWorkspaceCaches(existing.WorkspaceDirectory, stagedWorkspace, cancellationToken);

            var state = BuildInstallState(install.PreviousState, installDirectory, manifest, release,
                compileInputs: null, install.CanonicalRetroRoot, install.Options.RetroWfcPayloadMode);
            PrepareMetadata(staging, manifest, state);

            cancellationToken.ThrowIfCancellationRequested();
            var entries = new List<InstallTransactionEntry>();

            AddToolkitEntries(entries, toolkitRefresh, staging, installDirectory);
            AddComponent(entries, staging, installDirectory, InstalledLayout.WorkspaceDirectoryName);
            if (publishGameAssets) AddComponent(entries, staging, installDirectory, "GameAssets");

            Publish(staging, installDirectory, install.CanonicalRetroRoot, state,
                entries, cancellationToken, progressPercent: 8, completionPercent: 10,
                createShortcuts: install.FirstInstall);
        }
        catch
        {
            RestoreWorkspaceCaches(existing.WorkspaceDirectory, stagedWorkspace);
            throw;
        }

        var verified = EnsurePublishedToolkitIntact(install, cancellationToken);

        _reporter.Progress(InstallStages.BuildBase,
            "Producing the installed products with the published toolkit...", 11);

        var reconciliation = await ReconcileSameToolkitAsync(existing, install.RetroCompileInputs,
            install.CanonicalRetroRoot, install.Options.RetroWfcPayloadMode, repairScratch: null,
            release.DolSha256, release.RelSha256, cancellationToken, verified);
        EnsureReconciliationCurrent(existing, manifest.ToolkitFingerprint, install.RetroCompileInputs,
            release.DolSha256, release.RelSha256, reconciliation.CachedRetroWfcPayloadMatches);
    }

    /// <summary>
    /// Moves the installed workspace's build caches into a staged workspace, so a toolkit update
    /// keeps the base translation, native build directory, verified inputs and payload cache. Each
    /// is re-validated by the build, so stale caches degrade to a clean rebuild, never reuse.
    /// </summary>
    private static void GraftWorkspaceCaches(string installedWorkspace, string stagedWorkspace,
        CancellationToken cancellationToken)
    {
        foreach (var cacheDirectory in WorkspaceCacheDirectories)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var source = Path.Combine(installedWorkspace, cacheDirectory);
            if (!Directory.Exists(source)) continue;
            var destination = Path.Combine(stagedWorkspace, cacheDirectory);
            if (Directory.Exists(destination)) Directory.Delete(destination, recursive: true);
            Directory.Move(source, destination);
        }
    }

    /// <summary>
    /// Moves caches grafted into a staged workspace that was never published back to the installed
    /// workspace, so a failed or cancelled publication does not throw the build caches away.
    /// </summary>
    private static void RestoreWorkspaceCaches(string installedWorkspace, string stagedWorkspace)
    {
        if (!Directory.Exists(installedWorkspace)) return;
        foreach (var cacheDirectory in WorkspaceCacheDirectories)
        {
            var source = Path.Combine(stagedWorkspace, cacheDirectory);
            var destination = Path.Combine(installedWorkspace, cacheDirectory);
            try
            {
                if (Directory.Exists(source) && !Directory.Exists(destination)) Directory.Move(source, destination);
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                // Best effort: a lost cache only costs a clean rebuild.
            }
        }
    }

    /// <summary>
    /// After publication, every shipped Toolkit/BuildWorkspace file must exist with its exact size
    /// (and, for the toolkit, its payload timestamp) and nothing foreign may sit beside them. A root
    /// that fails is extracted and published once more; a second failure is reported instead of
    /// leaving the user in a loop. Returns the identities the compile may use without re-hashing,
    /// derived from the verified inventory, or null to make the compile hash the toolkit itself.
    /// </summary>
    internal ToolkitFingerprintComponents? EnsurePublishedToolkitIntact(InstallContext install,
        CancellationToken cancellationToken)
    {
        var inventory = install.Inventory;
        if (inventory is null) return null;
        var root = install.Existing.Root;
        (List<string> Toolkit, List<string> Workspace) Check() => (
            inventory.StatCheck(root, InstalledLayout.ToolkitEntryPrefix, requireNormalizedTimestamp: true,
                cancellationToken: cancellationToken),
            inventory.StatCheck(root, InstalledLayout.WorkspaceEntryPrefix, requireNormalizedTimestamp: false,
                cancellationToken: cancellationToken));

        var problems = Check();
        if (problems.Toolkit.Count > 0 || problems.Workspace.Count > 0)
        {
            _reporter.Diagnostic("The published toolkit does not match the release; publishing it once more: " +
                                 string.Join("; ", problems.Toolkit.Concat(problems.Workspace)));
            RepublishDamagedRoots(install, problems.Toolkit.Count > 0, problems.Workspace.Count > 0,
                cancellationToken);
            problems = Check();
            if (problems.Toolkit.Count > 0 || problems.Workspace.Count > 0)
                throw new InvalidOperationException(
                    "Setup published the release's recompilation toolkit twice, but its files still do not " +
                    "match the release (" + string.Join("; ", problems.Toolkit.Concat(problems.Workspace)) +
                    "). Security software may be removing or changing them; allow the installation folder " +
                    "and run setup again.");
        }
        return VerifiedComponents(install, cancellationToken);
    }

    private void RepublishDamagedRoots(InstallContext install, bool toolkit, bool workspace,
        CancellationToken cancellationToken)
    {
        var installDirectory = install.Existing.Root;
        var retry = Path.Combine(install.Staging, "republish");
        if (Directory.Exists(retry)) Directory.Delete(retry, recursive: true);
        var extracted = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        var entries = new List<InstallTransactionEntry>();
        if (toolkit)
        {
            ExtractToolkit(install.Payload, ToolkitRefresh.Full, InstalledLayout.Toolkit(retry), extracted);
            VerifyExtraction(install.Inventory, ToolkitRefresh.Full, WorkspaceExtraction.None, extracted);
            AddComponent(entries, retry, installDirectory, InstalledLayout.ToolkitDirectoryName);
        }
        if (workspace)
        {
            var stagedWorkspace = InstalledLayout.Workspace(retry);
            ExtractWorkspace(install.Payload, WorkspaceExtraction.Full, install.Existing, stagedWorkspace,
                extracted, cancellationToken);
            VerifyExtraction(install.Inventory, ToolkitRefresh.None, WorkspaceExtraction.Full, extracted);
            AddComponent(entries, retry, installDirectory, InstalledLayout.WorkspaceDirectoryName);
        }

        cancellationToken.ThrowIfCancellationRequested();
        RunningProductGuard.EnsureProductsNotRunning(installDirectory);
        var retryWorkspace = InstalledLayout.Workspace(retry);
        try
        {
            if (workspace)
                GraftWorkspaceCaches(install.Existing.WorkspaceDirectory, retryWorkspace, cancellationToken);
            using var transaction = InstallTransaction.Begin(installDirectory, _reporter, entries.ToArray());
            transaction.Publish();
            transaction.Commit();
        }
        catch
        {
            if (workspace) RestoreWorkspaceCaches(install.Existing.WorkspaceDirectory, retryWorkspace);
            throw;
        }
    }

    /// <summary>
    /// The toolkit identities of the just-verified installation, computed from the release
    /// inventory exactly the way <see cref="ToolkitFingerprint.ComputeComponents"/> computes them
    /// from disk. Any disagreement with the manifest discards them, so the compile hashes instead.
    /// </summary>
    private ToolkitFingerprintComponents? VerifiedComponents(InstallContext install,
        CancellationToken cancellationToken)
    {
        cancellationToken.ThrowIfCancellationRequested();
        var components = ComputeInventoryComponents(install.Inventory!, install.Existing.Root,
            install.Manifest, _reporter.Diagnostic);
        return components;
    }

    /// <summary>
    /// Derives the toolkit identities from a release inventory whose files were verified to be the
    /// ones at <paramref name="root"/>. The native index is the one document whose content (not only
    /// its hash) the identities read; it is read back from disk and must hash to its inventory entry.
    /// Returns null when the inventory does not reproduce the manifest's identities.
    /// </summary>
    internal static ToolkitFingerprintComponents? ComputeInventoryComponents(PayloadInventory inventory,
        string root, PayloadManifest manifest, Action<string> diagnostic)
    {
        string? ReadVerifiedText(string relative)
        {
            if (!inventory.ByPath.TryGetValue(relative, out var shipped)) return null;
            try
            {
                var bytes = File.ReadAllBytes(Path.Combine(root, relative.Replace('/', Path.DirectorySeparatorChar)));
                var sha = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(bytes)).ToLowerInvariant();
                return sha.Equals(shipped.Sha256, StringComparison.OrdinalIgnoreCase)
                    ? System.Text.Encoding.UTF8.GetString(bytes)
                    : null;
            }
            catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
            {
                return null;
            }
        }

        ToolkitFingerprintComponents components;
        try
        {
            components = ToolkitFingerprint.ComputeComponentsFromHashes(inventory.Hashes(), ReadVerifiedText,
                inventory.TranslationInputLists());
        }
        catch (InvalidDataException ex)
        {
            diagnostic("The toolkit identities could not be derived from the release inventory: " + ex.Message);
            return null;
        }
        if (!components.Compile.Equals(manifest.ToolkitFingerprint, StringComparison.Ordinal) ||
            (manifest.TranslationFingerprint.Length > 0 &&
             !components.Translation.Equals(manifest.TranslationFingerprint, StringComparison.Ordinal)) ||
            (manifest.NativeToolchainFingerprint.Length > 0 &&
             !components.NativeToolchain.Equals(manifest.NativeToolchainFingerprint, StringComparison.Ordinal)))
        {
            diagnostic("The release inventory does not reproduce the release's toolkit identities; " +
                       "the toolkit is hashed instead.");
            return null;
        }
        var shippedByRelease = manifest.TranslationFingerprintsByRelease ?? new Dictionary<string, string>();
        var sameByRelease = shippedByRelease.Count == components.TranslationByRelease.Count &&
                            shippedByRelease.All(pair =>
                                components.TranslationByRelease.TryGetValue(pair.Key, out var value) &&
                                value == pair.Value);
        return sameByRelease
            ? components
            : components with { TranslationByRelease = new SortedDictionary<string, string>(StringComparer.Ordinal) };
    }

    private Task<ProductRepairService.ReconciliationResult> ReconcileSameToolkitAsync(Installation existing,
        RetroRewindCompileInputs? retroCompileInputs, string? canonicalRetroRoot,
        RetroWfcPayloadMode payloadMode, string? repairScratch, string expectedDolSha256,
        string expectedRelSha256, CancellationToken cancellationToken,
        ToolkitFingerprintComponents? preverifiedToolkitComponents = null)
    {
        var repair = new ProductRepairService(existing, _reporter);
        var options = new ProductRepairService.ReconcileOptions
        {
            ScratchRoot = repairScratch,
            ExpectedDolSha256 = expectedDolSha256,
            ExpectedRelSha256 = expectedRelSha256,
            CanonicalRetroRewindRoot = canonicalRetroRoot,
            PreverifiedToolkitComponents = preverifiedToolkitComponents
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
            ToolkitReleaseTag = manifest.ToolkitReleaseTag,
            ToolchainPackageFingerprint = manifest.ToolchainPackageFingerprint ?? "",
            TranslatorPackageFingerprint = manifest.TranslatorPackageFingerprint ?? "",
            TranslationFingerprintsByRelease = new Dictionary<string, string>(
                manifest.TranslationFingerprintsByRelease ?? new Dictionary<string, string>(), StringComparer.Ordinal),
            NativePrebuiltGateProvenanceSha256 = manifest.NativePrebuiltGateProvenanceSha256 ?? ""
        });
        JsonState.Write(Path.Combine(staging, InstalledLayout.InstallStateFileName), state);
    }

    private static InstallState BuildInstallState(InstallState? previousState, string installDirectory,
        PayloadManifest manifest, GameRelease release, RetroRewindCompileInputs? compileInputs,
        string? canonicalRetroRoot,
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
            DolSha256 = release.DolSha256,
            RelSha256 = release.RelSha256,
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

    private static void ValidateRetroOptions(string? canonicalRetroRoot, RetroWfcPayloadMode mode)
    {
        if (canonicalRetroRoot is null && mode != RetroWfcPayloadMode.NotApplicable)
            throw new InvalidOperationException(
                "A Retro-WFC payload option requires the canonical Retro Rewind folder.");
        if (canonicalRetroRoot is not null && mode == RetroWfcPayloadMode.NotApplicable)
            throw new InvalidOperationException(
                "Download the Retro-WFC payload, or explicitly skip the optional payload.");
    }

    private string? FindReusableGameAssets(Installation existing, GameRelease release)
    {
        var dataRoot = existing.GameDataDirectory;
        // No fst.bin simply means nothing was extracted here before; anything past that gate is an
        // existing extraction that fails reuse, which the log must say before the expensive
        // re-extraction quietly repairs it.
        if (!File.Exists(Path.Combine(dataRoot, "sys", "fst.bin"))) return null;
        try
        {
            ValidateExtractedGame(dataRoot, release);
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
        GameRelease release, CancellationToken cancellationToken)
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
        ValidateExtractedGame(dataRoot, release);
    }

    internal static void ValidateExtractedGame(string dataRoot, GameRelease release)
    {
        var dol = Path.Combine(dataRoot, "sys", "main.dol");
        var rel = Path.Combine(dataRoot, "files", "rel", "StaticR.rel");
        var fst = Path.Combine(dataRoot, "sys", "fst.bin");
        var boot = Path.Combine(dataRoot, "sys", "boot.bin");
        var missing = new[] { dol, rel, fst, boot }.Where(path => !File.Exists(path))
            .Select(path => Path.GetRelativePath(dataRoot, path)).ToList();
        if (missing.Count > 0)
            throw new InvalidDataException(
                "The disc extraction is missing required Mario Kart Wii files: " +
                string.Join(", ", missing) + ".");

        var mismatches = new List<string>();
        var discGameId = Installation.ReadDiscGameId(dataRoot);
        if (!discGameId.Equals(release.GameId, StringComparison.Ordinal))
            mismatches.Add($"sys/boot.bin names {discGameId}");
        foreach (var (name, path, expected) in new[]
                 {
                     ("main.dol", dol, release.DolSha256),
                     ("StaticR.rel", rel, release.RelSha256)
                 })
        {
            var actual = InputValidation.Sha256File(path);
            if (!actual.Equals(expected, StringComparison.OrdinalIgnoreCase))
                mismatches.Add($"{name} is {new FileInfo(path).Length:N0} bytes with SHA-256 {actual} " +
                               $"(expected {expected})");
        }

        if (mismatches.Count > 0)
            throw new InvalidDataException(
                $"The disc is {release.GameId} but its game data does not match the supported clean revision: " +
                string.Join("; ", mismatches) + ". " +
                "Patched or otherwise modified game code cannot be installed safely.");
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
