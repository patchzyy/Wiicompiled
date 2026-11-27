using System.Text.RegularExpressions;
using Translator.Core.IO;
using Translator.Core.Loading;
using Translator.Core.Parsing.Dol;
using Translator.Core.Parsing.Rel;

namespace Translator.Core.Mods;

public sealed class BaseManifestBuildResult
{
    public required BaseManifest Manifest { get; init; }
    public required string ManifestPath { get; init; }
    public required string FunctionRangesPath { get; init; }
}

public static partial class BaseManifestBuilder
{
    public static BaseManifestBuildResult BuildAndWrite(
        DolFile dol,
        RelFile rel,
        RelImage relImage,
        ProgramImage image,
        string generatedFunctionsDir,
        string outputDir,
        string manifestFormat,
        string gameId,
        string region,
        string fileStem = "base",
        uint lowMemoryStart = MemoryLayout.RamBase,
        uint lowMemoryEnd = 0x80004000u,
        string dolSourceName = "main.dol",
        string relSourceName = "module.rel",
        BaseTranslationOutputMetadata? translationOutput = null)
    {
        TrackedFs.CreateDirectory(outputDir);

        var sections = new List<BaseSectionMetadata>();
        var dolTextPath = Path.Combine(outputDir, $"{fileStem}_main_text.bin");
        var relTextPath = Path.Combine(outputDir, $"{fileStem}_rel_text.bin");

        var dolTextOffsets = WriteDolExecutableImage(dol, dolTextPath);
        sections.Add(new BaseSectionMetadata(
            "lowmem",
            "runtime-lowmem",
            lowMemoryStart,
            lowMemoryEnd,
            Executable: false,
            Writable: true,
            ImageFile: null,
            ImageOffset: 0));

        foreach (var section in dol.Sections.Where(s => s.Size > 0))
        {
            sections.Add(new BaseSectionMetadata(
                section.Name,
                dolSourceName,
                section.VirtualAddress,
                checked(section.VirtualAddress + section.Size),
                section.IsExecutable,
                section.Kind is SectionKind.Data or SectionKind.Bss,
                section.IsExecutable ? Path.GetFileName(dolTextPath) : null,
                section.IsExecutable && dolTextOffsets.TryGetValue(section.Name, out var imageOffset) ? imageOffset : 0));
        }

        var relTextOffsets = WriteRelExecutableImage(rel, relImage, relTextPath);
        foreach (var section in rel.Sections.Where(s => s.Size > 0))
        {
            var guestStart = relImage.BaseAddress + section.FileOffset;
            sections.Add(new BaseSectionMetadata(
                $"{relSourceName}:{section.Index}",
                relSourceName,
                guestStart,
                checked(guestStart + section.Size),
                section.Executable,
                !section.Executable,
                section.Executable ? Path.GetFileName(relTextPath) : null,
                section.Executable && relTextOffsets.TryGetValue(section.Index, out var imageOffset) ? imageOffset : 0));
        }

        var functions = BuildFunctionRanges(sections, generatedFunctionsDir, translationOutput);
        var manifest = new BaseManifest(
            manifestFormat,
            1,
            gameId,
            region,
            image.Sha256,
            relImage.BaseAddress,
            sections,
            functions,
            $"{fileStem}_function_ranges.json");

        var manifestPath = Path.Combine(outputDir, $"{fileStem}_manifest.json");
        var functionRangesPath = Path.Combine(outputDir, manifest.FunctionRangesFile);

        JsonWrite(manifestPath, manifest);
        JsonWrite(functionRangesPath, functions);

        return new BaseManifestBuildResult
        {
            Manifest = manifest,
            ManifestPath = manifestPath,
            FunctionRangesPath = functionRangesPath
        };
    }

    private static Dictionary<string, uint> WriteDolExecutableImage(DolFile dol, string path)
    {
        var offsets = new Dictionary<string, uint>(StringComparer.OrdinalIgnoreCase);
        using var output = TrackedFs.Create(path);
        foreach (var section in dol.Sections.Where(s => s.IsExecutable && s.Size > 0).OrderBy(s => s.VirtualAddress))
        {
            offsets[section.Name] = checked((uint)output.Position);
            output.Write(section.Data.Span);
        }
        return offsets;
    }

    private static Dictionary<int, uint> WriteRelExecutableImage(RelFile rel, RelImage relImage, string path)
    {
        var offsets = new Dictionary<int, uint>();
        using var output = TrackedFs.Create(path);
        foreach (var section in rel.Sections.Where(s => s.Executable && s.Size > 0).OrderBy(s => s.FileOffset))
        {
            if (section.FileOffset + section.Size > relImage.Data.Length)
            {
                continue;
            }

            offsets[section.Index] = checked((uint)output.Position);
            output.Write(relImage.Data.AsSpan(checked((int)section.FileOffset), checked((int)section.Size)));
        }
        return offsets;
    }

    private static IReadOnlyList<BaseFunctionRangeMetadata> BuildFunctionRanges(
        IReadOnlyList<BaseSectionMetadata> sections,
        string generatedFunctionsDir,
        BaseTranslationOutputMetadata? translationOutput)
    {
        var starts = (translationOutput is null
                ? DiscoverCanonicalGeneratedFunctionStarts(generatedFunctionsDir)
                : DiscoverCanonicalGeneratedFunctionStarts(translationOutput))
            .Where(start => sections.Any(s => s.Executable && start >= s.GuestStart && start < s.GuestEnd))
            .Distinct()
            .OrderBy(x => x)
            .ToList();

        var bySection = sections.Where(s => s.Executable).OrderBy(s => s.GuestStart).ToList();
        var result = new List<BaseFunctionRangeMetadata>(starts.Count);

        foreach (var section in bySection)
        {
            var sectionStarts = starts.Where(start => start >= section.GuestStart && start < section.GuestEnd).ToList();
            for (var i = 0; i < sectionStarts.Count; i++)
            {
                var start = sectionStarts[i];
                var end = i + 1 < sectionStarts.Count ? sectionStarts[i + 1] : section.GuestEnd;
                if (end <= start)
                {
                    continue;
                }

                result.Add(new BaseFunctionRangeMetadata(
                    start,
                    end,
                    $"func_{start:X8}",
                    section.Name,
                    checked(section.ImageOffset + (start - section.GuestStart)),
                    "generated-function-starts-next-start-or-section-end",
                    ["Executable", "BaseFunction", "CanBeOverridden", "HasKnownTranslation"]));
            }
        }

        return result;
    }

    internal static IReadOnlyList<uint> DiscoverCanonicalGeneratedFunctionStarts(string generatedFunctionsDir)
    {
        var files = new List<(uint Start, string Path)>();
        if (!TrackedFs.DirectoryExists(generatedFunctionsDir))
        {
            return Array.Empty<uint>();
        }

        foreach (var path in TrackedFs.EnumerateFiles(generatedFunctionsDir, "*.cpp", SearchOption.AllDirectories))
        {
            var fileName = Path.GetFileNameWithoutExtension(path);
            var match = AddressRegex().Match(fileName);
            if (match.Success && GuestTargetParser.TryParseHexAddress(match.Groups[1].Value, out var start))
            {
                files.Add((start, path));
            }
        }

        // Recursive discovery can emit a standalone translation for a switch
        // case label as well as the real function containing that label. Such
        // aliases must not split the base function range: an executable patch
        // in the case block has to rebuild and override the enclosing function,
        // otherwise its internal goto bypasses the alias overlay entirely.
        var embeddedAliases = new HashSet<uint>();
        foreach (var (ownerStart, path) in files)
        {
            var source = TrackedFs.ReadAllText(path);
            foreach (Match match in LocalLabelRegex().Matches(source))
            {
                if (GuestTargetParser.TryParseHexAddress(match.Groups[1].Value, out var label) &&
                    label > ownerStart)
                {
                    embeddedAliases.Add(label);
                }
            }
        }

        return files
            .Select(f => f.Start)
            .Where(start => !embeddedAliases.Contains(start))
            .Distinct()
            .OrderBy(start => start)
            .ToList();
    }

    internal static IReadOnlyList<uint> DiscoverCanonicalGeneratedFunctionStarts(
        BaseTranslationOutputMetadata translationOutput)
    {
        // Fall-through interior entries (no branch target, so the label check below can't see them)
        // are excluded, or splitting the ranges there would re-home a mod patch onto an entry no
        // caller dispatches to while real callers keep running the enclosing translation unpatched.
        var provenFunctions = translationOutput.Functions
            .Where(static function => !function.InteriorToOtherTranslation)
            .ToList();

        // A separately emitted switch case can share an address with an internal
        // block of its owning translation. The analysis-owned labels retain that
        // relationship without parsing generated C++.
        var embeddedAliases = provenFunctions
            .SelectMany(function => function.LocalLabelAddresses
                .Where(label => label > function.EntryPoint))
            .ToHashSet();

        return provenFunctions
            .Select(function => function.EntryPoint)
            .Where(start => !embeddedAliases.Contains(start))
            .Distinct()
            .OrderBy(start => start)
            .ToList();
    }

    // The manifest and the function-range table are machine-read build inputs, so
    // they are written compact. The shared writer also makes them gated and
    // atomic; File.Create truncated the live file first, so an interrupted run
    // could publish a half-written manifest that later commands would parse.
    private static void JsonWrite<T>(string path, T value) =>
        JsonOutput.WriteIfChanged(path, value, JsonOutput.Compact);

    [GeneratedRegex("([0-9A-Fa-f]{8})")]
    private static partial Regex AddressRegex();

    [GeneratedRegex(@"\bloc_([0-9A-Fa-f]{8})\s*:")]
    private static partial Regex LocalLabelRegex();
}
