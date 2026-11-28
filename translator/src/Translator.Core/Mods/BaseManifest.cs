using Translator.Core.IO;
using System.Text.Json;

namespace Translator.Core.Mods;

public sealed record BaseManifest(
    string Format,
    int FormatVersion,
    string GameId,
    string Region,
    string ImageSha256,
    uint RelBaseAddress,
    IReadOnlyList<BaseSectionMetadata> Sections,
    IReadOnlyList<BaseFunctionRangeMetadata> Functions,
    string FunctionRangesFile);

public sealed record BaseSectionMetadata(
    string Name,
    string Source,
    uint GuestStart,
    uint GuestEnd,
    bool Executable,
    bool Writable,
    string? ImageFile,
    uint ImageOffset);

public sealed record BaseFunctionRangeMetadata(
    uint Start,
    uint End,
    string Name,
    string SourceSection,
    uint SourceImageOffset,
    string RangeSource,
    IReadOnlyList<string> Flags);

public sealed class BaseFunctionIndex
{
    private readonly List<BaseFunctionRangeMetadata> _functions;

    public BaseFunctionIndex(IEnumerable<BaseFunctionRangeMetadata> functions)
    {
        _functions = functions.OrderBy(f => f.Start).ToList();
    }

    public static BaseFunctionIndex Load(string path)
    {
        var json = TrackedFs.ReadAllText(path);
        var manifest = JsonSerializer.Deserialize<BaseManifest>(json)
            ?? throw new InvalidDataException($"Failed to parse base manifest: {path}");
        return new BaseFunctionIndex(manifest.Functions);
    }

    public BaseFunctionRangeMetadata? FindContaining(uint address)
    {
        var lo = 0;
        var hi = _functions.Count - 1;
        while (lo <= hi)
        {
            var mid = lo + ((hi - lo) / 2);
            var function = _functions[mid];
            if (address < function.Start)
            {
                hi = mid - 1;
            }
            else if (address >= function.End)
            {
                lo = mid + 1;
            }
            else
            {
                return function;
            }
        }

        return null;
    }
}
