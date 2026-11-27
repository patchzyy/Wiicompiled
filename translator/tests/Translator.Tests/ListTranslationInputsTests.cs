using Translator.Core.IO;

namespace Translator.Tests;

public sealed class ListTranslationInputsTests
{
    [Fact]
    public void ListCoversEveryFileTheBaseTranslationOpensExceptTheDiscPins()
    {
        using var workspace = StepProvenanceWorkspace.Create(prepare: false);
        // A serialized native index makes translation open it as well as the sources it digests.
        workspace.RunChecked(["emit-native-index", "--project", workspace.Project,
            "--native-source-dir", Path.Combine(workspace.Root, "runtime", "src"),
            "--out", Path.Combine(workspace.Root, "runtime", "native-index.json")]);
        var accessLog = Path.Combine(workspace.Root, "access-log.json");
        var translate = workspace.Run(workspace.TranslateBaseArguments(),
            new Dictionary<string, string> { [StepProvenance.AccessLogEnvironmentVariable] = accessLog });
        Assert.True(translate.ExitCode == 0, translate.Output + translate.Error);
        var opened = StepProvenanceScenarios.ReadRecord(accessLog).Inputs.Select(static input => input.Path).ToArray();

        var listed = ListInputs(workspace);

        string[] discPins = [workspace.Dol, workspace.Rel];
        Assert.All(discPins, pin => Assert.Contains(pin, opened));
        var uncovered = opened.Where(path => !discPins.Contains(path) && !listed.Contains(path)).ToArray();
        Assert.True(uncovered.Length == 0,
            "translate-recursive opened files list-translation-inputs does not report:" +
            Environment.NewLine + string.Join(Environment.NewLine, uncovered));
        Assert.DoesNotContain(workspace.Dol, listed);
        Assert.DoesNotContain(workspace.Rel, listed);
        // The project graph itself: manifest, function map, the profile's Code.pul, the runtime
        // index, the native sources and the guest address map.
        foreach (var expected in new[]
                 {
                     workspace.Project,
                     Path.Combine(workspace.Root, "MAP.txt"),
                     Path.Combine(workspace.Root, "Code.pul"),
                     Path.Combine(workspace.Root, "runtime", "native-index.json"),
                     Path.Combine(workspace.Root, "runtime", "src", "hle", "stub.cpp"),
                     Path.Combine(workspace.Root, "runtime", "include", "guest_abi", "guest_versions.inc"),
                 })
        {
            Assert.Contains(expected, listed);
            Assert.Contains(expected, opened);
        }
    }

    [Fact]
    public void ListNeedsNoDiscPinsAndFollowsTheOutputContract()
    {
        using var workspace = StepProvenanceWorkspace.Create(prepare: false);
        var withPins = ListInputs(workspace);
        File.Delete(workspace.Dol);
        File.Delete(workspace.Rel);

        var run = workspace.Run(["list-translation-inputs", "--project", workspace.Project]);

        Assert.Equal(0, run.ExitCode);
        Assert.Equal(string.Empty, run.Error);
        var lines = run.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(static line => line.TrimEnd('\r')).ToArray();
        Assert.Equal(withPins, lines);
        Assert.Equal(lines.Order(StringComparer.Ordinal), lines);
        Assert.Equal(lines.Length, lines.Distinct(StringComparer.Ordinal).Count());
        Assert.All(lines, line =>
        {
            Assert.True(Path.IsPathFullyQualified(line), line);
            Assert.Equal(Path.GetFullPath(line), line);
            Assert.True(File.Exists(line), line);
        });
    }

    [Fact]
    public void RegionalProjectListsItsPortMetadataWithoutGameFiles()
    {
        var repository = ProjectPathsForTests.FindRepositoryRoot();
        var project = Path.Combine(repository, "projects", "mkwii-ntsc-u", "recomp.yml");
        var run = CliRun.Invoke(repository, ["list-translation-inputs", "--project", project]);
        Assert.True(run.ExitCode == 0, run.Error);
        var lines = run.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(static line => line.TrimEnd('\r')).ToArray();
        foreach (var name in new[] { "recomp.yml", "MAP.txt", "region_port.json", "data_addresses.txt" })
            Assert.Contains(Path.Combine(repository, "projects", "mkwii-ntsc-u", name), lines);
        Assert.Contains(Path.Combine(repository, "runtime", "include", "guest_abi", "guest_versions.inc"), lines);
        Assert.Contains(lines, line => line.StartsWith(Path.Combine(repository, "runtime", "src") + Path.DirectorySeparatorChar, StringComparison.Ordinal));
        Assert.DoesNotContain(lines, line => line.EndsWith("main.dol", StringComparison.OrdinalIgnoreCase) ||
                                             line.EndsWith("StaticR.rel", StringComparison.OrdinalIgnoreCase));
    }

    [Fact]
    public void ListFailsWithoutAUsableProject()
    {
        using var workspace = StepProvenanceWorkspace.Create(prepare: false);
        var missing = workspace.Run(["list-translation-inputs", "--project", Path.Combine(workspace.Root, "absent.yml")]);
        Assert.NotEqual(0, missing.ExitCode);
        Assert.Equal(string.Empty, missing.Output);

        var noProject = workspace.Run(["list-translation-inputs"]);
        Assert.NotEqual(0, noProject.ExitCode);
        Assert.Equal(string.Empty, noProject.Output);

        File.WriteAllText(workspace.Project, "schema_version: 2\n");
        var unsupported = workspace.Run(["list-translation-inputs", "--project", workspace.Project]);
        Assert.NotEqual(0, unsupported.ExitCode);
        Assert.Equal(string.Empty, unsupported.Output);

        File.WriteAllText(workspace.Project, "schema_version: [1\n  bad: yaml");
        var unreadable = workspace.Run(["list-translation-inputs", "--project", workspace.Project]);
        Assert.NotEqual(0, unreadable.ExitCode);
        Assert.Equal(string.Empty, unreadable.Output);
    }

    private static string[] ListInputs(StepProvenanceWorkspace workspace)
    {
        var run = workspace.Run(["list-translation-inputs", "--project", workspace.Project]);
        Assert.True(run.ExitCode == 0, run.Error);
        return run.Output.Split('\n', StringSplitOptions.RemoveEmptyEntries).Select(static line => line.TrimEnd('\r')).ToArray();
    }
}
