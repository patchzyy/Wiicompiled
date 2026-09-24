using System.Text.RegularExpressions;

namespace Translator.Core;

/// <summary>
/// The per-region guest address table shared with the runtime (runtime/include/region/*.h).
/// The runtime spells every guest address it names as its PAL identity and resolves it through
/// <c>#define MKW_G_&lt;pal&gt; &lt;region&gt;</c> lines; this class reads the same header so the
/// translator's regex scan of the native sources sees the addresses the compiler will produce.
/// With no table loaded the scan reads the sources verbatim, which is the PAL build.
/// </summary>
public sealed partial class GuestAddressTable
{
    private readonly Dictionary<string, string> _tokens;

    private GuestAddressTable(Dictionary<string, string> tokens, string sourcePath)
    {
        _tokens = tokens;
        SourcePath = sourcePath;
    }

    public static GuestAddressTable? Current { get; set; }

    public string SourcePath { get; }

    public int Count => _tokens.Count;

    public static GuestAddressTable Load(string path)
    {
        if (!File.Exists(path))
            throw new FileNotFoundException("Configured guest address table was not found.", path);
        var tokens = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var line in File.ReadLines(path))
        {
            var match = DefinePattern().Match(line);
            if (!match.Success) continue;
            tokens[match.Groups["pal"].Value] = match.Groups["region"].Value;
        }
        if (tokens.Count == 0)
            throw new InvalidDataException($"Guest address table {path} defines no MKW_G_ tokens.");
        return new GuestAddressTable(tokens, path);
    }

    /// <summary>Resolves a PAL identity token; throws for an address the table does not carry.</summary>
    public string Resolve(string palToken)
    {
        if (_tokens.TryGetValue(palToken, out var region)) return region;
        throw new InvalidDataException(
            $"Guest address table {SourcePath} has no entry for PAL address {palToken}; " +
            "regenerate it with tools/region/gen_region_headers.py.");
    }

    /// <summary>
    /// Rewrites the address-carrying macro forms of a runtime source so every downstream regex
    /// (registrations, fatal stubs, ABI signatures) reads region addresses. MKW_GADDR(x) becomes
    /// the plain 0x literal it expands to.
    /// </summary>
    public string Rewrite(string source)
    {
        source = MacroTokenPattern().Replace(source, m => m.Groups["head"].Value + Resolve(m.Groups["pal"].Value));
        // No `u` suffix: the registration regexes expect the comma right after the eight digits.
        source = GaddrPattern().Replace(source, m => "0x" + Resolve(m.Groups["pal"].Value));
        source = GuestFuncPattern().Replace(source, m => "func_" + Resolve(m.Groups["pal"].Value));

        // This normalization is the one place the translator has to agree with the C
        // preprocessor. If any address macro survives it, the two have diverged - a spelling
        // these patterns do not match, in a file the compiler resolves fine - and every
        // downstream scan would silently read a PAL address as though it were this region's.
        // Fail loudly instead: a surviving macro is a bug in this file, not in the source.
        // Skips macro definitions, whose argument is a parameter name rather than an address
        // (region/guest_region.h defines MKW_GADDR itself; GX_FATAL_STUB expands it around its
        // own `addr` parameter). Those resolve at the call site, which does carry a literal.
        // Definitions continue across lines, so the continuation has to be skipped too.
        var inDefine = false;
        foreach (var raw in source.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            var wasContinued = inDefine && line.TrimEnd().EndsWith("\\", StringComparison.Ordinal);
            var startsDefine = line.TrimStart().StartsWith("#define", StringComparison.Ordinal);
            var skip = inDefine || startsDefine;
            inDefine = (startsDefine || wasContinued) && line.TrimEnd().EndsWith("\\", StringComparison.Ordinal);
            if (skip) continue;
            var leftover = LeftoverMacroPattern().Match(line);
            if (!leftover.Success) continue;
            throw new InvalidDataException(
                $"Guest address macro '{leftover.Value}' was not resolved against {SourcePath}. " +
                "GuestAddressTable's patterns and the runtime's macro spellings have diverged: " +
                $"the unresolved use is '{line.Trim()}'.");
        }
        return source;
    }

    [GeneratedRegex(@"^\s*#\s*define\s+MKW_G_(?<pal>[0-9A-Fa-f]{8})\s+0x(?<region>[0-9A-Fa-f]{8})u", RegexOptions.CultureInvariant)]
    private static partial Regex DefinePattern();

    [GeneratedRegex(@"(?<head>(?:PPC_NATIVE_OVERRIDE(?:_VOID)?|GX_FATAL_STUB)\s*\(\s*)(?<pal>[0-9A-Fa-f]{8})\b", RegexOptions.CultureInvariant)]
    private static partial Regex MacroTokenPattern();

    [GeneratedRegex(@"MKW_GADDR\s*\(\s*(?<pal>[0-9A-Fa-f]{8})\s*\)", RegexOptions.CultureInvariant)]
    private static partial Regex GaddrPattern();

    [GeneratedRegex(@"MKW_GUEST_FUNC\s*\(\s*(?<pal>[0-9A-Fa-f]{8})\s*\)", RegexOptions.CultureInvariant)]
    private static partial Regex GuestFuncPattern();

    [GeneratedRegex(@"\bMKW_(?:GADDR|GUEST_FUNC)\s*\(", RegexOptions.CultureInvariant)]
    private static partial Regex LeftoverMacroPattern();
}
