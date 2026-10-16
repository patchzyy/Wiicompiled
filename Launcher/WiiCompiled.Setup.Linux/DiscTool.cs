using System.Security.Cryptography;
using WiiCompiled.Setup.Common;

namespace WiiCompiled.Setup.Linux;

/// <summary>
/// Validates and extracts the user's own Mario Kart Wii disc via `nodtool` (see
/// WiiCompiled.Setup.Common/NodToolProvider.cs) - a prebuilt, MIT/Apache-2.0-licensed CLI from
/// encounter/nod, replacing the earlier dependency on a system-installed `dolphin-tool`
/// (GPL-2.0-or-later, and not reliably packaged standalone by every distro).
/// </summary>
internal static class DiscTool
{
    public static async Task ValidateAndExtractAsync(
        string isoPath, ProjectManifest manifest, string assetsDirectory, string workspace,
        string? nodToolBin, IInstallReporter reporter, CancellationToken cancellationToken)
    {
        var nodTool = nodToolBin ?? await NodToolProvider.ResolveAsync(workspace, cancellationToken);

        // `nodtool info` only decodes the disc/partition headers (milliseconds); `nodtool extract`
        // copies the whole data partition to disk (tens of seconds for a custom-track-heavy MKWii
        // ISO). Checking the game ID first, before extracting, means a wrong disc fails fast -
        // matching the original dolphin-tool `header` step this replaces.
        reporter.Progress(InstallStages.ExtractDisc, "Reading the disc header", 2);
        var info = NodToolInfoParser.Parse(await RunInfoAsync(nodTool, isoPath, cancellationToken));
        if (!string.Equals(info.GameId, manifest.GameId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"This disc is '{info.GameId}', not the expected '{manifest.GameId}' (Mario Kart Wii, region {manifest.Region}). " +
                "Only your own legally-owned copy of that exact game/region can be used.");
        }

        // Extracted straight into Assets/DATA (kept, not a scratch dir) - the runtime reads course/
        // texture/audio data from this directory live via [paths] dvd_root, not just at translation
        // time, so it has to survive past this install (see Program.cs, which points dvd_root here).
        reporter.Progress(InstallStages.ExtractDisc, "Extracting the disc image", 4);
        var dataDir = Path.Combine(assetsDirectory, "DATA");
        if (Directory.Exists(dataDir)) Directory.Delete(dataDir, recursive: true);
        await RunExtractAsync(nodTool, isoPath, dataDir, cancellationToken);

        var dolPath = Path.Combine(dataDir, "sys", "main.dol");
        var relPath = Path.Combine(dataDir, "files", "rel", "StaticR.rel");
        if (!File.Exists(dolPath)) throw new FileNotFoundException("nodtool did not produce main.dol", dolPath);
        if (!File.Exists(relPath)) throw new FileNotFoundException("nodtool did not produce StaticR.rel", relPath);

        var dolSha = Sha256Of(dolPath);
        var relSha = Sha256Of(relPath);
        if (!string.Equals(dolSha, manifest.DolSha256, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"main.dol sha256 mismatch: expected {manifest.DolSha256}, got {dolSha}. " +
                "This disc revision does not match what this project's manifest is pinned to.");
        }
        if (!string.Equals(relSha, manifest.RelSha256, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"StaticR.rel sha256 mismatch: expected {manifest.RelSha256}, got {relSha}. " +
                "This disc revision does not match what this project's manifest is pinned to.");
        }

        Directory.CreateDirectory(assetsDirectory);
        File.Copy(dolPath, Path.Combine(assetsDirectory, "main.dol"), overwrite: true);
        File.Copy(relPath, Path.Combine(assetsDirectory, "StaticR.rel"), overwrite: true);
        reporter.Progress(InstallStages.ExtractDisc, "Disc validated and extracted", 6);
    }

    /// <summary>
    /// Checks a game Wheel Wizard already extracted against the pinned clean disc and copies only its two code
    /// files into <paramref name="assetsDirectory"/> for translation. The game itself stays where it is; the
    /// runtime reads it in place. Returns its <c>DATA</c> folder.
    /// </summary>
    public static string ValidateExtracted(string gameDirectory, ProjectManifest manifest, string assetsDirectory,
        IInstallReporter reporter)
    {
        reporter.Progress(InstallStages.ExtractDisc, "Checking the extracted game", 2);
        var dataDir = ExtractedGameDirectory.ResolveDataDirectory(gameDirectory);
        var header = ExtractedGameDirectory.ReadHeader(dataDir);
        if (!string.Equals(header.GameId, manifest.GameId, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                $"This game is '{header.GameId}', not the expected '{manifest.GameId}' (Mario Kart Wii, region {manifest.Region}).");
        }

        var dolPath = Path.Combine(dataDir, "sys", "main.dol");
        var relPath = Path.Combine(dataDir, "files", "rel", "StaticR.rel");
        if (!File.Exists(relPath)) throw new FileNotFoundException("The extracted game has no StaticR.rel", relPath);
        if (!string.Equals(Sha256Of(dolPath), manifest.DolSha256, StringComparison.Ordinal) ||
            !string.Equals(Sha256Of(relPath), manifest.RelSha256, StringComparison.Ordinal))
        {
            throw new InvalidOperationException(
                "The extracted game's main.dol or StaticR.rel is not the clean game code this project is pinned to.");
        }

        Directory.CreateDirectory(assetsDirectory);
        File.Copy(dolPath, Path.Combine(assetsDirectory, "main.dol"), overwrite: true);
        File.Copy(relPath, Path.Combine(assetsDirectory, "StaticR.rel"), overwrite: true);
        reporter.Progress(InstallStages.ExtractDisc, "Extracted game validated", 6);
        return dataDir;
    }

    /// <summary>Removes the workspace's own extraction once the extracted game is read in place instead.</summary>
    public static void RemoveOwnExtraction(string assetsDirectory, IInstallReporter reporter)
    {
        var own = Path.Combine(assetsDirectory, "DATA");
        try
        {
            if (Directory.Exists(own)) Directory.Delete(own, recursive: true);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            reporter.Diagnostic("The game copy WiiCompiled no longer uses could not be removed yet: " + ex.Message);
        }
    }

    private static async Task<string> RunInfoAsync(string nodTool, string isoPath, CancellationToken cancellationToken)
    {
        var startInfo = new System.Diagnostics.ProcessStartInfo(nodTool)
        {
            ArgumentList = { "info", isoPath },
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };
        using var process = System.Diagnostics.Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start {nodTool}.");
        var stdout = await process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderr = await process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"nodtool could not read this disc image (exit {process.ExitCode}): {stderr}{stdout}".Trim());
        }
        return stdout;
    }

    private static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant();
    }

    private static async Task RunExtractAsync(string nodTool, string isoPath, string outDir, CancellationToken cancellationToken)
    {
        var startInfo = new System.Diagnostics.ProcessStartInfo(nodTool)
        {
            ArgumentList = { "extract", isoPath, outDir, "-q" },
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            UseShellExecute = false,
        };

        using var process = System.Diagnostics.Process.Start(startInfo)
            ?? throw new InvalidOperationException($"Failed to start {nodTool}.");
        var stdout = await process.StandardOutput.ReadToEndAsync(cancellationToken);
        var stderr = await process.StandardError.ReadToEndAsync(cancellationToken);
        await process.WaitForExitAsync(cancellationToken);
        if (process.ExitCode != 0)
        {
            throw new InvalidOperationException(
                $"nodtool extract {isoPath} failed (exit {process.ExitCode}): {stderr}{stdout}");
        }
    }
}
