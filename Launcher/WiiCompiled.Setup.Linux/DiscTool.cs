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
        if (IsAlreadyExtracted(manifest, assetsDirectory))
        {
            reporter.Progress(InstallStages.ExtractDisc, "Reusing the verified extracted disc", 6);
            return;
        }

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
        Directory.CreateDirectory(assetsDirectory);
        File.Delete(Path.Combine(assetsDirectory, ExtractionMarker));
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
        File.WriteAllText(Path.Combine(assetsDirectory, ExtractionMarker), ExtractionRecord(dolSha, dataDir));
        reporter.Progress(InstallStages.ExtractDisc, "Disc validated and extracted", 6);
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

    private const string ExtractionMarker = ".disc-extracted";

    // Only one disc revision is accepted, so a completed, verified extraction is identical to a fresh one.
    private static bool IsAlreadyExtracted(ProjectManifest manifest, string assetsDirectory)
    {
        var marker = Path.Combine(assetsDirectory, ExtractionMarker);
        var dol = Path.Combine(assetsDirectory, "main.dol");
        var rel = Path.Combine(assetsDirectory, "StaticR.rel");
        var dataDir = Path.Combine(assetsDirectory, "DATA");
        if (!File.Exists(marker) || !File.Exists(dol) || !File.Exists(rel) ||
            !Directory.Exists(Path.Combine(dataDir, "files")))
            return false;
        return string.Equals(File.ReadAllText(marker), ExtractionRecord(manifest.DolSha256, dataDir),
                   StringComparison.Ordinal) &&
               string.Equals(Sha256Of(dol), manifest.DolSha256, StringComparison.Ordinal) &&
               string.Equals(Sha256Of(rel), manifest.RelSha256, StringComparison.Ordinal);
    }

    // The DOL hash plus every extracted file's path and size, so a later deletion or truncation inside
    // DATA forces a fresh extraction.
    private static string ExtractionRecord(string dolSha, string dataDir)
    {
        var record = new System.Text.StringBuilder(dolSha).Append('\n');
        foreach (var path in Directory.EnumerateFiles(dataDir, "*", SearchOption.AllDirectories)
                     .Select(path => Path.GetRelativePath(dataDir, path).Replace('\\', '/'))
                     .Order(StringComparer.Ordinal))
            record.Append(new FileInfo(Path.Combine(dataDir, path)).Length).Append(' ').Append(path).Append('\n');
        return record.ToString();
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
