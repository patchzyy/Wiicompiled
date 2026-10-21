using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods.Mkwii;

public static class RetroRewindRegionPatches
{
    private const uint PalRelStart = 0x805102E0;

    // Hooks the pack writes at PAL vtable slots that its NTSC-U port map leaves unported:
    // PAL slot, NTSC-U slot, and the function that NTSC-U slot holds in the clean StaticR.rel.
    private static readonly (uint Pal, uint Us, uint UsTarget)[] UnportedUsHooks =
    [
        (0x808B4D48, 0x808B0698, 0x80563BE8), // Kart::Damage::ApplyBananaDamage
        (0x808B4DFC, 0x808B074C, 0x80563CD8), // Kart::Damage::ApplyZapperDamage
        (0x808B4E14, 0x808B0764, 0x80563CCC), // Kart::Damage::ApplyShockDamage
        (0x808CB02C, 0x808C5D24, 0x8072E068), // AI::EnemyRouteController::Update
        (0x808DBCD8, 0x808D7580, 0x806FAB44), // Audio::RaceActor::HoldSoundLimited
    ];

    /// <summary>
    /// Moves the NTSC-U chunk's unported PAL vtable hooks to their NTSC-U slots. A StaticR.rel
    /// address the NTSC-U chunk shares with the PAL chunk is always unported, so an unknown one fails.
    /// </summary>
    public static KamekChunk CorrectUnportedUsHooks(KamekChunk usChunk, KamekChunk palChunk, string? gameId,
        ProgramImage image)
    {
        if (!string.Equals(gameId, "RMCE01", StringComparison.OrdinalIgnoreCase))
            return usChunk;
        if (usChunk.Commands.Count != palChunk.Commands.Count)
            throw new InvalidDataException("Retro Rewind's PAL and NTSC-U chunks have different command lists.");

        var commands = usChunk.Commands.ToArray();
        var changed = false;
        for (var i = 0; i < commands.Length; i++)
        {
            var us = commands[i];
            var pal = palChunk.Commands[i];
            if (!us.AddressIsAbsolute || !pal.AddressIsAbsolute || us.Address != pal.Address || us.Address < PalRelStart)
                continue;
            var hook = UnportedUsHooks.FirstOrDefault(h => h.Pal == us.Address);
            if (hook.Pal == 0)
                throw new InvalidDataException($"Retro Rewind's NTSC-U chunk patches unported PAL address 0x{us.Address:X8}.");
            if (us.Id != KamekCommandId.Addr32 || us.Arguments.Count != 1 || us.Arguments[0] >= usChunk.CodeSize)
                throw new InvalidDataException($"Unexpected Retro Rewind command at PAL address 0x{us.Address:X8}.");
            if (!image.Contains(hook.Us, 4) ||
                BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(image.GetOffset(hook.Us, 4), 4)) != hook.UsTarget)
                throw new InvalidDataException($"NTSC-U vtable slot check failed at 0x{hook.Us:X8}.");
            commands[i] = us with { Address = hook.Us };
            changed = true;
        }
        return changed
            ? new KamekChunk(usChunk.Index, usChunk.FileOffset, usChunk.BssSize, usChunk.CodeSize,
                usChunk.CtorStart, usChunk.CtorEnd, usChunk.ChunkSize, usChunk.CodeBlob, commands)
            : usChunk;
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
