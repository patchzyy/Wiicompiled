using System.Text;
using System.Text.Json;
using Translator.Core.IO;
using Translator.Core.Translation;
using Translator.Core.Loading;

namespace Translator.Core.Mods;

/// <summary>Compact, deterministic description of a base translation output tree, the source of truth
/// for downstream manifests and pruning so they don't need to parse tens of thousands of C++ files.</summary>
public sealed record BaseTranslationOutputMetadata(
    string Format,
    int FormatVersion,
    string? TranslationIdentityHash,
    TranslationQualityMetadata Quality,
    IReadOnlyList<BaseTranslationFunctionMetadata> Functions,
    string? SourceBundlePath = null,
    IReadOnlyList<BaseTranslationModPatchAwareness>? ModPatchAwareness = null)
{
    public const string CurrentFormat = "mkw-base-translation-output";
    public const int CurrentFormatVersion = 2;

    public static BaseTranslationOutputMetadata Create(
        IEnumerable<BaseTranslationFunctionMetadata> functions,
        TranslationQualityMetadata quality,
        string? translationIdentityHash = null,
        string? sourceBundlePath = null,
        IReadOnlyList<BaseTranslationModPatchAwareness>? modPatchAwareness = null)
    {
        var ordered = functions
            .OrderBy(function => function.RelativePath, StringComparer.Ordinal)
            .ToArray();
        if (ordered.Length == 0)
        {
            throw new InvalidOperationException("Base translation output metadata cannot be empty.");
        }
        quality.Validate("base translation output metadata");

        var duplicatePath = ordered
            .GroupBy(function => function.RelativePath, StringComparer.OrdinalIgnoreCase)
            .FirstOrDefault(group => group.Count() > 1);
        if (duplicatePath is not null)
        {
            throw new InvalidOperationException(
                $"Base translation output metadata contains duplicate path '{duplicatePath.Key}'.");
        }

        return new BaseTranslationOutputMetadata(
            CurrentFormat,
            CurrentFormatVersion,
            translationIdentityHash,
            quality,
            ordered,
            sourceBundlePath,
            modPatchAwareness);
    }

    public void RequireReleaseEligible(string source)
    {
        Quality.Validate(source);
        if (Quality.UnsupportedInstructionCount != 0 || Quality.InvalidSsaFunctionCount != 0)
        {
            throw new InvalidDataException(
                $"Release translation quality failure in '{source}': " +
                $"{Quality.UnsupportedInstructionCount} unsupported instruction(s), " +
                $"{Quality.InvalidSsaFunctionCount} invalid SSA function(s).");
        }
    }
}

public sealed record TranslationQualityMetadata(
    int UnsupportedInstructionCount,
    int InvalidSsaFunctionCount)
{
    public static TranslationQualityMetadata Clean { get; } = new(0, 0);

    internal void Validate(string source)
    {
        if (UnsupportedInstructionCount < 0 || InvalidSsaFunctionCount < 0)
        {
            throw new InvalidDataException($"Translation quality counts cannot be negative in '{source}'.");
        }
    }
}

/// <summary>
/// Records that the base translation knows one mod profile's patch set (leaf-inlining blocks and
/// residency fences at every address the mod can win). A mod translation must refuse a base tree
/// whose awareness doesn't cover its own Code.pul, or it silently bakes in vanilla code paths.
/// </summary>
public sealed record BaseTranslationModPatchAwareness(
    string Profile,
    string CodePulSha256);

public sealed record BaseTranslationFunctionMetadata(
    string RelativePath,
    long Size,
    string Sha256,
    uint EntryPoint,
    IReadOnlyList<uint> LocalLabelAddresses,
    BaseTranslationFunctionBuildMetadata? Build = null,
    // True for a fall-through interior address (e.g. a split-switch artifact) that another
    // translated function's control flow already executes. Registered and dispatchable, but the
    // base manifest must not let it split function ranges: patches have to rebuild the enclosing
    // function since every real caller runs its inline copy of the bytes, not this alias.
    bool InteriorToOtherTranslation = false)
{
    public static BaseTranslationFunctionMetadata FromTranslation(
        string outputRoot,
        string outputPath,
        FunctionTranslationResult translation)
    {
        var root = Path.GetFullPath(outputRoot).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);
        var path = Path.GetFullPath(outputPath);
        var prefix = root + Path.DirectorySeparatorChar;
        if (!path.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"Generated function path '{path}' is outside output root '{root}'.");
        }

        var relativePath = path[prefix.Length..].Replace(Path.DirectorySeparatorChar, '/');
        var bytes = Encoding.UTF8.GetBytes(translation.CxxCode);
        var localLabels = translation.Ssa.Function.Blocks
            .Select(block => TryParseLocalLabelAddress(block.Label))
            .Where(address => address.HasValue)
            .Select(address => address!.Value)
            .Where(address => address > translation.EntryPoint)
            .Distinct()
            .OrderBy(address => address)
            .ToArray();

        // The emitter reports what it emitted. Recovering these facts by running
        // regular expressions over C++ text produced in this same process was a
        // silent-failure hazard; the comment markers inside the text remain the
        // cross-run persistence format that build sharding parses from files.
        var emission = translation.Emission;
        var registration = emission?.Registration
            ?? throw new InvalidDataException(
                $"Translated function 0x{translation.EntryPoint:X8} has no build registration metadata.");
        var directCalls = emission!.EmittedDirectCallTargets;
        var abiComment = emission.GuestAbiMarker;
        var build = new BaseTranslationFunctionBuildMetadata(
            registration.Symbol,
            registration.PreservesNonvolatileFprs,
            registration.NonvolatileFprWriteMask,
            directCalls,
            string.IsNullOrWhiteSpace(abiComment) ? null : abiComment,
            translation.Ssa.Function.Blocks.Sum(block => block.Instructions.Count));

        return new BaseTranslationFunctionMetadata(
            relativePath,
            bytes.LongLength,
            ChecksumUtilities.Sha256Hex(bytes),
            translation.EntryPoint,
            localLabels,
            build);
    }

    private static uint? TryParseLocalLabelAddress(string label) =>
        GuestTargetParser.TryParseLocalLabelAddress(label);
}

public sealed record BaseTranslationFunctionBuildMetadata(
    string Symbol,
    bool PreservesNonvolatileFprs,
    uint NonvolatileFprWriteMask,
    IReadOnlyList<uint> DirectCallDependencies,
    string? GuestAbiComment,
    int EstimatedIrInstructions);

public static class BaseTranslationOutputMetadataFile
{
    // Machine-read only: indentation tripled the payload of a file that already
    // measures tens of megabytes and is never opened by a human.
    private static readonly JsonSerializerOptions JsonOptions = JsonOutput.CompactCamelCase;

    public static BaseTranslationOutputMetadata Read(string path)
    {
        var metadata = JsonSerializer.Deserialize<BaseTranslationOutputMetadata>(TrackedFs.ReadAllText(path), JsonOptions)
            ?? throw new InvalidDataException($"Base translation output metadata is empty: {path}");
        Validate(metadata, path);
        return metadata;
    }

    public static bool WriteIfChangedAtomic(string path, BaseTranslationOutputMetadata metadata)
    {
        Validate(metadata, path);
        // Serialise straight into a byte buffer. The previous shape built a
        // multi-megabyte UTF-16 string and then read the whole live file back as a
        // second one purely to answer "did anything change?".
        return JsonOutput.WriteIfChanged(path, metadata, JsonOptions);
    }

    private static void Validate(BaseTranslationOutputMetadata metadata, string source)
    {
        if (!string.Equals(metadata.Format, BaseTranslationOutputMetadata.CurrentFormat, StringComparison.Ordinal) ||
            metadata.FormatVersion != BaseTranslationOutputMetadata.CurrentFormatVersion)
        {
            throw new InvalidDataException(
                $"Unsupported base translation output metadata in '{source}': {metadata.Format} v{metadata.FormatVersion}.");
        }
        if (metadata.Quality is null)
        {
            throw new InvalidDataException($"Base translation output metadata has no quality counters in '{source}'.");
        }
        metadata.Quality.Validate(source);

        var seenPaths = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var seenEntries = new HashSet<uint>();
        foreach (var function in metadata.Functions)
        {
            if (string.IsNullOrWhiteSpace(function.RelativePath) ||
                Path.IsPathRooted(function.RelativePath) ||
                function.RelativePath.Split('/', '\\').Any(component => component == "..") ||
                !function.RelativePath.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase))
            {
                throw new InvalidDataException(
                    $"Invalid generated function path '{function.RelativePath}' in '{source}'.");
            }
            if (!seenPaths.Add(function.RelativePath) || !seenEntries.Add(function.EntryPoint))
            {
                throw new InvalidDataException($"Duplicate generated function entry in '{source}'.");
            }
            if (function.Size < 0 || function.Sha256.Length != 64 ||
                !IsHexadecimal(function.Sha256))
            {
                throw new InvalidDataException(
                    $"Invalid size or SHA-256 for '{function.RelativePath}' in '{source}'.");
            }
        }
    }

    // Enumerable.All over a string allocates a char enumerator and a delegate
    // invocation per character, for tens of thousands of digests per run.
    private static bool IsHexadecimal(string value)
    {
        foreach (var character in value.AsSpan())
        {
            if (!Uri.IsHexDigit(character)) return false;
        }
        return true;
    }
}
