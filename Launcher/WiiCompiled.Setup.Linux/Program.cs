using System.Security.Cryptography;
using System.Runtime.Versioning;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Linux;

internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        // Checked anywhere in argv, not just args[0]: AppRun (Launcher/build-appimage.sh) prepends
        // --workspace <cache> ahead of whatever the caller passed, so these can't assume position 0.
        if (args.Length == 0 || args.Contains("-h") || args.Contains("--help")) { PrintUsage(); return 0; }
        if (args.Contains("--version")) { Console.WriteLine(ProductInfo.Version); return 0; }

        using var cts = new CancellationTokenSource();
        // Replaces CancellationSignal.cs's named-EventWaitHandle IPC (Windows-only): SIGINT/SIGTERM
        // are the portable, standard way for a parent (Wheel Wizard or a shell) to cancel this
        // process and the build it spawned.
        using var sigint = System.Runtime.InteropServices.PosixSignalRegistration.Create(
            System.Runtime.InteropServices.PosixSignal.SIGINT, context => { context.Cancel = true; cts.Cancel(); });
        using var sigterm = System.Runtime.InteropServices.PosixSignalRegistration.Create(
            System.Runtime.InteropServices.PosixSignal.SIGTERM, context => { context.Cancel = true; cts.Cancel(); });
        return await RunAsync(args, cts);
    }

    private static async Task<int> RunAsync(string[] args, CancellationTokenSource cts)
    {
        // AppRun (Launcher/build-appimage.sh) invokes this as `wiicompiled-setup --workspace
        // <cache> <command> [options]` - a global flag ahead of the subcommand - so the command
        // word is whichever token isn't part of a --flag/value pair, not strictly args[0].
        var (command, flags) = ParseArgs(args);
        if (command is null) { PrintUsage(); return 1; }
        var progressJson = flags.ContainsKey("progress-json");
        IInstallReporter reporter = progressJson ? new NdjsonInstallReporter() : new ConsoleInstallReporter();

        try
        {
            switch (command)
            {
                case "install":
                case "silent":
                    await InstallAsync(flags, reporter, cts.Token);
                    break;
                case "repair-products":
                    await RepairProductsAsync(flags, reporter, cts.Token);
                    break;
                case "uninstall":
                case "silent-uninstall":
                    Uninstall();
                    break;
                case "launch-base":
                case "launch_base":
                    return Launch("base", flags);
                case "launch-retro":
                case "launch_retro":
                    return Launch("retro-rewind", flags);
                case "check-products":
                case "check_products":
                    if (OperatingSystem.IsMacOS())
                        return CheckMacProducts(flags, reporter);
                    CheckProducts();
                    break;
                default:
                    Console.Error.WriteLine($"Unknown command: {command}");
                    PrintUsage();
                    return 1;
            }
            (reporter as NdjsonInstallReporter)?.Success(flags.GetValueOrDefault("install-dir") ?? "");
            return 0;
        }
        catch (OperationCanceledException)
        {
            Console.Error.WriteLine("Cancelled.");
            (reporter as NdjsonInstallReporter)?.Failure("cancelled");
            return 130;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine($"error: {ex.Message}");
            (reporter as NdjsonInstallReporter)?.Failure(ex.Message);
            return 1;
        }
    }

    private static async Task InstallAsync(Dictionary<string, string?> flags, IInstallReporter reporter, CancellationToken token)
    {
        EnsureSupportedHost();
        var retroDir = flags.GetValueOrDefault("retro-dir");
        var installsRetro = !string.IsNullOrEmpty(retroDir);
        var downloadPayload = flags.ContainsKey("download-retro-wfc-payload");
        var skipPayload = flags.ContainsKey("skip-retro-wfc-payload");
        if (installsRetro)
        {
            if (downloadPayload == skipPayload)
                throw new ArgumentException(
                    "Choose exactly one Retro-WFC mode: --download-retro-wfc-payload or --skip-retro-wfc-payload.");
        }
        else if (downloadPayload || skipPayload)
        {
            throw new ArgumentException("A Retro-WFC payload option is valid only with --retro-dir.");
        }

        // Canonicalizes to the exact RetroRewind6 folder (accepting a parent folder or a symlink),
        // the same validation Windows applies via this same shared method - local-build.sh's own
        // check further down is a simpler backstop, not the primary validation anymore.
        if (installsRetro) retroDir = RetroRewindSource.ResolveRetroRewind6(retroDir!);

        var workspace = flags.GetValueOrDefault("workspace") ?? FindWorkspace();
        var manifest = ProjectManifest.Load(Path.Combine(workspace, "projects", "mkwii", "recomp.yml"));
        var assetsDir = Path.Combine(workspace, "Assets");

        reporter.Progress(InstallStages.Validate, "Checking prerequisites", 1);
        if (flags.TryGetValue("game", out var isoPath) && !string.IsNullOrEmpty(isoPath))
        {
            await DiscTool.ValidateAndExtractAsync(isoPath, manifest, assetsDir, workspace,
                flags.GetValueOrDefault("disc-tool-bin"), reporter, token);
        }
        else
        {
            var dol = Path.Combine(assetsDir, "main.dol");
            var rel = Path.Combine(assetsDir, "StaticR.rel");
            if (!File.Exists(dol) || !File.Exists(rel))
            {
                throw new InvalidOperationException(
                    "No --game ISO was given and Assets/main.dol + Assets/StaticR.rel are not already present. " +
                    "Either pass --game <path-to-iso>, or extract them yourself first (see translator/README.md).");
            }
        }

        var state = JsonState.TryRead<InstallState>(StatePath) ?? new InstallState { Workspace = workspace };
        state.Workspace = workspace;

        var profile = installsRetro ? "both" : "base";
        var profiles = installsRetro ? new[] { "base", "retro-rewind" } : new[] { "base" };
        var baseInstallDir = installsRetro ? DefaultInstallDir("base") : null;
        var installDir = flags.GetValueOrDefault("install-dir") ?? DefaultInstallDir(installsRetro ? "retro-rewind" : "base");
        var translatorBin = flags.GetValueOrDefault("translator-bin");
        var cmakeBin = flags.GetValueOrDefault("cmake");
        var ninjaBin = flags.GetValueOrDefault("ninja");
        if (OperatingSystem.IsMacOS())
        {
            translatorBin ??= BundledTool("Translator.Cli");
            if (BundledTool("nodtool") is { } nodtool)
                flags.TryAdd("disc-tool-bin", nodtool);
        }

        string? retroWfcOfflineDir = null;
        if (downloadPayload)
        {
            var cacheDir = Path.Combine(workspace, "generated", "retro-wfc-payload");
            reporter.Progress(InstallStages.Validate,
                "Downloading the current Retro-WFC payload", 1);
            try
            {
                // A valid signature authenticates a payload, but does not prove it is the latest
                // signed revision. Always ask the fixed endpoint for the current snapshot; the
                // downloader verifies it before atomically replacing the cache.
                await RetroWfcPayload.DownloadRetroWfcPayloadAsync(
                    RetroWfcPayload.CurrentRetroWfcPayloadUri, cacheDir, token);
            }
            catch (Exception downloadFailure) when (!token.IsCancellationRequested &&
                                                     downloadFailure is HttpRequestException or TimeoutException
                                                         or IOException)
            {
                // Offline installs may continue with a previously authenticated snapshot. Do not
                // use this path for a newly downloaded payload that failed signature validation:
                // that must remain a hard failure instead of hiding possible endpoint tampering.
                try
                {
                    RetroWfcPayload.ValidateStagedRetroWfcPayloadDirectory(cacheDir);
                }
                catch (Exception cacheFailure) when (cacheFailure is IOException or
                                                     UnauthorizedAccessException or InvalidDataException)
                {
                    throw new InvalidOperationException(
                        "The current Retro-WFC payload could not be downloaded and no valid cached " +
                        $"payload is available ({cacheFailure.Message.TrimEnd('.')}).", downloadFailure);
                }

                reporter.Diagnostic(
                    "The current Retro-WFC payload could not be downloaded; using the previously " +
                    $"verified cached payload instead ({downloadFailure.Message.TrimEnd('.')}).");
            }
            retroWfcOfflineDir = cacheDir;
        }

        var sysroot = flags.GetValueOrDefault("sysroot");
        // --sysroot explicitly provided (even as bare flag at end of argv, which ParseArgs
        // stores as null) must carry a path; omitting --sysroot entirely is fine (local-build.sh
        // adds -UCMAKE_SYSROOT to clear any stale cached value from a prior configure).
        if (flags.ContainsKey("sysroot") && string.IsNullOrWhiteSpace(sysroot))
        {
            throw new ArgumentException("--sysroot requires a non-empty directory path.");
        }

        await BuildRunner.RunAsync(
            workspace, profile, installDir, baseInstallDir,
            retroDir,
            retroWfcOfflineDir,
            skipPayload,
            flags.ContainsKey("force-clean-build"),
            translatorBin,
            flags.GetValueOrDefault("cc"),
            flags.GetValueOrDefault("cxx"),
            flags.GetValueOrDefault("fuse-ld"),
            cmakeBin,
            ninjaBin,
            flags.GetValueOrDefault("native-prebuilt-dir"),
            sysroot,
            reporter, token);

        reporter.Progress(InstallStages.Shortcuts, "Creating shortcuts", 98);
        var dolSha = Sha256Of(Path.Combine(assetsDir, "main.dol"));
        var relSha = Sha256Of(Path.Combine(assetsDir, "StaticR.rel"));

        foreach (var p in profiles)
        {
            var dir = p == "base" ? (baseInstallDir ?? installDir) : installDir;
            var exeName = OperatingSystem.IsMacOS()
                ? Path.Combine(p == "base" ? "WiiCompiled.app" : "RetroRewind.app", "Contents", "MacOS",
                    p == "base" ? "WiiCompiled" : "RetroRewind")
                : p == "base" ? "WiiCompiled" : "RetroRewind";
            var displayName = p == "base" ? "WiiCompiled (base game)" : "WiiCompiled (Retro Rewind)";
            state.Products.RemoveAll(r => r.Profile == p);
            state.Products.Add(new ProductInstallRecord
            {
                Profile = p,
                InstallDirectory = dir,
                ExecutableName = exeName,
                DolSha256 = dolSha,
                RelSha256 = relSha,
                BuiltUtc = DateTime.UtcNow.ToString("O"),
                RetroRewindDirectory = p == "retro-rewind" ? retroDir : null,
                UsesRetroWfcPayload = p == "retro-rewind" && downloadPayload,
                CodePulSha256 = p == "retro-rewind" && retroDir is not null
                    ? Sha256IfExists(Path.Combine(retroDir, "Binaries", "Code.pul"))
                    : null,
            });
            if (!OperatingSystem.IsMacOS())
                DesktopEntry.Create(p, displayName, Path.Combine(dir, exeName));
        }
        JsonState.Write(StatePath, state);

        // The runtime reads course/texture/audio data live from dvd_root at every launch, not just
        // at translation time - without this the game fatally errors the instant it needs any file
        // that isn't main.dol/StaticR.rel. Linux has no --portable flag, so this is always the
        // per-user Config.toml (RuntimeConfiguration.ResolveConfigPath's Windows-only portable-root
        // lookup has nothing to find here either way).
        var configPath = OperatingSystem.IsMacOS() ? MacConfigPath : RuntimeConfiguration.ApplicationDataConfigPath;
        var dataDir = Path.Combine(assetsDir, "DATA");
        if (Directory.Exists(dataDir))
        {
            RuntimeConfiguration.SetDvdRoot(configPath, dataDir);
        }
        if (installsRetro)
        {
            RuntimeConfiguration.SetRetroRewindRoot(configPath, retroDir!);
        }

        reporter.Progress(InstallStages.Shortcuts, "Install complete", 99);
    }

    private static async Task RepairProductsAsync(
        Dictionary<string, string?> flags, IInstallReporter reporter, CancellationToken token)
    {
        var state = JsonState.TryRead<InstallState>(StatePath);
        if (state is null || state.Products.Count == 0)
            throw new InvalidOperationException("Nothing is installed to repair. Run 'install' first.");

        var retroRecord = state.Products.FirstOrDefault(record => record.Profile == "retro-rewind");
        var retro = flags.GetValueOrDefault("retro-dir") ??
                    (retroRecord is null
                        ? null
                        : retroRecord.RetroRewindDirectory ??
                          (OperatingSystem.IsMacOS() ? RuntimeConfiguration.GetRetroRewindRoot(MacConfigPath) : null));
        if (retroRecord is not null && string.IsNullOrWhiteSpace(retro))
            throw new InvalidOperationException(
                "The installed Retro Rewind source directory is not recorded. Supply --retro-dir <RetroRewind6> to repair it.");
        if (retro is not null)
        {
            flags["retro-dir"] = retro;
            if (!flags.ContainsKey("download-retro-wfc-payload") &&
                !flags.ContainsKey("skip-retro-wfc-payload"))
                flags[retroRecord?.UsesRetroWfcPayload == true
                    ? "download-retro-wfc-payload"
                    : "skip-retro-wfc-payload"] = null;
        }
        flags["install-dir"] = flags.GetValueOrDefault("install-dir") ??
                               state.Products.FirstOrDefault(record => record.Profile == "retro-rewind")?.InstallDirectory ??
                               state.Products[0].InstallDirectory;
        await InstallAsync(flags, reporter, token);
    }

    private static void Uninstall()
    {
        // Remove the installed products but keep the build workspace and user-owned disc data.
        var state = JsonState.TryRead<InstallState>(StatePath) ?? new InstallState();
        foreach (var record in state.Products.ToList())
        {
            if (OperatingSystem.IsMacOS())
            {
                var appBundle = Path.Combine(record.InstallDirectory,
                    record.Profile == "base" ? "WiiCompiled.app" : "RetroRewind.app");
                if (appBundle is not null && Directory.Exists(appBundle))
                    Directory.Delete(appBundle, recursive: true);
            }
            else
            {
                if (Directory.Exists(record.InstallDirectory))
                    Directory.Delete(record.InstallDirectory, recursive: true);
                DesktopEntry.Remove(record.Profile);
            }
            state.Products.Remove(record);
            Console.WriteLine($"Removed {record.Profile} from {record.InstallDirectory}");
        }
        JsonState.Write(StatePath, state);
    }

    private static int Launch(string profile, Dictionary<string, string?> flags)
    {
        var state = JsonState.TryRead<InstallState>(StatePath);
        var record = state?.Products.FirstOrDefault(r => r.Profile == profile);
        if (record is null)
        {
            var installHint = profile == "retro-rewind"
                ? "install --retro-dir <RetroRewind6> {--download-retro-wfc-payload | --skip-retro-wfc-payload}"
                : "install --game <RMCP01 ISO>";
            Console.Error.WriteLine($"{profile} is not installed. Run '{installHint}' first.");
            return 1;
        }
        var exePath = Path.Combine(record.InstallDirectory, record.ExecutableName);
        if (!File.Exists(exePath))
        {
            Console.Error.WriteLine($"Installed executable is missing: {exePath}. Run 'repair-products' to rebuild it.");
            return 1;
        }
        var startInfo = new System.Diagnostics.ProcessStartInfo(exePath)
        {
            WorkingDirectory = record.InstallDirectory,
            UseShellExecute = false,
        };
        using var process = System.Diagnostics.Process.Start(startInfo);
        process?.WaitForExit();
        return process?.ExitCode ?? 1;
    }

    private static void CheckProducts()
    {
        var state = JsonState.TryRead<InstallState>(StatePath);
        if (state is null || state.Products.Count == 0)
        {
            Console.WriteLine("Nothing installed.");
            return;
        }
        var assetsDir = Path.Combine(state.Workspace, "Assets");
        var currentDol = Sha256IfExists(Path.Combine(assetsDir, "main.dol"));
        var currentRel = Sha256IfExists(Path.Combine(assetsDir, "StaticR.rel"));
        foreach (var record in state.Products)
        {
            var exePath = Path.Combine(record.InstallDirectory, record.ExecutableName);
            var present = File.Exists(exePath);
            var stale = present && (currentDol != record.DolSha256 || currentRel != record.RelSha256);
            var status = !present ? "MISSING" : stale ? "STALE (game assets changed since last build)" : "current";
            Console.WriteLine($"{record.Profile,-14} {status,-45} {record.InstallDirectory}");
        }
    }

    private static int CheckMacProducts(Dictionary<string, string?> flags, IInstallReporter reporter)
    {
        var state = JsonState.TryRead<InstallState>(StatePath) ?? new InstallState();
        var assetsDirectory = string.IsNullOrWhiteSpace(state.Workspace)
            ? ""
            : Path.Combine(state.Workspace, "Assets");
        var currentDol = string.IsNullOrEmpty(assetsDirectory)
            ? null
            : Sha256IfExists(Path.Combine(assetsDirectory, "main.dol"));
        var currentRel = string.IsNullOrEmpty(assetsDirectory)
            ? null
            : Sha256IfExists(Path.Combine(assetsDirectory, "StaticR.rel"));
        var baseState = GetMacProductState(state, "base", currentDol, currentRel);
        var retroState = GetMacProductState(state, "retro-rewind", currentDol, currentRel);
        var rebuildRequired = NeedsMacRepair(baseState.Status) || NeedsMacRepair(retroState.Status);
        var installDirectory = Path.GetFullPath(flags.GetValueOrDefault("install-dir") ??
                                                DefaultInstallDir("base"));

        Console.Out.WriteLine(System.Text.Json.JsonSerializer.Serialize(new
        {
            type = "products",
            setupVersion = ProductInfo.Version,
            installDir = installDirectory,
            rebuildRequired,
            @base = baseState,
            retroRewind = retroState,
        }, new System.Text.Json.JsonSerializerOptions
        {
            PropertyNamingPolicy = System.Text.Json.JsonNamingPolicy.CamelCase,
        }));
        Console.Out.Flush();
        (reporter as NdjsonInstallReporter)?.Success(installDirectory);
        return rebuildRequired ? 2 : 0;
    }

    private static MacProductStatus GetMacProductState(
        InstallState state, string profile, string? currentDol, string? currentRel)
    {
        var record = state.Products.FirstOrDefault(product => product.Profile == profile);
        if (record is null)
            return new MacProductStatus("absent", profile == "base"
                ? "WiiCompiled is not installed here."
                : "Retro Rewind is not installed.");

        var executable = Path.Combine(record.InstallDirectory, record.ExecutableName);
        if (!File.Exists(executable))
            return new MacProductStatus("broken", $"Installed executable is missing: {executable}");
        if (currentDol is null || currentRel is null)
            return new MacProductStatus("inputs-missing", "The workspace game inputs are missing.");
        if (currentDol != record.DolSha256 || currentRel != record.RelSha256)
            return new MacProductStatus("compile-inputs-changed",
                "The local game inputs changed since this product was built.");
        if (profile == "retro-rewind")
        {
            var codePulPath = record.RetroRewindDirectory is null
                ? null
                : Path.Combine(record.RetroRewindDirectory, "Binaries", "Code.pul");
            var codePulSha = codePulPath is null ? null : Sha256IfExists(codePulPath);
            if (codePulSha is null)
                return new MacProductStatus("inputs-missing", "The recorded Retro Rewind Code.pul is missing.");
            if (record.CodePulSha256 is null || codePulSha != record.CodePulSha256)
                return new MacProductStatus("code-pul-changed",
                    "Retro Rewind Code.pul changed since this product was built.");
        }
        return new MacProductStatus("current", "");
    }

    private static bool NeedsMacRepair(string status) => status is not "current" and not "absent";

    private sealed record MacProductStatus(string Status, string Detail);

    private static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static string? Sha256IfExists(string path) => File.Exists(path) ? Sha256Of(path) : null;

    private static string StatePath => Path.Combine(
        OperatingSystem.IsMacOS() ? MacApplicationDataDirectory :
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),
        OperatingSystem.IsMacOS() ? "install-state.json" : Path.Combine("WiiCompiled", "install-state.json"));

    private static string DefaultInstallDir(string profile) => Path.Combine(
        OperatingSystem.IsMacOS() ? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Applications", "WiiCompiled") :
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WiiCompiled", "Install"),
        profile == "base" ? "Base" : "RetroRewind");

    private static string MacApplicationDataDirectory => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.UserProfile), "Library", "Application Support", "WiiCompiled");
    private static string MacConfigPath => Path.Combine(MacApplicationDataDirectory, "Config.toml");

    private static string? BundledTool(string relativePath)
    {
        var path = Path.Combine(AppContext.BaseDirectory, "tools", relativePath);
        return File.Exists(path) ? path : null;
    }

    private static void EnsureSupportedHost()
    {
        if (!OperatingSystem.IsMacOS()) return;
        if (System.Runtime.InteropServices.RuntimeInformation.OSArchitecture !=
            System.Runtime.InteropServices.Architecture.Arm64)
            throw new PlatformNotSupportedException("The macOS setup requires Apple Silicon (arm64).");
        if (!OperatingSystem.IsMacOSVersionAtLeast(14))
            throw new PlatformNotSupportedException("The macOS setup requires macOS 14 (Sonoma) or later.");
    }

    private static string FindWorkspace()
    {
        var marker = OperatingSystem.IsMacOS() ? "local-build-macos.command" : "local-build.sh";
        var current = new DirectoryInfo(AppContext.BaseDirectory);
        for (var level = 0; level <= 8 && current is not null; level++, current = current.Parent)
        {
            var packagedWorkspace = Path.Combine(current.FullName, "workspace");
            if (File.Exists(Path.Combine(packagedWorkspace, "Launcher", marker)))
                return OperatingSystem.IsMacOS()
                    ? PrepareMacWorkspace(packagedWorkspace)
                    : packagedWorkspace;
            if (File.Exists(Path.Combine(current.FullName, "Launcher", marker)))
                return current.FullName;
        }
        throw new InvalidOperationException($"Could not find Launcher/{marker}. Pass --workspace <path-to-checkout> explicitly.");
    }

    [SupportedOSPlatform("macos")]
    private static string PrepareMacWorkspace(string packagedWorkspace)
    {
        var versionFile = Path.Combine(packagedWorkspace, ".setup-source-version");
        if (!File.Exists(versionFile))
            throw new InvalidDataException("The packaged macOS setup is missing its source version marker.");

        var persistentWorkspace = Path.Combine(MacApplicationDataDirectory, "BuildWorkspace");
        var persistentVersionFile = Path.Combine(persistentWorkspace, ".setup-source-version");
        var version = File.ReadAllText(versionFile).Trim();
        if (File.Exists(persistentVersionFile) &&
            string.Equals(File.ReadAllText(persistentVersionFile).Trim(), version, StringComparison.Ordinal) &&
            File.Exists(Path.Combine(persistentWorkspace, "Launcher", "local-build-macos.command")))
            return persistentWorkspace;

        Directory.CreateDirectory(persistentWorkspace);
        foreach (var directory in new[] { "projects", "runtime", "aurora-main" })
            ReplacePackagedDirectory(Path.Combine(packagedWorkspace, directory),
                Path.Combine(persistentWorkspace, directory));

        var sourceLauncher = Path.Combine(packagedWorkspace, "Launcher");
        var targetLauncher = Path.Combine(persistentWorkspace, "Launcher");
        Directory.CreateDirectory(Path.Combine(targetLauncher, "macos"));
        foreach (var script in new[]
                 {
                     "local-build-macos.command",
                     Path.Combine("macos", "extract-disc.command"),
                     Path.Combine("macos", "publish-app.command"),
                 })
        {
            var source = Path.Combine(sourceLauncher, script);
            var target = Path.Combine(targetLauncher, script);
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(source, target, overwrite: true);
            File.SetUnixFileMode(target, UnixFileMode.UserRead | UnixFileMode.UserWrite |
                                          UnixFileMode.UserExecute | UnixFileMode.GroupRead |
                                          UnixFileMode.GroupExecute | UnixFileMode.OtherRead |
                                          UnixFileMode.OtherExecute);
        }

        foreach (var name in new[] { "LICENSE", "THIRD-PARTY-NOTICES.md" })
            File.Copy(Path.Combine(packagedWorkspace, name),
                Path.Combine(persistentWorkspace, name), overwrite: true);

        File.WriteAllText(persistentVersionFile, version);
        return persistentWorkspace;
    }

    [SupportedOSPlatform("macos")]
    private static void ReplacePackagedDirectory(string source, string destination)
    {
        if (!Directory.Exists(source))
            throw new DirectoryNotFoundException($"The packaged setup source directory is missing: {source}");
        if (Directory.Exists(destination))
            Directory.Delete(destination, recursive: true);
        CopyPackagedDirectory(source, destination);
    }

    [SupportedOSPlatform("macos")]
    private static void CopyPackagedDirectory(string source, string destination)
    {
        var sourceInfo = new DirectoryInfo(source);
        if (sourceInfo.LinkTarget is { } linkTarget)
        {
            Directory.CreateSymbolicLink(destination, linkTarget);
            return;
        }

        Directory.CreateDirectory(destination);
        foreach (var entry in sourceInfo.EnumerateFileSystemInfos())
        {
            var target = Path.Combine(destination, entry.Name);
            if (entry is DirectoryInfo directory)
            {
                if (directory.LinkTarget is { } directoryTarget)
                    Directory.CreateSymbolicLink(target, directoryTarget);
                else
                    CopyPackagedDirectory(directory.FullName, target);
            }
            else if (entry is FileInfo file)
            {
                if (file.LinkTarget is { } fileTarget)
                    File.CreateSymbolicLink(target, fileTarget);
                else
                {
                    File.Copy(file.FullName, target);
                    File.SetUnixFileMode(target, File.GetUnixFileMode(file.FullName));
                }
            }
        }
    }

    /// <summary>
    /// A single pass that finds both the command word and every --flag[=value] pair, regardless
    /// of order - a --flag may appear before or after the command (see the AppRun caller note in
    /// RunAsync). The first token that is neither a --flag nor a value already consumed by the
    /// preceding --flag is taken as the command.
    /// </summary>
    private static (string? Command, Dictionary<string, string?> Flags) ParseArgs(string[] args)
    {
        string? command = null;
        var flags = new Dictionary<string, string?>();
        static string? CommandForOption(string option) => option switch
        {
            "--install" or "--silent" => "install",
            "--uninstall" or "--silent-uninstall" => "uninstall",
            "--launch-base" or "--launch_base" => "launch-base",
            "--launch-retro" or "--launch_retro" => "launch-retro",
            "--check-products" or "--check_products" => "check-products",
            "--repair-products" => "repair-products",
            _ => null,
        };

        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (arg.StartsWith("--", StringComparison.Ordinal))
            {
                var name = arg[2..];
                var optionCommand = CommandForOption(arg);
                if (optionCommand is not null)
                {
                    if (command is not null)
                        throw new ArgumentException($"Only one command may be specified (already selected '{command}').");
                    command = optionCommand;
                    continue;
                }
                if (i + 1 < args.Length && !args[i + 1].StartsWith("--", StringComparison.Ordinal))
                {
                    flags[name] = args[++i];
                }
                else
                {
                    flags[name] = null; // boolean flag
                }
            }
            else if (command is null)
            {
                command = arg;
            }
        }
        return (command, flags);
    }

    private static void PrintUsage()
    {
        Console.WriteLine("""
        Usage: wiicompiled-setup <command> [options]

          install [--game ISO_PATH] [--install-dir DIR] [--retro-dir DIR
                  {--download-retro-wfc-payload | --skip-retro-wfc-payload}]
                  [--force-clean-build] [--translator-bin PATH] [--disc-tool-bin PATH]
                  [--cc PATH] [--cxx PATH] [--fuse-ld NAME_OR_PATH] [--cmake PATH] [--ninja PATH]
                  [--native-prebuilt-dir DIR] [--sysroot PATH] [--progress-json] [--workspace DIR]
          uninstall
          launch-base
          launch-retro
          check-products
          repair-products
          --version
        """);
    }
}
