using System.Security.Cryptography;
using Translator.Core.IO;

namespace Translator.Tests;

/// <summary>
/// Proves the gate sees every file a step depends on, without trusting the facade's own record
/// of what was read: every file in the workspace is mutated (appended to, then deleted) one at a
/// time, and whenever the gate would still have skipped, the step is re-run for real and must
/// behave exactly as it did before. A read that bypassed the tracking facade shows up as a file
/// whose mutation changes the step while the gate stays "current".
/// </summary>
internal static class FacadeCoverage
{
    private static readonly string[] Mutations = ["append", "delete"];

    public static void AssertGateCoversEveryDependency(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var record = StepProvenanceScenarios.ReadRecord(workspace.RecordPath(step));
        Assert.Null(StepProvenance.CheckFileSystem(record));

        var pristine = workspace.Root + ".pristine";
        CopyDirectory(workspace.Root, pristine);
        try
        {
            void Restore()
            {
                Directory.Delete(workspace.Root, recursive: true);
                CopyDirectory(pristine, workspace.Root);
            }

            (int ExitCode, Dictionary<string, string> Tree) Behaviour(string? excluded)
            {
                var run = workspace.RunStep(step, gated: false);
                return (run.ExitCode, Tree(workspace, excluded));
            }

            Restore();
            var baseline = Behaviour(null);
            Assert.Equal(0, baseline.ExitCode);
            Restore();
            var repeat = Behaviour(null);
            Assert.True(SameBehaviour(baseline, repeat, null), $"{step} is not deterministic, so this test cannot judge it");

            var files = Directory.EnumerateFiles(pristine, "*", SearchOption.AllDirectories)
                .Select(path => Path.Combine(workspace.Root, Path.GetRelativePath(pristine, path)))
                .Where(path => !path.StartsWith(workspace.ProvenanceDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                .Order(StringComparer.Ordinal)
                .ToArray();
            var inputs = record.Inputs.Select(static input => input.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);
            Assert.NotEmpty(files);

            var violations = new List<string>();
            var behaviourChanges = 0;
            var gateMisses = 0;
            foreach (var file in files)
            {
                foreach (var mutation in Mutations)
                {
                    Restore();
                    if (mutation == "append") File.AppendAllText(file, "\n// mutated\n");
                    else File.Delete(file);

                    var miss = StepProvenance.CheckFileSystem(record);
                    if (inputs.Contains(file))
                        Assert.True(miss is not null, $"{mutation} of recorded input {file} did not invalidate {step}");
                    if (miss is not null)
                    {
                        gateMisses++;
                        continue;
                    }

                    // The gate would have skipped: the step must therefore be unaffected.
                    var mutated = Behaviour(file);
                    if (!SameBehaviour(baseline, mutated, file))
                    {
                        behaviourChanges++;
                        violations.Add($"{mutation} {Path.GetRelativePath(workspace.Root, file)} changed {step} but the gate still skipped it");
                    }
                }
            }

            Assert.True(violations.Count == 0, string.Join(Environment.NewLine, violations));
            Assert.Equal(0, behaviourChanges);
            Assert.True(gateMisses >= inputs.Count, "every recorded input invalidates the gate");
        }
        finally
        {
            if (Directory.Exists(pristine)) Directory.Delete(pristine, recursive: true);
        }
    }

    /// <summary>Content hash of every file in the workspace after a run, apart from the provenance records and the mutated file.</summary>
    private static Dictionary<string, string> Tree(StepProvenanceWorkspace workspace, string? excluded) =>
        Directory.EnumerateFiles(workspace.Root, "*", SearchOption.AllDirectories)
            .Where(path => !path.StartsWith(workspace.ProvenanceDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
            .Where(path => excluded is null || !string.Equals(path, excluded, StringComparison.OrdinalIgnoreCase))
            .ToDictionary(
                path => Path.GetRelativePath(workspace.Root, path),
                static path => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(path))),
                StringComparer.OrdinalIgnoreCase);

    private static bool SameBehaviour(
        (int ExitCode, Dictionary<string, string> Tree) expected,
        (int ExitCode, Dictionary<string, string> Tree) actual,
        string? excluded)
    {
        if (expected.ExitCode != actual.ExitCode) return false;
        var expectedTree = expected.Tree.Where(pair => excluded is null ||
            !excluded.EndsWith(Path.DirectorySeparatorChar + pair.Key, StringComparison.OrdinalIgnoreCase));
        return expectedTree.Count() == actual.Tree.Count &&
               expectedTree.All(pair => actual.Tree.TryGetValue(pair.Key, out var hash) && hash == pair.Value);
    }

    private static void CopyDirectory(string source, string destination)
    {
        Directory.CreateDirectory(destination);
        foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, directory)));
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            var target = Path.Combine(destination, Path.GetRelativePath(source, file));
            File.Copy(file, target, overwrite: true);
            File.SetLastWriteTimeUtc(target, File.GetLastWriteTimeUtc(file));
        }
    }
}

public sealed class TranslateModFacadeCoverageTests
{
    [Fact]
    public void GateCoversEveryFileTranslateModDependsOn() =>
        FacadeCoverage.AssertGateCoversEveryDependency(StepProvenanceWorkspace.TranslateMod);
}

public sealed class GenerateDataInitFacadeCoverageTests
{
    [Fact]
    public void GateCoversEveryFileGenerateDataInitDependsOn() =>
        FacadeCoverage.AssertGateCoversEveryDependency(StepProvenanceWorkspace.GenerateDataInit);
}

public sealed class EmitBuildShardsFacadeCoverageTests
{
    [Fact]
    public void GateCoversEveryFileEmitBuildShardsDependsOn() =>
        FacadeCoverage.AssertGateCoversEveryDependency(StepProvenanceWorkspace.EmitBuildShards);
}

/// <summary>
/// The static half of the guarantee: nothing in the translator touches the file system or the
/// environment except through <see cref="TrackedFs"/> (and the gate's own record handling).
/// </summary>
public sealed class TrackedFileSystemSourceTests
{
    private static readonly string[] Exempt =
    [
        Path.Combine("Translator.Core", "IO", "TrackedFs.cs"),
        Path.Combine("Translator.Core", "IO", "FileAccessRecorder.cs"),
        Path.Combine("Translator.Core", "IO", "StepProvenance.cs"),
    ];

    private static readonly (string Pattern, string Why)[] Banned =
    [
        (@"(?<![\w.])(System\.IO\.)?File\s*\.", "System.IO.File"),
        (@"(?<![\w.])(System\.IO\.)?Directory\s*\.", "System.IO.Directory"),
        (@"\b(FileInfo|DirectoryInfo|FileSystemInfo|DriveInfo|FileSystemWatcher|MemoryMappedFile|RandomAccess|SafeFileHandle)\b", "file-system type"),
        (@"\bnew\s+(FileStream|StreamReader|StreamWriter)\s*\(", "path-taking stream constructor"),
        (@"\b(XmlReader\.Create|XDocument\.Load|XElement\.Load|XmlDocument)\b", "XML loading from a path"),
        (@"\bEnvironment\s*\.\s*(ExpandEnvironmentVariables|GetEnvironmentVariable|CurrentDirectory)\b", "environment read"),
        (@"\bProcess\s*\.\s*Start\b", "child process"),
        (@"using\s+static\s+System\.IO\b", "static import of System.IO"),
        (@"=\s*System\.IO\.(File|Directory)\b", "alias of System.IO"),
    ];

    [Fact]
    public void TranslatorSourcesTouchFilesOnlyThroughTheTrackingFacade()
    {
        var sourceRoot = Path.Combine(ProjectPathsForTests.FindRepositoryRoot(), "translator", "src");
        var offenders = new List<string>();
        var scanned = 0;
        foreach (var path in Directory.EnumerateFiles(sourceRoot, "*.cs", SearchOption.AllDirectories))
        {
            var relative = Path.GetRelativePath(sourceRoot, path);
            var top = relative.Split(Path.DirectorySeparatorChar);
            if (top.Length > 1 && (top[1] is "bin" or "obj")) continue;
            if (Exempt.Contains(relative, StringComparer.OrdinalIgnoreCase)) continue;
            scanned++;
            var lines = File.ReadAllLines(path);
            for (var index = 0; index < lines.Length; index++)
            {
                var code = StripCommentsAndStrings(lines[index]);
                foreach (var (pattern, why) in Banned)
                {
                    if (System.Text.RegularExpressions.Regex.IsMatch(code, pattern))
                        offenders.Add($"{relative}:{index + 1}: {why}: {lines[index].Trim()}");
                }
            }
        }

        Assert.True(scanned > 50, $"only {scanned} sources scanned under {sourceRoot}");
        Assert.True(offenders.Count == 0,
            "Route these through Translator.Core.IO.TrackedFs so --provenance-file sees them:" +
            Environment.NewLine + string.Join(Environment.NewLine, offenders));
    }

    [Fact]
    public void TheSourceScanCatchesDirectAccess()
    {
        string[] samples =
        [
            "var text = File.ReadAllText(path);",
            "foreach (var f in System.IO.Directory.EnumerateFiles(root)) { }",
            "var length = new FileInfo(path).Length;",
            "using var stream = new FileStream(path, FileMode.Open);",
            "var home = Environment.GetEnvironmentVariable(\"HOME\");",
        ];
        foreach (var sample in samples)
        {
            var code = StripCommentsAndStrings(sample);
            Assert.Contains(Banned, banned => System.Text.RegularExpressions.Regex.IsMatch(code, banned.Pattern));
        }
        Assert.DoesNotContain(Banned, banned => System.Text.RegularExpressions.Regex.IsMatch(
            StripCommentsAndStrings("TrackedFs.ReadAllText(path); // File.ReadAllText is banned"), banned.Pattern));
        Assert.DoesNotContain(Banned, banned => System.Text.RegularExpressions.Regex.IsMatch(
            StripCommentsAndStrings("throw new InvalidDataException(\"File.Exists failed\"); var x = entry.File.Length;"), banned.Pattern));
    }

    /// <summary>Drops string/char literals and // comments from one line; good enough for a ban list.</summary>
    private static string StripCommentsAndStrings(string line)
    {
        var result = new System.Text.StringBuilder(line.Length);
        for (var index = 0; index < line.Length; index++)
        {
            var character = line[index];
            if (character == '/' && index + 1 < line.Length && line[index + 1] == '/') break;
            if (character == '"' || character == '\'')
            {
                var verbatim = character == '"' && index > 0 && line[index - 1] == '@';
                index++;
                while (index < line.Length && line[index] != character)
                {
                    if (!verbatim && line[index] == '\\') index++;
                    index++;
                }
                result.Append(' ');
                continue;
            }
            result.Append(character);
        }
        return result.ToString();
    }
}
