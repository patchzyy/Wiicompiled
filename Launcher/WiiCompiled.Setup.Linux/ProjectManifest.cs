using System.Text.RegularExpressions;

namespace WiiCompiled.Setup.Linux;

/// <summary>
/// The handful of facts this tool needs out of a projects/*/recomp.yml. Parsed literally line by
/// line - the same approach Launcher/NativeBuildFlags.ps1's Get-MkwProjectPins and
/// Launcher/local-build.sh already use - rather than pulling in a YAML library, since the manifest
/// is machine-written with a fixed shape.
/// </summary>
internal sealed class ProjectManifest
{
    public required string GameId { get; init; }
    public required string Region { get; init; }
    public required string DolSha256 { get; init; }
    public required string RelSha256 { get; init; }

    public static ProjectManifest Load(string path)
    {
        if (!File.Exists(path)) throw new FileNotFoundException("Translation project file is missing", path);

        string? gameId = null, region = null, dolSha = null, relSha = null;
        string section = "";
        string inputKey = "";

        foreach (var raw in File.ReadLines(path))
        {
            var line = Regex.Replace(raw, "#.*$", "");
            if (string.IsNullOrWhiteSpace(line)) continue;

            var sectionMatch = Regex.Match(line, "^([A-Za-z0-9_]+):");
            if (sectionMatch.Success)
            {
                section = sectionMatch.Groups[1].Value;
                inputKey = "";
                continue;
            }

            if (section == "inputs")
            {
                var keyMatch = Regex.Match(line, @"^\s{2}([A-Za-z0-9_]+):\s*$");
                if (keyMatch.Success) { inputKey = keyMatch.Groups[1].Value; continue; }

                var shaMatch = Regex.Match(line, @"^\s*sha256:\s*([0-9a-fA-F]{64})\s*$");
                if (shaMatch.Success)
                {
                    var value = shaMatch.Groups[1].Value.ToLowerInvariant();
                    if (inputKey == "dol") dolSha = value;
                    else if (inputKey == "rel") relSha = value;
                }
            }
            else if (section == "project")
            {
                var idMatch = Regex.Match(line, @"^\s*game_id:\s*(\S+)\s*$");
                if (idMatch.Success) gameId = idMatch.Groups[1].Value;

                var regionMatch = Regex.Match(line, @"^\s*region:\s*(\S+)\s*$");
                if (regionMatch.Success) region = regionMatch.Groups[1].Value;
            }
        }

        if (gameId is null || region is null || dolSha is null || relSha is null)
        {
            throw new InvalidDataException(
                $"{path} does not pin game_id/region/dol.sha256/rel.sha256; the project file is not the shape this tool expects.");
        }

        return new ProjectManifest { GameId = gameId, Region = region, DolSha256 = dolSha, RelSha256 = relSha };
    }

    /// <summary>Every supported release: one per projects/*/recomp.yml, as local-build.sh selects them.</summary>
    public static IReadOnlyList<ProjectManifest> LoadAll(string workspace)
    {
        var projects = Directory.EnumerateDirectories(Path.Combine(workspace, "projects"))
            .Select(directory => Path.Combine(directory, "recomp.yml"))
            .Where(File.Exists)
            .Order(StringComparer.Ordinal)
            .Select(Load)
            .ToList();
        if (projects.Count == 0)
            throw new InvalidOperationException($"No projects/*/recomp.yml was found under {workspace}.");
        return projects;
    }

    public static string Describe(IEnumerable<ProjectManifest> projects) =>
        string.Join(", ", projects.Select(project => $"{project.GameId} (region {project.Region})"));
}
