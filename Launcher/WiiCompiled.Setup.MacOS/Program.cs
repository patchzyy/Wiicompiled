using System.Diagnostics;
using System.Reflection;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text.Json;
using WiiCompiled.Setup.Common;
using WiiCompiled.Setup.Windows;

return await MacSetup.RunAsync(args);

internal static class MacSetup
{
    internal static readonly string Version = typeof(MacSetup).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()!
        .InformationalVersion.Split('+')[0];
    internal static readonly JsonSerializerOptions Json = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase, PropertyNameCaseInsensitive = true };
    static CancellationToken cancellation;
    static readonly string Resources = AppContext.BaseDirectory.TrimEnd(Path.DirectorySeparatorChar);
    static void Emit(object value) => Console.WriteLine(JsonSerializer.Serialize(value, Json));
    static int lastPercent;
    static void Progress(string message, int percent = 10)
    {
        lastPercent = Math.Max(lastPercent, Math.Clamp(percent, 0, 100));
        Emit(new { type = "progress", stage = "build", message, percent = lastPercent });
    }
    internal static string Hash(string path) { using var file = File.OpenRead(path); return Convert.ToHexString(SHA256.HashData(file)); }
    static string StatePath(string install) => Path.Combine(install, "install-state.json");
    static State? ReadState(string install) => File.Exists(StatePath(install)) ? JsonSerializer.Deserialize<State>(File.ReadAllText(StatePath(install)), Json) : null;
    static string Executable(string install, string product) => Path.Combine(install, product + ".app", "Contents", "MacOS", product);

    public static async Task<int> RunAsync(string[] args)
    {
        string? install = null;
        using var cancelSource = new CancellationTokenSource();
        cancellation = cancelSource.Token;
        var cancellationFile = Environment.GetEnvironmentVariable("MKWCOMPILED_CANCEL_FILE");
        var monitor = Task.Run(async () =>
        {
            try
            {
                while (!cancelSource.IsCancellationRequested)
                {
                    if (cancellationFile is not null && File.Exists(cancellationFile)) { cancelSource.Cancel(); break; }
                    await Task.Delay(100, cancelSource.Token);
                }
            }
            catch (OperationCanceledException) { }
        });
        try
        {
            var options = Options.Parse(args);
            if (options.Mode == "--version") { Console.WriteLine(Version); return 0; }
            if (!OperatingSystem.IsMacOSVersionAtLeast(14) || RuntimeInformation.ProcessArchitecture != Architecture.Arm64)
                throw new InvalidOperationException("WiiCompiled requires macOS 14 or later and a native Apple Silicon process.");
            install = Path.GetFullPath(options.Install ?? (options.Mode.StartsWith("--launch-") ? Path.Combine(Resources, "..") : throw new ArgumentException("--install-dir is required.")));
            FileSystemUtilities.EnsureUsableLocation(install, "The installation directory");
            var root = Path.GetDirectoryName(install)!;
            // Constrain ownership: the backend only publishes into the portable Install directory.
            if (Path.GetFileName(install) != "Install") throw new ArgumentException("The Mac backend requires a portable root's Install directory.");
            Directory.CreateDirectory(root);
            using var operationLock = new FileStream(Path.Combine(root, ".mkwc-operation-macos.lock"), FileMode.OpenOrCreate, FileAccess.ReadWrite, FileShare.None);
            Recover(install);
            var state = ReadState(install);
            if (options.Mode == "--check-products")
            {
                var report = Check(install, state, options.Retro);
                Emit(new { type = "products", setupVersion = Version, installDir = install, rebuildRequired = report.NeedsRepair, @base = report.Base, retroRewind = report.Retro });
                return report.NeedsRepair ? 2 : 0;
            }
            if (options.Mode.StartsWith("--launch-"))
            {
                var report = Check(install, state, state?.RetroRoot);
                if (report.NeedsRepair) throw new InvalidOperationException("The installed products need repair before launch.");
                var product = options.Mode == "--launch-retro" ? "RetroRewind" : "WiiCompiled";
                var binary = Executable(install, product);
                if (!File.Exists(binary)) throw new FileNotFoundException("The requested game is not installed.", binary);
                // Run the Mach-O directly and wait for it, retaining the installation lock for the play session.
                return await Run(binary, [], Path.GetDirectoryName(binary), game: true);
            }
            if (options.Mode == "--repair-products" && state is null) throw new InvalidOperationException("No installed state is available for repair.");
            if (options.Mode == "--silent" && !options.Portable) throw new ArgumentException("Mac installation requires --portable.");
            var retro = options.Retro is null ? null : RetroRewindSource.ResolveRetroRewind6(options.Retro);
            if (retro is not null && options.Skip == options.Download) throw new ArgumentException("Choose exactly one Retro-WFC payload option with --retro-dir.");
            await Install(install, root, state, options, retro);
            Emit(new { type = "result", success = true, version = Version, installDir = install });
            return 0;
        }
        catch (Exception ex)
        {
            Console.Error.WriteLine(ex);
            Emit(new { type = "result", success = false, error = ex.Message, installDir = install });
            return 1;
        }
        finally { cancelSource.Cancel(); await monitor; }
    }

    internal static string HashTree(string directory)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        foreach (var file in Directory.EnumerateFiles(directory, "*", SearchOption.AllDirectories).Order(StringComparer.Ordinal))
        {
            cancellation.ThrowIfCancellationRequested();
            hash.AppendData(System.Text.Encoding.UTF8.GetBytes(Path.GetRelativePath(directory, file) + "\0" + Hash(file) + "\n"));
        }
        return Convert.ToHexString(hash.GetHashAndReset());
    }
    internal static string ToolkitHash() => HashTree(Resources);

    internal static Report Check(string install, State? state, string? retro)
    {
        if (state is null || state.SchemaVersion != 1 || state.SetupVersion != Version || state.InstallDir != install)
            return new(new("toolkit-changed", "The installation identity does not match this setup."), new("toolkit-changed", "Install the current setup."));
        if (state.ToolkitHash != ToolkitHash())
            return new(new("toolkit-changed", "The installed source or build tools changed."), new("toolkit-changed", "Repair using a verified setup."));
        Product CheckProduct(string name, string? expected)
        {
            var binary = Executable(install, name);
            if (!File.Exists(binary) || expected is null || HashTree(Path.Combine(install, name + ".app")) != expected)
                return new("broken", $"{name} is missing or changed.");
            foreach (var asset in new[] { "dsp_coef.bin", "initial_pipeline_cache.db", "wii_bootstrap" })
            {
                var path = Path.Combine(install, name + ".app", "Contents", "Resources", asset);
                if (!File.Exists(path) && !Directory.Exists(path)) return new("broken", $"Missing runtime asset: {asset}");
            }
            return new("current", "The installed product matches its inputs.");
        }
        var baseStatus = CheckProduct("WiiCompiled", state.BaseHash);
        var config = RuntimeConfiguration.ResolveConfigPath(install);
        var dvd = RuntimeConfiguration.GetResolvedPath(config, "dvd_root");
        if (dvd is null || !Directory.Exists(dvd)) baseStatus = new("inputs-missing", "The extracted game data is missing.");
        Product retroStatus;
        if (!state.RetroRewindInstalled) retroStatus = new("absent", "Retro Rewind has not been built.");
        else
        {
            retroStatus = CheckProduct("RetroRewind", state.RetroHash);
            try
            {
                var inputs = CompileInputsFingerprint.Compute(retro ?? state.RetroRoot ?? "");
                if (inputs.CompileInputsSha256 != state.CompileHash) retroStatus = new("compile-inputs-changed", "Retro Rewind compile inputs changed.");
                else if (RuntimeConfiguration.GetRetroRewindRoot(config) != inputs.RetroRewindRoot)
                    retroStatus = new("inputs-missing", "The Retro Rewind asset path needs repair.");
            }
            catch (Exception ex) { retroStatus = new("inputs-missing", ex.Message); }
        }
        return new(baseStatus, retroStatus);
    }

    static async Task Install(string install, string root, State? previous, Options options, string? retro)
    {
        await RunChecked("/usr/bin/xcode-select", ["-p"]);
        var cache = Path.Combine(root, "Cache");
        var workspace = Path.Combine(cache, "BuildWorkspace");
        var source = Path.Combine(Resources, "workspace");
        var tools = Path.Combine(Resources, "tools");
        if (!File.Exists(Path.Combine(source, "projects/mkwii/recomp.yml"))) throw new FileNotFoundException("The bundled source workspace is missing.");
        Directory.CreateDirectory(cache);
        // Versioned source is replaced on every operation; generated output and verified disc data remain local.
        Progress("Preparing the native Apple Silicon build", 5);
        Directory.CreateDirectory(workspace);
        foreach (var item in new[] { "aurora-main", "projects", "runtime", "translator", "Launcher" })
        {
            var destination = Path.Combine(workspace, item);
            if (Directory.Exists(destination)) Directory.Delete(destination, true);
            await RunChecked("/usr/bin/ditto", [Path.Combine(source, item), destination]);
        }
        var game = options.Game ?? previous?.GamePath;
        if (string.IsNullOrWhiteSpace(game) || !File.Exists(game)) throw new FileNotFoundException("Select your clean PAL Mario Kart Wii disc image in WheelWizard settings.");
        var assets = Path.Combine(workspace, "Assets");
        // Never reuse an unverified or different disc extraction.
        var discId = $"{Path.GetFullPath(game)}|{new FileInfo(game).Length}|{File.GetLastWriteTimeUtc(game).Ticks}";
        var discMarker = Path.Combine(cache, "disc-source.txt");
        var verified = File.Exists(Path.Combine(assets, "main.dol")) && File.Exists(Path.Combine(assets, "StaticR.rel"))
            && Hash(Path.Combine(assets, "main.dol")).Equals("80d18895b39c63bd80f457398bfcbb91b7d16ac116a41a88967e954080155b05", StringComparison.OrdinalIgnoreCase)
            && Hash(Path.Combine(assets, "StaticR.rel")).Equals("16d9d146112541fefea701ecb5bc1a496f9d50e4a752fbb5b6778e7c6399f67d", StringComparison.OrdinalIgnoreCase)
            && File.Exists(discMarker) && File.ReadAllText(discMarker) == discId;
        if (!verified)
        {
            await RunChecked("/bin/bash", [Path.Combine(workspace, "Launcher/macos/extract-disc.command"), "--game", game, "--assets-dir", assets, "--nodtool", Path.Combine(tools, "nodtool")]);
            File.WriteAllText(discMarker, discId);
        }
        var payload = Path.Combine(cache, "RetroWfcPayload");
        if (retro is not null && options.Download)
        {
            Progress("Preparing verified online-play support", 15);
            try { await RetroWfcPayload.DownloadRetroWfcPayloadAsync(RetroWfcPayload.CurrentRetroWfcPayloadUri, payload, cancellation); }
            catch (Exception) when (!cancellation.IsCancellationRequested) { RetroWfcPayload.ValidateStagedRetroWfcPayloadDirectory(payload); }
        }
        var staging = install + ".staging";
        if (Directory.Exists(staging)) Directory.Delete(staging, true);
        Directory.CreateDirectory(staging);
        var snapshotDir = Path.Combine(cache, "CompileSnapshot");
        if (Directory.Exists(snapshotDir)) Directory.Delete(snapshotDir, true);
        var inputs = retro is null ? null : CompileInputsFingerprint.Snapshot(retro, snapshotDir);
        var arguments = new List<string> { Path.Combine(workspace, "Launcher/local-build-macos.command"), "--workspace", workspace,
            "--profile", retro is null ? "base" : "both", "--output-dir", staging,
            "--cmake", Path.Combine(tools, "cmake/bin/cmake"), "--ninja", Path.Combine(tools, "ninja"), "--translator-bin", Path.Combine(tools, "Translator.Cli") };
        if (retro is not null)
        {
            arguments.AddRange(["--base-output-dir", staging, "--retro-rewind-package-dir", inputs!.RetroRewindRoot]);
            if (options.Skip) arguments.Add("--skip-retro-wfc-payload");
            else arguments.AddRange(["--retro-wfc-offline-dir", payload]);
        }
        Progress("Compiling native game apps. The first build can take several minutes.", 20);
        await RunChecked("/bin/bash", arguments);
        if (retro is not null && CompileInputsFingerprint.Compute(retro).CompileInputsSha256 != inputs!.CompileInputsSha256)
            throw new IOException("Retro Rewind changed during compilation. Retry after its update finishes.");
        // The installed helper owns its source and tools, so repair and launch work without the original download.
        await RunChecked("/usr/bin/ditto", [Resources, Path.Combine(staging, "Setup")]);
        File.WriteAllText(Path.Combine(staging, "WiiCompiled-Setup.run"), "#!/bin/bash\nexec \"$(cd \"$(dirname \"$0\")\" && pwd)/Setup/WiiCompiled.Setup.MacOS\" \"$@\"\n");
        var state = new State { SchemaVersion = 1, SetupVersion = Version, InstallDir = install, GamePath = game,
            RetroRewindInstalled = retro is not null, RetroWfcPayloadMode = retro is null ? null : options.Skip ? "skipped" : "downloaded",
            RetroRoot = retro, CompileHash = inputs?.CompileInputsSha256, ToolkitHash = ToolkitHash(), BaseHash = HashTree(Path.Combine(staging, "WiiCompiled.app")),
            RetroHash = retro is null ? null : HashTree(Path.Combine(staging, "RetroRewind.app")) };
        File.WriteAllText(StatePath(staging), JsonSerializer.Serialize(state, Json));
        cancellation.ThrowIfCancellationRequested();
        PortableRoot.Create(root);
        var config = RuntimeConfiguration.ResolveConfigPath(install);
        var oldConfig = RuntimeConfiguration.Capture(config);
        // Store the config snapshot before moving anything, allowing the next invocation to recover after a killed process.
        File.WriteAllText(install + ".config-backup", JsonSerializer.Serialize(oldConfig, Json));
        var backup = install + ".previous";
        try
        {
            if (Directory.Exists(install)) Directory.Move(install, backup);
            RuntimeConfiguration.SetDvdRoot(config, Path.Combine(assets, "DATA"));
            if (retro is not null) RuntimeConfiguration.SetRetroRewindRoot(config, retro);
            Directory.Move(staging, install);
            File.Delete(install + ".config-backup"); // commit point
        }
        catch { Recover(install); throw; }
        if (Directory.Exists(backup)) Directory.Delete(backup, true);
        Progress("Native installation ready", 100);
    }

    internal static void Recover(string install)
    {
        var journal = install + ".config-backup";
        var backup = install + ".previous";
        if (File.Exists(journal))
        {
            var snapshot = JsonSerializer.Deserialize<RuntimeConfigSnapshot>(File.ReadAllText(journal), Json)!;
            RuntimeConfiguration.Restore(RuntimeConfiguration.ResolveConfigPath(install), snapshot);
            if (Directory.Exists(backup))
            {
                if (Directory.Exists(install)) Directory.Delete(install, true);
                Directory.Move(backup, install);
            }
            else if (Directory.Exists(install) && !Directory.Exists(install + ".staging")) Directory.Delete(install, true);
            File.Delete(journal);
        }
        else if (Directory.Exists(backup)) Directory.Delete(backup, true);
    }

    static async Task RunChecked(string executable, IEnumerable<string> arguments)
    {
        var code = await Run(executable, arguments);
        if (code != 0) throw new InvalidOperationException($"{Path.GetFileName(executable)} failed (exit {code}). See the WiiCompiled setup log for details.");
    }
    static async Task<int> Run(string executable, IEnumerable<string> arguments, string? cwd = null, bool game = false)
    {
        var start = new ProcessStartInfo(executable) { UseShellExecute = false, RedirectStandardOutput = true, RedirectStandardError = true, WorkingDirectory = cwd ?? Resources };
        foreach (var argument in arguments) start.ArgumentList.Add(argument);
        // Finder has a minimal PATH; all packaged tools are absolute and Apple's compiler tools live here.
        start.Environment["PATH"] = "/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin";
        cancellation.ThrowIfCancellationRequested();
        using var process = Process.Start(start) ?? throw new IOException($"Could not start {executable}.");
        async Task Drain(StreamReader reader)
        {
            while (await reader.ReadLineAsync() is { } line)
            {
                Console.Error.WriteLine(line);
                if (!game && line.StartsWith("MKWCBUILD:STEP:")) Progress(line[15..], 35);
            }
        }
        var readers = Task.WhenAll(Drain(process.StandardOutput), Drain(process.StandardError));
        try { await process.WaitForExitAsync(cancellation); }
        catch (OperationCanceledException)
        {
            if (!process.HasExited) process.Kill(entireProcessTree: true);
            await process.WaitForExitAsync();
            await readers;
            throw;
        }
        await readers;
        return process.ExitCode;
    }
}

internal sealed class State
{
    public int SchemaVersion { get; set; }
    public string? SetupVersion { get; set; }
    public string? InstallDir { get; set; }
    public string? GamePath { get; set; }
    public bool RetroRewindInstalled { get; set; }
    public string? RetroWfcPayloadMode { get; set; }
    public string? RetroRoot { get; set; }
    public string? CompileHash { get; set; }
    public string? ToolkitHash { get; set; }
    public string? BaseHash { get; set; }
    public string? RetroHash { get; set; }
}
internal sealed record Product(string Status, string Detail) { public bool NeedsRepair => Status is not ("current" or "absent"); }
internal sealed record Report(Product Base, Product Retro) { public bool NeedsRepair => Base.NeedsRepair || Retro.NeedsRepair; }
internal sealed record Options(string Mode, string? Install, string? Game, string? Retro, bool Portable, bool Download, bool Skip)
{
    internal static Options Parse(string[] args)
    {
        string? mode = null, install = null, game = null, retro = null;
        bool portable = false, download = false, skip = false;
        for (var i = 0; i < args.Length; i++)
        {
            string Value() => ++i < args.Length && !string.IsNullOrWhiteSpace(args[i]) ? args[i] : throw new ArgumentException("Missing option value.");
            switch (args[i])
            {
                case "--version": case "--silent": case "--repair-products": case "--check-products": case "--launch-retro": case "--launch-base":
                    if (mode is not null) throw new ArgumentException("Choose exactly one setup operation.");
                    mode = args[i]; break;
                case "--install-dir": install = Value(); break;
                case "--game": game = Value(); break;
                case "--retro-dir": retro = Value(); break;
                case "--portable": portable = true; break;
                case "--download-retro-wfc-payload": download = true; break;
                case "--skip-retro-wfc-payload": skip = true; break;
                case "--progress-json": break;
                default: throw new ArgumentException($"Unknown option: {args[i]}");
            }
        }
        return new(mode ?? throw new ArgumentException("A setup operation is required."), install, game, retro, portable, download, skip);
    }
}
