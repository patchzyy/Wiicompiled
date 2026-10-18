using System.Text.RegularExpressions;
using Translator.Core.Analysis;
using Translator.Core.CodeGen;

namespace Translator.Core;

// todo: the entire reading of cpp/h files using regex is a bit fragile, but it works for now. 
// however this does eventually need to be replaced with another solution.
public sealed record RuntimeNativeRegistration(
    uint Address,
    string Symbol,
    string SourceFile,
    bool IsTranslatedOverride,
    bool ExcludesBaseTranslation);

public sealed record RuntimeNativeAbiEntry(
    uint Address,
    string[] ArgumentRegisters,
    string[] ScalarFloatArgumentRegisters);

public sealed record RuntimeNativeEffectEntry(
    uint Address,
    GuestAbiContract Contract,
    bool IsPrecise);

/// <summary>
/// One process-local view of the runtime's native registrations and their guest
/// ABI contracts. C++ remains the only source of truth: the translator builds
/// this index once and shares it across all consumers. Only emit-native-index
/// persists it (<see cref="RuntimeNativeIndexFile"/>).
/// </summary>
public sealed record RuntimeNativeIndex(
    RuntimeNativeRegistration[] Registrations,
    RuntimeNativeAbiEntry[] VoidStubAbis,
    RuntimeNativeEffectEntry[] Effects)
{
    public RuntimeNativeGuestEffectSet ToGuestEffectSet()
    {
        var contracts = Effects.ToDictionary(static entry => entry.Address, static entry => entry.Contract);
        var precise = Effects.Where(static entry => entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        var conservative = Effects.Where(static entry => !entry.IsPrecise)
            .Select(static entry => entry.Address).ToHashSet();
        return new RuntimeNativeGuestEffectSet(contracts, precise, conservative);
    }
}

public static class RuntimeNativeIndexBuilder
{
    public static RuntimeNativeIndex Build(string nativeSourceDirectory)
    {
        var sourceRoot = Path.GetFullPath(nativeSourceDirectory);
        if (!Directory.Exists(sourceRoot))
            return new RuntimeNativeIndex([], [], []);

        var sources = NativeSourceParsing.ReadDirectory(sourceRoot);
        var effects = RuntimeNativeGuestEffectAnalyzer.AnalyzeSources(sources);
        var abis = RuntimeNativeFunctionAbiProvider.AnalyzeVoidStubAbis(sources);
        return new RuntimeNativeIndex(
            ScanRegistrations(sources).ToArray(),
            abis.OrderBy(static item => item.Key)
                .Select(static item => new RuntimeNativeAbiEntry(
                    item.Key,
                    item.Value.ArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray(),
                    item.Value.ScalarFloatArgumentRegisters.Order(StringComparer.OrdinalIgnoreCase).ToArray()))
                .ToArray(),
            effects.Contracts.OrderBy(static item => item.Key)
                .Select(item => new RuntimeNativeEffectEntry(
                    item.Key, item.Value, effects.PreciseContracts.Contains(item.Key)))
                .ToArray());
    }

    /// <summary>
    /// The index for <paramref name="nativeSourceDirectory"/>: the serialized index at
    /// <paramref name="indexPath"/> when it matches the sources (or the sources are absent),
    /// otherwise a fresh scan.
    /// </summary>
    public static RuntimeNativeIndex Resolve(
        string nativeSourceDirectory, string? indexPath, TextWriter? diagnostics = null)
    {
        var sourceRoot = Path.GetFullPath(nativeSourceDirectory);
        if (string.IsNullOrWhiteSpace(indexPath) || !File.Exists(indexPath))
            return Build(sourceRoot);

        var expectedDigest = Directory.Exists(sourceRoot)
            ? RuntimeNativeIndexFile.ComputeSourceDigest(sourceRoot)
            : null;
        if (RuntimeNativeIndexFile.TryLoad(indexPath, expectedDigest, out var index, out var reason))
            return index;
        if (expectedDigest is null)
            throw new InvalidOperationException(
                $"The native index {indexPath} is unusable ({reason}) and the native sources it describes are missing: {sourceRoot}");

        diagnostics?.WriteLine($"[translator] ignoring native index {indexPath} ({reason}); scanning {sourceRoot}");
        return Build(sourceRoot);
    }

    private static IEnumerable<RuntimeNativeRegistration> ScanRegistrations(
        IReadOnlyList<NativeSourceFile> sources)
    {
        var registrations = new List<RuntimeNativeRegistration>();
        foreach (var sourceFile in sources)
        {
            var source = sourceFile.Content;
            foreach (Match match in GeneratedMarkers.NativeFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, !match.Groups["as"].Success);
            foreach (Match match in GeneratedMarkers.TranslatedFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, true, true);
            foreach (Match match in GeneratedMarkers.NativeOverridePattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, true);
            foreach (Match match in GeneratedMarkers.FatalStubPattern().Matches(source))
                Add(match, $"GX_FATAL_STUB_{match.Groups["address"].Value}", false, true);

            void Add(Match match, string symbol, bool translated, bool excludesBase) =>
                registrations.Add(new RuntimeNativeRegistration(
                    ParseAddress(match.Groups["address"].Value), symbol, sourceFile.RelativePath,
                    translated, excludesBase));
        }

        return registrations
            .Distinct()
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
            .ThenBy(static registration => registration.SourceFile, StringComparer.Ordinal)
            .ThenBy(static registration => registration.IsTranslatedOverride)
            .ThenBy(static registration => registration.ExcludesBaseTranslation);
    }

    private static uint ParseAddress(string value) => GuestTargetParser.ParseHexAddress(value);
}
