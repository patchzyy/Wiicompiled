using Translator.Core.IO;
using Translator.Core.Mods;

namespace Translator.Core.Build;

/// <summary>
/// What one prune pass did. Warnings are collected rather than printed: pruning
/// runs inside Core, and the CLI owns every byte of console output.
/// </summary>
public sealed record GeneratedOutputPruneResult(int RemovedFiles, IReadOnlyList<string> Warnings)
{
    public static GeneratedOutputPruneResult Empty { get; } = new(0, Array.Empty<string>());
}

public static class GeneratedOutputPruner
{
    public static GeneratedOutputPruneResult Prune(
        string baseOutDir,
        IReadOnlySet<string> emittedPaths,
        BaseTranslationOutputMetadata? previousOutputMetadata = null)
    {
        if (!TrackedFs.OutputDirectoryExists(baseOutDir))
            return GeneratedOutputPruneResult.Empty;

        var comparison = OperatingSystem.IsWindows()
            ? StringComparison.OrdinalIgnoreCase
            : StringComparison.Ordinal;
        var root = Path.GetFullPath(baseOutDir)
            .TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var rootPrefix = root + Path.DirectorySeparatorChar;
        IEnumerable<string> candidates;
        if (previousOutputMetadata is null)
        {
            // Compatibility migration for trees produced before translator-owned
            // metadata: every generated body under the root is a prune candidate.
            candidates = TrackedFs.EnumerateOutputFiles(root, "*.cpp", SearchOption.AllDirectories);
        }
        else
        {
            candidates = previousOutputMetadata.Functions.Select(function =>
                Path.Combine(root, function.RelativePath.Replace('/', Path.DirectorySeparatorChar)));
        }

        var removed = 0;
        var warnings = new List<string>();
        foreach (var file in candidates.Distinct(
                     OperatingSystem.IsWindows() ? StringComparer.OrdinalIgnoreCase : StringComparer.Ordinal))
        {
            var fullPath = Path.GetFullPath(file);
            if (!fullPath.StartsWith(rootPrefix, comparison))
            {
                warnings.Add($"refused to prune generated path outside output root: {fullPath}");
                continue;
            }
            if (emittedPaths.Contains(fullPath) || !TrackedFs.OutputFileExists(fullPath))
                continue;

            try
            {
                TrackedFs.Delete(fullPath);
                removed++;
            }
            catch (Exception ex)
            {
                warnings.Add($"failed to prune stale generated file {fullPath}: {ex.Message}");
            }
        }

        return new GeneratedOutputPruneResult(removed, warnings);
    }
}
