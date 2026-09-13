using Translator.Core.Disassembly;
using Translator.Core.Mods;

namespace Translator.Tests;

public sealed class LrContinuationAnalysisTests
{
    private const uint Entry = 0x80010000;
    private const uint Tail = Entry - 0x40;

    [Fact]
    public void TailHelperUsesTheWrappersLrState()
    {
        var result = Analyze(new Dictionary<uint, uint[]>
        {
            [Entry] = [0x7D8802A6, 0x398C0014, 0x4BFFFFB8], // mflr; addi +20; b Tail
            [Tail] = [0x7D8803A6, 0x4E800020] // mtlr r12; blr
        });
        Assert.True(result.IsComplete);
        Assert.Equal(new[] { 20 }, result.Offsets);
    }

    [Fact]
    public void TailHelperUsesTheWrappersStackFrame()
    {
        var result = Analyze(new Dictionary<uint, uint[]>
        {
            [Entry] = [0x7D8802A6, 0x9421FFF0, 0x91810014, 0x4BFFFFB4],
            [Tail] = [0x38210010, 0x81810004, 0x398C0014, 0x7D8803A6, 0x4E800020]
        });
        Assert.True(result.IsComplete);
        Assert.Equal(new[] { 20 }, result.Offsets);
    }

    [Fact]
    public void OrdinaryTailCycleTerminatesWithoutInventingOffsets()
    {
        var result = Analyze(new Dictionary<uint, uint[]>
        {
            [Entry] = [0x4BFFFFC0],
            [Tail] = [0x48000040]
        });
        Assert.True(result.IsComplete);
        Assert.False(result.MaySkipReturn);
    }

    [Fact]
    public void AdjustingTailCycleReportsIncompleteAnalysis()
    {
        var result = Analyze(new Dictionary<uint, uint[]>
        {
            [Entry] = [0x7D8802A6, 0x4800003C],
            [Entry + 0x40] = [0x398C0004, 0x4BFFFFFC]
        });
        Assert.False(result.IsComplete);
        Assert.True(result.WasTruncated);
        Assert.True(result.MaySkipReturn);
    }

    [Fact]
    public void UndecodableTailDoesNotProveAnOrdinaryReturn()
    {
        var result = Analyze(new Dictionary<uint, uint[]> { [Entry] = [0x4BFFFFC0] });
        Assert.False(result.IsComplete);
        Assert.True(result.WasTruncated);
        Assert.True(result.MaySkipReturn);
    }

    [Fact]
    public void NormalCallDoesNotInheritItsCalleesSkipOffset()
    {
        var visited = new List<uint>();
        var result = LrContinuationAnalysis.Analyze(Entry, address =>
        {
            visited.Add(address);
            return Decode(address, [0x7FE802A6, 0x4BFFFFBD, 0x7FE803A6, 0x4E800020]);
        });
        Assert.Equal(new[] { Entry }, visited);
        Assert.True(result.IsComplete);
        Assert.False(result.MaySkipReturn);
    }

    [Fact]
    public void UnknownIndirectTailIsIncomplete()
    {
        var result = Analyze(new Dictionary<uint, uint[]> { [Entry] = [0x7D8903A6, 0x4E800420] });
        Assert.False(result.IsComplete);
        Assert.False(result.WasTruncated);
        Assert.True(result.MaySkipReturn);
    }

    [Fact]
    public void ConditionalCallDoesNotTraverseATargetAlsoUsedByATailBranch()
    {
        var result = Analyze(new Dictionary<uint, uint[]>
        {
            [Entry] = [0x7FE802A6, 0x4182FFBD, 0x3BE00000, 0x4BFFFFB4],
            [Tail] = [0x3BFF0014, 0x7FE803A6, 0x4E800020]
        });
        Assert.Empty(result.Offsets);
    }

    [Fact]
    public void TailDiscoveryBudgetDoesNotProveAnOrdinaryReturn()
    {
        var calls = 0;
        var result = LrContinuationAnalysis.Analyze(Entry, address =>
        {
            calls++;
            return Decode(address, [0x48000040]);
        });
        Assert.InRange(calls, 1, 256);
        Assert.False(result.IsComplete);
        Assert.True(result.WasTruncated);
        Assert.True(result.MaySkipReturn);
    }

    private static LrContinuationAnalysis Analyze(Dictionary<uint, uint[]> functions) =>
        LrContinuationAnalysis.Analyze(Entry, address => functions.TryGetValue(address, out var words)
            ? Decode(address, words)
            : throw new InvalidOperationException("No synthetic function at this address."));

    private static PpcInstruction[] Decode(uint address, uint[] words) =>
        words.Select((word, index) => PpcDecoder.Decode(address + (uint)index * 4, word)).ToArray();
}
