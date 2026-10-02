using System.Diagnostics;

namespace WiiCompiled.Setup.Linux;

/// <summary>
/// Invokes the native platform build script and turns its stdout into progress reports. Replaces
/// LocalBuildService.cs's hardcoded Windows PowerShell 5.1 invocation - there is no PowerShell
/// dependency here at all, just bash.
/// </summary>
internal static class BuildRunner
{
    public static async Task RunAsync(
        string workspace, string profile, string outputDir, string? baseOutputDir,
        string? retroDir, string? retroWfcOfflineDir, bool skipRetroWfcPayload,
        bool forceCleanBuild, string? translatorBin, string? ccBin, string? cxxBin, string? fuseLd,
        string? cmakeBin, string? ninjaBin, string? nativePrebuiltDir, string? sysroot,
        IInstallReporter reporter,
        CancellationToken cancellationToken)
    {
        var macOS = OperatingSystem.IsMacOS();
        if (macOS && sysroot is not null)
            throw new ArgumentException("--sysroot is not supported by the macOS build.");

        var script = Path.Combine(workspace, "Launcher",
            macOS ? "local-build-macos.command" : "local-build.sh");
        if (!File.Exists(script)) throw new FileNotFoundException($"{Path.GetFileName(script)} is missing", script);

        var startInfo = new ProcessStartInfo("bash")
        {
            WorkingDirectory = workspace,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        startInfo.ArgumentList.Add(script);
        startInfo.ArgumentList.Add("--profile"); startInfo.ArgumentList.Add(profile);
        startInfo.ArgumentList.Add("--output-dir"); startInfo.ArgumentList.Add(outputDir);
        if (!string.IsNullOrEmpty(baseOutputDir))
        {
            startInfo.ArgumentList.Add("--base-output-dir"); startInfo.ArgumentList.Add(baseOutputDir);
        }
        if (!string.IsNullOrEmpty(retroDir))
        {
            startInfo.ArgumentList.Add("--retro-rewind-package-dir"); startInfo.ArgumentList.Add(retroDir);
        }
        if (!string.IsNullOrEmpty(retroWfcOfflineDir))
        {
            startInfo.ArgumentList.Add("--retro-wfc-offline-dir"); startInfo.ArgumentList.Add(retroWfcOfflineDir);
        }
        if (skipRetroWfcPayload) startInfo.ArgumentList.Add("--skip-retro-wfc-payload");
        if (forceCleanBuild) startInfo.ArgumentList.Add("--force-clean-build");
        if (!string.IsNullOrEmpty(translatorBin))
        {
            startInfo.ArgumentList.Add("--translator-bin"); startInfo.ArgumentList.Add(translatorBin);
        }
        if (macOS)
        {
            if (!string.IsNullOrEmpty(cmakeBin))
            {
                startInfo.ArgumentList.Add("--cmake"); startInfo.ArgumentList.Add(cmakeBin);
            }
            if (!string.IsNullOrEmpty(ninjaBin))
            {
                startInfo.ArgumentList.Add("--ninja"); startInfo.ArgumentList.Add(ninjaBin);
            }
        }
        else
        {
            // Forwarded by AppRun so the AppImage's bundled clang/lld (see prepare-portable-tools.sh)
            // is used instead of local-build.sh's own default of whatever clang is on $PATH.
            if (!string.IsNullOrEmpty(ccBin))
            {
                startInfo.ArgumentList.Add("--cc"); startInfo.ArgumentList.Add(ccBin);
            }
            if (!string.IsNullOrEmpty(cxxBin))
            {
                startInfo.ArgumentList.Add("--cxx"); startInfo.ArgumentList.Add(cxxBin);
            }
            if (!string.IsNullOrEmpty(fuseLd))
            {
                startInfo.ArgumentList.Add("--fuse-ld"); startInfo.ArgumentList.Add(fuseLd);
            }
            if (!string.IsNullOrEmpty(cmakeBin))
            {
                startInfo.ArgumentList.Add("--cmake"); startInfo.ArgumentList.Add(cmakeBin);
            }
            if (!string.IsNullOrEmpty(ninjaBin))
            {
                startInfo.ArgumentList.Add("--ninja"); startInfo.ArgumentList.Add(ninjaBin);
            }
            // Forwarded by AppRun so the AppImage's bundled precompiled aurora/third-party package
            // is used instead of local-build.sh compiling aurora-main itself.
            if (!string.IsNullOrEmpty(nativePrebuiltDir))
            {
                startInfo.ArgumentList.Add("--native-prebuilt-dir"); startInfo.ArgumentList.Add(nativePrebuiltDir);
            }
            if (!string.IsNullOrEmpty(sysroot))
            {
                startInfo.ArgumentList.Add("--sysroot"); startInfo.ArgumentList.Add(sysroot);
            }
        }

        using var process = new Process { StartInfo = startInfo };
        var window = new BuildProgressWindow(reporter, InstallStages.Build, start: 6, end: 96);

        process.OutputDataReceived += (_, e) => { if (e.Data is not null) window.Observe(e.Data); };
        process.ErrorDataReceived += (_, e) => { if (e.Data is not null) reporter.Diagnostic(e.Data); };

        process.Start();
        process.BeginOutputReadLine();
        process.BeginErrorReadLine();

        try
        {
            await process.WaitForExitAsync(cancellationToken);
        }
        catch (OperationCanceledException)
        {
            KillProcessTree(process);
            throw;
        }

        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException($"{Path.GetFileName(script)} failed (exit {process.ExitCode}). See diagnostics above.");
        }
    }

    private static void KillProcessTree(Process process)
    {
        try { process.Kill(entireProcessTree: true); } catch { /* best-effort */ }
    }
}
