using System.Buffers.Binary;
using System.Diagnostics;
using System.Text.Json;
using Translator.Cli.Configuration;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;

namespace Translator.Tests;

/// <summary>
/// A throwaway workspace laid out like the real one (project, pinned DOL/REL, function map,
/// runtime sources, a mod pack, a base translation) that runs the same translator steps the
/// local build scripts run, each in its own translator process.
/// </summary>
internal sealed class StepProvenanceWorkspace : IDisposable
{
    public const string TranslateMod = "translate-mod";
    public const string GenerateDataInit = "generate-data-init";
    public const string EmitBuildShards = "emit-build-shards";

    private const uint Caller = 0x80004000;
    private const uint Module = 0x80010000;

    private StepProvenanceWorkspace(string root) => Root = root;

    public string Root { get; }
    public string Project => Path.Combine(Root, "recomp.yml");
    public string Dol => Path.Combine(Root, "main.dol");
    public string Rel => Path.Combine(Root, "empty.rel");
    public string ProvenanceDirectory => Path.Combine(Root, "generated", "step-provenance");
    public string ModOutput => Path.Combine(Root, "build", "mods", "mod_cpp");

    public string RecordPath(string step) => Path.Combine(ProvenanceDirectory, step + ".json");

    /// <summary>A fresh workspace; with <paramref name="prepare"/> the base is translated and every gated step has run once, ungated.</summary>
    public static StepProvenanceWorkspace Create(bool prepare = true)
    {
        var workspace = new StepProvenanceWorkspace(
            Path.Combine(Path.GetTempPath(), $"translator-provenance-{Guid.NewGuid():N}"));
        try
        {
            workspace.WriteSources();
            if (prepare)
            {
                workspace.RunChecked(workspace.TranslateBaseArguments());
                foreach (var step in new[] { TranslateMod, GenerateDataInit, EmitBuildShards })
                    workspace.RunChecked(workspace.StepArguments(step, gated: false));
            }
            return workspace;
        }
        catch
        {
            workspace.Dispose();
            throw;
        }
    }

    public void Dispose()
    {
        if (Directory.Exists(Root)) Directory.Delete(Root, recursive: true);
    }

    public string[] TranslateBaseArguments() =>
    [
        "translate-recursive", $"0x{Caller:X8}", "--project", Project,
        "--outdir", Path.Combine(Root, "generated", "functions"),
        "--output-metadata", Path.Combine(Root, "generated", "base_translation_output.json"),
        "--production-source-bundle", Path.Combine(Root, "generated", "base_translation_sources.bin"),
        "--no-function-files", "--prune-stale", "--threads", "1"
    ];

    /// <summary>The step exactly as Launcher/local-build.sh runs it, plus --provenance-file when gated.</summary>
    public string[] StepArguments(string step, bool gated = true, string threads = "1")
    {
        var arguments = step switch
        {
            TranslateMod => new List<string>
            {
                "translate-mod", "--project", Project, "--profile", "mod",
                "--base-manifest", Path.Combine(Root, "base.json"),
                "--base-translation-output-metadata", Path.Combine(Root, "generated", "base_translation_output.json"),
                "--code-pul", Path.Combine(Root, "Code.pul"), "--mod-root", Path.Combine(Root, "mod"),
                "--mod-name", "Provenance Test", "--region", "P", "--out", ModOutput,
                "--prefer-cached-inputs", "--emit-cpp", "--threads", threads, "--skip-retro-wfc"
            },
            GenerateDataInit => new List<string> { "generate-data-init", "--project", Project },
            EmitBuildShards => new List<string>
            {
                "emit-build-shards", "--project", Project,
                "--base-metadata", Path.Combine(Root, "generated", "base_translation_output.json"),
                "--base-functions-dir", Path.Combine(Root, "generated", "functions"),
                "--native-source-dir", Path.Combine(Root, "runtime", "src"),
                "--out", Path.Combine(Root, "generated", "build_shards"),
                "--resolved-profile", Path.Combine(ModOutput, "resolved_dispatch_profile.json"),
                "--retro-cpp-dir", Path.Combine(ModOutput, "cpp")
            },
            _ => throw new ArgumentOutOfRangeException(nameof(step), step, null)
        };
        if (gated)
        {
            arguments.Add("--provenance-file");
            arguments.Add(RecordPath(step));
        }
        return arguments.ToArray();
    }

    public CliRun RunStep(string step, bool gated = true) => Run(StepArguments(step, gated));

    public CliRun RunChecked(string[] arguments)
    {
        var run = Run(arguments);
        Assert.True(run.ExitCode == 0, $"{string.Join(' ', arguments)} failed ({run.ExitCode}):\n{run.Output}\n{run.Error}");
        return run;
    }

    public CliRun Run(string[] arguments, IReadOnlyDictionary<string, string>? environment = null) =>
        CliRun.Invoke(Root, arguments, environment);

    /// <summary>Every file the step's last run left behind, by path, with its bytes and modification time.</summary>
    public static Dictionary<string, (byte[] Bytes, DateTime Modified)> Capture(IEnumerable<string> paths) =>
        paths.Where(File.Exists).ToDictionary(
            static path => path,
            static path => (File.ReadAllBytes(path), File.GetLastWriteTimeUtc(path)),
            StringComparer.OrdinalIgnoreCase);

    private void WriteSources()
    {
        Directory.CreateDirectory(Root);
        // Caller + 24 holds a real instruction so the hook's continuation has somewhere to resume.
        uint[] caller = [0x60000000u, 0x38630001u, 0x38630001u, 0x38630001u,
            0x38630001u, 0x38630001u, 0x38630002u, 0x4E800020u];
        var baseWords = Enumerable.Repeat(0x4E800020u, 8).ToArray();
        caller.CopyTo(baseWords, 0);
        var baseBytes = Words(baseWords);
        File.WriteAllBytes(Dol, SyntheticDolFactory.CreateBytes(
            Caller,
            sections:
            [
                SyntheticDolFactory.Text(0, Caller, baseWords),
                SyntheticDolFactory.Data(1, 0x80006000, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88)
            ]));
        File.WriteAllBytes(Path.Combine(Root, "base.bin"), baseBytes);
        // translate-mod needs a REL; this one has no code and no relocations.
        File.WriteAllBytes(Rel, new byte[0x48]);
        File.WriteAllText(Path.Combine(Root, "MAP.txt"), $"{Caller:X8} caller\n");

        var manifest = new BaseManifest("synthetic", 1, "TEST01", "P", "", 0,
            [new BaseSectionMetadata(".text", "main.dol", Caller, Caller + (uint)baseBytes.Length, true, false, "base.bin", 0)],
            [new BaseFunctionRangeMetadata(Caller, Caller + 32, "caller", ".text", 0, "synthetic", ["Executable"])],
            "ranges.json");
        File.WriteAllText(Path.Combine(Root, "base.json"), JsonSerializer.Serialize(manifest));

        Directory.CreateDirectory(Path.Combine(Root, "runtime", "src", "hle"));
        File.WriteAllText(Path.Combine(Root, "runtime", "src", "hle", "stub.cpp"), "// native sources the runtime scan reads\nint stub() { return 0; }\n");
        File.WriteAllText(Path.Combine(Root, "runtime", "src", "notes.txt"), "not a native source\n");
        Directory.CreateDirectory(Path.Combine(Root, "runtime", "include", "guest_abi"));
        File.WriteAllText(Path.Combine(Root, "runtime", "include", "guest_abi", "guest_versions.inc"),
            "// one regional range\nMKW_REGION_RANGE(NtscU, 80000000, 8001FFFF, +0x0)\n");
        Directory.CreateDirectory(Path.Combine(Root, "mod", "files"));
        File.WriteAllText(Path.Combine(Root, "mod", "files", "readme.txt"), "pack contents\n");

        // One blr-skipping hook at the caller, as KamekLrContinuationIntegrationTests builds it.
        var moduleWords = Enumerable.Repeat(0x4E800020u, 0x40 / 4 + 4).ToArray();
        new uint[] { 0x7D8802A6u, 0x398C0014u, 0x7D8803A6u, 0x4E800020u }.CopyTo(moduleWords, 0x40 / 4);
        var commands = new uint[] { ((uint)KamekCommandId.BranchLink << 24) | 0x00FFFFFEu, Caller, 0x40u };
        var code = Words(moduleWords);
        var commandBytes = Words(commands);
        var pul = new byte[KamekChunk.HeaderSize + code.Length + commandBytes.Length];
        WriteWord(pul, 0, KamekChunk.Magic0);
        WriteWord(pul, 4, KamekChunk.Magic1);
        WriteWord(pul, 12, (uint)code.Length);
        WriteWord(pul, 24, (uint)pul.Length);
        code.CopyTo(pul, KamekChunk.HeaderSize);
        commandBytes.CopyTo(pul, KamekChunk.HeaderSize + code.Length);
        File.WriteAllBytes(Path.Combine(Root, "Code.pul"), pul);

        File.WriteAllText(Project, $"""
            schema_version: 1
            project:
              id: provenance-test
            memory:
              base: 0x80000000
              size: 0x00020000
              sda_base: 0x80002000
              sda2_base: 0x80003000
            inputs:
              dol:
                path: main.dol
              rel:
                path: empty.rel
                load_address: 0x80008000
            translation:
              entry_points:
                - 0x{Caller:X8}
              function_map:
                path: MAP.txt
            runtime:
              native_registration_root: runtime/src
              native_abi_directories:
                - runtime/src/hle
            output:
              root: generated
            profiles:
              mod:
                enabled: true
                code_pul: Code.pul
                mod_root: mod
                region: P
                module_guest_base: 0x{Module:X8}
                module_link_base: 0x{Module:X8}
                output: build/mods/mod_cpp
            """);
    }

    private static byte[] Words(uint[] words)
    {
        var bytes = new byte[words.Length * 4];
        for (var index = 0; index < words.Length; index++) WriteWord(bytes, index * 4, words[index]);
        return bytes;
    }

    private static void WriteWord(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);
}

internal sealed record CliRun(int ExitCode, string Output, string Error)
{
    public static CliRun Invoke(string workingDirectory, string[] arguments, IReadOnlyDictionary<string, string>? environment = null)
    {
        var startInfo = new ProcessStartInfo("dotnet")
        {
            WorkingDirectory = workingDirectory,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            ArgumentList = { typeof(TranslationProjectConfig).Assembly.Location }
        };
        foreach (var argument in arguments) startInfo.ArgumentList.Add(argument);
        startInfo.Environment.Remove(Translator.Core.IO.StepProvenance.AccessLogEnvironmentVariable);
        foreach (var (name, value) in environment ?? new Dictionary<string, string>()) startInfo.Environment[name] = value;

        using var process = Process.Start(startInfo)!;
        var output = process.StandardOutput.ReadToEndAsync();
        var error = process.StandardError.ReadToEndAsync();
        if (!process.WaitForExit(120_000))
        {
            process.Kill(entireProcessTree: true);
            process.WaitForExit();
            throw new TimeoutException($"Translator subprocess timed out: {string.Join(' ', arguments)}");
        }
        return new CliRun(process.ExitCode, output.GetAwaiter().GetResult(), error.GetAwaiter().GetResult());
    }

    public bool Skipped(string step) =>
        Output.Contains($"MKWCBUILD: {step} is current; skipping", StringComparison.Ordinal);
}
