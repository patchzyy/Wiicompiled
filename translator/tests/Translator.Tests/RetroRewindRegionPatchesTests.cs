using System.Buffers.Binary;
using Translator.Core.Loading;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;

namespace Translator.Tests;

public sealed class RetroRewindRegionPatchesTests
{
    [Fact]
    public void CorrectsOnlyTheStaleUsPointerAndPreservesItsModuleTarget()
    {
        var hook = Command(KamekCommandId.Addr32, 0x808DBCD8);
        var other = Command(KamekCommandId.Branch, 0x807B9F34);
        var chunk = Chunk(hook, other);
        var corrected = RetroRewindRegionPatches.CorrectUsVoiceHook(chunk, "RMCE01", VoiceImage());

        Assert.Equal(0x808D7580u, corrected.Commands[0].Address);
        Assert.Equal(hook.Arguments, corrected.Commands[0].Arguments);
        Assert.Same(other, corrected.Commands[1]);
        Assert.Equal(0x808DBCD8u, chunk.Commands[0].Address);
        Assert.Same(chunk.CodeBlob, corrected.CodeBlob);
    }

    [Theory]
    [InlineData("RMCP01")]
    [InlineData("RMCJ01")]
    public void LeavesOtherRegionsUntouched(string gameId)
    {
        var chunk = Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8));
        Assert.Same(chunk, RetroRewindRegionPatches.CorrectUsVoiceHook(chunk, gameId, VoiceImage()));
    }

    [Fact]
    public void AcceptsAnAlreadyCorrectedPackWithoutRewritingIt()
    {
        var chunk = Chunk(Command(KamekCommandId.Addr32, 0x808D7580));
        Assert.Same(chunk, RetroRewindRegionPatches.CorrectUsVoiceHook(chunk, "RMCE01", VoiceImage()));
    }

    [Theory]
    [InlineData(0x808D7540u)]
    [InlineData(0x808D7580u)]
    [InlineData(0x808D75C0u)]
    public void RejectsTheCorrectionWhenTheVtableDoesNotMatch(uint address)
    {
        var image = VoiceImage();
        image.Memory[image.GetOffset(address)] ^= 1;
        Assert.Throws<InvalidDataException>(() => RetroRewindRegionPatches.CorrectUsVoiceHook(
            Chunk(Command(KamekCommandId.Addr32, 0x808DBCD8)), "RMCE01", image));
    }

    [Theory]
    [InlineData(KamekCommandId.Write32)]
    [InlineData(KamekCommandId.Branch)]
    public void RejectsAnUnexpectedCommandAtThePalAddress(KamekCommandId id)
    {
        Assert.Throws<InvalidDataException>(() => RetroRewindRegionPatches.CorrectUsVoiceHook(
            Chunk(Command(id, 0x808DBCD8)), "RMCE01", VoiceImage()));
    }

    [Fact]
    public void RejectsDuplicateOrConflictingHooks()
    {
        var hook = Command(KamekCommandId.Addr32, 0x808DBCD8);
        foreach (var other in new[] { hook, Command(KamekCommandId.Addr32, 0x808D7580) })
            Assert.Throws<InvalidDataException>(() => RetroRewindRegionPatches.CorrectUsVoiceHook(
                Chunk(hook, other), "RMCE01", VoiceImage()));
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

    private static ProgramImage VoiceImage()
    {
        var memory = new byte[0x84];
        uint[] entries =
        [
            0x806FC03C, 0x806FBC94, 0x806FBC3C, 0x806FBC34, 0x806FA3A8, 0x806FAA04,
            0x806FAB44, 0x806FAD58, 0x806FA7B0, 0x806FA83C, 0x806FA8D0, 0x808611E8, 0x806FA824
        ];
        int[] offsets = [0, 4, 0x30, 0x34, 0x38, 0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x7C, 0x80];
        for (var i = 0; i < entries.Length; i++)
            BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(offsets[i], 4), entries[i]);
        var range = AddressRange.FromStartAndSize(0x808D7540, (uint)memory.Length);
        return new ProgramImage(memory, range, default, range, "fixture", range.Start);
    }
}
