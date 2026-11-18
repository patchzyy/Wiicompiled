using System.Reflection;
using System.Diagnostics;
using System.Threading.Tasks;
using Translator.Cli.Configuration;
using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public sealed class TranslateRecursiveRuntimeConfigTests
{
    [Fact]
    public void BoundedIrMatchesBufferedFilesMetadataAndBundleAcrossThreadCounts()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-bounded-ir-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            File.WriteAllBytes(Path.Combine(root, "main.dol"), SyntheticDolFactory.CreateBytes(entry,
                sections:
                [
                    SyntheticDolFactory.Text(0, entry,
                        0x38A30001u, // addi r5,r3,1
                        0x4800001Du, // bl 0x80001020
                        0x38650001u, // addi r3,r5,1
                        0x4E800020u),
                    SyntheticDolFactory.Text(1, entry + 0x20,
                        0x38C40001u, // addi r6,r4,1
                        0x4BFFFFDDu, // bl 0x80001000
                        0x38660001u, // addi r3,r6,1
                        0x4E800020u)
                ]));
            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(projectPath,
                """
                schema_version: 1
                project:
                  id: bounded-ir-test
                memory:
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000, 0x80001020]
                output:
                  root: generated
                """);

            (Dictionary<string, string> Sources, byte[] Metadata, byte[] Bundle) Translate(
                string name, int threads, bool bounded, bool emitFiles = true)
            {
                var output = Path.Combine(root, name);
                var functions = Path.Combine(output, "functions");
                var metadata = Path.Combine(output, "metadata.json");
                var bundle = Path.Combine(output, "sources.bundle");
                var args = new List<string>
                {
                    "translate-recursive", $"0x{entry:X8}", "--project", projectPath,
                    "--outdir", functions, "--output-metadata", metadata,
                    "--production-source-bundle", bundle, "--threads", threads.ToString()
                };
                if (bounded) args.Add("--bounded-ir");
                if (!emitFiles) args.Add("--no-function-files");
                var run = InvokeCliProcess(root, args.ToArray());
                Assert.Equal(0, run.ExitCode);
                Assert.Contains(
                    $"[translator] Canonical IR mode: {(bounded ? "bounded" : "buffered")}.",
                    run.Output, StringComparison.Ordinal);
                var sources = Directory.Exists(functions)
                    ? Directory.GetFiles(functions, "*.cpp").ToDictionary(
                        static path => Path.GetFileName(path), static path => File.ReadAllText(path))
                    : new Dictionary<string, string>();
                return (sources, File.ReadAllBytes(metadata), File.ReadAllBytes(bundle));
            }

            var buffered = Translate("buffered", 1, bounded: false);
            Assert.Equal(2, buffered.Sources.Count);

            var metadataCollision = InvokeCliProcess(root, "translate-recursive", "--project", projectPath,
                $"0x{entry:X8}", "--outdir", "--output-metadata", "--bounded-ir",
                "--output-metadata", "result.json", "--output-metadata", "ignored.json", "--threads", "0x1");
            Assert.Equal(0, metadataCollision.ExitCode);
            Assert.Contains("[translator] Canonical IR mode: bounded.", metadataCollision.Output, StringComparison.Ordinal);
            Assert.Contains("[translator] Using up to 1 worker thread(s).", metadataCollision.Output, StringComparison.Ordinal);
            Assert.Equal(2, BaseTranslationOutputMetadataFile.Read(Path.Combine(root, "result.json")).Functions.Count);
            Assert.Equal(2, Directory.GetFiles(Path.Combine(root, "--output-metadata"), "*.cpp").Length);
            Assert.False(File.Exists(Path.Combine(root, "ignored.json")));
            Assert.False(File.Exists(Path.Combine(root, "--bounded-ir")));

            var flagOperands = InvokeCliProcess(root, "translate-recursive", $"0x{entry:X8}",
                "--outdir", "--bounded-ir", "--output-metadata", "--project",
                "--production-source-bundle", "--no-function-files", "--threads", "-1", "--project", projectPath);
            Assert.Equal(0, flagOperands.ExitCode);
            Assert.Contains("[translator] Canonical IR mode: buffered.", flagOperands.Output, StringComparison.Ordinal);
            Assert.Contains("[translator] Using up to 1 worker thread(s).", flagOperands.Output, StringComparison.Ordinal);
            Assert.Equal(2, Directory.GetFiles(Path.Combine(root, "--bounded-ir"), "*.cpp").Length);
            Assert.Equal(2, BaseTranslationOutputMetadataFile.Read(Path.Combine(root, "--project")).Functions.Count);
            Assert.True(File.Exists(Path.Combine(root, "--no-function-files")));

            foreach (var threads in new[] { 1, 3 })
            {
                var lazy = Translate($"lazy-{threads}", threads, bounded: true);
                Assert.Equal(buffered.Sources.Keys.OrderBy(static name => name),
                    lazy.Sources.Keys.OrderBy(static name => name));
                foreach (var pair in buffered.Sources) Assert.Equal(pair.Value, lazy.Sources[pair.Key]);
                Assert.Equal(buffered.Metadata, lazy.Metadata);
                Assert.Equal(buffered.Bundle, lazy.Bundle);
            }
            var bundleOnly = Translate("bundle-only", 3, bounded: true, emitFiles: false);
            Assert.Empty(bundleOnly.Sources);
            Assert.Equal(buffered.Metadata, bundleOnly.Metadata);
            Assert.Equal(buffered.Bundle, bundleOnly.Bundle);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Theory]
    [InlineData("--unknown", "--help")]
    [InlineData("--unknown", "-h")]
    [InlineData("--outdir", "-?")]
    public void CommandHelpTakesPrecedenceOverOtherArguments(string option, string help)
    {
        var run = InvokeCliProcess(Path.GetTempPath(), "translate-recursive", option, help);
        Assert.Equal(0, run.ExitCode);
        Assert.Contains("translator translate-recursive <start_addr>", run.Output, StringComparison.Ordinal);
        Assert.Contains("[--bounded-ir]", run.Output, StringComparison.Ordinal);
        Assert.Empty(run.Error);
    }

    [Fact]
    public void UnsupportedOpcodeFailsStrictTranslation()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-unsupported-opcode-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections: [SyntheticDolFactory.Text(
                        0,
                        entry,
                        0x04000000u, // unassigned primary opcode; deliberately unsupported by the lifter
                        0x4E800020u)])); // blr
            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: unsupported-opcode-test
                memory:
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                output:
                  root: generated
                """);
            Assert.False(TranslationProjectConfig.Load(projectPath).Translation.AllowUnsupportedInstructions);

            var strictError = Assert.Throws<TargetInvocationException>(() => InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project", projectPath,
                "--outdir", Path.Combine(root, "strict", "functions"),
                "--output-metadata", Path.Combine(root, "strict", "metadata.json"),
                "--threads", "1"));
            Assert.Contains("UNIMPLEMENTED", strictError.InnerException?.ToString(), StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void RepeatedRunsReuseUnchangedOutputsAndPruneStaleFiles()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-recursive-prune-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            const uint entry = 0x80001000u;
            File.WriteAllBytes(
                Path.Combine(root, "main.dol"),
                SyntheticDolFactory.CreateBytes(
                    entry,
                    sections: [SyntheticDolFactory.Text(
                        0,
                        entry,
                        0x60000000u, // nop
                        0x60000000u, // nop
                        0x4E800020u)])); // blr
            var projectPath = Path.Combine(root, "recomp.yml");
            File.WriteAllText(
                projectPath,
                """
                schema_version: 1
                project:
                  id: recursive-prune-test
                  display_name: Recursive Prune Test
                memory:
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                translation:
                  entry_points: [0x80001000]
                output:
                  root: generated
                """);

            var stagedFunctions = Path.Combine(root, "staging", "functions");
            var stagedOutputMetadata = Path.Combine(root, "staging", "functions_metadata.json");
            var exitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--threads",
                "1");

            Assert.Equal(0, exitCode);
            var firstMetadata = BaseTranslationOutputMetadataFile.Read(stagedOutputMetadata);
            Assert.Null(firstMetadata.TranslationIdentityHash);
            Assert.Equal(0, firstMetadata.Quality.UnsupportedInstructionCount);
            Assert.Equal(0, firstMetadata.Quality.InvalidSsaFunctionCount);
            Assert.Single(firstMetadata.Functions);
            Assert.Single(Directory.GetFiles(stagedFunctions, "*.cpp"));
            var generatedPath = Directory.GetFiles(stagedFunctions, "*.cpp").Single();
            var unchangedTimestamp = new DateTime(2004, 5, 6, 7, 8, 10, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(generatedPath, unchangedTimestamp);
            // A populated output tree is not an input to recursive discovery.
            // This stale filename used to become a synthetic function boundary
            // and truncate the second translation at entry + 4.
            File.WriteAllText(Path.Combine(stagedFunctions, $"func_{entry + 4:X8}.cpp"), "stale output hint");
            var metadataTimestamp = new DateTime(2004, 5, 6, 7, 8, 12, DateTimeKind.Utc);
            File.SetLastWriteTimeUtc(stagedOutputMetadata, metadataTimestamp);

            var secondExitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--threads",
                "1");

            Assert.Equal(0, secondExitCode);
            Assert.Equal(unchangedTimestamp, File.GetLastWriteTimeUtc(generatedPath));
            Assert.Equal(metadataTimestamp, File.GetLastWriteTimeUtc(stagedOutputMetadata));

            var stalePath = Path.Combine(stagedFunctions, "func_80001004.cpp");
            var unlistedPath = Path.Combine(stagedFunctions, "unlisted.cpp");
            File.WriteAllText(stalePath, "// stale");
            File.WriteAllText(unlistedPath, "// not translator-owned");
            BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(
                stagedOutputMetadata,
                BaseTranslationOutputMetadata.Create(
                    firstMetadata.Functions.Concat([
                        new BaseTranslationFunctionMetadata(
                            "func_80001004.cpp",
                            8,
                            new string('e', 64),
                            0x80001004u,
                            [])
                    ]),
                    TranslationQualityMetadata.Clean));

            var pruneExitCode = InvokeCli(
                "translate-recursive",
                $"0x{entry:X8}",
                "--project",
                projectPath,
                "--outdir",
                stagedFunctions,
                "--output-metadata",
                stagedOutputMetadata,
                "--prune-stale",
                "--threads",
                "1");
            Assert.Equal(0, pruneExitCode);
            Assert.False(File.Exists(stalePath));
            Assert.True(File.Exists(unlistedPath));
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    private static (int ExitCode, string Output, string Error) InvokeCliProcess(string workingDirectory, params string[] args)
    {
        var startInfo = new ProcessStartInfo("dotnet")
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            ArgumentList = { typeof(TranslationProjectConfig).Assembly.Location }
        };
        foreach (var arg in args) startInfo.ArgumentList.Add(arg);

        using var process = Process.Start(startInfo)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(30_000))
        {
            process.Kill(entireProcessTree: true);
            process.WaitForExit();
            throw new TimeoutException("Translator subprocess timed out.");
        }
        return (process.ExitCode, output.GetAwaiter().GetResult(), error.GetAwaiter().GetResult());
    }

    private static int InvokeCli(params string[] args)
    {
        var entryPoint = typeof(TranslationProjectConfig).Assembly.EntryPoint;
        Assert.NotNull(entryPoint);
        var result = entryPoint!.Invoke(null, new object?[] { args });
        return result switch
        {
            int exitCode => exitCode,
            Task<int> intTask => intTask.GetAwaiter().GetResult(),
            Task task => CompleteTask(task),
            null => 0,
            _ => throw new InvalidOperationException($"Unexpected CLI entry point result type: {result.GetType().FullName}")
        };
    }

    private static int CompleteTask(Task task)
    {
        task.GetAwaiter().GetResult();
        return 0;
    }
}
