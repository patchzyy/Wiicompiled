using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;

namespace Translator.Tests;

public sealed class RetroRewindRegionPatchesTests
{
    private static readonly (uint Pal, uint Us, uint Target)[] UnportedHooks =
    [
        (0x808B4D48, 0x808B0698, 0x80563BE8),
        (0x808B4DFC, 0x808B074C, 0x80563CD8),
        (0x808B4E14, 0x808B0764, 0x80563CCC),
        (0x808CB02C, 0x808C5D24, 0x8072E068),
        (0x808DBCD8, 0x808D7580, 0x806FAB44),
    ];

    [Fact]
    public void MovesEveryUnportedHookToItsUsSlotAndKeepsPortedOnes()
    {
        var pal = Chunk([.. UnportedHooks.Select(h => Command(KamekCommandId.Addr32, h.Pal)),
            Command(KamekCommandId.Branch, 0x807C8994)]);
        var us = Chunk([.. UnportedHooks.Select(h => Command(KamekCommandId.Addr32, h.Pal)),
            Command(KamekCommandId.Branch, 0x807B9F34)]);
        var corrected = RetroRewindRegionPatches.CorrectUnportedUsHooks(us, pal, "RMCE01", SlotImage());

        for (var i = 0; i < UnportedHooks.Length; i++)
        {
            Assert.Equal(UnportedHooks[i].Us, corrected.Commands[i].Address);
            Assert.Equal(us.Commands[i].Arguments, corrected.Commands[i].Arguments);
        }
        Assert.Same(us.Commands[^1], corrected.Commands[^1]);
        Assert.Same(us.CodeBlob, corrected.CodeBlob);
    }

    [Theory]
    [InlineData("RMCP01")]
    [InlineData("RMCJ01")]
    public void LeavesOtherRegionsUntouched(string gameId)
    {
        var chunk = Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8));
        Assert.Same(chunk, RetroRewindRegionPatches.CorrectUnportedUsHooks(chunk, chunk, gameId, SlotImage()));
    }

    [Fact]
    public void AcceptsAPackWhosePortMapCoversTheHooks()
    {
        var pal = Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8));
        var us = Chunk(Command(KamekCommandId.Addr32, 0x808D7580));
        Assert.Same(us, RetroRewindRegionPatches.CorrectUnportedUsHooks(us, pal, "RMCE01", SlotImage()));
    }

    [Fact]
    public void RejectsAnUnknownUnportedRelAddress()
    {
        var chunk = Chunk(Command(KamekCommandId.Addr32, 0x808B4D60));
        Assert.Throws<InvalidDataException>(() =>
            RetroRewindRegionPatches.CorrectUnportedUsHooks(chunk, chunk, "RMCE01", SlotImage()));
    }

    [Fact]
    public void IgnoresSharedAddressesOutsideStaticR()
    {
        var chunk = Chunk(Command(KamekCommandId.Write32, 0x80001A80));
        Assert.Same(chunk, RetroRewindRegionPatches.CorrectUnportedUsHooks(chunk, chunk, "RMCE01", SlotImage()));
    }

    [Fact]
    public void RejectsAMismatchedSlotOrCommand()
    {
        var hook = Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8));
        var image = SlotImage();
        image.Memory[image.GetOffset(0x808D7580)] ^= 1;
        Assert.Throws<InvalidDataException>(() =>
            RetroRewindRegionPatches.CorrectUnportedUsHooks(hook, hook, "RMCE01", image));
        var write = Chunk(Command(KamekCommandId.Write32, 0x808DBCD8));
        Assert.Throws<InvalidDataException>(() =>
            RetroRewindRegionPatches.CorrectUnportedUsHooks(write, write, "RMCE01", SlotImage()));
        Assert.Throws<InvalidDataException>(() => RetroRewindRegionPatches.CorrectUnportedUsHooks(
            hook, Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8), Command(KamekCommandId.Branch, 0x80000100)),
            "RMCE01", SlotImage()));
    }

    private static readonly uint[] EsrbHook =
        [0x3C608000, 0x88030003, 0x2C000045, 0x4C820020, 0x3C804800, 0x3C608060, 0x3804001C, 0x90034094, 0x4E800020];

    [Fact]
    public void AppliesTheUsEsrbSkipStaticallyAndDropsTheStore()
    {
        var chunk = EsrbChunk(0x40);
        var patched = RetroRewindRegionPatches.ApplyUsEsrbSkip(chunk, "RMCE01", EsrbImage(0x3C608038));

        var write = Assert.Single(patched.Commands);
        Assert.Equal(KamekCommandId.Write32, write.Id);
        Assert.True(write.AddressIsAbsolute);
        Assert.Equal(0x80604094u, write.Address);
        Assert.Equal([0x4800001Cu], write.Arguments);
        Assert.Equal(0x60000000u, BinaryPrimitives.ReadUInt32BigEndian(patched.CodeBlob.AsSpan(0x40 + 7 * 4, 4)));
        Assert.Equal(0x90034094u, BinaryPrimitives.ReadUInt32BigEndian(chunk.CodeBlob.AsSpan(0x40 + 7 * 4, 4)));
    }

    [Theory]
    [InlineData("RMCP01")]
    [InlineData("RMCJ01")]
    public void LeavesTheEsrbHookAloneOutsideNtscU(string gameId)
    {
        var chunk = EsrbChunk(0x40);
        Assert.Same(chunk, RetroRewindRegionPatches.ApplyUsEsrbSkip(chunk, gameId, EsrbImage(0x3C608038)));
    }

    [Fact]
    public void LeavesAPackWithoutTheEsrbHookAlone()
    {
        var chunk = Chunk();
        Assert.Same(chunk, RetroRewindRegionPatches.ApplyUsEsrbSkip(chunk, "RMCE01", EsrbImage(0x3C608038)));
    }

    [Fact]
    public void RejectsAnUnexpectedEsrbTargetOrDuplicateHook()
    {
        Assert.Throws<InvalidDataException>(() =>
            RetroRewindRegionPatches.ApplyUsEsrbSkip(EsrbChunk(0x40), "RMCE01", EsrbImage(0x4800001C)));
        var twice = EsrbChunk(0x40);
        for (var i = 0; i < EsrbHook.Length; i++)
            BinaryPrimitives.WriteUInt32BigEndian(twice.CodeBlob.AsSpan(0x100 + i * 4, 4), EsrbHook[i]);
        Assert.Throws<InvalidDataException>(() =>
            RetroRewindRegionPatches.ApplyUsEsrbSkip(twice, "RMCE01", EsrbImage(0x3C608038)));
    }

    private static KamekChunk EsrbChunk(int offset)
    {
        var chunk = Chunk();
        for (var i = 0; i < EsrbHook.Length; i++)
            BinaryPrimitives.WriteUInt32BigEndian(chunk.CodeBlob.AsSpan(offset + i * 4, 4), EsrbHook[i]);
        return chunk;
    }

    private static ProgramImage EsrbImage(uint instruction)
    {
        var memory = new byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(memory, instruction);
        var range = AddressRange.FromStartAndSize(0x80604094, 4);
        return new ProgramImage(memory, range, default, range, "fixture", range.Start);
    }

    private static KamekCommand Command(KamekCommandId id, uint address) =>
        new(0, 0, id, true, address, [0x100u]);

    private static KamekChunk Chunk(params KamekCommand[] commands) =>
        new(1, 0, 0x20, 0x200, 0, 0, 0x220, new byte[0x200], commands);

    private static ProgramImage SlotImage()
    {
        const uint start = 0x808B0000;
        var memory = new byte[0x28000];
        foreach (var (_, us, target) in UnportedHooks)
            BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan((int)(us - start), 4), target);
        var range = AddressRange.FromStartAndSize(start, (uint)memory.Length);
        return new ProgramImage(memory, range, default, range, "fixture", range.Start);
    }
}
