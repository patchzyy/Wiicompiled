using Translator.Core.IO;
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;

namespace Translator.Core.Loading;

/// <summary>
/// The authoritative list of function entry points and, where known, symbol names: one
/// <c>hexaddr name</c> line each (unnamed entries repeat the address). Replaces the old heuristic
/// discovery seeders and the hand-maintained force-translate list that patched their misses.
/// </summary>
public sealed class FunctionMap
{
    private readonly uint[] _addresses;
    private readonly Dictionary<uint, string> _namesByAddress;
    private readonly Dictionary<string, uint> _addressesByName;

    private FunctionMap(
        string sourcePath,
        uint[] addresses,
        Dictionary<uint, string> namesByAddress,
        Dictionary<string, uint> addressesByName)
    {
        SourcePath = sourcePath;
        _addresses = addresses;
        _namesByAddress = namesByAddress;
        _addressesByName = addressesByName;
    }

    /// <summary>Path the map was read from, for diagnostics.</summary>
    public string SourcePath { get; }

    /// <summary>Every function start in the map, ascending and distinct.</summary>
    public IReadOnlyList<uint> Addresses => _addresses;

    /// <summary>Entries that carry a real symbol name.</summary>
    public int NamedCount => _namesByAddress.Count;

    public bool Contains(uint address) => _namesByAddress.ContainsKey(address) || IndexOf(address) >= 0;

    /// <summary>The map's symbol for <paramref name="address"/>, or null when unnamed or absent.</summary>
    public string? NameOf(uint address) =>
        _namesByAddress.TryGetValue(address, out var name) ? name : null;

    /// <summary>Resolves a symbol name to its address.</summary>
    public bool TryGetAddress(string symbol, out uint address) =>
        _addressesByName.TryGetValue(symbol, out address);

    /// <summary>
    /// Named entries matching "<paramref name="prefix"/>{int}", keyed by that integer. Recovers CodeWarrior
    /// save/restore thunk ranges from the map instead of hardcoding addresses.
    /// </summary>
    public IReadOnlyDictionary<int, uint> NumberedFamily(string prefix)
    {
        var family = new Dictionary<int, uint>();
        foreach (var (address, name) in _namesByAddress)
        {
            if (!name.StartsWith(prefix, StringComparison.Ordinal)) continue;
            var suffix = name.AsSpan(prefix.Length);
            if (suffix.Length == 0) continue;
            if (!int.TryParse(suffix, NumberStyles.None, CultureInfo.InvariantCulture, out var index)) continue;
            family[index] = address;
        }

        return family;
    }

    public static FunctionMap Load(string path)
    {
        if (!TrackedFs.FileExists(path))
        {
            throw new FileNotFoundException("Configured function map was not found.", path);
        }

        return Parse(TrackedFs.ReadLines(path), path);
    }

    public static FunctionMap Parse(IEnumerable<string> lines, string sourcePath)
    {
        var namesByAddress = new Dictionary<uint, string>();
        var addressesByName = new Dictionary<string, uint>(StringComparer.Ordinal);
        var addresses = new HashSet<uint>();
        var lineNumber = 0;

        foreach (var rawLine in lines)
        {
            lineNumber++;
            var line = rawLine.Trim();
            if (line.Length == 0 || line[0] == '#') continue;

            var separator = line.IndexOfAny([' ', '\t']);
            var addressText = separator < 0 ? line : line[..separator];
            if (!uint.TryParse(addressText, NumberStyles.HexNumber, CultureInfo.InvariantCulture, out var address))
            {
                throw new InvalidDataException(
                    $"Invalid function map entry at {sourcePath}:{lineNumber}: '{rawLine}' does not start with a hexadecimal address.");
            }

            addresses.Add(address);

            var name = separator < 0 ? string.Empty : line[(separator + 1)..].Trim();
            // Unnamed entries repeat the address as "0x{addr}"; that is a
            // placeholder, not a symbol, so it must never become a lookup key.
            if (name.Length == 0 || name.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
            {
                continue;
            }

            namesByAddress[address] = name;
            // A symbol can legitimately repeat (static functions in different
            // translation units); the lowest address wins so lookups are stable.
            if (!addressesByName.TryGetValue(name, out var existing) || address < existing)
            {
                addressesByName[name] = address;
            }
        }

        if (addresses.Count == 0)
        {
            throw new InvalidDataException($"Function map {sourcePath} contains no entries.");
        }

        var sorted = addresses.ToArray();
        Array.Sort(sorted);
        return new FunctionMap(sourcePath, sorted, namesByAddress, addressesByName);
    }

    private int IndexOf(uint address) => Array.BinarySearch(_addresses, address);
}
