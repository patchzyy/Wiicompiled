using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Coalesces consecutive statically-known gather-pipe stores into one <c>GX_HLE_FIFO_WriteBurst</c>.
/// Output must stay byte-identical to individual GX_HLE_FIFO_Write* calls, and a run must never span
/// a guest load, since a recording display list can observe the deferred write in guest memory.
/// </summary>
public class GpuFifoBurstCodeGenTests
{
    // lis rX, 0xCC01 / stX rY, -0x8000(rX) - how every nw4r GD* helper spells a
    // gather-pipe write.
    private const int GatherPipeBase = unchecked((int)0xCC010000);
    private const int GatherPipeDisplacement = -32768;

    private static string Emit(IrFunction function) =>
        new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(Enumerable.Range(0, 12)
                .ToDictionary(index => $"r{index}", _ => ValueRepresentation.UInt32)));

    private static IrStore FifoStore(string source, int sizeBytes) =>
        new(new IrAddress("r10", GatherPipeDisplacement), IrValue.Register(source), sizeBytes);

    private static IrFunction Function(string name, params IrInstruction[] body) =>
        new(name, "entry",
        [
            new IrBasicBlock("entry",
                new IrInstruction[] { new IrAssign("r10", IrValue.Imm(GatherPipeBase)) }
                    .Concat(body).ToArray())
        ]);

    [Fact]
    public void ThirteenMixedWidthStoresBecomeOneBurstWithBigEndianLayout()
    {
        // Alternating byte/word fields, the shape a GD command header plus its
        // payload registers has.
        var stores = new List<IrInstruction>();
        for (var index = 0; index < 13; index++)
            stores.Add(FifoStore("r3", index % 2 == 0 ? 1 : 4));
        stores.Add(new IrReturn(null));

        var code = Emit(Function("burst_mixed", stores.ToArray()));

        // 7 single bytes + 6 words = 31 bytes, one buffer, one call.
        Assert.Contains("uint8_t mkw_fifo_burst_0[31];", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_WriteBurst(mkw_fifo_burst_0, 31u);", code, StringComparison.Ordinal);
        Assert.Equal(1, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
        Assert.DoesNotContain("GX_HLE_FIFO_Write8(", code, StringComparison.Ordinal);
        Assert.DoesNotContain("GX_HLE_FIFO_Write32(", code, StringComparison.Ordinal);

        // Byte 0 is the first 8-bit field; bytes 1..4 are the first word,
        // most-significant byte first.
        Assert.Contains("mkw_fifo_burst_0[0] = static_cast<uint8_t>(r3);", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[1] = static_cast<uint8_t>((mkw_fifo_word >> 24));", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[2] = static_cast<uint8_t>((mkw_fifo_word >> 16));", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[3] = static_cast<uint8_t>((mkw_fifo_word >> 8));", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[4] = static_cast<uint8_t>(mkw_fifo_word);", code, StringComparison.Ordinal);
        // ... and the run ends on the 13th store, at byte 30.
        Assert.Contains("mkw_fifo_burst_0[30] = static_cast<uint8_t>(r3);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void SixteenBitStoresSerializeTwoBigEndianBytes()
    {
        var stores = Enumerable.Range(0, 4)
            .Select(_ => (IrInstruction)FifoStore("r3", 2))
            .Append(new IrReturn(null))
            .ToArray();

        var code = Emit(Function("burst_halfwords", stores));

        Assert.Contains("uint8_t mkw_fifo_burst_0[8];", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[0] = static_cast<uint8_t>((mkw_fifo_word >> 8));", code, StringComparison.Ordinal);
        Assert.Contains("mkw_fifo_burst_0[1] = static_cast<uint8_t>(mkw_fifo_word);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatStoresSerializeTheirIeeeBitsBigEndian()
    {
        var stores = Enumerable.Range(0, 4)
            .Select(_ => (IrInstruction)new IrStore(
                new IrAddress("r10", GatherPipeDisplacement), IrValue.Register("f1"), 4))
            .Append(new IrReturn(null))
            .ToArray();

        var code = Emit(Function("burst_floats", stores));

        Assert.Contains("uint8_t mkw_fifo_burst_0[16];", code, StringComparison.Ordinal);
        Assert.Contains("PpcBitCastToU32Inline(static_cast<float>(", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_WriteBurst(mkw_fifo_burst_0, 16u);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("GX_HLE_FIFO_WriteFloat(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void RegisterArithmeticBetweenStoresStaysInsideTheRun()
    {
        var stores = new List<IrInstruction>();
        for (var index = 0; index < 4; index++)
        {
            stores.Add(new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"));
            stores.Add(FifoStore("r3", 4));
        }
        stores.Add(new IrReturn(null));

        var code = Emit(Function("burst_with_arithmetic", stores.ToArray()));

        Assert.Contains("GX_HLE_FIFO_WriteBurst(mkw_fifo_burst_0, 16u);", code, StringComparison.Ordinal);
        Assert.Equal(1, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
    }

    [Fact]
    public void RunOfThreeIsNotCoalesced()
    {
        var stores = Enumerable.Range(0, 3)
            .Select(_ => (IrInstruction)FifoStore("r3", 4))
            .Append(new IrReturn(null))
            .ToArray();

        var code = Emit(Function("short_run", stores));

        Assert.DoesNotContain("GX_HLE_FIFO_WriteBurst", code, StringComparison.Ordinal);
        Assert.DoesNotContain("mkw_fifo_burst_0", code, StringComparison.Ordinal);
        Assert.Equal(3, CountOccurrences(code, "GX_HLE_FIFO_Write32("));
    }

    [Fact]
    public void OrdinaryGuestStoreSplitsTheRun()
    {
        // A store to guest RAM cannot be reordered past a FIFO write: while a
        // display list is recording, the FIFO writer itself writes guest memory.
        var instructions = new List<IrInstruction>();
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrStore(new IrAddress("r4", 0), IrValue.Register("r5"), 4));
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrReturn(null));

        var code = Emit(Function("split_by_store", instructions.ToArray()));

        Assert.Equal(2, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
        Assert.Contains("GX_HLE_FIFO_WriteBurst(mkw_fifo_burst_0, 16u);", code, StringComparison.Ordinal);
        Assert.Contains("GX_HLE_FIFO_WriteBurst(mkw_fifo_burst_1, 16u);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void GuestLoadSplitsTheRun()
    {
        var instructions = new List<IrInstruction>();
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrLoad("r5", new IrAddress("r4", 0), 4));
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrReturn(null));

        var code = Emit(Function("split_by_load", instructions.ToArray()));

        Assert.Equal(2, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
    }

    [Fact]
    public void CallSplitsTheRun()
    {
        var instructions = new List<IrInstruction>();
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrCall(string.Empty, "func_80002000", []));
        for (var index = 0; index < 4; index++) instructions.Add(FifoStore("r3", 4));
        instructions.Add(new IrReturn(null));

        var code = Emit(Function("split_by_call", instructions.ToArray()));

        Assert.Equal(2, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
    }

    [Fact]
    public void BranchSplitsTheRunAndEachSideBurstsIndependently()
    {
        IrInstruction[] Stores(int count, IrInstruction terminator) =>
            Enumerable.Range(0, count)
                .Select(_ => (IrInstruction)FifoStore("r3", 4))
                .Append(terminator)
                .ToArray();

        var function = new IrFunction("split_by_branch", "entry",
        [
            new IrBasicBlock("entry",
                new IrInstruction[] { new IrAssign("r10", IrValue.Imm(GatherPipeBase)) }
                    .Concat(Stores(4, new IrBranch("ne", "left", "right"))).ToArray()),
            new IrBasicBlock("left", Stores(4, new IrReturn(null))),
            // Three stores on this side stay individual: below the run minimum.
            new IrBasicBlock("right", Stores(3, new IrReturn(null)))
        ]);

        var code = Emit(function);

        Assert.Equal(2, CountOccurrences(code, "GX_HLE_FIFO_WriteBurst("));
        Assert.Equal(3, CountOccurrences(code, "GX_HLE_FIFO_Write32("));
    }

    [Fact]
    public void StoresOutsideTheGatherPipeAreNeverCoalesced()
    {
        var instructions = new List<IrInstruction>
        {
            new IrAssign("r10", IrValue.Imm(unchecked((int)0x80300000)))
        };
        for (var index = 0; index < 8; index++)
            instructions.Add(new IrStore(new IrAddress("r10", index * 4), IrValue.Register("r3"), 4));
        instructions.Add(new IrReturn(null));

        var code = Emit(new IrFunction("not_gather_pipe", "entry",
            [new IrBasicBlock("entry", instructions)]));

        Assert.DoesNotContain("GX_HLE_FIFO_WriteBurst", code, StringComparison.Ordinal);
    }

    [Fact]
    public void UnsupportedIntegerWidthFallsBackToGuestMemory()
    {
        var code = Emit(Function(
            "fifo_integer_width8",
            FifoStore("r3", 8),
            new IrReturn(null)));

        Assert.DoesNotContain("GX_HLE_FIFO_Write", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWrite64", code, StringComparison.Ordinal);
    }

    [Fact]
    public void UnsupportedFloatWidthFallsBackToGuestMemory()
    {
        var code = Emit(Function(
            "fifo_float_width8",
            new IrStore(
                new IrAddress("r10", GatherPipeDisplacement),
                IrValue.Register("f1"),
                8),
            new IrReturn(null)));

        Assert.DoesNotContain("GX_HLE_FIFO_Write", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64", code, StringComparison.Ordinal);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void ResumableBodiesKeepIndividualFifoStores(bool resumable)
    {
        // A hook resuming at a later store would otherwise flush burst slots it never wrote.
        var body = new List<IrInstruction>
        {
            new IrAssign("lr", IrValue.Imm(0x80001008)),
            new IrCall(string.Empty, "0x8179AC3C", Array.Empty<IrValue>()),
            new IrTracePpc(0x80001004u, "bl", "0x48000001")
        };
        for (var index = 0; index < 4; index++)
        {
            body.Add(FifoStore("r3", 4));
            body.Add(new IrTracePpc(0x80001008u + (uint)index * 4, "stw", "0x90000000"));
        }
        body.Add(new IrReturn(null));
        var function = Function("burst_resumable", body.ToArray());
        var registers = Enumerable.Range(0, 12)
            .ToDictionary(index => $"r{index}", _ => ValueRepresentation.UInt32);
        registers["lr"] = ValueRepresentation.UInt32;

        var code = new CxxLinearCodeGenerator().Emit(
            0x80001000u,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(registers),
            lrContinuationCallTargets: resumable ? new HashSet<uint> { 0x8179AC3Cu } : new HashSet<uint>());

        if (!resumable)
        {
            Assert.Contains("GX_HLE_FIFO_WriteBurst(", code, StringComparison.Ordinal);
            return;
        }

        Assert.DoesNotContain("GX_HLE_FIFO_WriteBurst(", code, StringComparison.Ordinal);
        Assert.Equal(4, CountOccurrences(code, "GX_HLE_FIFO_Write32("));
    }

    private static int CountOccurrences(string text, string value)
    {
        var count = 0;
        var index = text.IndexOf(value, StringComparison.Ordinal);
        while (index >= 0)
        {
            ++count;
            index = text.IndexOf(value, index + value.Length, StringComparison.Ordinal);
        }

        return count;
    }
}
