using Translator.Core.Disassembly;
using Translator.Core.Mods;

namespace Translator.Tests;

public sealed class LrRelativeAnalysisCompletenessTests
{
    [Fact]
    public void EmptyResultReportsWhenPathStatesWereDropped()
    {
        var capped = false;
        var offsets = Analyze(SyntheticLrHookFactory.ManyOrdinaryReturnPaths(), () => capped = true);

        Assert.Empty(offsets);
        Assert.True(capped, "An empty result from capped exploration must not prove an ordinary return.");
    }

    [Theory]
    [InlineData(0)]
    [InlineData(20)]
    public void ExhaustiveAnalysisDoesNotReportAStateCap(int offset)
    {
        var capped = false;
        var offsets = Analyze(
        [
            0x7D8802A6u, // mflr r12
            0x398C0000u | (ushort)offset, // addi r12,r12,offset
            0x7D8803A6u, // mtlr r12
            0x4E800020u // blr
        ], () => capped = true);

        Assert.False(capped);
        Assert.Equal(offset == 0 ? Array.Empty<int>() : [offset], offsets);
    }

    [Fact]
    public void CappedArmDoesNotDiscardAnOffsetFoundOnAnotherArm()
    {
        var capped = false;
        var offsets = Analyze(
        [
            0x7D8802A6u, // +00: mflr r12
            0x2C030000u, // +04: cmpwi r3,0
            0x41820010u, // +08: beq +0x18
            0x398C0014u, // +0C: addi r12,r12,20
            0x7D8803A6u, // +10: mtlr r12
            0x4E800020u, // +14: blr
            .. SyntheticLrHookFactory.ManyOrdinaryReturnPaths() // +18
        ], () => capped = true);

        Assert.True(capped);
        Assert.Equal(new[] { 20 }, offsets);
    }

    private static int[] Analyze(uint[] words, Action onStateCapExceeded)
    {
        var instructions = words.Select((word, index) =>
            PpcDecoder.Decode(0x80010000u + (uint)index * 4, word)).ToArray();
        return ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(instructions, onStateCapExceeded).ToArray();
    }
}

internal static class SyntheticLrHookFactory
{
    public static uint[] ManyOrdinaryReturnPaths()
    {
        // 1,024 possible states exceed the 512-state cap without an unbounded loop.
        var words = new List<uint> { 0x7FE802A6u }; // mflr r31
        for (uint register = 3; register <= 12; register++)
        {
            words.Add(0x2C000000u | (register << 16)); // cmpwi rN,0
            words.Add(0x41820008u); // beq +8
            words.Add(0x38000004u | (register << 21) | (31u << 16)); // addi rN,r31,4
        }
        words.Add(0x4E800020u); // blr with unchanged LR
        return words.ToArray();
    }
}
