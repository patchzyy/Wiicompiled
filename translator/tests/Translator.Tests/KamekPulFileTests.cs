using System.Buffers.Binary;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;
using Xunit;

namespace Translator.Tests;

public class KamekPulFileTests
{
    [Fact]
    public void ParsesRawSingleChunk()
    {
        var chunk = BuildChunk(
            bssSize: 0x20,
            code: [0x60, 0x00, 0x00, 0x00],
            commands:
            [
                BuildCommand(KamekCommandId.Write32, absolute: true, address: 0x805850BC, args: [0x60000000])
            ]);

        var pul = KamekPulFile.Parse(chunk);
        var selected = pul.SelectRegion("P");

        Assert.False(pul.IsCombined);
        Assert.Single(pul.Chunks);
        Assert.Equal(0x20u, selected.BssSize);
        Assert.Equal(4u, selected.CodeSize);
        Assert.Single(selected.Commands);
        Assert.Equal(KamekCommandId.Write32, selected.Commands[0].Id);
        Assert.True(selected.Commands[0].AddressIsAbsolute);
        Assert.Equal(0x805850BCu, selected.Commands[0].Address);
    }

    [Fact]
    public void ParsesCombinedChunksAndSelectsRegion()
    {
        var p = BuildChunk(0x10, [0x4E, 0x80, 0x00, 0x20], [BuildCommand(KamekCommandId.Branch, true, 0x8053369C, [0x100])]);
        var e = BuildChunk(0x20, [0x60, 0x00, 0x00, 0x00], [BuildCommand(KamekCommandId.Write8, false, 0x10, [0x7F])]);
        var combined = new byte[0x10 + p.Length + e.Length];
        WriteU32(combined, 0, (uint)p.Length);
        WriteU32(combined, 4, (uint)e.Length);
        p.CopyTo(combined, 0x10);
        e.CopyTo(combined, 0x10 + p.Length);

        var pul = KamekPulFile.Parse(combined);

        Assert.True(pul.IsCombined);
        Assert.Equal(2, pul.Chunks.Count);
        Assert.Equal(0x10u, pul.SelectRegion("P").BssSize);
        Assert.Equal(0x20u, pul.SelectRegion("E").BssSize);
    }

    [Fact]
    public void ParsesKamekV2CombinedChunks()
    {
        var p = BuildChunk(0x10, [0x4E, 0x80, 0x00, 0x20],
            [BuildCommand(KamekCommandId.Branch, true, 0x8053369C, [0x100])], KamekChunk.Magic1V2);
        var combined = new byte[0x10 + p.Length];
        WriteU32(combined, 0, (uint)p.Length);
        p.CopyTo(combined, 0x10);

        var pul = KamekPulFile.Parse(combined);

        Assert.True(pul.IsCombined);
        Assert.Equal(0x10u, pul.SelectRegion("P").BssSize);
        Assert.Equal(KamekCommandId.Branch, pul.SelectRegion("P").Commands.Single().Id);

        var raw = KamekPulFile.Parse(p);

        Assert.False(raw.IsCombined);
        Assert.Equal((uint)p.Length, raw.Chunks.Single().ChunkSize);
    }

    [Fact]
    public void EncodesPpcBranchLikeKamek()
    {
        Assert.Equal(0x48000144u, KamekPpcEncoding.EncodeBranch(0x8053369C, 0x805337E0, link: false));
        Assert.Equal(0x4BFFFFFDu, KamekPpcEncoding.EncodeBranch(0x8053369C, 0x80533698, link: true));
        Assert.Throws<ArgumentOutOfRangeException>(() => KamekPpcEncoding.EncodeBranch(0x8053369C, 0x8053369D, link: false));
    }

    [Theory]
    [InlineData("P", 0x800ED6E8u)]
    [InlineData("E", 0x800ED648u)]
    [InlineData("J", 0x800ED608u)]
    public void ParsesBundledRetroRewindCodePulWhenPresent(string region, uint bootstrapHook)
    {
        var repoRoot = ProjectPathsForTests.FindRepositoryRoot();
        var codePul = Path.Combine(repoRoot, "PulsarPacks", "completed", "RetroRewind", "RetroRewind6", "Binaries", "Code.pul");
        if (!File.Exists(codePul))
        {
            return;
        }

        var pul = KamekPulFile.Load(codePul);
        var chunk = pul.SelectRegion(region);

        Assert.True(pul.IsCombined);
        Assert.Equal(3, pul.Chunks.Count);
        Assert.All(pul.CombinedChunkSizes.Take(3), size => Assert.True(size >= KamekChunk.HeaderSize));
        Assert.Equal(0u, pul.CombinedChunkSizes[3]);
        Assert.NotEmpty(chunk.CodeBlob);
        Assert.True(chunk.BssSize > 0);
        Assert.True(chunk.CtorStart <= chunk.CtorEnd);
        Assert.True(chunk.CtorEnd <= chunk.CodeSize);
        Assert.NotEmpty(chunk.Commands);
        Assert.Equal(chunk.Commands.Count, chunk.RelativeCommandCount + chunk.AbsoluteCommandCount);
        Assert.Equal(chunk.Commands.Count, chunk.CommandCounts.Values.Sum());
        var lowered = RetroWfcBootstrapSuppressor.Suppress(chunk, bootstrapHook);
        Assert.Equal(chunk.Commands.Count - 1, lowered.Commands.Count);
        Assert.DoesNotContain(lowered.Commands, command => command.AddressIsAbsolute && command.Address == bootstrapHook);
    }

    private static byte[] BuildChunk(uint bssSize, byte[] code, byte[][] commands,
        uint magic1 = KamekChunk.Magic1)
    {
        var commandSize = commands.Sum(c => c.Length);
        var chunkSize = KamekChunk.HeaderSize + code.Length + commandSize;
        var data = new byte[chunkSize];
        WriteU32(data, 0x00, KamekChunk.Magic0);
        WriteU32(data, 0x04, magic1);
        WriteU32(data, 0x08, bssSize);
        WriteU32(data, 0x0C, (uint)code.Length);
        WriteU32(data, 0x10, 0);
        WriteU32(data, 0x14, 0);
        WriteU32(data, 0x18, magic1 == KamekChunk.Magic1V2 ? 0u : (uint)chunkSize);
        code.CopyTo(data, KamekChunk.HeaderSize);

        var offset = KamekChunk.HeaderSize + code.Length;
        foreach (var command in commands)
        {
            command.CopyTo(data, offset);
            offset += command.Length;
        }

        return data;
    }

    private static byte[] BuildCommand(KamekCommandId id, bool absolute, uint address, uint[] args)
    {
        var size = 4 + (absolute ? 4 : 0) + args.Length * 4;
        var data = new byte[size];
        var commandWord = ((uint)(byte)id << 24) | (absolute ? 0x00FFFFFEu : address);
        WriteU32(data, 0, commandWord);
        var offset = 4;
        if (absolute)
        {
            WriteU32(data, offset, address);
            offset += 4;
        }
        foreach (var arg in args)
        {
            WriteU32(data, offset, arg);
            offset += 4;
        }
        return data;
    }

    private static void WriteU32(byte[] data, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(data.AsSpan(offset, 4), value);
}

internal static class ProjectPathsForTests
{
    public static string FindRepositoryRoot()
    {
        var dir = new DirectoryInfo(AppContext.BaseDirectory);
        while (dir is not null)
        {
            if (File.Exists(Path.Combine(dir.FullName, "translator", "Translator.sln")) &&
                File.Exists(Path.Combine(dir.FullName, "projects", "mkwii", "recomp.yml")))
            {
                return dir.FullName;
            }
            dir = dir.Parent;
        }

        throw new DirectoryNotFoundException("Could not find repository root.");
    }
}
