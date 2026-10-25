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
    bool ExcludesBaseTranslation,
    NativeModOverlay ModOverlay = NativeModOverlay.Unspecified);

/// <summary>What happens to a mod overlay at a native's address.</summary>
public enum NativeModOverlay
{
    /// <summary>Plain registration: the overlay is lost, and reported as such.</summary>
    Unspecified,
    /// <summary>The native calls through to the overlay (<c>MKW_GUEST_NEXT</c>).</summary>
    Wrapper,
    /// <summary>The native deliberately takes over the overlay's address.</summary>
    Replacement
}

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

    /// <summary>
    /// The index is keyed by the PAL addresses the runtime sources name; this is the same index
    /// keyed by <paramref name="region"/>'s addresses. Every address must have a counterpart in that region.
    /// </summary>
    public RuntimeNativeIndex ForRegion(GuestAddressTable table, GuestRegion region) => new(
        Registrations.Select(entry => entry with { Address = table.Map(entry.Address, region) }).ToArray(),
        VoidStubAbis.Select(entry => entry with { Address = table.Map(entry.Address, region) }).ToArray(),
        Effects.Select(entry => entry with
        {
            Address = table.Map(entry.Address, region),
            Contract = entry.Contract with
            {
                DirectCallTargets = entry.Contract.DirectCallTargets.Select(target => table.Map(target, region)).ToArray()
            }
        }).ToArray());
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
            {
                var modOverlay = match.Groups["modOverlay"];
                Add(match, match.Groups["symbol"].Value, false,
                    !match.Groups["as"].Success && !modOverlay.Success,
                    modOverlay.Value switch
                    {
                        "WRAPPER" => NativeModOverlay.Wrapper,
                        "REPLACEMENT" => NativeModOverlay.Replacement,
                        _ => NativeModOverlay.Unspecified
                    });
            }
            foreach (Match match in GeneratedMarkers.TranslatedFunctionRegistrationPattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, true, true);
            foreach (Match match in GeneratedMarkers.NativeOverridePattern().Matches(source))
                Add(match, match.Groups["symbol"].Value, false, true);
            foreach (Match match in GeneratedMarkers.FatalStubPattern().Matches(source))
                Add(match, $"GX_FATAL_STUB_{match.Groups["address"].Value}", false, true);

            void Add(Match match, string symbol, bool translated, bool excludesBase,
                NativeModOverlay modOverlay = NativeModOverlay.Unspecified) =>
                registrations.Add(new RuntimeNativeRegistration(
                    ParseAddress(match.Groups["address"].Value), symbol, sourceFile.RelativePath,
                    translated, excludesBase, modOverlay));
        }

        return registrations
            .Distinct()
            .OrderBy(static registration => registration.Address)
            .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
            .ThenBy(static registration => registration.SourceFile, StringComparer.Ordinal)
            .ThenBy(static registration => registration.IsTranslatedOverride)
            .ThenBy(static registration => registration.ExcludesBaseTranslation)
            .ThenBy(static registration => registration.ModOverlay);
    }

    private static uint ParseAddress(string value) => GuestTargetParser.ParseHexAddress(value);
}
