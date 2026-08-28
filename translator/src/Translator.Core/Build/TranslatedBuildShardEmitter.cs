using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using Translator.Core.CodeGen;
using Translator.Core.IO;
using Translator.Core.Loading;
using Translator.Core.Mods;

namespace Translator.Core.Build;

public sealed record TranslatedBuildShardOptions(
    string BaseMetadataPath,
    string BaseFunctionsDirectory,
    string OutputDirectory,
    string NativeSourceDirectory,
    string? RetroResolvedProfilePath = null,
    string? RetroCppDirectory = null,
    int BaseShardCount = 72,
    int ModShardCount = 48,
    int RegistrationShardCount = 16,
    RuntimeNativeIndex? NativeIndex = null);

/// <summary>
/// Per-shard compile-cost spread for one partition. Frozen membership does not
/// rebalance, so this is the drift signal that says when a repack is due.
/// </summary>
public sealed record TranslatedShardBalance(
    int ShardCount,
    long MinCompileCostWeight,
    long MaxCompileCostWeight,
    long TotalCompileCostWeight)
{
    public double MeanCompileCostWeight =>
        ShardCount == 0 ? 0d : (double)TotalCompileCostWeight / ShardCount;

    public double SpreadRatio =>
        MinCompileCostWeight <= 0 ? 0d : (double)MaxCompileCostWeight / MinCompileCostWeight;
}

public sealed record TranslatedBuildShardResult(
    int BaseFunctionCount,
    int SharedBaseFunctionCount,
    int ProfileSensitiveTargetCount,
    int ProfileSensitiveCallerCount,
    int ModFunctionCount,
    int BaseShardCount,
    int ModShardCount,
    string CMakeManifestPath,
    string? BaseCommonShardMapPath = null,
    bool BaseCommonBoundariesReused = false,
    TranslatedShardBalance? BaseCommonBalance = null);

/// <summary>
/// Emits the only generated-C++ membership consumed by CMake. The small
/// per-function files remain useful translator artifacts, but compiler and
/// linker topology is expressed as deterministic, content-addressed shards.
/// </summary>
public static partial class TranslatedBuildShardEmitter
{
    private const string ShardBoundaryFormat = "mkw-base-common-shard-boundaries";
    private const int ShardBoundaryVersion = 1;
    // Translator-owned state, not a build product: it lives beside the shards it describes so a
    // checkout carries its frozen membership with its sources. Not a prune candidate, same as
    // shards.cmake.
    private const string BaseCommonShardMapFileName = "base_common_shard_map.json";

    private sealed class FunctionRecord
    {
        public required uint Address { get; init; }
        public required string Symbol { get; init; }
        public required string Name { get; init; }
        public required string SourcePath { get; init; }
        public required string SourceFingerprint { get; init; }
        public required string RegistrationKind { get; init; }
        public required uint Priority { get; init; }
        public required ulong ModuleId { get; init; }
        public required bool PreservesNonvolatileFprs { get; init; }
        public required uint NonvolatileFprWriteMask { get; init; }
        public required IReadOnlyList<uint> DirectCalls { get; init; }
        public required long CompileCostWeight { get; init; }
        public string? SourceText { get; init; }
        public bool ExcludedByNativeOverride { get; set; }
    }

    private sealed record ProfileEntry(
        uint Address,
        string Symbol,
        string Name,
        string Kind,
        uint Priority,
        bool DirectCallAvailable,
        bool PreservesNonvolatileFprs,
        uint NonvolatileFprWriteMask,
        bool MustRemainDynamicallyDispatchable);

    private sealed record NativeOverrideSets(
        IReadOnlySet<uint> Winners,
        IReadOnlySet<uint> TranslationExclusions,
        IReadOnlyDictionary<uint, RawTranslatedOverride> RawTranslatedOverrides);

    private sealed record RawTranslatedOverride(
        string Symbol,
        bool PreservesNonvolatileFprs,
        uint NonvolatileFprWriteMask);

    private sealed record Trait(
        bool Available,
        bool PreservesNonvolatileFprs,
        uint NonvolatileFprWriteMask,
        bool MustRemainDynamicallyDispatchable,
        string? WinnerSymbol,
        string WinnerKind,
        uint WinnerPriority);

    private sealed record ShardInfo(
        string Partition,
        string Path,
        string Fingerprint,
        long CompileCostWeight,
        IReadOnlyList<FunctionRecord> Functions);


    /// <summary>
    /// Frozen cut points for a weight-packed partition. Slot k owns every
    /// address in [StartAddresses[k], StartAddresses[k + 1]); the last slot owns
    /// everything from its start upwards. Starts are strictly ascending.
    /// </summary>
    private sealed record ShardBoundaryTable(IReadOnlyList<uint> StartAddresses);

    private sealed record ShardBoundaryResolution(
        ShardBoundaryTable Table,
        string Path,
        bool Reused);

    public static TranslatedBuildShardResult Emit(TranslatedBuildShardOptions options)
    {
        ValidateOptions(options);
        var outputRoot = Path.GetFullPath(options.OutputDirectory);
        Directory.CreateDirectory(outputRoot);

        var metadata = BaseTranslationOutputMetadataFile.Read(options.BaseMetadataPath);
        metadata.RequireReleaseEligible(options.BaseMetadataPath);
        TranslationSourceBundle? baseSourceBundle = null;
        if (!string.IsNullOrWhiteSpace(metadata.SourceBundlePath))
        {
            var bundlePath = Path.IsPathRooted(metadata.SourceBundlePath)
                ? metadata.SourceBundlePath
                : Path.Combine(Path.GetDirectoryName(Path.GetFullPath(options.BaseMetadataPath))!, metadata.SourceBundlePath);
            baseSourceBundle = TranslationSourceBundle.Read(bundlePath);
        }
        var nativeOverrides = ReadNativeOverrides(options.NativeSourceDirectory, options.NativeIndex);
        var retroEntries = ReadResolvedProfile(options.RetroResolvedProfilePath);
        var baseRecords = ReadBaseFunctions(metadata, options.BaseFunctionsDirectory, baseSourceBundle);
        foreach (var record in baseRecords)
        {
            record.ExcludedByNativeOverride = nativeOverrides.TranslationExclusions.Contains(record.Address);
        }

        var activeBase = baseRecords.Where(static record => !record.ExcludedByNativeOverride).ToArray();
        var baseTraits = activeBase.ToDictionary(
            static record => record.Address,
            record =>
            {
                var nativeWinner = nativeOverrides.Winners.Contains(record.Address);
                return new Trait(
                Available: !nativeWinner,
                record.PreservesNonvolatileFprs,
                record.NonvolatileFprWriteMask,
                MustRemainDynamicallyDispatchable: nativeWinner,
                WinnerSymbol: nativeWinner ? null : record.Symbol,
                WinnerKind: nativeWinner ? "native" : "base",
                WinnerPriority: nativeWinner ? 10_000u : 0u);
            });
        foreach (var (address, rawOverride) in nativeOverrides.RawTranslatedOverrides)
        {
            baseTraits[address] = new Trait(
                Available: true,
                rawOverride.PreservesNonvolatileFprs,
                rawOverride.NonvolatileFprWriteMask,
                MustRemainDynamicallyDispatchable: false,
                rawOverride.Symbol,
                WinnerKind: "base",
                WinnerPriority: 0u);
        }
        var rrTraits = new Dictionary<uint, Trait>(BuildRetroTraits(activeBase, baseTraits, retroEntries));
        foreach (var address in nativeOverrides.RawTranslatedOverrides.Keys)
            rrTraits[address] = baseTraits[address];
        var sensitiveTargets = FindProfileSensitiveTargets(baseTraits, rrTraits);
        var sensitiveCallers = activeBase
            .Where(record => record.DirectCalls.Any(sensitiveTargets.Contains))
            .Select(static record => record.Address)
            .ToHashSet();

        var nonCoffSensitiveTargets = baseTraits.Keys
            .Where(address => !rrTraits.TryGetValue(address, out var rr) ||
                              baseTraits[address].Available != rr.Available ||
                              !string.Equals(baseTraits[address].WinnerSymbol, rr.WinnerSymbol, StringComparison.Ordinal))
            .ToHashSet();
        var nonCoffSensitiveCallers = activeBase
            .Where(record => record.DirectCalls.Any(nonCoffSensitiveTargets.Contains))
            .Select(static record => record.Address)
            .ToHashSet();
        nonCoffSensitiveCallers.UnionWith(sensitiveCallers);

        var commonBase = activeBase.Where(record => !sensitiveCallers.Contains(record.Address)).ToArray();
        var portableCommonBase = activeBase.Where(record => !nonCoffSensitiveCallers.Contains(record.Address)).ToArray();
        var portableSensitiveBase = activeBase.Where(record => nonCoffSensitiveCallers.Contains(record.Address)).ToArray();
        var retroSourceBundlePath = options.RetroCppDirectory is { } retroCppDir && !string.IsNullOrWhiteSpace(retroCppDir)
            ? Path.Combine(Path.GetDirectoryName(Path.GetFullPath(retroCppDir))!, "translated_sources.bin")
            : null;
        if (retroSourceBundlePath is not null && !File.Exists(retroSourceBundlePath)) retroSourceBundlePath = null;
        var modRecords = ReadModFunctions(options.RetroCppDirectory, retroSourceBundlePath);

        var shards = new List<ShardInfo>();
        // base_common is weighted-sequential, so one function's weight changing would shift every
        // downstream cut point and rename every shard after it. Freezing cut points in an emitted
        // table keeps shard identity independent of other functions' compile cost.
        var baseCommonBoundaries = ResolveBaseCommonBoundaries(
            outputRoot, portableCommonBase, options.BaseShardCount);
        var baseCommonShards = WriteFunctionShards(
            outputRoot, "base_common", portableCommonBase, options.BaseShardCount, baseTraits,
            "profile-neutral shared base", weightedSequential: true,
            frozenBoundaries: baseCommonBoundaries?.Table);
        shards.AddRange(baseCommonShards);
        shards.AddRange(WriteFunctionShards(
            outputRoot, "base_portable_sensitive", portableSensitiveBase,
            Math.Min(24, Math.Max(1, portableSensitiveBase.Length)), baseTraits,
            "base profile", weightedSequential: false));
        shards.AddRange(WriteFunctionShards(
            outputRoot, "retro_portable_sensitive", portableSensitiveBase,
            Math.Min(24, Math.Max(1, portableSensitiveBase.Length)), rrTraits,
            "Retro Rewind profile", weightedSequential: false));
        shards.AddRange(WriteFunctionShards(
            outputRoot, "retro_mod", modRecords, options.ModShardCount, rrTraits,
            "Retro Rewind mod", weightedSequential: false));

        var baseRegistration = WriteRegistrationShards(
            outputRoot, "base", activeBase, baseTraits, options.RegistrationShardCount).ToList();
        baseRegistration.Add(WriteIndirectDispatchTable(outputRoot, "base", baseTraits));
        var rrRegistrationRecords = activeBase.Concat(modRecords).OrderBy(static record => record.Address).ThenBy(static record => record.RegistrationKind, StringComparer.Ordinal).ToArray();
        var rrRegistration = WriteRegistrationShards(
            outputRoot, "retro_rewind", rrRegistrationRecords, rrTraits, options.RegistrationShardCount).ToList();
        rrRegistration.Add(WriteIndirectDispatchTable(outputRoot, "retro_rewind", rrTraits));

        var extraRetroSources = ReadRetroExtraSources(options.RetroCppDirectory, modRecords);
        var cmakeManifestPath = Path.Combine(outputRoot, "shards.cmake");
        WriteCMakeManifest(
            cmakeManifestPath,
            outputRoot,
            commonBase.Length,
            portableCommonBase.Length,
            activeBase.Length,
            modRecords.Count,
            sensitiveTargets.Count,
            sensitiveCallers.Count,
            shards,
            baseRegistration,
            rrRegistration,
            extraRetroSources);
        PruneStaleShardSources(outputRoot, shards, baseRegistration, rrRegistration);

        return new TranslatedBuildShardResult(
            baseRecords.Count,
            commonBase.Length,
            sensitiveTargets.Count,
            sensitiveCallers.Count,
            modRecords.Count,
            shards.Count(shard => shard.Partition.StartsWith("base_", StringComparison.Ordinal)),
            shards.Count(shard => shard.Partition == "retro_mod"),
            cmakeManifestPath,
            baseCommonBoundaries?.Path,
            baseCommonBoundaries?.Reused ?? false,
            MeasureBalance(baseCommonShards));
    }

    private static TranslatedShardBalance? MeasureBalance(IReadOnlyList<ShardInfo> shards) =>
        shards.Count == 0
            ? null
            : new TranslatedShardBalance(
                shards.Count,
                shards.Min(static shard => shard.CompileCostWeight),
                shards.Max(static shard => shard.CompileCostWeight),
                shards.Sum(static shard => shard.CompileCostWeight));

    private static void ValidateOptions(TranslatedBuildShardOptions options)
    {
        if (!File.Exists(options.BaseMetadataPath))
        {
            throw new FileNotFoundException("Required shard input is missing.", options.BaseMetadataPath);
        }
        foreach (var path in new[] { options.BaseFunctionsDirectory, options.NativeSourceDirectory })
        {
            if (!Directory.Exists(path)) throw new DirectoryNotFoundException(path);
        }
        if (options.BaseShardCount < 1 || options.ModShardCount < 1 || options.RegistrationShardCount < 1)
        {
            throw new ArgumentOutOfRangeException(nameof(options), "Shard counts must be positive.");
        }
    }

    private static IReadOnlyList<FunctionRecord> ReadBaseFunctions(
        BaseTranslationOutputMetadata metadata,
        string functionsDirectory,
        TranslationSourceBundle? sourceBundle)
    {
        var root = Path.GetFullPath(functionsDirectory);
        var records = new List<FunctionRecord>(metadata.Functions.Count);
        foreach (var item in metadata.Functions.OrderBy(static function => function.EntryPoint))
        {
            var path = Path.GetFullPath(Path.Combine(root, item.RelativePath.Replace('/', Path.DirectorySeparatorChar)));
            string? source = null;
            if (sourceBundle is not null)
            {
                if (!sourceBundle.TryGet(item.EntryPoint, out var bundled))
                    throw new InvalidDataException($"Source bundle is missing base function 0x{item.EntryPoint:X8}.");
                source = bundled.Source;
                // Read already verified the digest, so compare it directly instead of re-hashing
                // tens of thousands of sources again.
                if (bundled.Sha256 is { } verifiedHash)
                {
                    if (bundled.SourceByteLength != item.Size ||
                        !verifiedHash.Equals(item.Sha256, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException($"Source bundle metadata mismatch for 0x{item.EntryPoint:X8}.");
                }
                else
                {
                    var bytes = Encoding.UTF8.GetBytes(source);
                    var hash = ChecksumUtilities.Sha256Hex(bytes);
                    if (bytes.LongLength != item.Size || !hash.Equals(item.Sha256, StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException($"Source bundle metadata mismatch for 0x{item.EntryPoint:X8}.");
                }
            }
            else if (item.Build is null)
            {
                source = File.ReadAllText(path);
            }
            (uint Address, string Symbol, bool Preserves, uint FprMask) registration = item.Build is null
                ? ParseBaseRegistration(source!, path)
                : (item.EntryPoint, item.Build.Symbol, item.Build.PreservesNonvolatileFprs, item.Build.NonvolatileFprWriteMask);
            if (registration.Address != item.EntryPoint)
                throw new InvalidDataException($"Base registration address mismatch in '{path}'.");
            var record = CreateRecord(
                registration.Address,
                registration.Symbol,
                registration.Symbol,
                path,
                item.Sha256,
                item.Size,
                "base",
                0,
                0,
                registration.Preserves,
                registration.FprMask,
                source,
                item.Build?.DirectCallDependencies,
                item.Build?.EstimatedIrInstructions);
            records.Add(record);
        }
        return records;
    }

    private static IReadOnlyList<FunctionRecord> ReadModFunctions(string? cppDirectory, string? sourceBundlePath)
    {
        if (string.IsNullOrWhiteSpace(cppDirectory) ||
            (!Directory.Exists(cppDirectory) && (string.IsNullOrWhiteSpace(sourceBundlePath) || !File.Exists(sourceBundlePath))))
        {
            return Array.Empty<FunctionRecord>();
        }
        var records = new List<FunctionRecord>();
        var sources = !string.IsNullOrWhiteSpace(sourceBundlePath) && File.Exists(sourceBundlePath)
            ? TranslationSourceBundle.Read(sourceBundlePath).Entries
                .Select(entry => (Path: Path.GetFullPath(Path.Combine(cppDirectory, entry.VirtualPath)), entry.Source))
            : Directory.EnumerateFiles(cppDirectory, "*.cpp", SearchOption.AllDirectories)
                .Order(StringComparer.OrdinalIgnoreCase)
                .Select(path => (Path: path, Source: File.ReadAllText(path)));
        foreach (var (path, source) in sources)
        {
            var match = ModRegistrationRegex().Match(source);
            if (!match.Success) continue;
            var address = ParseHexUInt32(match.Groups["address"].Value);
            var symbol = match.Groups["symbol"].Value;
            var preserves = bool.Parse(match.Groups["preserves"].Value);
            var mask = ParseHexUInt32(match.Groups["mask"].Value);
            var priority = uint.Parse(match.Groups["priority"].Value, CultureInfo.InvariantCulture);
            var moduleId = ulong.Parse(match.Groups["moduleId"].Value, CultureInfo.InvariantCulture);
            var bytes = Encoding.UTF8.GetBytes(source);
            records.Add(CreateRecord(
                address,
                symbol,
                symbol,
                Path.GetFullPath(path),
                ChecksumUtilities.Sha256Hex(bytes),
                bytes.LongLength,
                "mod",
                priority,
                moduleId,
                preserves,
                mask,
                source));
        }
        return records.OrderBy(static record => record.Address).ThenBy(static record => record.Symbol, StringComparer.Ordinal).ToArray();
    }

    private static FunctionRecord CreateRecord(
        uint address,
        string symbol,
        string name,
        string path,
        string fingerprint,
        long bytes,
        string kind,
        uint priority,
        ulong moduleId,
        bool preserves,
        uint fprMask,
        string? source,
        IReadOnlyList<uint>? knownCalls = null,
        int? knownEstimatedIr = null)
    {
        var calls = knownCalls?.Distinct().Order().ToArray() ?? DirectCallRegex().Matches(source!).Cast<Match>()
            .Select(match => ParseHexUInt32(match.Groups["address"].Value)).Distinct().Order().ToArray();
        var estimatedIr = Math.Max(1, knownEstimatedIr ?? source!.Count(static character => character == ';'));
        var locality = calls.Count(target => (target >> 16) == (address >> 16));
        var weight = checked(bytes + estimatedIr * 24L + calls.Length * 96L - locality * 16L);
        var fullPath = Path.GetFullPath(path);
        return new FunctionRecord
        {
            Address = address,
            Symbol = symbol,
            Name = name,
            SourcePath = fullPath,
            SourceFingerprint = fingerprint,
            RegistrationKind = kind,
            Priority = priority,
            ModuleId = moduleId,
            PreservesNonvolatileFprs = preserves,
            NonvolatileFprWriteMask = fprMask,
            DirectCalls = calls,
            CompileCostWeight = Math.Max(1, weight),
            SourceText = source
        };
    }

    private static (uint Address, string Symbol, bool Preserves, uint FprMask) ParseBaseRegistration(string source, string path)
    {
        var match = BaseRegistrationRegex().Match(source);
        if (!match.Success) throw new InvalidDataException($"Missing base registration metadata in '{path}'.");
        return (
            ParseHexUInt32(match.Groups["address"].Value),
            match.Groups["symbol"].Value,
            bool.Parse(match.Groups["preserves"].Value),
            ParseHexUInt32(match.Groups["mask"].Value));
    }

    private static NativeOverrideSets ReadNativeOverrides(
        string directory,
        RuntimeNativeIndex? index = null)
    {
        var registrations = (index ?? RuntimeNativeIndexBuilder.Build(directory)).Registrations;
        var winners = registrations.Select(static registration => registration.Address).ToHashSet();
        var exclusions = registrations
            .Where(static registration => registration.ExcludesBaseTranslation)
            .Select(static registration => registration.Address)
            .ToHashSet();
        // REGISTER_TRANSLATED_FUNCTION carries no FPR metadata, so a hand-written
        // runtime override stays fully conservative: assume it may write every
        // nonvolatile FPR.
        var rawTranslatedOverrides = registrations
            .Where(static registration => registration.IsTranslatedOverride)
            .GroupBy(static registration => registration.Address)
            .ToDictionary(
                static group => group.Key,
                static group => new RawTranslatedOverride(
                    group.First().Symbol,
                    PreservesNonvolatileFprs: false,
                    NonvolatileFprWriteMask: uint.MaxValue));
        return new NativeOverrideSets(winners, exclusions, rawTranslatedOverrides);
    }

    private static IReadOnlyDictionary<uint, ProfileEntry> ReadResolvedProfile(string? path)
    {
        if (string.IsNullOrWhiteSpace(path) || !File.Exists(path)) return new Dictionary<uint, ProfileEntry>();
        using var document = JsonDocument.Parse(File.ReadAllText(path));
        return document.RootElement.GetProperty("Entries").EnumerateArray()
            .Select(element => new ProfileEntry(
                element.GetProperty("Address").GetUInt32(),
                element.GetProperty("Symbol").GetString() ?? "",
                element.GetProperty("Name").GetString() ?? "",
                element.GetProperty("Kind").GetString() ?? "",
                element.GetProperty("Priority").GetUInt32(),
                element.GetProperty("DirectCallAvailable").GetBoolean(),
                element.GetProperty("PreservesNonvolatileFprs").GetBoolean(),
                element.GetProperty("NonvolatileFprWriteMask").GetUInt32(),
                element.GetProperty("MustRemainDynamicallyDispatchable").GetBoolean()))
            .GroupBy(static entry => entry.Address)
            .ToDictionary(static group => group.Key, static group => group.First());
    }

    private static IReadOnlyDictionary<uint, Trait> BuildRetroTraits(
        IReadOnlyList<FunctionRecord> baseFunctions,
        IReadOnlyDictionary<uint, Trait> baseTraits,
        IReadOnlyDictionary<uint, ProfileEntry> retroEntries)
    {
        if (retroEntries.Count == 0) return new Dictionary<uint, Trait>(baseTraits);
        var traits = new Dictionary<uint, Trait>(baseTraits.Count + retroEntries.Count);
        foreach (var function in baseFunctions)
        {
            if (!retroEntries.TryGetValue(function.Address, out var resolved))
            {
                traits[function.Address] = baseTraits[function.Address];
                continue;
            }
            var translated = resolved.DirectCallAvailable &&
                             (resolved.Kind.Equals("base", StringComparison.OrdinalIgnoreCase) ||
                              resolved.Kind.Equals("mod", StringComparison.OrdinalIgnoreCase) ||
                              resolved.Kind.Equals("rr", StringComparison.OrdinalIgnoreCase));
            traits[function.Address] = new Trait(
                translated,
                resolved.PreservesNonvolatileFprs,
                resolved.NonvolatileFprWriteMask,
                resolved.MustRemainDynamicallyDispatchable,
                translated ? resolved.Symbol : null,
                resolved.Kind,
                resolved.Priority);
        }
        foreach (var resolved in retroEntries.Values)
        {
            if (traits.ContainsKey(resolved.Address)) continue;
            var translated = resolved.DirectCallAvailable &&
                             (resolved.Kind.Equals("base", StringComparison.OrdinalIgnoreCase) ||
                              resolved.Kind.Equals("mod", StringComparison.OrdinalIgnoreCase) ||
                              resolved.Kind.Equals("rr", StringComparison.OrdinalIgnoreCase));
            if (!translated) continue;
            traits[resolved.Address] = new Trait(
                true,
                resolved.PreservesNonvolatileFprs,
                resolved.NonvolatileFprWriteMask,
                resolved.MustRemainDynamicallyDispatchable,
                resolved.Symbol,
                resolved.Kind,
                resolved.Priority);
        }
        return traits;
    }

    private static HashSet<uint> FindProfileSensitiveTargets(
        IReadOnlyDictionary<uint, Trait> baseTraits,
        IReadOnlyDictionary<uint, Trait> rrTraits)
    {
        var result = new HashSet<uint>();
        foreach (var (address, baseTrait) in baseTraits)
        {
            if (!rrTraits.TryGetValue(address, out var rrTrait) ||
                baseTrait.Available != rrTrait.Available ||
                baseTrait.PreservesNonvolatileFprs != rrTrait.PreservesNonvolatileFprs ||
                baseTrait.NonvolatileFprWriteMask != rrTrait.NonvolatileFprWriteMask)
            {
                result.Add(address);
            }
        }
        return result;
    }

    private static string BuildTraitsHeader(
        IReadOnlyDictionary<uint, Trait> traits,
        string label)
    {
        var output = new StringBuilder();
        output.AppendLine("#pragma once");
        output.AppendLine($"// Translator-owned direct-call traits: {label}.");
        foreach (var (address, trait) in traits.OrderBy(static pair => pair.Key))
        {
            if (!trait.Available || string.IsNullOrWhiteSpace(trait.WinnerSymbol)) continue;
            // Dynamic-dispatch and preserves_nonvolatile_fprs are deliberately not arguments here:
            // direct calls are always non-overridable literal edges, and the macro guard is
            // selected by the mask alone.
            output.AppendLine(
                $"MKW_TRANSLATED_TRAIT({address:X8}, {trait.WinnerSymbol}, 0x{trait.NonvolatileFprWriteMask:X8}u);");
        }
        return output.ToString();
    }

    private static IReadOnlyList<ShardInfo> WriteFunctionShards(
        string outputRoot,
        string partition,
        IReadOnlyList<FunctionRecord> functions,
        int requestedShardCount,
        IReadOnlyDictionary<uint, Trait> traits,
        string traitsLabel,
        bool weightedSequential,
        ShardBoundaryTable? frozenBoundaries = null)
    {
        if (functions.Count == 0) return Array.Empty<ShardInfo>();
        var shardCount = Math.Min(requestedShardCount, functions.Count);
        // A frozen boundary table supersedes the packer that produced it: the
        // membership it encodes is identical to that packer run, and it stays
        // identical when a later run reweights the very same functions.
        var groups = (frozenBoundaries is not null
            ? PartitionFrozenBoundaries(functions, frozenBoundaries)
            : weightedSequential
                ? PartitionWeightedSequential(functions, shardCount)
                : PartitionStableHash(functions, shardCount))
            .Select(static group => group.ToList())
            .ToList();

        var directory = Path.Combine(outputRoot, partition);
        Directory.CreateDirectory(directory);
        // Groups own disjoint records and distinct files, so they build concurrently; results are
        // collected positionally to keep the emitted list byte-for-byte deterministic. Never widen
        // this parallelism across WriteFunctionShards calls, base and Retro Rewind share records.
        var emitted = new ShardInfo?[groups.Count];
        Parallel.For(0, groups.Count, index =>
        {
            var group = groups[index];
            if (group.Count == 0) return;
            var dependencyTraits = group
                .SelectMany(static function => function.DirectCalls)
                .Distinct()
                .Where(traits.ContainsKey)
                .ToDictionary(address => address, address => traits[address]);
            var traitIdentity = string.Join("\n", dependencyTraits.OrderBy(static pair => pair.Key).Select(pair =>
                $"{pair.Key:X8}:{pair.Value.Available}:{pair.Value.PreservesNonvolatileFprs}:{pair.Value.NonvolatileFprWriteMask:X8}:{pair.Value.WinnerSymbol}"));
            var identity = string.Join("\n", group.Select(function => $"{function.Address:X8}:{function.SourceFingerprint}")) + "\n" + traitIdentity;
            var hash = ChecksumUtilities.Sha256Hex(Encoding.UTF8.GetBytes($"{partition}\n{identity}"));
            // Membership, not the transient scheduling position, is the shard
            // identity.  This lets an unchanged shard retain its source/object
            // cache identity when another shard becomes heavier or lighter.
            var path = Path.Combine(directory, $"shard_{hash[..24]}.cpp");
            var traitsPath = Path.Combine(directory, $"shard_{hash[..24]}_traits.h");
            var source = new StringBuilder();
            source.AppendLine("// Translator-owned content-addressed aggregate shard; do not edit.");
            source.AppendLine("#include \"abi_bridge.h\"");
            // Quoted include resolves the sibling traits header on every toolchain and keeps
            // the shard text free of the emitting machine's absolute paths.
            source.AppendLine($"#include \"{Path.GetFileName(traitsPath)}\"");
            source.AppendLine("#define MKW_STATIC_TRANSLATED_CALL(Target, Entry, Context) do { ApplyRuntimeCallOptions(Target, Context); Entry(Context); } while (false)");
            foreach (var function in group
                         .OrderBy(static function => function.Address)
                         .ThenBy(static function => function.Symbol, StringComparer.Ordinal))
            {
                var functionSource = function.SourceText ?? File.ReadAllText(function.SourcePath);
                functionSource = RegistrationMarkerRegex().Replace(functionSource, string.Empty);
                functionSource = LowerStableDirectCalls(functionSource, traits);
                // Diagnostics only; the named file no longer exists on disk (sources live in the
                // bundle), so only the bare file name is kept, not the emitting machine's path.
                source.Append($"#line 1 \"{Escape(Path.GetFileName(function.SourcePath))}\"{Environment.NewLine}");
                source.Append(functionSource);
                if (!functionSource.EndsWith('\n'))
                {
                    source.AppendLine();
                }
            }
            source.AppendLine("#undef MKW_STATIC_TRANSLATED_CALL");
            WriteIfChanged(traitsPath, BuildTraitsHeader(dependencyTraits, traitsLabel));
            WriteIfChanged(path, source.ToString());
            emitted[index] = new ShardInfo(
                partition, path, hash, group.Sum(static function => function.CompileCostWeight), group);
        });

        var shards = new List<ShardInfo>(groups.Count);
        foreach (var shard in emitted)
        {
            if (shard is not null) shards.Add(shard);
        }
        return shards;
    }

    private static string LowerStableDirectCalls(
        string source,
        IReadOnlyDictionary<uint, Trait> traits)
    {
        return DirectCallRegex().Replace(source, match =>
        {
            var address = ParseHexUInt32(match.Groups["address"].Value);
            if (!traits.TryGetValue(address, out var trait) ||
                !trait.Available ||
                trait.NonvolatileFprWriteMask != 0 ||
                trait.MustRemainDynamicallyDispatchable ||
                trait.WinnerSymbol is null ||
                trait.WinnerKind.Equals("native", StringComparison.OrdinalIgnoreCase) ||
                RequiresGenericDirectCall(address))
            {
                return match.Value;
            }

            return $"MKW_STATIC_TRANSLATED_CALL(0x{address:X8}u, {trait.WinnerSymbol}, ctx)";
        });
    }

    private static bool RequiresGenericDirectCall(uint address) => address switch
    {
        // Runtime-toggle diagnostics and phase tracing intentionally remain on
        // the generic cold path. Ordinary calls have no runtime branch.
        0x80008EF0 or 0x80008FB4 or 0x80009194 or 0x80243D18 or 0x80243D6C or 0x808897F0 or
        0x802226D8 or 0x805C3218 or 0x805E7460 or 0x8063C470 or 0x8063C4D4 or 0x8063C560 or 0x8063C714 or
        0x80198CA8 or 0x80199038 or 0x801992A8 or 0x801998A4 or 0x80226C78 or 0x80226EBC or 0x80229814 or 0x80229C5C or 0x80229DCC or 0x80229DD8 or
        0x801A7424 or 0x80672CC8 => true,
        _ => false
    };

    private static IReadOnlyList<List<FunctionRecord>> PartitionWeightedSequential(IReadOnlyList<FunctionRecord> functions, int count)
    {
        var ordered = functions.OrderBy(static function => function.Address).ToArray();
        return PartitionWeightedIndices(ordered.Select(static function => function.CompileCostWeight).ToArray(), count)
            .Select(indices => indices.Select(index => ordered[index]).ToList())
            .ToArray();
    }

    internal static IReadOnlyList<IReadOnlyList<int>> PartitionWeightedIndices(
        IReadOnlyList<long> weights,
        int count)
    {
        if (count < 1 || count > weights.Count || weights.Any(static weight => weight <= 0))
            throw new ArgumentOutOfRangeException(nameof(count));
        var result = Enumerable.Range(0, count).Select(_ => new List<int>()).ToArray();
        long remainingWeight = weights.Sum();
        var nextFunction = 0;
        for (var shard = 0; shard < count; shard++)
        {
            var remainingBins = count - shard;
            long accumulated = 0;
            while (nextFunction < weights.Count)
            {
                var remainingFunctions = weights.Count - nextFunction;
                if (result[shard].Count != 0 && remainingFunctions == remainingBins - 1)
                    break;

                var weight = weights[nextFunction];
                var target = (remainingWeight + remainingBins - 1) / remainingBins;
                var without = Math.Abs(accumulated - target);
                var with = Math.Abs(checked(accumulated + weight) - target);
                if (result[shard].Count != 0 && without <= with)
                    break;

                result[shard].Add(nextFunction);
                accumulated = checked(accumulated + weight);
                nextFunction++;
            }

            remainingWeight -= accumulated;
        }

        if (nextFunction != weights.Count || result.Any(static group => group.Count == 0))
            throw new InvalidOperationException("Weighted shard partitioning failed to produce non-empty deterministic bins.");
        return result;
    }

    /// <summary>
    /// Assigns each function to the frozen slot whose start address is the greatest one at or below it
    /// (gaps join the range in front, addresses below the table join slot 0), ignoring compile cost
    /// weight so one function's size changing can never move another function to a different shard.
    /// </summary>
    private static IReadOnlyList<List<FunctionRecord>> PartitionFrozenBoundaries(
        IReadOnlyList<FunctionRecord> functions,
        ShardBoundaryTable boundaries)
    {
        var starts = boundaries.StartAddresses;
        var result = Enumerable.Range(0, starts.Count).Select(_ => new List<FunctionRecord>()).ToArray();
        foreach (var function in functions.OrderBy(static function => function.Address))
        {
            // Slot counts are in the tens, so an ascending scan is both the
            // cheapest and the most obviously deterministic lookup.
            var slot = 0;
            for (var index = 1; index < starts.Count; index++)
            {
                if (starts[index] > function.Address) break;
                slot = index;
            }
            result[slot].Add(function);
        }
        return result;
    }

    /// <summary>
    /// Reuses the recorded base_common boundary table if present, otherwise packs once and
    /// records the cut points chosen. Rebalancing is never automatic: deleting the table is the
    /// only way to move a frozen boundary, which renames every base_common shard.
    /// </summary>
    private static ShardBoundaryResolution? ResolveBaseCommonBoundaries(
        string outputRoot,
        IReadOnlyList<FunctionRecord> functions,
        int requestedShardCount)
    {
        if (functions.Count == 0) return null;
        var path = Path.Combine(outputRoot, BaseCommonShardMapFileName);
        var shardCount = Math.Min(requestedShardCount, functions.Count);
        // A different requested shard count is a topology change rather than
        // drift, so the recorded table is replaced instead of silently
        // overriding what the caller asked for.
        var existing = ReadShardBoundaryTable(path, shardCount);
        if (existing is not null)
        {
            // Content-gated, so an unchanged table keeps its modification time.
            WriteShardBoundaryTable(path, "base_common", existing);
            return new ShardBoundaryResolution(existing, path, Reused: true);
        }

        var packed = PartitionWeightedSequential(functions, shardCount);
        for (var index = 1; index < packed.Count; index++)
        {
            // Containment lookup requires consecutive bins to not share an address; fail loudly
            // rather than freeze a table that silently reshuffles membership later.
            if (packed[index - 1][^1].Address >= packed[index][0].Address)
            {
                throw new InvalidOperationException(
                    "Weighted shard partitioning produced overlapping address ranges; " +
                    "base_common boundaries cannot be frozen.");
            }
        }

        var table = new ShardBoundaryTable(packed.Select(static group => group[0].Address).ToArray());
        WriteShardBoundaryTable(path, "base_common", table);
        return new ShardBoundaryResolution(table, path, Reused: false);
    }

    private static ShardBoundaryTable? ReadShardBoundaryTable(string path, int expectedShardCount)
    {
        if (!File.Exists(path)) return null;
        try
        {
            using var document = JsonDocument.Parse(File.ReadAllText(path));
            var root = document.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("format", out var format) ||
                !string.Equals(format.GetString(), ShardBoundaryFormat, StringComparison.Ordinal) ||
                !root.TryGetProperty("formatVersion", out var version) ||
                version.GetInt32() != ShardBoundaryVersion ||
                !root.TryGetProperty("shardStartAddresses", out var starts) ||
                starts.ValueKind != JsonValueKind.Array)
            {
                return null;
            }

            var parsed = new List<uint>(starts.GetArrayLength());
            foreach (var element in starts.EnumerateArray())
            {
                var text = element.GetString();
                if (string.IsNullOrWhiteSpace(text)) return null;
                parsed.Add(ParseHexUInt32(text));
            }
            if (parsed.Count != expectedShardCount) return null;
            for (var index = 1; index < parsed.Count; index++)
            {
                if (parsed[index] <= parsed[index - 1]) return null;
            }
            return new ShardBoundaryTable(parsed);
        }
        catch (Exception ex) when (ex is JsonException or IOException or FormatException
                                       or OverflowException or InvalidOperationException)
        {
            // An unreadable or malformed table is treated as absent: the packer
            // re-derives the cut points and the file is rewritten in canonical
            // form. That costs one full rebuild, never a wrong build.
            return null;
        }
    }

    private static void WriteShardBoundaryTable(string path, string partition, ShardBoundaryTable table)
    {
        var starts = table.StartAddresses;
        var output = new StringBuilder();
        output.AppendLine("{");
        output.AppendLine($"  \"format\": \"{ShardBoundaryFormat}\",");
        output.AppendLine($"  \"formatVersion\": {ShardBoundaryVersion.ToString(CultureInfo.InvariantCulture)},");
        output.AppendLine($"  \"partition\": \"{Escape(partition)}\",");
        output.AppendLine($"  \"shardCount\": {starts.Count.ToString(CultureInfo.InvariantCulture)},");
        // Ascending, fixed-width, invariant hexadecimal: the file is byte-stable
        // for a given set of cut points regardless of host culture.
        output.AppendLine("  \"shardStartAddresses\": [");
        for (var index = 0; index < starts.Count; index++)
        {
            output.AppendLine($"    \"0x{starts[index]:X8}\"{(index == starts.Count - 1 ? "" : ",")}");
        }
        output.AppendLine("  ]");
        output.AppendLine("}");
        WriteIfChanged(path, output.ToString());
    }

    private static IReadOnlyList<List<FunctionRecord>> PartitionStableHash(IReadOnlyList<FunctionRecord> functions, int count)
    {
        var result = Enumerable.Range(0, count).Select(_ => new List<FunctionRecord>()).ToArray();
        foreach (var function in functions)
        {
            var mixed = function.Address ^ (function.Address >> 11) ^ (function.Address >> 19);
            result[(int)(mixed % (uint)count)].Add(function);
        }
        return result;
    }

    private static IReadOnlyList<string> WriteRegistrationShards(
        string outputRoot,
        string profile,
        IReadOnlyList<FunctionRecord> records,
        IReadOnlyDictionary<uint, Trait> traits,
        int requestedCount)
    {
        if (records.Count == 0) return Array.Empty<string>();
        var count = Math.Min(requestedCount, records.Count);
        var groups = PartitionStableHash(records, count);
        var directory = Path.Combine(outputRoot, $"{profile}_registration");
        Directory.CreateDirectory(directory);
        var paths = new List<string>();
        for (var index = 0; index < count; index++)
        {
            var group = groups[index];
            if (group.Count == 0) continue;
            var identity = string.Join("\n", group.Select(record => $"{record.Address:X8}:{record.Symbol}:{record.SourceFingerprint}"));
            var hash = ChecksumUtilities.Sha256Hex(Encoding.UTF8.GetBytes($"{profile}\n{identity}"));
            var path = Path.Combine(directory, $"registration_{index:D2}_{hash[..16]}.cpp");
            var source = new StringBuilder();
            source.AppendLine("// Translator-owned bulk registration; do not edit.");
            source.AppendLine("#include \"abi_bridge.h\"");
            source.AppendLine("#include <cstddef>");
            foreach (var symbol in group.Select(static record => record.Symbol).Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal))
            {
                source.AppendLine($"extern \"C\" void {symbol}(CpuContext* ctx);");
            }
            source.AppendLine("namespace {");
            source.AppendLine("const BulkTranslatedFunctionRecord kRecords[] = {");
            foreach (var record in group.OrderBy(static record => record.Address).ThenBy(static record => record.RegistrationKind, StringComparer.Ordinal))
            {
                var dynamic = traits.TryGetValue(record.Address, out var trait) && trait.MustRemainDynamicallyDispatchable;
                var kind = record.RegistrationKind == "mod" ? "FunctionKind::ModTranslated" : "FunctionKind::BaseTranslated";
                source.AppendLine(
                    $"    {{0x{record.Address:X8}u, \"{Escape(record.Name)}\", &{record.Symbol}, {kind}, {Bool(record.PreservesNonvolatileFprs)}, 0x{record.NonvolatileFprWriteMask:X8}u, {record.Priority}u, {record.ModuleId}ull, {Bool(dynamic)}}},");
            }
            source.AppendLine("};");
            source.AppendLine("const BulkTranslatedFunctionRegistrar kRegistrar(kRecords, std::size(kRecords));");
            source.AppendLine("} // namespace");
            WriteIfChanged(path, source.ToString());
            paths.Add(path);
        }
        return paths;
    }

    private static string WriteIndirectDispatchTable(
        string outputRoot,
        string profile,
        IReadOnlyDictionary<uint, Trait> traits)
    {
        var entries = traits
            .Where(static pair => pair.Value.Available &&
                                  pair.Value.WinnerSymbol is not null &&
                                  !pair.Value.WinnerKind.Equals("native", StringComparison.OrdinalIgnoreCase))
            .OrderBy(static pair => pair.Key)
            .ToArray();
        if (entries.Length == 0)
            throw new InvalidDataException($"Profile '{profile}' has no verified translated indirect dispatch entries.");

        var identity = string.Join("\n", entries.Select(static pair =>
            $"{pair.Key:X8}:{pair.Value.WinnerSymbol}:{pair.Value.WinnerKind}:" +
            $"{pair.Value.PreservesNonvolatileFprs}:{pair.Value.NonvolatileFprWriteMask:X8}"));
        var hash = ChecksumUtilities.Sha256Hex(Encoding.UTF8.GetBytes($"{profile}\n{identity}"));
        var directory = Path.Combine(outputRoot, $"{profile}_dispatch");
        var path = Path.Combine(directory, $"indirect_dispatch_{hash[..16]}.cpp");
        var source = new StringBuilder();
        source.AppendLine("// Translator-owned immutable indirect dispatch table; do not edit.");
        source.AppendLine("#include \"abi_bridge.h\"");
        source.AppendLine("#include <iterator>");
        foreach (var symbol in entries.Select(static pair => pair.Value.WinnerSymbol!)
                     .Distinct(StringComparer.Ordinal).Order(StringComparer.Ordinal))
        {
            source.AppendLine($"extern \"C\" void {symbol}(CpuContext* ctx);");
        }
        source.AppendLine("namespace {");
        source.AppendLine("const RawDispatchRecord kEntries[] = {");
        foreach (var (address, trait) in entries)
        {
            var mask = trait.PreservesNonvolatileFprs
                ? 0u
                : trait.NonvolatileFprWriteMask & 0xFFFFC000u;
            source.AppendLine(
                $"    {{0x{address:X8}u, &{trait.WinnerSymbol}, 0x{mask:X8}u, false}},");
        }
        source.AppendLine("};");

        var entryIndices = entries
            .Select(static (pair, index) => (pair.Key, Index: index))
            .ToDictionary(static pair => pair.Key, static pair => pair.Index);
        var segments = entries
            .GroupBy(static pair => (byte)(pair.Key >> 24))
            .ToDictionary(static group => group.Key, static group => group.ToArray());
        foreach (var (segmentNumber, segmentEntries) in segments.OrderBy(static pair => pair.Key))
        {
            var firstPage = segmentEntries.Min(static pair => (pair.Key >> 12) & 0x0FFFu);
            var lastPage = segmentEntries.Max(static pair => (pair.Key >> 12) & 0x0FFFu);
            source.AppendLine($"const StaticIndirectDispatchPage kPages_{segmentNumber:X2}[] = {{");
            for (var page = firstPage; page <= lastPage; ++page)
            {
                var pageEntries = segmentEntries
                    .Where(pair => ((pair.Key >> 12) & 0x0FFFu) == page)
                    .OrderBy(static pair => pair.Key)
                    .ToArray();
                if (pageEntries.Length == 0)
                {
                    source.AppendLine("    {0u, 0u, 0u},");
                    continue;
                }
                var firstEntry = entryIndices[pageEntries[0].Key];
                source.AppendLine($"    {{{firstEntry}u, {pageEntries.Length}u, 0u}},");
            }
            source.AppendLine("};");
        }

        source.AppendLine("const StaticIndirectDispatchSegment kSegments[256] = {");
        for (var segmentNumber = 0; segmentNumber < 256; ++segmentNumber)
        {
            if (!segments.TryGetValue((byte)segmentNumber, out var segmentEntries))
            {
                source.AppendLine("    {nullptr, 0u, 0u},");
                continue;
            }
            var firstPage = segmentEntries.Min(static pair => (pair.Key >> 12) & 0x0FFFu);
            var lastPage = segmentEntries.Max(static pair => (pair.Key >> 12) & 0x0FFFu);
            source.AppendLine(
                $"    {{kPages_{segmentNumber:X2}, 0x{firstPage:X3}u, {lastPage - firstPage + 1}u}},");
        }
        source.AppendLine("};");
        source.AppendLine(
            $"const StaticIndirectDispatchTable kTable{{\"{Escape(profile)}\", kSegments, kEntries, std::size(kEntries)}};");
        source.AppendLine("const StaticIndirectDispatchTableRegistrar kRegistrar(&kTable);");
        source.AppendLine("} // namespace");
        WriteIfChanged(path, source.ToString());
        return path;
    }

    private static IReadOnlyList<string> ReadRetroExtraSources(string? cppDirectory, IReadOnlyList<FunctionRecord> modRecords)
    {
        if (string.IsNullOrWhiteSpace(cppDirectory) || !Directory.Exists(cppDirectory)) return Array.Empty<string>();
        var functionPaths = modRecords.Select(static record => record.SourcePath).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return Directory.EnumerateFiles(cppDirectory, "*.*", SearchOption.AllDirectories)
            .Where(path => path.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) || path.EndsWith(".S", StringComparison.OrdinalIgnoreCase))
            .Select(Path.GetFullPath)
            .Where(path => !functionPaths.Contains(path))
            .Order(StringComparer.OrdinalIgnoreCase)
            .ToArray();
    }

    private static void WriteCMakeManifest(
        string path,
        string outputRoot,
        int sharedBaseCount,
        int portableSharedBaseCount,
        int activeBaseCount,
        int modCount,
        int sensitiveTargetCount,
        int sensitiveCallerCount,
        IReadOnlyList<ShardInfo> shards,
        IReadOnlyList<string> baseRegistration,
        IReadOnlyList<string> rrRegistration,
        IReadOnlyList<string> retroExtras)
    {
        var output = new StringBuilder();
        output.AppendLine("# Translator-owned stable shard graph; do not edit.");
        output.AppendLine($"set(MKW_TRANSLATED_SHARD_ROOT \"{CMakePath(outputRoot)}\")");
        output.AppendLine($"set(MKW_BASE_FUNCTION_COUNT {activeBaseCount})");
        output.AppendLine($"set(MKW_SHARED_BASE_FUNCTION_COUNT {sharedBaseCount})");
        output.AppendLine($"set(MKW_PORTABLE_SHARED_BASE_FUNCTION_COUNT {portableSharedBaseCount})");
        output.AppendLine($"set(MKW_PROFILE_SENSITIVE_TARGET_COUNT {sensitiveTargetCount})");
        output.AppendLine($"set(MKW_PROFILE_SENSITIVE_CALLER_COUNT {sensitiveCallerCount})");
        output.AppendLine($"set(MKW_RETRO_REWIND_FUNCTION_COUNT {modCount})");
        // Ninja seeds ready edges in manifest order.  Put longest predicted jobs
        // first (LPT) so a large shard cannot become a serial tail.
        IEnumerable<string> Scheduled(string partition) => shards
            .Where(shard => shard.Partition == partition)
            .OrderByDescending(static shard => shard.CompileCostWeight)
            .ThenBy(static shard => shard.Fingerprint, StringComparer.Ordinal)
            .Select(static shard => shard.Path);
        AppendCMakeList(output, "MKW_BASE_COMMON_SHARDS", Scheduled("base_common"), preserveOrder: true);
        AppendCMakeList(output, "MKW_BASE_PORTABLE_SENSITIVE_SHARDS", Scheduled("base_portable_sensitive"), preserveOrder: true);
        AppendCMakeList(output, "MKW_RETRO_PORTABLE_SENSITIVE_SHARDS", Scheduled("retro_portable_sensitive"), preserveOrder: true);
        AppendCMakeList(output, "MKW_RETRO_MOD_SHARDS", Scheduled("retro_mod"), preserveOrder: true);
        AppendCMakeList(output, "MKW_BASE_REGISTRATION_SOURCES", baseRegistration);
        AppendCMakeList(output, "MKW_RETRO_REGISTRATION_SOURCES", rrRegistration);
        AppendCMakeList(output, "MKW_RETRO_EXTRA_SOURCES", retroExtras);
        output.AppendLine($"set(MKW_HAVE_RETRO_REWIND_SHARDS {(modCount > 0 ? "ON" : "OFF")})");
        WriteIfChanged(path, output.ToString());
    }

    private static void AppendCMakeList(
        StringBuilder output,
        string name,
        IEnumerable<string> values,
        bool preserveOrder = false)
    {
        output.AppendLine($"set({name}");
        var ordered = preserveOrder ? values : values.Order(StringComparer.OrdinalIgnoreCase);
        foreach (var value in ordered) output.AppendLine($"  \"{CMakePath(value)}\"");
        output.AppendLine(")");
    }

    private static void PruneStaleShardSources(
        string outputRoot,
        IReadOnlyList<ShardInfo> shards,
        IReadOnlyList<string> baseRegistration,
        IReadOnlyList<string> rrRegistration)
    {
        var keep = shards.Select(static shard => Path.GetFullPath(shard.Path))
            .Concat(shards.Select(static shard => Path.Combine(
                Path.GetDirectoryName(shard.Path)!,
                Path.GetFileNameWithoutExtension(shard.Path) + "_traits.h")))
            .Concat(baseRegistration)
            .Concat(rrRegistration)
            .Select(Path.GetFullPath)
            .ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var directory in Directory.EnumerateDirectories(outputRoot))
        {
            foreach (var file in Directory.EnumerateFiles(directory, "*.*", SearchOption.AllDirectories)
                         .Where(static file => file.EndsWith(".cpp", StringComparison.OrdinalIgnoreCase) ||
                                               file.EndsWith(".h", StringComparison.OrdinalIgnoreCase)))
            {
                if (!keep.Contains(Path.GetFullPath(file))) File.Delete(file);
            }
        }
    }

    // Aggregate shards total hundreds of megabytes; the unchanged-check streams the
    // live file instead of decoding all of it into a string just to throw it away.
    private static void WriteIfChanged(string path, string content) =>
        FileOutput.WriteTextIfChanged(path, content);

    private static string Bool(bool value) => value ? "true" : "false";
    private static string CMakePath(string value) => Path.GetFullPath(value).Replace('\\', '/').Replace("\"", "\\\"");
    private static string Escape(string value) => CxxStringLiteralEscaping.Escape(value);
    private static uint ParseHexUInt32(string value) => GuestTargetParser.ParseHexAddress(value);

    private static Regex BaseRegistrationRegex() => GeneratedMarkers.BaseRegistrationPattern();

    private static Regex ModRegistrationRegex() => GeneratedMarkers.ModRegistrationPattern();

    private static Regex RegistrationMarkerRegex() => GeneratedMarkers.RegistrationLinePattern();

    [GeneratedRegex(@"InvokeDirectCpu<0x(?<address>[0-9A-Fa-f]{8})u>\s*\(\s*ctx\s*\)")]
    private static partial Regex DirectCallRegex();

}
