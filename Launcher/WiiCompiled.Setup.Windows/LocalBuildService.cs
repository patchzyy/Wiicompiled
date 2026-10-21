using System.Diagnostics;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Windows;

/// <summary>
/// <see cref="Both"/> runs one retro-aware translation and compiles the two products from a single
/// build graph, sharing every profile-neutral shard object. Building the legs separately re-emits
/// the shared base shards with retro-aware content in the second leg and recompiles all of them.
/// </summary>
internal enum BuildProfile { Base, RetroRewind, Both }

internal sealed class LocalBuildService
{
    private readonly IInstallReporter _reporter;

    public LocalBuildService(IInstallReporter reporter) => _reporter = reporter;

    /// <summary>
    /// Runs the bundled translate-and-compile script against <paramref name="installStaging"/>, which
    /// holds a complete <c>Toolkit</c> and <c>BuildWorkspace</c> pair - either an installation being
    /// staged or an existing installation being repaired in place.
    /// </summary>
    public async Task BuildAsync(string installStaging, BuildProfile profile, string outputDirectory,
        RetroWfcPayloadMode retroWfcPayloadMode, string? retroWfcOfflinePayloadDirectory,
        string? retroWfcGameId, CancellationToken cancellationToken,
        ToolkitFingerprintComponents? toolkitComponents = null,
        bool forceCleanBuild = false, BuildProgressWindow? progress = null,
        string? retroRewindPackageDirectory = null, string? baseOutputDirectory = null)
    {
        var workspace = InstalledLayout.Workspace(installStaging);
        var toolkit = InstalledLayout.Toolkit(installStaging);
        var script = Path.Combine(workspace, "LocalBuild.ps1");
        if (!File.Exists(script)) throw new FileNotFoundException("The local build script is missing.", script);

        var buildsRetro = profile is BuildProfile.RetroRewind or BuildProfile.Both;
        if (!buildsRetro && retroWfcPayloadMode != RetroWfcPayloadMode.NotApplicable)
            throw new ArgumentException("The base build cannot select a Retro-WFC payload.");
        if (buildsRetro && retroWfcPayloadMode == RetroWfcPayloadMode.NotApplicable)
            throw new ArgumentException("The Retro Rewind build must select or explicitly skip the Retro-WFC payload.");
        if (retroRewindPackageDirectory is not null && !buildsRetro)
            throw new ArgumentException("Only Retro Rewind can select an explicit package snapshot.");
        if ((profile == BuildProfile.Both) != (baseOutputDirectory is not null))
            throw new ArgumentException("A combined build takes exactly one base output directory.");
        if (retroRewindPackageDirectory is not null)
            retroRewindPackageDirectory = RetroRewindSource.ResolveRetroRewind6(
                retroRewindPackageDirectory);
        if (retroWfcPayloadMode == RetroWfcPayloadMode.Online)
            retroWfcOfflinePayloadDirectory = InputValidation.ValidateStagedRetroWfcPayloadDirectory(
                retroWfcOfflinePayloadDirectory!,
                retroWfcGameId ?? throw new ArgumentException("The Retro-WFC payload needs its game id."));

        var arguments = new List<string>
        {
            "-NoLogo", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script,
            "-Workspace", workspace, "-Toolkit", toolkit,
            "-Profile", profile switch
            {
                BuildProfile.Base => "base",
                BuildProfile.RetroRewind => "retro-rewind",
                _ => "both"
            },
            "-OutputDirectory", outputDirectory,
            "-X86CpuProfile", HostCpuRequirement.SelectBuildProfile() == X86CpuProfile.V2 ? "v2" : "v3"
        };
        if (baseOutputDirectory is not null)
        {
            arguments.Add("-BaseOutputDirectory");
            arguments.Add(baseOutputDirectory);
        }
        // The script is the single owner of every cache-reuse decision; these identities are the
        // evidence it compares against the provenance it recorded with the caches themselves.
        // Without them (or with -ForceCleanBuild) it degrades to a full clean translate and build.
        if (toolkitComponents is not null)
        {
            arguments.Add("-TranslationFingerprint");
            arguments.Add(toolkitComponents.Translation);
            arguments.Add("-NativeToolchainFingerprint");
            arguments.Add(toolkitComponents.NativeToolchain);
        }
        if (forceCleanBuild)
            arguments.Add("-ForceCleanBuild");
        if (retroRewindPackageDirectory is not null)
        {
            arguments.Add("-RetroRewindPackageDirectory");
            arguments.Add(retroRewindPackageDirectory);
        }
        if (retroWfcPayloadMode == RetroWfcPayloadMode.Online)
        {
            arguments.Add("-RetroWfcOfflineDirectory");
            arguments.Add(retroWfcOfflinePayloadDirectory!);
            arguments.Add("-RetroWfcPayloadOrigin");
            arguments.Add("downloaded");
        }
        else if (retroWfcPayloadMode == RetroWfcPayloadMode.Skipped)
        {
            arguments.Add("-SkipRetroWfcPayload");
        }
        // Scrubs the ambient VS environment so the bundled clang-mingw toolchain is the only one visible.
        // The actual search path is set by LocalBuild.ps1's one canonical definition (Get-MkwToolchainPath,
        // shared with Prepare-NativePrebuilt.ps1), not duplicated here, to avoid drift.
        void ScrubEnvironment(ProcessStartInfo start)
        {
            start.WorkingDirectory = workspace;
            foreach (var inherited in new[]
                     { "INCLUDE", "LIB", "LIBPATH", "VSINSTALLDIR", "VCToolsInstallDir", "WindowsSdkDir" })
                start.Environment.Remove(inherited);
        }

        void Observe(string line)
        {
            if (string.IsNullOrWhiteSpace(line)) return;
            if (progress is not null) progress.Observe(line);
            else _reporter.Diagnostic(line);
        }

        var build = await ProcessRunner.RunAsync(
            Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System),
                "WindowsPowerShell", "v1.0", "powershell.exe"),
            arguments, Observe, cancellationToken, ScrubEnvironment, capture: false,
            ex => _reporter.Diagnostic(
                "The cancelled build process could not be terminated immediately: " + ex.Message));
        if (build.ExitCode != 0)
            throw new InvalidOperationException($"Local recompilation failed with exit code {build.ExitCode}. See the setup log for the failed translator or compiler command.");

        var expectedOutputs = profile switch
        {
            BuildProfile.Base => [(outputDirectory, "WiiCompiled.exe")],
            BuildProfile.RetroRewind => [(outputDirectory, "RetroRewind.exe")],
            _ => new[] { (baseOutputDirectory!, "WiiCompiled.exe"), (outputDirectory, "RetroRewind.exe") }
        };
        foreach (var (directory, executableName) in expectedOutputs)
        {
            var executable = Path.Combine(directory, executableName);
            if (!File.Exists(executable) || new FileInfo(executable).Length < 64 * 1024)
                throw new InvalidDataException("The local build completed without producing a valid game executable.");
            if (!File.Exists(Path.Combine(directory, "local-build.json")))
                throw new InvalidDataException("The local build did not produce provenance information.");
        }
    }

    /// <summary>
    /// Records what this product was built from: <paramref name="toolkitFingerprint"/> ties it to the
    /// translator/workspace, <paramref name="codePulSha256"/> to Retro Rewind's embedded Code.pul, so a
    /// later launch or update can tell whether the binary is still correct.
    /// </summary>
    public static void WriteFingerprint(string outputDirectory, BuildProfile profile, string toolkitFingerprint,
        string dolSha256, string relSha256, string codePulSha256, RetroWfcPayloadMode retroWfcPayloadMode,
        string retroRewindCompileInputsSha256 = "", string retroWfcPayloadSha256 = "",
        long retroWfcPayloadLength = 0)
    {
        var executable = Path.Combine(outputDirectory,
            profile == BuildProfile.Base ? "WiiCompiled.exe" : "RetroRewind.exe");
        if (!File.Exists(executable))
            throw new FileNotFoundException("Cannot record build provenance because the compiled executable is missing.",
                executable);
        if (profile == BuildProfile.RetroRewind)
        {
            var local = JsonState.TryRead<LocalBuildProvenance>(
                Path.Combine(outputDirectory, LocalBuildProvenance.FileName));
            var expectedMode = retroWfcPayloadMode == RetroWfcPayloadMode.Online
                ? "downloaded"
                : "skipped";
            if (local is not { SchemaVersion: 1 } ||
                !string.Equals(local.DolSha256, dolSha256, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(local.RelSha256, relSha256, StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(local.CodePulSha256, codePulSha256,
                    StringComparison.OrdinalIgnoreCase) ||
                !string.Equals(local.RetroWfcPayloadMode, expectedMode, StringComparison.Ordinal) ||
                !string.Equals(local.RetroWfcPayloadSha256,
                    retroWfcPayloadMode == RetroWfcPayloadMode.Online ? retroWfcPayloadSha256 : null,
                    StringComparison.OrdinalIgnoreCase) ||
                (local.RetroWfcPayloadLength ?? 0) !=
                (retroWfcPayloadMode == RetroWfcPayloadMode.Online ? retroWfcPayloadLength : 0))
            {
                throw new InvalidDataException(
                    "The local Retro Rewind build did not consume the selected Retro-WFC payload snapshot.");
            }
        }

        var fingerprint = new ProductFingerprint
        {
            Profile = profile == BuildProfile.Base ? "base" : "retro-rewind",
            ToolkitFingerprint = toolkitFingerprint,
            DolSha256 = dolSha256,
            RelSha256 = relSha256,
            ExecutableSha256 = InputValidation.Sha256File(executable),
            CodePulSha256 = codePulSha256,
            RetroRewindCompileInputsSha256 = retroRewindCompileInputsSha256,
            RetroWfcPayloadMode = retroWfcPayloadMode switch
            {
                RetroWfcPayloadMode.Online => "downloaded",
                RetroWfcPayloadMode.Skipped => "skipped",
                _ => ""
            },
            RetroWfcPayloadSha256 = retroWfcPayloadSha256,
            RetroWfcPayloadLength = retroWfcPayloadLength
        };
        JsonState.Write(Path.Combine(outputDirectory, ProductFingerprint.FileName), fingerprint);
    }
}
