using System.Text.RegularExpressions;

namespace Translator.Core;

/// <summary>Region index the runtime uses (GuestRegion::Id in guest_abi/guest_region.h).</summary>
public enum GuestRegion
{
    Pal = 0,
    NtscU = 1,
    NtscJ = 2,
}

public static class GuestRegions
{
    /// <summary>
    /// The runtime region for a project's game id. Mario Kart Wii releases without a column in
    /// the address table are rejected; any other game has no region-specific runtime addresses.
    /// </summary>
    public static GuestRegion FromGameId(string? gameId) => gameId?.ToUpperInvariant() switch
    {
        "RMCE01" => GuestRegion.NtscU,
        "RMCJ01" => GuestRegion.NtscJ,
        "RMCP01" => GuestRegion.Pal,
        { } id when id.StartsWith("RMC", StringComparison.Ordinal) =>
            throw new InvalidDataException($"Mario Kart Wii release '{gameId}' is not supported."),
        _ => GuestRegion.Pal,
    };
}

/// <summary>
/// The runtime names guest addresses by their PAL address. This is the range map it resolves
/// them through (Retro Rewind versions.txt style), read from the same file the C++ includes.
/// </summary>
public sealed partial class GuestAddressTable
{
    private readonly Dictionary<GuestRegion, Range[]> _ranges;

    private readonly record struct Range(uint First, uint Last, int Delta);

    private GuestAddressTable(Dictionary<GuestRegion, Range[]> ranges, string sourcePath)
    {
        _ranges = ranges;
        SourcePath = sourcePath;
    }

    public string SourcePath { get; }

    public int Count => _ranges.Values.Sum(static ranges => ranges.Length);

    /// <summary>The map beside a runtime whose native sources live in <paramref name="nativeSourceDirectory"/>.</summary>
    public static string DefaultPath(string nativeSourceDirectory) =>
        Path.GetFullPath(Path.Combine(nativeSourceDirectory, "..", "include", "guest_abi", "guest_versions.inc"));

    public static GuestAddressTable Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("The runtime's guest address map was not found.", path);
        var ranges = new Dictionary<GuestRegion, List<Range>>();
        foreach (var line in File.ReadLines(path))
        {
            if (!line.TrimStart().StartsWith("MKW_REGION_RANGE", StringComparison.Ordinal)) continue;
            var match = RangePattern().Match(line);
            if (!match.Success)
                throw new InvalidDataException($"{path} has a malformed range: {line.Trim()}");
            var regionName = match.Groups["region"].Value;
            if (!Enum.TryParse<GuestRegion>(regionName, out var region) || !Enum.IsDefined(region) ||
                region == GuestRegion.Pal || char.IsDigit(regionName[0]))
                throw new InvalidDataException($"{path} has a range for unknown region '{regionName}'.");
            var magnitude = Convert.ToInt64(match.Groups["delta"].Value, 16);
            if (magnitude > int.MaxValue)
                throw new InvalidDataException($"{path} has an out-of-range delta: {line.Trim()}");
            var range = new Range(
                Convert.ToUInt32(match.Groups["first"].Value, 16),
                Convert.ToUInt32(match.Groups["last"].Value, 16),
                (match.Groups["sign"].Value == "-" ? -1 : 1) * (int)magnitude);
            if (range.First > range.Last)
                throw new InvalidDataException($"{path} has an empty range at 0x{range.First:X8}.");
            if (!ranges.TryGetValue(region, out var list))
                ranges[region] = list = [];
            if (list.Count != 0 && list[^1].Last >= range.First)
                throw new InvalidDataException($"{path} ranges for {region} overlap or are unsorted at 0x{range.First:X8}.");
            list.Add(range);
        }
        if (ranges.Count == 0)
            throw new InvalidDataException($"{path} has no MKW_REGION_RANGE rows.");
        return new GuestAddressTable(ranges.ToDictionary(static pair => pair.Key, static pair => pair.Value.ToArray()), path);
    }

    public uint Map(uint pal, GuestRegion region)
    {
        if (region == GuestRegion.Pal)
            return pal;
        if (_ranges.TryGetValue(region, out var ranges))
        {
            var lo = 0;
            var hi = ranges.Length;
            while (lo < hi)
            {
                var mid = lo + (hi - lo) / 2;
                if (pal < ranges[mid].First) hi = mid;
                else lo = mid + 1;
            }
            if (lo > 0 && pal <= ranges[lo - 1].Last)
                return (uint)(pal + (long)ranges[lo - 1].Delta);
        }
        throw new InvalidDataException($"PAL address 0x{pal:X8} has no {region} address in {SourcePath}.");
    }

    [GeneratedRegex(@"^\s*MKW_REGION_RANGE\(\s*(?<region>\w+)\s*,\s*(?<first>[0-9A-Fa-f]{8})\s*,\s*(?<last>[0-9A-Fa-f]{8})\s*,\s*(?<sign>[+-])0x(?<delta>[0-9A-Fa-f]{1,8})\s*\)\s*(//.*)?$", RegexOptions.CultureInvariant)]
    private static partial Regex RangePattern();
}
