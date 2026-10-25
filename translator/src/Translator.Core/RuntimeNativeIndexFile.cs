using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using Translator.Core.Analysis;
using Translator.Core.IO;

namespace Translator.Core;

/// <summary>
/// On-disk form of <see cref="RuntimeNativeIndex"/>. It is only trusted while its
/// source digest matches the native sources it was extracted from.
/// </summary>
public static class RuntimeNativeIndexFile
{
    public const string Format = "wiicompiled-runtime-native-index";
    public const int FormatVersion = 2;
    // Bump whenever the scanner or the ABI/effect analyzers can produce different output for the same sources.
    public const int ExtractorVersion = 2;
    public const string DefaultFileName = "native-index.json";

    private static readonly JsonSerializerOptions Options = new()
    {
        PropertyNamingPolicy = JsonNamingPolicy.CamelCase,
        UnmappedMemberHandling = JsonUnmappedMemberHandling.Disallow,
        WriteIndented = true
    };

    /// <summary>
    /// Lowercase hex SHA-256 over one <c>"&lt;file sha256&gt;  &lt;relative path&gt;\n"</c> line per
    /// scanned file, ordered by relative path (ordinal, '/' separators).
    /// </summary>
    public static string ComputeSourceDigest(string nativeSourceDirectory)
    {
        var root = Path.GetFullPath(nativeSourceDirectory);
        var manifest = new StringBuilder();
        foreach (var (relative, path) in NativeSourceParsing.EnumerateSourcePaths(root)
                     .Select(path => (Relative: NativeSourceParsing.RelativePath(root, path), Path: path))
                     .OrderBy(static item => item.Relative, StringComparer.Ordinal))
        {
            using var stream = File.OpenRead(path);
            manifest.Append(Convert.ToHexString(SHA256.HashData(stream)).ToLowerInvariant())
                .Append("  ").Append(relative).Append('\n');
        }
        return Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(manifest.ToString()))).ToLowerInvariant();
    }

    public static byte[] Serialize(RuntimeNativeIndex index, string sourceDigest)
    {
        var document = new IndexDocument
        {
            Format = Format,
            FormatVersion = FormatVersion,
            ExtractorVersion = ExtractorVersion,
            SourceDigest = sourceDigest,
            Registrations = index.Registrations
                .OrderBy(static registration => registration.Address)
                .ThenBy(static registration => registration.Symbol, StringComparer.Ordinal)
                .ThenBy(static registration => registration.SourceFile, StringComparer.Ordinal)
                .ThenBy(static registration => registration.IsTranslatedOverride)
                .ThenBy(static registration => registration.ExcludesBaseTranslation)
                .ThenBy(static registration => registration.ModOverlay)
                .Select(static registration => new RegistrationDto
                {
                    Address = FormatAddress(registration.Address),
                    Symbol = registration.Symbol,
                    SourceFile = registration.SourceFile,
                    IsTranslatedOverride = registration.IsTranslatedOverride,
                    ExcludesBaseTranslation = registration.ExcludesBaseTranslation,
                    ModOverlay = registration.ModOverlay.ToString()
                })
                .ToList(),
            VoidStubAbis = index.VoidStubAbis
                .OrderBy(static entry => entry.Address)
                .Select(static entry => new AbiDto
                {
                    Address = FormatAddress(entry.Address),
                    ArgumentRegisters = entry.ArgumentRegisters.ToList(),
                    ScalarFloatArgumentRegisters = entry.ScalarFloatArgumentRegisters.ToList()
                })
                .ToList(),
            Effects = index.Effects
                .OrderBy(static entry => entry.Address)
                .Select(static entry => new EffectDto
                {
                    Address = FormatAddress(entry.Address),
                    IsPrecise = entry.IsPrecise,
                    Contract = ToDto(entry.Address, entry.Contract)
                })
                .ToList()
        };

        // Indented output uses the platform newline on net8; pin it so the bytes are identical everywhere.
        var json = JsonSerializer.Serialize(document, Options).Replace("\r\n", "\n", StringComparison.Ordinal);
        return Encoding.UTF8.GetBytes(json + "\n");
    }

    public static bool Write(string path, RuntimeNativeIndex index, string sourceDigest) =>
        FileOutput.WriteBytesIfChanged(path, Serialize(index, sourceDigest));

    /// <summary>
    /// Loads the index at <paramref name="path"/> when it was produced by this extractor and, unless
    /// <paramref name="expectedSourceDigest"/> is null, from exactly those sources.
    /// </summary>
    public static bool TryLoad(
        string path,
        string? expectedSourceDigest,
        out RuntimeNativeIndex index,
        out string reason)
    {
        index = new RuntimeNativeIndex([], [], []);
        IndexDocument? document;
        try
        {
            using var parsed = JsonDocument.Parse(File.ReadAllBytes(path));
            var root = parsed.RootElement;
            if (root.ValueKind != JsonValueKind.Object ||
                !root.TryGetProperty("format", out var format) || format.ValueKind != JsonValueKind.String ||
                format.GetString() != Format)
            {
                reason = "not a runtime native index";
                return false;
            }
            if (!root.TryGetProperty("formatVersion", out var formatVersion) ||
                formatVersion.ValueKind != JsonValueKind.Number || formatVersion.GetInt32() != FormatVersion)
            {
                reason = "format version differs";
                return false;
            }
            if (!root.TryGetProperty("extractorVersion", out var extractorVersion) ||
                extractorVersion.ValueKind != JsonValueKind.Number || extractorVersion.GetInt32() != ExtractorVersion)
            {
                reason = "extractor version differs";
                return false;
            }
            document = root.Deserialize<IndexDocument>(Options);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or JsonException or FormatException)
        {
            reason = $"unreadable: {ex.Message}";
            return false;
        }

        if (document is null)
        {
            reason = "empty";
            return false;
        }
        if (expectedSourceDigest is not null &&
            !string.Equals(document.SourceDigest, expectedSourceDigest, StringComparison.OrdinalIgnoreCase))
        {
            reason = "native sources changed";
            return false;
        }

        try
        {
            index = new RuntimeNativeIndex(
                Present(document.Registrations, "registrations")
                    .Select(static registration => new RuntimeNativeRegistration(
                        ParseAddress(registration.Address),
                        Present(registration.Symbol, "symbol"),
                        Present(registration.SourceFile, "sourceFile"),
                        registration.IsTranslatedOverride,
                        registration.ExcludesBaseTranslation,
                        ParseModOverlay(Present(registration.ModOverlay, "modOverlay"))))
                    .ToArray(),
                Present(document.VoidStubAbis, "voidStubAbis")
                    .Select(static entry => new RuntimeNativeAbiEntry(
                        ParseAddress(entry.Address),
                        Present(entry.ArgumentRegisters, "argumentRegisters").Select(static r => Present(r, "register")).ToArray(),
                        Present(entry.ScalarFloatArgumentRegisters, "scalarFloatArgumentRegisters").Select(static r => Present(r, "register")).ToArray()))
                    .ToArray(),
                Present(document.Effects, "effects")
                    .Select(static entry => new RuntimeNativeEffectEntry(
                        ParseAddress(entry.Address), FromDto(Present(entry.Contract, "contract")), entry.IsPrecise))
                    .ToArray());
        }
        catch (FormatException ex)
        {
            reason = $"unreadable: {ex.Message}";
            return false;
        }

        reason = string.Empty;
        return true;
    }

    private static string FormatAddress(uint address) => $"0x{address:X8}";

    private static NativeModOverlay ParseModOverlay(string text) =>
        Enum.TryParse<NativeModOverlay>(text, ignoreCase: false, out var value) && Enum.IsDefined(value)
            ? value
            : throw new FormatException($"Unknown modOverlay '{text}'.");

    private static uint ParseAddress(string text) => GuestTargetParser.ParseHexAddress(text);

    private static T Present<T>(T? value, string name) where T : class =>
        value ?? throw new FormatException($"'{name}' is null.");

    private static ContractDto ToDto(uint address, GuestAbiContract contract)
    {
        var dto = new ContractDto
        {
            GprReadBeforeWriteMask = contract.GprReadBeforeWriteMask,
            GprPossibleWriteMask = contract.GprPossibleWriteMask,
            GprReturnMask = contract.GprReturnMask,
            FprReadBeforeWriteMask = contract.FprReadBeforeWriteMask,
            FprPossibleWriteMask = contract.FprPossibleWriteMask,
            FprReturnMask = contract.FprReturnMask,
            CrReadBeforeWriteMask = contract.CrReadBeforeWriteMask,
            CrPossibleWriteMask = contract.CrPossibleWriteMask,
            ReadsXerBeforeWrite = contract.ReadsXerBeforeWrite,
            MayWriteXer = contract.MayWriteXer,
            ReadsCtrBeforeWrite = contract.ReadsCtrBeforeWrite,
            MayWriteCtr = contract.MayWriteCtr,
            ReadsLrBeforeWrite = contract.ReadsLrBeforeWrite,
            MayWriteLr = contract.MayWriteLr,
            BoundaryFlags = (int)contract.BoundaryFlags
        };
        // Native contracts only ever set the positional masks; anything else would be lost on load.
        if (!FromDto(dto).Equals(contract))
            throw new InvalidOperationException(
                $"Native contract for 0x{address:X8} uses fields the native index does not serialize.");
        return dto;
    }

    private static GuestAbiContract FromDto(ContractDto dto) =>
        new(dto.GprReadBeforeWriteMask, dto.GprPossibleWriteMask, dto.GprReturnMask,
            dto.FprReadBeforeWriteMask, dto.FprPossibleWriteMask, dto.FprReturnMask,
            dto.CrReadBeforeWriteMask, dto.CrPossibleWriteMask,
            dto.ReadsXerBeforeWrite, dto.MayWriteXer,
            dto.ReadsCtrBeforeWrite, dto.MayWriteCtr,
            dto.ReadsLrBeforeWrite, dto.MayWriteLr,
            (GuestCallBoundaryFlags)dto.BoundaryFlags, Array.Empty<uint>());

    private sealed class IndexDocument
    {
        public required string Format { get; init; }
        public required int FormatVersion { get; init; }
        public required int ExtractorVersion { get; init; }
        public required string SourceDigest { get; init; }
        public required List<RegistrationDto> Registrations { get; init; }
        public required List<AbiDto> VoidStubAbis { get; init; }
        public required List<EffectDto> Effects { get; init; }
    }

    private sealed class RegistrationDto
    {
        public required string Address { get; init; }
        public required string Symbol { get; init; }
        public required string SourceFile { get; init; }
        public required bool IsTranslatedOverride { get; init; }
        public required bool ExcludesBaseTranslation { get; init; }
        public required string ModOverlay { get; init; }
    }

    private sealed class AbiDto
    {
        public required string Address { get; init; }
        public required List<string> ArgumentRegisters { get; init; }
        public required List<string> ScalarFloatArgumentRegisters { get; init; }
    }

    private sealed class EffectDto
    {
        public required string Address { get; init; }
        public required bool IsPrecise { get; init; }
        public required ContractDto Contract { get; init; }
    }

    private sealed class ContractDto
    {
        public required uint GprReadBeforeWriteMask { get; init; }
        public required uint GprPossibleWriteMask { get; init; }
        public required uint GprReturnMask { get; init; }
        public required uint FprReadBeforeWriteMask { get; init; }
        public required uint FprPossibleWriteMask { get; init; }
        public required uint FprReturnMask { get; init; }
        public required byte CrReadBeforeWriteMask { get; init; }
        public required byte CrPossibleWriteMask { get; init; }
        public required bool ReadsXerBeforeWrite { get; init; }
        public required bool MayWriteXer { get; init; }
        public required bool ReadsCtrBeforeWrite { get; init; }
        public required bool MayWriteCtr { get; init; }
        public required bool ReadsLrBeforeWrite { get; init; }
        public required bool MayWriteLr { get; init; }
        public required int BoundaryFlags { get; init; }
    }
}
