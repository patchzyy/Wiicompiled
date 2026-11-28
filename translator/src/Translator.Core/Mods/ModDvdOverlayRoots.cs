using Translator.Core.IO;
namespace Translator.Core.Mods;

/// <summary>
/// A Riivolution option choice pinned by the distribution spec. Section may be empty,
/// meaning "match any section"; Choice is the 1-based choice index, 0 meaning disabled.
/// </summary>
public sealed record ModRiivolutionOption(string Section, string Option, uint Choice);

/// <summary>
/// The pack-relative directories the runtime mounts over the disc file system.
/// These reach the runtime as <c>RecompMod::RegisterDvdOverlayRoot</c> calls in the
/// generated mod data-patch translation unit, not as a side-car manifest.
/// </summary>
public static class ModDvdOverlayRoots
{
    public static IReadOnlyList<string> Discover(string? modRoot)
    {
        if (string.IsNullOrWhiteSpace(modRoot))
        {
            return [];
        }

        var sourceRoot = Path.GetFullPath(modRoot);
        var packageRoot = Path.GetFileName(sourceRoot.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        if (string.IsNullOrWhiteSpace(packageRoot))
        {
            return [];
        }

        var candidates = new[]
        {
            (Source: sourceRoot, Package: packageRoot),
            (Source: Path.Combine(sourceRoot, "files"), Package: Path.Combine(packageRoot, "files"))
        };

        return candidates
            .Where(candidate => TrackedFs.DirectoryExists(candidate.Source))
            .Select(candidate => candidate.Package.Replace('\\', '/'))
            .Distinct(StringComparer.OrdinalIgnoreCase)
            .ToList();
    }
}
