using System.Text.RegularExpressions;

namespace Translator.Core;

/// <summary>Column order of runtime/include/guest_abi/guest_address_table.inc.</summary>
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
/// The runtime names guest addresses by their PAL address. This is the table it resolves them
/// through, read from the same file the C++ includes.
/// </summary>
public sealed partial class GuestAddressTable
{
    private readonly Dictionary<uint, uint[]> _rows;

    private GuestAddressTable(Dictionary<uint, uint[]> rows, string sourcePath)
    {
        _rows = rows;
        SourcePath = sourcePath;
    }

    public string SourcePath { get; }

    public int Count => _rows.Count;

    /// <summary>The table beside a runtime whose native sources live in <paramref name="nativeSourceDirectory"/>.</summary>
    public static string DefaultPath(string nativeSourceDirectory) =>
        Path.GetFullPath(Path.Combine(nativeSourceDirectory, "..", "include", "guest_abi", "guest_address_table.inc"));

    public static GuestAddressTable Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("The runtime's guest address table was not found.", path);
        var rows = new Dictionary<uint, uint[]>();
        foreach (var line in File.ReadLines(path))
        {
            var match = RowPattern().Match(line);
            if (!match.Success) continue;
            var pal = Convert.ToUInt32(match.Groups["pal"].Value, 16);
            if (!rows.TryAdd(pal, [pal, Convert.ToUInt32(match.Groups["u"].Value, 16), Convert.ToUInt32(match.Groups["j"].Value, 16)]))
                throw new InvalidDataException($"{path} lists PAL address 0x{pal:X8} twice.");
        }
        if (rows.Count == 0)
            throw new InvalidDataException($"{path} has no MKW_GUEST_ADDRESS rows.");
        return new GuestAddressTable(rows, path);
    }

    public uint Map(uint pal, GuestRegion region)
    {
        if (_rows.TryGetValue(pal, out var row))
            return row[(int)region];
        throw new InvalidDataException(
            $"PAL address 0x{pal:X8} is missing from {SourcePath}; regenerate it with tools/region/gen_guest_address_table.py.");
    }

    [GeneratedRegex(@"^\s*MKW_GUEST_ADDRESS\(\s*(?<pal>[0-9A-Fa-f]{8})\s*,\s*(?<u>[0-9A-Fa-f]{8})\s*,\s*(?<j>[0-9A-Fa-f]{8})\s*\)", RegexOptions.CultureInvariant)]
    private static partial Regex RowPattern();
}
