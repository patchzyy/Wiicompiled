using System.Text.Json;
using System.Text.Json.Nodes;
using Translator.Core.IO;

namespace Translator.Tests;

/// <summary>
/// The opt-in --provenance-file skip gate of translate-mod, generate-data-init and
/// emit-build-shards, driven through real translator processes like the build scripts drive it.
/// Split over several classes only so xunit runs them in parallel.
/// </summary>
internal static class StepProvenanceScenarios
{
    public static TheoryData<string> Steps => new()
    {
        StepProvenanceWorkspace.TranslateMod,
        StepProvenanceWorkspace.GenerateDataInit,
        StepProvenanceWorkspace.EmitBuildShards
    };

    /// <summary>One output each step always writes.</summary>
    public static string KnownOutput(StepProvenanceWorkspace workspace, string step) => step switch
    {
        StepProvenanceWorkspace.TranslateMod => Path.Combine(workspace.ModOutput, "cpp", "mod_data_patches.cpp"),
        StepProvenanceWorkspace.GenerateDataInit => Path.Combine(workspace.Root, "generated", "data_sections_init.cpp"),
        StepProvenanceWorkspace.EmitBuildShards => Path.Combine(workspace.Root, "generated", "build_shards", "shards.cmake"),
        _ => throw new ArgumentOutOfRangeException(nameof(step))
    };

    /// <summary>Runs the gated step, which must actually execute (not skip) and leave a record.</summary>
    public static StepProvenanceRecord RunAndRecord(StepProvenanceWorkspace workspace, string step)
    {
        var run = workspace.RunChecked(workspace.StepArguments(step));
        Assert.False(run.Skipped(step), $"{step} skipped unexpectedly:\n{run.Output}\n{run.Error}");
        AssertNoTemporaries(workspace);
        return ReadRecord(workspace.RecordPath(step));
    }

    public static void AssertSkips(StepProvenanceWorkspace workspace, string step)
    {
        var run = workspace.RunChecked(workspace.StepArguments(step));
        Assert.True(run.Skipped(step), $"{step} did not skip:\n{run.Output}\n{run.Error}");
    }

    public static void AssertReruns(StepProvenanceWorkspace workspace, string step, string because)
    {
        var run = workspace.RunChecked(workspace.StepArguments(step));
        Assert.False(run.Skipped(step), $"{step} skipped although {because}:\n{run.Output}");
        Assert.Contains($"[translator] {step} must run:", run.Error, StringComparison.Ordinal);
        Assert.True(File.Exists(workspace.RecordPath(step)), "a successful rerun writes a fresh record");
        AssertNoTemporaries(workspace);
        // The fresh record describes the new state, so the next identical run is current again.
        AssertSkips(workspace, step);
    }

    public static StepProvenanceRecord ReadRecord(string path)
    {
        var record = StepProvenance.TryRead(path, out var reason);
        Assert.True(record is not null, reason);
        return record!;
    }

    public static void AssertNoTemporaries(StepProvenanceWorkspace workspace)
    {
        if (!Directory.Exists(workspace.ProvenanceDirectory)) return;
        Assert.Empty(Directory.EnumerateFiles(workspace.ProvenanceDirectory, "*.tmp"));
    }
}

public sealed class StepProvenanceGateTests
{
    public static TheoryData<string> Steps => StepProvenanceScenarios.Steps;

    [Theory]
    [MemberData(nameof(Steps))]
    public void SecondIdenticalRunSkipsWithoutTouchingOutputs(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        var unrecorded = StepProvenanceWorkspace.Capture(Directory.EnumerateFiles(workspace.Root, "*", SearchOption.AllDirectories));
        var record = StepProvenanceScenarios.RunAndRecord(workspace, step);
        // Recording must not change a single byte the step produces.
        var recorded = StepProvenanceWorkspace.Capture(Directory.EnumerateFiles(workspace.Root, "*", SearchOption.AllDirectories)
            .Where(path => !path.StartsWith(workspace.ProvenanceDirectory + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)));
        Assert.Equal(unrecorded.Keys.Order(StringComparer.OrdinalIgnoreCase), recorded.Keys.Order(StringComparer.OrdinalIgnoreCase));
        foreach (var (path, (bytes, _)) in unrecorded) Assert.Equal(bytes, recorded[path].Bytes);
        Assert.Equal(StepProvenance.SchemaVersion, record.SchemaVersion);
        Assert.Equal(step, record.Step);
        Assert.NotNull(record.TranslatorIdentity);
        Assert.NotEmpty(record.Inputs);
        Assert.NotEmpty(record.Outputs);
        Assert.Contains(record.Inputs, input => input.Path == workspace.Project);
        Assert.Contains(record.Outputs, output => output.Path == StepProvenanceScenarios.KnownOutput(workspace, step));
        Assert.Equal(workspace.StepArguments(step), record.Arguments);
        var recordBytes = File.ReadAllBytes(workspace.RecordPath(step));
        var outputs = StepProvenanceWorkspace.Capture(record.Outputs.Select(static output => output.Path));
        Assert.Equal(record.Outputs.Count, outputs.Count);

        var second = workspace.RunChecked(workspace.StepArguments(step));

        Assert.Equal($"MKWCBUILD: {step} is current; skipping", second.Output.Trim());
        Assert.Equal(recordBytes, File.ReadAllBytes(workspace.RecordPath(step)));
        var after = StepProvenanceWorkspace.Capture(outputs.Keys);
        foreach (var (path, (bytes, modified)) in outputs)
        {
            Assert.Equal(bytes, after[path].Bytes);
            Assert.Equal(modified, after[path].Modified);
        }
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void MissingProvenanceReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        File.Delete(workspace.RecordPath(step));
        StepProvenanceScenarios.AssertReruns(workspace, step, "there was no record");
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void CorruptOrForeignProvenanceReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var recordPath = workspace.RecordPath(step);
        var valid = File.ReadAllText(recordPath);
        string Edit(Action<JsonObject> change)
        {
            var node = JsonNode.Parse(valid)!.AsObject();
            change(node);
            return node.ToJsonString();
        }

        var corruptions = new[]
        {
            "{ this is not json",
            "",
            valid[..(valid.Length / 2)],
            Edit(static node => node["SchemaVersion"] = 0),
            Edit(static node => node["SchemaVersion"] = StepProvenance.SchemaVersion + 1),
            Edit(static node => node.Remove("SchemaVersion")),
            Edit(static node => node["UnknownField"] = true),
            Edit(static node => node["Step"] = "some-other-step"),
            Edit(static node => node["TranslatorIdentity"] = new string('0', 64)),
            Edit(static node => node["TranslatorIdentity"] = null),
            Edit(static node => node["WorkingDirectory"] = Path.GetTempPath()),
        };
        foreach (var corruption in corruptions)
        {
            File.WriteAllText(recordPath, corruption);
            StepProvenanceScenarios.AssertReruns(workspace, step, $"the record was '{corruption[..Math.Min(40, corruption.Length)]}'");
        }
    }

    [Fact]
    public void ChangedArgumentsRerun()
    {
        using var workspace = StepProvenanceWorkspace.Create();
        const string step = StepProvenanceWorkspace.TranslateMod;
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var run = workspace.RunChecked(workspace.StepArguments(step, threads: "2"));
        Assert.False(run.Skipped(step), run.Output);
        Assert.Equal(2, StepProvenanceScenarios.ReadRecord(workspace.RecordPath(step)).EffectiveThreads);
        Assert.True(workspace.RunChecked(workspace.StepArguments(step, threads: "2")).Skipped(step));
        Assert.False(workspace.RunChecked(workspace.StepArguments(step, threads: "1")).Skipped(step));
    }
}

public sealed class StepProvenanceInvalidationTests
{
    public static TheoryData<string> Steps => StepProvenanceScenarios.Steps;

    [Theory]
    [MemberData(nameof(Steps))]
    public void ModifiedInputReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var input = step switch
        {
            StepProvenanceWorkspace.TranslateMod => Path.Combine(workspace.Root, "base.json"),
            StepProvenanceWorkspace.GenerateDataInit => Path.Combine(workspace.Root, "MAP.txt"),
            _ => Path.Combine(workspace.Root, "runtime", "src", "hle", "stub.cpp"),
        };
        File.AppendAllText(input, "\n");
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{input} changed");

        // The project file is an input of every step.
        File.AppendAllText(workspace.Project, "# edited\n");
        StepProvenanceScenarios.AssertReruns(workspace, step, "the project file changed");
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void NewFileInAScannedDirectoryReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        var record = StepProvenanceScenarios.RunAndRecord(workspace, step);
        var added = step == StepProvenanceWorkspace.GenerateDataInit
            ? Path.Combine(workspace.Root, "generated", "data_sections_init_blobs", "added.bin")
            : Path.Combine(workspace.Root, "runtime", "src", "hle", "added.cpp");
        Assert.Contains(record.Scans, scan => scan.Directory == Path.GetDirectoryName(added) ||
                                              (scan.Recursive && added.StartsWith(scan.Directory + Path.DirectorySeparatorChar)));
        File.WriteAllText(added, "// added after the record\n");
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{added} was added");
        if (step == StepProvenanceWorkspace.GenerateDataInit)
            Assert.False(File.Exists(added), "the rerun prunes the stale blob it was invalidated by");
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void DeletedOutputReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var output = StepProvenanceScenarios.KnownOutput(workspace, step);
        var bytes = File.ReadAllBytes(output);
        File.Delete(output);
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{output} was deleted");
        Assert.Equal(bytes, File.ReadAllBytes(output));
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void ResizedOrTruncatedOutputReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var output = StepProvenanceScenarios.KnownOutput(workspace, step);
        var bytes = File.ReadAllBytes(output);

        File.WriteAllBytes(output, bytes[..(bytes.Length / 2)]);
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{output} was truncated");
        Assert.Equal(bytes, File.ReadAllBytes(output));

        File.WriteAllBytes(output, [.. bytes, (byte)'\n']);
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{output} grew");
        Assert.Equal(bytes, File.ReadAllBytes(output));
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void SameSizeEditedOutputReruns(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        var output = StepProvenanceScenarios.KnownOutput(workspace, step);
        var bytes = File.ReadAllBytes(output);

        var edited = bytes.ToArray();
        edited[0] ^= 0x20;
        File.WriteAllBytes(output, edited);
        File.SetLastWriteTimeUtc(output, File.GetLastWriteTimeUtc(output).AddMinutes(1));
        StepProvenanceScenarios.AssertReruns(workspace, step, $"{output} was edited in place");
        Assert.Equal(bytes, File.ReadAllBytes(output));
    }
}

public sealed class StepProvenanceSafetyTests
{
    public static TheoryData<string> Steps => StepProvenanceScenarios.Steps;

    [Theory]
    [MemberData(nameof(Steps))]
    public void FailureMidStepLeavesNoProvenance(string step)
    {
        using var workspace = StepProvenanceWorkspace.Create();
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        Assert.True(File.Exists(workspace.RecordPath(step)));

        // Each break lets the step start (project load succeeds) and fail part-way through.
        switch (step)
        {
            case StepProvenanceWorkspace.TranslateMod:
                File.WriteAllText(Path.Combine(workspace.Root, "base.json"), "{ truncated");
                break;
            case StepProvenanceWorkspace.GenerateDataInit:
                File.WriteAllBytes(workspace.Dol, [0x00, 0x01, 0x02]);
                break;
            default:
                File.WriteAllBytes(Path.Combine(workspace.Root, "generated", "base_translation_sources.bin"), [0x42, 0x42, 0x42, 0x42]);
                break;
        }

        var failed = workspace.RunStep(step);

        Assert.NotEqual(0, failed.ExitCode);
        Assert.False(failed.Skipped(step));
        Assert.False(File.Exists(workspace.RecordPath(step)), "a failed step must not leave a record behind");
        StepProvenanceScenarios.AssertNoTemporaries(workspace);
        // And it stays that way: the next attempt runs (and fails) again rather than skipping.
        var again = workspace.RunStep(step);
        Assert.NotEqual(0, again.ExitCode);
        Assert.False(again.Skipped(step));
        Assert.False(File.Exists(workspace.RecordPath(step)));
    }

    [Fact]
    public void ARecordIsDeletedBeforeTheStepRuns()
    {
        using var workspace = StepProvenanceWorkspace.Create();
        const string step = StepProvenanceWorkspace.GenerateDataInit;
        StepProvenanceScenarios.RunAndRecord(workspace, step);
        // A stale record that no longer matches is gone even when the step dies before finishing.
        File.AppendAllText(Path.Combine(workspace.Root, "MAP.txt"), "\n");
        File.WriteAllBytes(workspace.Dol, [0x00]);
        var run = workspace.RunStep(step);
        Assert.NotEqual(0, run.ExitCode);
        Assert.Contains($"[translator] {step} must run:", run.Error, StringComparison.Ordinal);
        Assert.False(File.Exists(workspace.RecordPath(step)));
    }

    [Theory]
    [MemberData(nameof(Steps))]
    public void WithoutTheFlagNothingIsRecordedOrSkipped(string step)
    {
        // Create() already ran every step once without the flag.
        using var workspace = StepProvenanceWorkspace.Create();
        Assert.False(Directory.Exists(workspace.ProvenanceDirectory));
        var run = workspace.RunChecked(workspace.StepArguments(step, gated: false));
        Assert.DoesNotContain("MKWCBUILD", run.Output, StringComparison.Ordinal);
        Assert.DoesNotContain("must run", run.Error, StringComparison.Ordinal);
        Assert.False(Directory.Exists(workspace.ProvenanceDirectory));
    }

    [Fact]
    public void ProvenanceFlagIsOnlyAcceptedByTheGatedSteps()
    {
        using var workspace = StepProvenanceWorkspace.Create(prepare: false);
        var run = workspace.Run(["emit-base-manifest", "--project", workspace.Project, "--provenance-file", workspace.RecordPath("x")]);
        Assert.Equal(2, run.ExitCode);
        Assert.Contains("Unrecognized option '--provenance-file'", run.Error, StringComparison.Ordinal);
        var empty = workspace.Run(["generate-data-init", "--project", workspace.Project, "--provenance-file", " "]);
        Assert.NotEqual(0, empty.ExitCode);
        Assert.False(Directory.Exists(workspace.ProvenanceDirectory));
    }
}
