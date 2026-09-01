using System.Security.Cryptography;
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
                    await InstallAsync(flags, reporter, cts.Token);
                    break;
                case "uninstall":
                    Uninstall();
                    break;
                case "launch-base":
                    return Launch("base", flags);
                case "launch-retro":
                    return Launch("retro-rewind", flags);
                case "check-products":
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

        var workspace = flags.GetValueOrDefault("workspace") ?? WorkspaceLocator.FindFrom(AppContext.BaseDirectory);
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

        string? retroWfcOfflineDir = null;
        if (downloadPayload)
        {
            // Reused if a previous install already downloaded and it's still valid - matches
            // Windows's own reuse-if-valid behavior instead of re-downloading on every install.
            var cacheDir = Path.Combine(workspace, "generated", "retro-wfc-payload");
            reporter.Progress(InstallStages.Validate, "Preparing the Retro-WFC payload", 1);
            try
            {
                RetroWfcPayload.ValidateStagedRetroWfcPayloadDirectory(cacheDir);
            }
            catch (InvalidDataException)
            {
                await RetroWfcPayload.DownloadRetroWfcPayloadAsync(
                    RetroWfcPayload.CurrentRetroWfcPayloadUri, cacheDir, token);
            }
            retroWfcOfflineDir = cacheDir;
        }

        await BuildRunner.RunAsync(
            workspace, profile, installDir, baseInstallDir,
            retroDir,
            retroWfcOfflineDir,
            skipPayload,
            flags.ContainsKey("force-clean-build"),
            flags.GetValueOrDefault("translator-bin"),
            flags.GetValueOrDefault("cc"),
            flags.GetValueOrDefault("cxx"),
            flags.GetValueOrDefault("fuse-ld"),
            flags.GetValueOrDefault("cmake"),
            flags.GetValueOrDefault("ninja"),
            reporter, token);

        reporter.Progress(InstallStages.Shortcuts, "Creating shortcuts", 98);
        var dolSha = Sha256Of(Path.Combine(assetsDir, "main.dol"));
        var relSha = Sha256Of(Path.Combine(assetsDir, "StaticR.rel"));

        foreach (var p in profiles)
        {
            var dir = p == "base" ? (baseInstallDir ?? installDir) : installDir;
            var exeName = p == "base" ? "WiiCompiled" : "RetroRewind";
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
            });
            DesktopEntry.Create(p, displayName, Path.Combine(dir, exeName));
        }
        JsonState.Write(StatePath, state);

        // The runtime reads course/texture/audio data live from dvd_root at every launch, not just
        // at translation time - without this the game fatally errors the instant it needs any file
        // that isn't main.dol/StaticR.rel. Linux has no --portable flag, so this is always the
        // per-user Config.toml (RuntimeConfiguration.ResolveConfigPath's Windows-only portable-root
        // lookup has nothing to find here either way).
        var configPath = RuntimeConfiguration.ApplicationDataConfigPath;
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

    private static void Uninstall()
    {
        // Matches Windows: UninstallService.cs removes the whole install directory unconditionally -
        // there is no partial-product uninstall on either platform.
        var state = JsonState.TryRead<InstallState>(StatePath) ?? new InstallState();
        foreach (var record in state.Products.ToList())
        {
            if (Directory.Exists(record.InstallDirectory))
            {
                Directory.Delete(record.InstallDirectory, recursive: true);
            }
            DesktopEntry.Remove(record.Profile);
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
                : $"install --profile {profile}";
            Console.Error.WriteLine($"{profile} is not installed. Run '{installHint}' first.");
            return 1;
        }
        var exePath = Path.Combine(record.InstallDirectory, record.ExecutableName);
        if (!File.Exists(exePath))
        {
            Console.Error.WriteLine($"Installed executable is missing: {exePath}. Run 'install --profile {profile}' again.");
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

    private static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static string? Sha256IfExists(string path) => File.Exists(path) ? Sha256Of(path) : null;

    private static string StatePath => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WiiCompiled", "install-state.json");

    private static string DefaultInstallDir(string profile) => Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "WiiCompiled", "Install",
        profile == "base" ? "Base" : "RetroRewind");

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
        for (var i = 0; i < args.Length; i++)
        {
            var arg = args[i];
            if (arg.StartsWith("--", StringComparison.Ordinal))
            {
                var name = arg[2..];
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
                  [--progress-json] [--workspace DIR]
          uninstall
          launch-base
          launch-retro
          check-products
          --version
        """);
    }
}
