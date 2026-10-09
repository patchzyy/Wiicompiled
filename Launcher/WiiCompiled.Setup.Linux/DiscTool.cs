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
    private static readonly Dictionary<string, string> ProjectPathsByGameId = new(StringComparer.OrdinalIgnoreCase)
    {
        ["RMCP01"] = Path.Combine("projects", "mkwii", "recomp.yml"),
        ["RMCE01"] = Path.Combine("projects", "mkwii-ntsc-u", "recomp.yml"),
        ["RMCJ01"] = Path.Combine("projects", "mkwii-ntsc-j", "recomp.yml"),
        ["RMCK01"] = Path.Combine("projects", "mkwii-ntsc-k", "recomp.yml"),
    };

    public static async Task<ProjectManifest> ValidateAndExtractAsync(
        string isoPath, string assetsDirectory, string workspace,
        string? nodToolBin, IInstallReporter reporter, CancellationToken cancellationToken)
    {
        var nodTool = nodToolBin ?? await NodToolProvider.ResolveAsync(workspace, cancellationToken);

        // `nodtool info` only decodes the disc/partition headers (milliseconds); `nodtool extract`
        // copies the whole data partition to disk (tens of seconds for a custom-track-heavy MKWii
        // ISO). Checking the game ID first, before extracting, means a wrong disc fails fast -
        // matching the original dolphin-tool `header` step this replaces.
        reporter.Progress(InstallStages.ExtractDisc, "Reading the disc header", 2);
        var info = NodToolInfoParser.Parse(await RunInfoAsync(nodTool, isoPath, cancellationToken));
        if (!ProjectPathsByGameId.TryGetValue(info.GameId, out var projectRelativePath))
        {
            throw new InvalidOperationException(
                $"This disc is '{info.GameId}', which is not a supported Mario Kart Wii release (supported: RMCP01, RMCE01, RMCJ01, RMCK01). " +
                "Only your own legally-owned copy of a supported game/region can be used.");
        }
        var manifest = ProjectManifest.Load(Path.Combine(workspace, projectRelativePath));

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
        if (!string.Equals(dolSha, manifest.DolSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"main.dol sha256 mismatch for {manifest.GameId}: expected {manifest.DolSha256}, got {dolSha}. " +
                "This disc revision does not match what this project's manifest is pinned to.");
        }
        if (!string.Equals(relSha, manifest.RelSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"StaticR.rel sha256 mismatch for {manifest.GameId}: expected {manifest.RelSha256}, got {relSha}. " +
                "This disc revision does not match what this project's manifest is pinned to.");
        }

        Directory.CreateDirectory(assetsDirectory);
        File.Copy(dolPath, Path.Combine(assetsDirectory, "main.dol"), overwrite: true);
        File.Copy(relPath, Path.Combine(assetsDirectory, "StaticR.rel"), overwrite: true);
        reporter.Progress(InstallStages.ExtractDisc, "Disc validated and extracted", 6);
        return manifest;
    }

    public static ProjectManifest ResolveProjectManifestForAssets(string assetsDirectory, string workspace)
    {
        var dolPath = Path.Combine(assetsDirectory, "main.dol");
        if (!File.Exists(dolPath)) throw new FileNotFoundException("main.dol is missing from Assets", dolPath);
        var relPath = Path.Combine(assetsDirectory, "StaticR.rel");
        if (!File.Exists(relPath)) throw new FileNotFoundException("StaticR.rel is missing from Assets", relPath);

        var dolSha = Sha256Of(dolPath);
        ProjectManifest? manifest = null;
        foreach (var projectRelPath in ProjectPathsByGameId.Values)
        {
            var candidate = ProjectManifest.Load(Path.Combine(workspace, projectRelPath));
            if (string.Equals(candidate.DolSha256, dolSha, StringComparison.OrdinalIgnoreCase))
            {
                manifest = candidate;
                break;
            }
        }
        if (manifest is null)
        {
            throw new InvalidOperationException(
                $"Assets/main.dol sha256 ({dolSha}) does not match any supported clean Mario Kart Wii release.");
        }

        var relSha = Sha256Of(relPath);
        if (!string.Equals(relSha, manifest.RelSha256, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"Assets/StaticR.rel sha256 mismatch for {manifest.GameId}: expected {manifest.RelSha256}, got {relSha}.");
        }

        var bootBin = Path.Combine(assetsDirectory, "DATA", "sys", "boot.bin");
        if (File.Exists(bootBin))
        {
            using var stream = File.OpenRead(bootBin);
            var buffer = new byte[6];
            var read = stream.Read(buffer, 0, 6);
            var gameId = System.Text.Encoding.ASCII.GetString(buffer, 0, read);
            if (gameId.Length >= 4 && !gameId.StartsWith("RMC" + manifest.Region, StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidOperationException(
                    $"Assets/DATA/sys/boot.bin is for disc '{gameId}', but main.dol is {manifest.GameId}. " +
                    "Stale disc assets detected; re-extract your game.");
            }
        }

        return manifest;
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
