using System.Text.Json;

namespace WiiCompiled.Setup;

internal static class ConsoleCommands
{
    public static int Help()
    {
        Console.Out.WriteLine($"{ProductInfo.Name} command-line setup {ProductInfo.Version}");
        Console.Out.WriteLine("Wheel Wizard is the graphical interface for installing and launching WiiCompiled.");
        Console.Out.WriteLine();
        Console.Out.WriteLine("Commands:");
        Console.Out.WriteLine("  --silent --game <image> --install-dir <dir> [--retro-dir <folder>] [--portable]");
        Console.Out.WriteLine("  --verify-inputs --game <image> [--retro-dir <folder>]");
        Console.Out.WriteLine("  --check-products [--install-dir <dir>] [--retro-dir <folder>] [--progress-json]");
        Console.Out.WriteLine("  --repair-products --install-dir <dir> --retro-dir <folder> " +
                             "(--download-retro-wfc-payload | --skip-retro-wfc-payload) [--progress-json]");
        Console.Out.WriteLine("  --launch-retro | --launch-base");
        Console.Out.WriteLine("  --uninstall --install-dir <dir>");
        Console.Out.WriteLine("  --version");
        return 0;
    }

    public static int Version()
    {
        Console.Out.WriteLine(ProductInfo.Version);
        return 0;
    }


    public static int EmitPayloadIdentities(CommandLine command)
    {
        var root = Path.GetFullPath(command.PayloadRootPath!);
        if (!Directory.Exists(InstalledLayout.Toolkit(root)) ||
            !Directory.Exists(InstalledLayout.Workspace(root)))
        {
            throw new InvalidDataException($"{root} is not a staged payload root: expected " +
                $"{InstalledLayout.ToolkitDirectoryName} and {InstalledLayout.WorkspaceDirectoryName} " +
                "directories.");
        }

        var components = ToolkitFingerprint.ComputeComponents(root);
        var identities = new PayloadIdentities(
            components.Compile,
            components.Translation,
            components.NativeToolchain,
            ToolkitFingerprint.ComputePackage(root),
            ToolkitFingerprint.ComputeRuntimeAssets(root));
        Console.Out.WriteLine(JsonSerializer.Serialize(identities));
        return 0;
    }

    private sealed record PayloadIdentities(string ToolkitFingerprint, string TranslationFingerprint,
        string NativeToolchainFingerprint, string ToolkitPackageFingerprint,
        string RuntimeAssetsFingerprint);

    public static int VerifyInputs(CommandLine command)
    {
        var reporter = command.ProgressJson ? new NdjsonInstallReporter() : null;
        var temp = Path.Combine(Path.GetTempPath(), "mkwc-verify-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(temp);
        try
        {
            using var payload = PayloadArchive.OpenCurrent();
            var manifest = payload.ReadManifest();
            var tool = Path.Combine(temp, "DolphinTool.exe");
            payload.ExtractEntry(InstalledLayout.ToolkitEntryPrefix + "DolphinTool.exe", tool);
            payload.ExtractDirectory(InstalledLayout.ToolkitEntryPrefix + "Redist", temp);
            reporter?.Progress(InstallStages.Validate, "Checking the Wii disc image...", 10);
            var header = InputValidation.ReadDiscHeaderAsync(tool, command.GamePath!).GetAwaiter().GetResult();
            InputValidation.EnsureCompatibleDisc(header, manifest);

            if (command.RetroDirectoryPath is not null)
            {
                reporter?.Progress(InstallStages.Validate, "Checking the Retro Rewind folder...", 80);
                _ = CompileInputsFingerprint.Compute(command.RetroDirectoryPath);
            }
            reporter?.Success(command.InstallDirectory is null
                ? ProductInfo.DefaultInstallDirectory
                : Path.GetFullPath(command.InstallDirectory));
            return 0;
        }
        catch (Exception ex) when (reporter is not null)
        {
            reporter.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            reporter?.EnsureFinished("Input verification stopped before it reached a result.");
            if (Directory.Exists(temp)) Directory.Delete(temp, recursive: true);
        }
    }

    public static async Task<int> Install(CommandLine command, CancellationToken cancellationToken = default)
    {
        // Per-process file name: a fixed shared path let two concurrent invocations (e.g. two
        // installs to different directories) race to open the same file, and the loser's
        // StreamWriter constructor throws before the try/ndjson protocol below is even set up -
        // an unhandled crash instead of a clean NDJSON failure line (flagged by CodeRabbit).
        var logPath = Path.Combine(Path.GetTempPath(), $"WiiCompiled-setup-{Environment.ProcessId}.log");
        var logLock = new object();
        // A build/repair can emit thousands of log lines; keep one file handle open for the
        // duration instead of paying an open+append+close syscall round trip per line.
        using var logWriter = new StreamWriter(logPath, append: false) { AutoFlush = false };
        void Log(string message)
        {
            lock (logLock)
            {
                logWriter.Write($"[{DateTime.Now:O}] {message}{Environment.NewLine}");
                logWriter.Flush();
            }
        }
        logWriter.Write($"{ProductInfo.Name} setup {ProductInfo.Version}{Environment.NewLine}");
        logWriter.Flush();

        var ndjson = command.ProgressJson ? new NdjsonInstallReporter(Log) : null;
        IInstallReporter reporter = ndjson ?? (IInstallReporter)new ConsoleInstallReporter(Log);
        var installDirectory = Path.GetFullPath(command.InstallDirectory!);
        try
        {
            var engine = new InstallerEngine(reporter);
            await engine.InstallAsync(new InstallOptions
            {
                GamePath = command.GamePath!,
                RetroDirectoryPath = command.RetroDirectoryPath,
                RetroWfcPayloadMode = command.RetroWfcPayloadMode,
                InstallDirectory = installDirectory,
                Portable = command.Portable
            }, cancellationToken);
            ndjson?.Success(installDirectory);
            return 0;
        }
        catch (Exception ex) when (ndjson is not null)
        {
            // The caller reads stdout as a protocol, so the failure has to arrive as the terminal
            // result line; the exception detail belongs on stderr and in the setup log.
            ndjson.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            // Nothing - not a cancellation, not a process-level abort path - may leave the caller
            // without the one line it waits for.
            ndjson?.EnsureFinished("The installer stopped before it reached a result.");
        }
    }

    /// <summary>Reports whether the installed products are current, without building, mutating, networking,
    /// or walking the Retro Rewind asset tree. Exit code 2 means work is required, not a protocol failure.</summary>
    public static int CheckProducts(CommandLine command,
        CancellationToken cancellationToken = default)
    {
        var reporter = command.ProgressJson ? new NdjsonInstallReporter() : null;
        var installation = new Installation(string.IsNullOrWhiteSpace(command.InstallDirectory)
            ? AppContext.BaseDirectory
            : Path.GetFullPath(command.InstallDirectory));
        try
        {
            // The lock also performs exact journal/scratch recovery. Acquire it for absent and
            // partial roots too: a killed first install can have recoverable state even when the
            // product executable/state file has not reached the live directory yet.
            using var operationLock = InstallOperationLock.Acquire(installation.Root, reporter);
            PortableInstallHealing.HealMovedInstall(installation, reporter);
            var report = InspectProducts(installation, command.RetroDirectoryPath,
                cancellationToken: cancellationToken);
            Console.Out.WriteLine(command.ProgressJson
                ? SerializeProductsReport(report)
                : $"base: {report.Base.Reason} {report.Base.Detail}".TrimEnd());
            if (!command.ProgressJson)
                Console.Out.WriteLine(
                    $"retro-rewind: {report.RetroRewind.Reason} {report.RetroRewind.Detail}".TrimEnd());
            Console.Out.Flush();
            reporter?.Success(installation.Root);
            return report.RebuildRequired ? 2 : 0;
        }
        catch (Exception ex) when (reporter is not null)
        {
            reporter.Failure(ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            reporter?.EnsureFinished("The product check stopped before it reached a result.");
        }
    }

    /// <summary>
    /// Reconciles only the product work identified by a preceding health check. Every invocation
    /// carries the canonical Retro Rewind folder and one payload option; the final inspection is
    /// authoritative, so success is emitted only when every required product is current.
    /// </summary>
    public static async Task<int> RepairProducts(CommandLine command,
        CancellationToken cancellationToken = default)
    {
        // See Install() above: per-process file name so two concurrent invocations can't race to
        // open the same fixed path before either has acquired its InstallOperationLock.
        var logPath = Path.Combine(Path.GetTempPath(), $"WiiCompiled-repair-{Environment.ProcessId}.log");
        var logLock = new object();
        // A build/repair can emit thousands of log lines; keep one file handle open for the
        // duration instead of paying an open+append+close syscall round trip per line.
        using var logWriter = new StreamWriter(logPath, append: false) { AutoFlush = false };
        void Log(string message)
        {
            lock (logLock)
            {
                logWriter.Write($"[{DateTime.Now:O}] {message}{Environment.NewLine}");
                logWriter.Flush();
            }
        }

        var ndjson = command.ProgressJson ? new NdjsonInstallReporter(Log) : null;
        IInstallReporter reporter = ndjson is null ? new ConsoleInstallReporter(Log) : ndjson;
        var installDirectory = Path.GetFullPath(command.InstallDirectory!);
        var installation = new Installation(installDirectory);
        try
        {
            if (!installation.IsPresent)
                throw new InvalidOperationException(
                    $"WiiCompiled is not installed at {installDirectory}.");

            using var operationLock = InstallOperationLock.Acquire(installDirectory, reporter);
            PortableInstallHealing.HealMovedInstall(installation, reporter);

            cancellationToken.ThrowIfCancellationRequested();
            var service = new ProductRepairService(installation, reporter);
            using var scratch = InstallScratchSpace.CreateInsideInstall(installDirectory, reporter);
            // Only compile inputs are copied out of the canonical installation, under the frontend's
            // source lease. Its assets stay where they are and are read live at launch.
            var canonicalRoot = RetroRewindSource.ResolveRetroRewind6(command.RetroDirectoryPath!);
            var snapshot = CompileInputsFingerprint.Snapshot(canonicalRoot,
                Path.Combine(scratch.Root, "compile-inputs"), cancellationToken);
            var options = new ProductRepairService.ReconcileOptions
            {
                ScratchRoot = scratch.Root,
                CanonicalRetroRewindRoot = canonicalRoot
            };
            var reconciliation = await service.RepairRetroAsync(snapshot, command.RetroWfcPayloadMode,
                options, cancellationToken);
            // ReconcileAsync is the completion barrier. Cancellation is observed throughout
            // preparation and immediately before publication; a request racing with or following
            // its uncancellable Publish/Commit must not turn committed state into a failure result.
            // Reconciliation returns the exact payload-cache observation it proved or published, so
            // terminal health does not hash the same payload a second time.
            var after = InspectProducts(installation, snapshot,
                reconciliation.CachedRetroWfcPayloadMatches);
            if (after.RebuildRequired)
                throw new InvalidOperationException(
                    "Product repair completed without producing a current installation: " +
                    after.ActionRequiredDetail);

            ndjson?.Success(installDirectory);
            return 0;
        }
        catch (Exception ex) when (ndjson is not null)
        {
            ndjson.Failure(ex is OperationCanceledException ? "Operation canceled." : ex.Message);
            Console.Error.WriteLine(ex);
            return 1;
        }
        finally
        {
            ndjson?.EnsureFinished("Product repair stopped before it reached a result.");
        }
    }

    /// <summary>
    /// The <c>products</c> record exactly as the v1 contract defines it: a typed status and a
    /// human-readable detail per product, plus one aggregate. Nothing else is derived, because
    /// nothing else is reported.
    /// </summary>
    internal sealed record ProductsReport(string InstallDirectory,
        ProductState Base, ProductState RetroRewind)
    {
        public bool RebuildRequired => Base.ActionRequired || RetroRewind.ActionRequired;

        public string ActionRequiredDetail => string.Join(" ",
            new[] { Base, RetroRewind }
                .Where(state => state.ActionRequired)
                .Select(state => state.Detail)
                .Where(detail => !string.IsNullOrWhiteSpace(detail)));
    }

    /// <summary>
    /// Inspects both products against the canonical Retro Rewind installation: the caller's explicit
    /// folder when it supplied one, otherwise the recorded <c>retro_rewind_root</c>.
    /// </summary>
    internal static ProductsReport InspectProducts(Installation installation,
        string? canonicalRetroDirectory = null, bool? cachedRetroWfcPayloadMatches = null,
        CancellationToken cancellationToken = default)
    {
        if (!installation.IsPresent) return AbsentReport(installation);
        var canonical = installation.ResolveCanonicalCompileInputs(canonicalRetroDirectory,
            out var canonicalError, cancellationToken);
        return BuildReport(installation, canonical, canonicalError, cachedRetroWfcPayloadMatches);
    }

    /// <summary>
    /// Builds the report from a compile-input identity the operation already observed under its
    /// lock, so a repair does not hash the same canonical inputs at every decision boundary.
    /// </summary>
    internal static ProductsReport InspectProducts(Installation installation,
        RetroRewindCompileInputs canonical, bool? cachedRetroWfcPayloadMatches = null) =>
        installation.IsPresent
            ? BuildReport(installation, canonical, null, cachedRetroWfcPayloadMatches)
            : AbsentReport(installation);

    private static ProductsReport AbsentReport(Installation installation) =>
        // Probing a path nothing was installed to is a valid answer. It must not create a directory
        // or turn the frontend's first-install probe into a repair request.
        new(installation.Root,
            new ProductState(ProductStatus.Absent, "WiiCompiled is not installed here."),
            new ProductState(ProductStatus.Absent, "Retro Rewind is not installed."));

    private static ProductsReport BuildReport(Installation installation,
        RetroRewindCompileInputs? canonical, string? canonicalError,
        bool? cachedRetroWfcPayloadMatches)
    {
        var toolkitFingerprint = installation.ResolveToolkitFingerprint();
        return new ProductsReport(installation.Root,
            installation.CheckBase(toolkitFingerprint),
            installation.CheckRetroRewind(toolkitFingerprint, canonical, canonicalError,
                cachedRetroWfcPayloadMatches));
    }

    /// <summary>
    /// Emits the stable <c>products</c> record. The frontend branches on <c>status</c> only, never
    /// on the detail text, and an unrecognized status fails closed on its side.
    /// </summary>
    internal static string SerializeProductsReport(ProductsReport report) =>
        System.Text.Json.JsonSerializer.Serialize(new
        {
            type = "products",
            setupVersion = ProductInfo.Version,
            installDir = report.InstallDirectory,
            rebuildRequired = report.RebuildRequired,
            @base = new { status = report.Base.Reason, detail = report.Base.Detail },
            retroRewind = new { status = report.RetroRewind.Reason, detail = report.RetroRewind.Detail }
        });
}
