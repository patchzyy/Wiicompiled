using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods.Mkwii;

public static class RetroRewindRegionPatches
{
    private const uint PalVoiceHook = 0x808DBCD8;
    private const uint UsVoiceHook = 0x808D7580;

    // CustomCharacterVoice.cpp's HoldRosalinaLumaSound pointer retains its PAL address
    // in the shipped US chunk. The clean US REL's relocated vtable identifies the slot;
    // these surrounding entries distinguish it from other tables using HoldSoundLimited.
    private static readonly (uint Address, uint Target)[] UsVoiceTable =
    [
        (0x808D7540, 0x806FC03C),
        (0x808D7544, 0x806FBC94),
        (0x808D7570, 0x806FBC3C),
        (0x808D7574, 0x806FBC34),
        (0x808D7578, 0x806FA3A8),
        (0x808D757C, 0x806FAA04),
        (UsVoiceHook, 0x806FAB44),
        (0x808D7584, 0x806FAD58),
        (0x808D7588, 0x806FA7B0),
        (0x808D758C, 0x806FA83C),
        (0x808D7590, 0x806FA8D0),
        (0x808D75BC, 0x808611E8),
        (0x808D75C0, 0x806FA824)
    ];

    public static KamekChunk CorrectUsVoiceHook(KamekChunk chunk, string? gameId, ProgramImage image)
    {
        if (!string.Equals(gameId, "RMCE01", StringComparison.OrdinalIgnoreCase))
            return chunk;

        var matching = chunk.Commands.Where(c => c.AddressIsAbsolute && c.Address == PalVoiceHook).ToArray();
        if (matching.Length == 0)
            return chunk;
        if (matching.Length != 1 || matching[0].Id != KamekCommandId.Addr32 ||
            matching[0].Arguments.Count != 1 || matching[0].Arguments[0] >= chunk.CodeSize ||
            chunk.Commands.Any(c => c.AddressIsAbsolute && c.Address == UsVoiceHook))
            throw new InvalidDataException("Unexpected Retro Rewind US voice hook commands; cannot correct the PAL address.");

        foreach (var (address, target) in UsVoiceTable)
        {
            if (!image.Contains(address, 4) ||
                BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(address, 4), 4)) != target)
                throw new InvalidDataException($"Retro Rewind US voice vtable check failed at 0x{address:X8}.");
        }

        return new KamekChunk(chunk.Index, chunk.FileOffset, chunk.BssSize, chunk.CodeSize,
            chunk.CtorStart, chunk.CtorEnd, chunk.ChunkSize, chunk.CodeBlob,
            chunk.Commands.Select(c => ReferenceEquals(c, matching[0]) ? c with { Address = UsVoiceHook } : c).ToArray());
    }

    private const uint EsrbBranchSite = 0x80604094;
    private const uint EsrbOriginalInstruction = 0x3C608038;
    private const uint EsrbSkipBranch = 0x4800001C;
    private const int EsrbStoreIndex = 7;

    // PulsarSystem.cpp's removeESRB boot hook: if (region == 'E') *(u32*)0x80604094 = 0x4800001C.
    private static readonly uint[] EsrbBootHook =
    [
        0x3C608000, 0x88030003, 0x2C000045, 0x4C820020, 0x3C804800, 0x3C608060, 0x3804001C, 0x90034094, 0x4E800020
    ];

    /// <summary>
    /// Translated code cannot be rewritten at run time, so the pack's NTSC-U ESRB skip becomes a
    /// static patch and its run-time store a nop.
    /// </summary>
    public static KamekChunk ApplyUsEsrbSkip(KamekChunk chunk, string? gameId, ProgramImage image)
    {
        if (!string.Equals(gameId, "RMCE01", StringComparison.OrdinalIgnoreCase))
            return chunk;

        var sites = new List<int>();
        for (var offset = 0; offset + EsrbBootHook.Length * 4 <= chunk.CodeBlob.Length; offset += 4)
        {
            var match = true;
            for (var i = 0; i < EsrbBootHook.Length && match; i++)
                match = BinaryPrimitives.ReadUInt32BigEndian(chunk.CodeBlob.AsSpan(offset + i * 4, 4)) == EsrbBootHook[i];
            if (match)
                sites.Add(offset);
        }
        if (sites.Count == 0)
            return chunk;
        if (sites.Count != 1 || chunk.Commands.Any(c => c.AddressIsAbsolute && c.Address == EsrbBranchSite))
            throw new InvalidDataException("Unexpected Retro Rewind ESRB skip; cannot apply it statically.");
        if (!image.Contains(EsrbBranchSite, 4) ||
            BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(EsrbBranchSite, 4), 4)) != EsrbOriginalInstruction)
            throw new InvalidDataException($"Retro Rewind ESRB skip target check failed at 0x{EsrbBranchSite:X8}.");

        var code = chunk.CodeBlob.ToArray();
        BinaryPrimitives.WriteUInt32BigEndian(code.AsSpan(sites[0] + EsrbStoreIndex * 4, 4), 0x60000000);
        var branch = new KamekCommand(-1, ((uint)KamekCommandId.Write32 << 24) | 0xFFFFFE, KamekCommandId.Write32,
            true, EsrbBranchSite, [EsrbSkipBranch]);
        return new KamekChunk(chunk.Index, chunk.FileOffset, chunk.BssSize, chunk.CodeSize,
            chunk.CtorStart, chunk.CtorEnd, chunk.ChunkSize, code, chunk.Commands.Append(branch).ToArray());
    }
}
