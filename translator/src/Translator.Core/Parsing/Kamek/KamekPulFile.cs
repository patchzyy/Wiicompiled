using Translator.Core.IO;
using System.Buffers.Binary;

namespace Translator.Core.Parsing.Kamek;

public sealed class KamekPulFile
{
    private static readonly char[] DefaultRegionOrder = ['P', 'E', 'J', 'K'];

    public KamekPulFile(bool isCombined, IReadOnlyList<uint> combinedChunkSizes, IReadOnlyList<KamekChunk> chunks)
    {
        IsCombined = isCombined;
        CombinedChunkSizes = combinedChunkSizes;
        Chunks = chunks;
    }

    public bool IsCombined { get; }
    public IReadOnlyList<uint> CombinedChunkSizes { get; }
    public IReadOnlyList<KamekChunk> Chunks { get; }

    public static KamekPulFile Load(string path) => Parse(TrackedFs.ReadAllBytes(path));

    public static KamekPulFile Parse(byte[] data)
    {
        if (data.Length < KamekChunk.HeaderSize)
        {
            throw new InvalidDataException("File is too small to contain a Kamek chunk.");
        }

        if (TryParseCombined(data, out var combined))
        {
            return combined;
        }

        if (!KamekChunk.HasMagic(data, 0))
        {
            throw new InvalidDataException("File is neither a combined Code.pul nor a raw supported Kamek chunk.");
        }

        var rawChunk = KamekChunk.Parse(data, 0, expectedSize: 0, index: 0);
        if (rawChunk.ChunkSize != data.Length)
        {
            throw new InvalidDataException($"Raw Kamek chunk size 0x{rawChunk.ChunkSize:X} does not match file length 0x{data.Length:X}.");
        }

        return new KamekPulFile(false, Array.Empty<uint>(), [rawChunk]);
    }

    public KamekChunk SelectRegion(string region)
    {
        if (region is not ("P" or "E" or "J" or "K"))
        {
            throw new ArgumentException("Region must be exactly P, E, J, or K.", nameof(region));
        }

        var regionChar = region[0];
        var regionIndex = Array.IndexOf(DefaultRegionOrder, regionChar);
        var chunk = Chunks.FirstOrDefault(c => c.Index == regionIndex);
        return chunk ?? throw new InvalidDataException($"Code.pul does not contain a non-empty chunk for region {regionChar} at index {regionIndex}.");
    }

    private static bool TryParseCombined(byte[] data, out KamekPulFile file)
    {
        file = null!;
        if (data.Length < 0x10 + KamekChunk.HeaderSize)
        {
            return false;
        }

        Span<uint> sizes = stackalloc uint[4];
        long totalSize = 0x10;
        var nonzeroCount = 0;
        for (var i = 0; i < 4; i++)
        {
            sizes[i] = BinaryPrimitives.ReadUInt32BigEndian(data.AsSpan(i * 4, 4));
            totalSize += sizes[i];
            if (sizes[i] != 0)
            {
                nonzeroCount++;
            }
        }

        if (nonzeroCount == 0 || totalSize != data.Length)
        {
            return false;
        }

        var chunks = new List<KamekChunk>();
        var offset = 0x10;
        for (var i = 0; i < 4; i++)
        {
            var size = sizes[i];
            if (size == 0)
            {
                continue;
            }

            if (size < KamekChunk.HeaderSize || offset + size > data.Length || !KamekChunk.HasMagic(data, offset))
            {
                return false;
            }

            chunks.Add(KamekChunk.Parse(data, offset, checked((int)size), i));
            offset += checked((int)size);
        }

        file = new KamekPulFile(true, sizes.ToArray(), chunks);
        return true;
    }
}
