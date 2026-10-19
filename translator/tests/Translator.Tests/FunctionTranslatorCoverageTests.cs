using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Reflection;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class FunctionTranslatorCoverageTests
{
    private static readonly Type TranslatorType = typeof(FunctionTranslator);

    private static T InvokePrivate<T>(string name, params object?[] args)
    {
        var method = TranslatorType.GetMethod(name, BindingFlags.NonPublic | BindingFlags.Static);
        Assert.NotNull(method);
        return (T)method!.Invoke(null, args)!;
    }

    [Fact]
    public void DiscoverStopsAfterSsaWithoutTypingOrCodeGeneration()
    {
        var memory = new byte[4];
        WriteWord(memory, MemoryLayout.RamBase, 0x4E800020);
        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, 4),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, 4),
            default,
            "discovery-only");

        var discovery = new FunctionTranslator(image).Discover(
            MemoryLayout.RamBase,
            TranslationOptions.Default with { AllowUnsupportedInstructions = true });

        Assert.Single(discovery.Instructions);
        Assert.NotEmpty(discovery.Ssa.Function.Blocks);
        Assert.Equal(TimeSpan.Zero, discovery.Metrics.RepresentationClassification);
        Assert.Equal(TimeSpan.Zero, discovery.Metrics.CodeGen);
    }

    [Theory]
    [InlineData(0x4D820021u)] // beqlrl
    [InlineData(0x4D820421u)] // bcctrl, BO=12 BI=2
    public void Discover_LinkedConditionalRegisterBranchUpdatesLrWhenNotTaken(uint branchWord)
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, branchWord);
        WriteWord(memory, 0x80001004, 0x7C6802A6); // mflr r3
        WriteWord(memory, 0x80001008, 0x4E800020); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0x1004),
            AddressRange.FromStartAndSize(0x80001000, 0x1004),
            default,
            "test");
        var discovery = new FunctionTranslator(image).Discover(
            0x80001000,
            TranslationOptions.Default with { MaxBytes = 0x20, AllowUnsupportedInstructions = true });

        var blocks = discovery.Ssa.Function.Blocks;
        var branch = Assert.Single(blocks.SelectMany(b => b.Instructions).OfType<IrBranch>());
        Assert.Equal("link_branch_80001004_80001004", branch.FalseLabel);

        var notTaken = Assert.Single(blocks, b => b.Label == branch.FalseLabel);
        var lrAssign = Assert.Single(notTaken.Instructions.OfType<IrAssign>());
        Assert.StartsWith("lr", lrAssign.Destination, StringComparison.Ordinal);
        Assert.Equal(unchecked((int)0x80001004u), lrAssign.Value.Constant);
        Assert.Equal("0x80001004", Assert.IsType<IrJump>(notTaken.Instructions[^1]).TargetLabel);
    }

    [Fact]
    public void Translate_PreservesFallthroughAfterDirectLinkBranch()
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, 0x48001001); // bl 0x80002000
        WriteWord(memory, 0x80001004, 0x80630048); // lwz r3, 0x48(r3)
        WriteWord(memory, 0x80001008, 0x38800000); // li r4, 0
        WriteWord(memory, 0x8000100C, 0x80630000); // lwz r3, 0(r3)
        WriteWord(memory, 0x80001010, 0x48000FF0); // b 0x80002000
        WriteWord(memory, 0x80002000, 0x4E800020); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0x1004),
            AddressRange.FromStartAndSize(0x80001000, 0x1004),
            default,
            "test");
        // Leaf inlining would otherwise splice the trivial (blr-only) 0x80002000
        // callee straight into the caller, eliding the direct-call boundary this
        // test exists to check the fallthrough ordering around.
        var result = new FunctionTranslator(image).Translate(
            0x80001000,
            TranslationOptions.Default with
            {
                MaxBytes = 0x20,
                AllowUnsupportedInstructions = true,
                EnableLeafInlining = false
            });

        var callIndex = result.CxxCode.IndexOf("InvokeDirectCpu<0x80002000u>(ctx);", StringComparison.Ordinal);
        var fallthroughIndex = result.CxxCode.IndexOf("MemoryInline::FlatRead32((r3 + 72))", StringComparison.Ordinal);

        Assert.True(callIndex >= 0, result.CxxCode);
        Assert.True(fallthroughIndex > callIndex);
        // 0x80001004 as an unsigned address rather than the signed decimal the
        // int-typed IR immediate used to print.
        Assert.Contains("ctx->lr = 0x80001004u;", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_KnownSiblingEntryBranchEmitsTailCallInsteadOfInliningBody()
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, 0x48000020); // b 0x80001020
        WriteWord(memory, 0x80001020, 0x38600007); // li r3, 7
        WriteWord(memory, 0x80001024, 0x4E800020); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0x28),
            AddressRange.FromStartAndSize(0x80001000, 0x28),
            default,
            "test");
        // Leaf inlining would otherwise treat this unconditional branch to a
        // known sibling entry as an inlinable leaf; isolate the tail-call
        // boundary this test exists to check.
        var result = new FunctionTranslator(image).Translate(
            0x80001000,
            TranslationOptions.Default with
            {
                MaxBytes = 0x40,
                AllowUnsupportedInstructions = true,
                KnownFunctionEntryPoints = new HashSet<uint> { 0x80001000u, 0x80001020u },
                EnableLeafInlining = false
            });

        Assert.Single(result.Instructions);
        Assert.Contains("InvokeDirectCpu<0x80001020u>(ctx);", result.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("loc_80001020", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_RecognizesFramePointerBasedMultipleRegisterSaveAreaAsStackMemory()
    {
        const uint entry = 0x80001000;
        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x9421FFC0), // stwu r1, -64(r1)
            (entry + 0x04, 0x7C0802A6), // mflr r0
            (entry + 0x08, 0x90010044), // stw r0, 68(r1)
            (entry + 0x0C, 0x39610040), // addi r11, r1, 64
            (entry + 0x10, 0xBF2BFFE4), // stmw r25, -28(r11)
            (entry + 0x14, 0xBB2BFFE4), // lmw r25, -28(r11)
            (entry + 0x18, 0x80010044), // lwz r0, 68(r1)
            (entry + 0x1C, 0x7C0803A6), // mtlr r0
            (entry + 0x20, 0x38210040), // addi r1, r1, 64
            (entry + 0x24, 0x4E800020)); // blr

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with { MaxBytes = 0x28 });

        // Stores still distinguish the stack fast path (FlatWriteRam skips the MMIO policy check)
        // from the general guarded write (FlatWrite); reads have no such distinction, both spell FlatRead.
        Assert.Contains("MemoryInline::FlatWriteRam32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatRead32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWrite32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_UsesStackFastPathForLocallyProvenEabiSaveRestoreThunks()
    {
        const uint entry = 0x80001000;
        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x9421FFC0), // stwu r1, -64(r1)
            (entry + 0x04, 0x39610040), // addi r11, r1, 64
            (entry + 0x08, EncodeB(entry + 0x08, 0x80021598, link: true)), // _savegpr_25
            (entry + 0x0C, 0x39610040), // addi r11, r1, 64
            (entry + 0x10, EncodeB(entry + 0x10, 0x800215E4, link: true)), // _restgpr_25
            (entry + 0x14, 0x38210040), // addi r1, r1, 64
            (entry + 0x18, 0x4E800020)); // blr

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with { MaxBytes = 0x1C });

        Assert.Contains("MemoryInline::ResolveRangeHost((r11 + -28), 0, 28u, false, true)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.Contains("MemoryInline::ResolveRangeHost((r11 + -28), 0, 28u, true, false)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWrite32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatRead32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_KeepsEabiThunkOnGenericMemoryWithoutLocalStackProof()
    {
        const uint entry = 0x80001000;
        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x39630000), // addi r11, r3, 0
            (entry + 0x04, EncodeB(entry + 0x04, 0x80021598, link: true)), // _savegpr_25
            (entry + 0x08, 0x4E800020)); // blr

        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with { MaxBytes = 0xC });

        Assert.Contains("MemoryInline::FlatWrite32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
        Assert.DoesNotContain("MemoryInline::FlatWriteRam32((r11 + -28)", result.CxxCode,
            StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_KnownSiblingEntryFallthroughEmitsTailCall()
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, 0x38600007); // li r3, 7
        WriteWord(memory, 0x80001004, 0x38800009); // li r4, 9
        WriteWord(memory, 0x80001008, 0x4E800020); // sibling continuation: blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0xC),
            AddressRange.FromStartAndSize(0x80001000, 0xC),
            default,
            "test");
        var result = new FunctionTranslator(image).Translate(
            0x80001000,
            TranslationOptions.Default with
            {
                MaxBytes = 0x8,
                AllowUnsupportedInstructions = true,
                KnownFunctionEntryPoints = new HashSet<uint> { 0x80001000u, 0x80001008u }
            });

        Assert.Contains("InvokeDirectCpu<0x80001008u>(ctx);", result.CxxCode, StringComparison.Ordinal);
        Assert.Contains("return;", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_KnownConditionalBranchTargetRemainsLocalControlFlow()
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, 0x2C030000); // cmpwi r3, 0
        WriteWord(memory, 0x80001004, EncodeBc(12, 2, 0x10)); // beq 0x80001014
        WriteWord(memory, 0x80001008, 0x38800001); // li r4, 1
        WriteWord(memory, 0x8000100C, 0x4E800020); // blr
        WriteWord(memory, 0x80001014, 0x38800002); // li r4, 2
        WriteWord(memory, 0x80001018, 0x4E800020); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0x1C),
            AddressRange.FromStartAndSize(0x80001000, 0x1C),
            default,
            "test");
        var result = new FunctionTranslator(image).Translate(
            0x80001000,
            TranslationOptions.Default with
            {
                MaxBytes = 0x40,
                AllowUnsupportedInstructions = true,
                KnownFunctionEntryPoints = new HashSet<uint> { 0x80001000u, 0x80001014u }
            });

        Assert.Contains("loc_80001014", result.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x80001014u>(ctx);", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_JumpTableKnownContinuationRemainsLocalControlFlow()
    {
        const uint entry = 0x80000000;
        const uint table = 0x80000100;
        const uint case0 = entry + 0x1C;
        const uint case1 = entry + 0x28;
        const uint common = entry + 0x34;

        var image = TranslatorCppTestHarness.CreateImage(
            (entry + 0x00, 0x28030002), // cmplwi r3, 2
            (entry + 0x04, 0x5463103A), // slwi r3, r3, 2
            (entry + 0x08, 0x3D808000), // lis r12, 0x8000
            (entry + 0x0C, 0x398C0100), // addi r12, r12, 0x100
            (entry + 0x10, 0x7C0C182E), // lwzx r0, r12, r3
            (entry + 0x14, 0x7C0903A6), // mtctr r0
            (entry + 0x18, 0x4E800420), // bctr
            (case0 + 0x00, 0x38600007), // li r3, 7
            (case0 + 0x04, 0x48000014), // b common
            (case1 + 0x00, 0x38600009), // li r3, 9
            (case1 + 0x04, 0x48000008), // b common
            (common + 0x00, 0x4E800020), // blr
            (table + 0x00, case0),
            (table + 0x04, case1),
            (table + 0x08, common));
        var result = new FunctionTranslator(image).Translate(
            entry,
            TranslationOptions.Default with
            {
                MaxBytes = 0x80,
                AllowUnsupportedInstructions = true,
                KnownFunctionEntryPoints = new HashSet<uint> { entry, case0, case1, common }
            });

        Assert.Contains("loc_80000034", result.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("InvokeDirectCpu<0x80000034u>(ctx);", result.CxxCode, StringComparison.Ordinal);
    }

    [Fact]
    public void Translate_ExternalConditionalBranchEmitsTailCallBlock()
    {
        var memory = new byte[MemoryLayout.RamSize];
        WriteWord(memory, 0x80001000, EncodeBc(12, 2, 0x1000)); // beq 0x80002000
        WriteWord(memory, 0x80001004, 0x4E800020); // blr

        var image = new ProgramImage(
            memory,
            AddressRange.FromStartAndSize(0x80001000, 0x8),
            AddressRange.FromStartAndSize(0x80001000, 0x8),
            default,
            "test");
        var result = new FunctionTranslator(image).Translate(
            0x80001000,
            TranslationOptions.Default with { MaxBytes = 0x8, AllowUnsupportedInstructions = true });

        Assert.Contains("InvokeDirectCpu<0x80002000u>(ctx);", result.CxxCode, StringComparison.Ordinal);
        Assert.DoesNotContain("synthetic missing target", result.CxxCode, StringComparison.Ordinal);
    }

    private static void WriteWord(byte[] memory, uint address, uint word)
    {
        var offset = checked((int)(address - MemoryLayout.RamBase));
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(offset, 4), word);
    }

    private static uint EncodeBc(uint bo, uint bi, int branchDelta, bool aa = false, bool lk = false)
        => (16u << 26) | (bo << 21) | (bi << 16) | ((((uint)branchDelta >> 2) & 0x3FFFu) << 2) | ((aa ? 1u : 0u) << 1) | (lk ? 1u : 0u);

    private static uint EncodeB(uint address, uint target, bool link = false)
        => (18u << 26) | ((target - address) & 0x03FFFFFCu) | (link ? 1u : 0u);

    [Fact]
    public void TranslationHelpers_ParseHexCallLabelsAndAbiArgs()
    {
        var hexArgs = new object?[] { "0x8000ABCD", 0u };
        Assert.True(InvokePrivate<bool>("TryParseHex", hexArgs));
        Assert.Equal(0x8000ABCDu, (uint)hexArgs[1]!);

        var plainArgs = new object?[] { "DEADBEEF", 0u };
        Assert.True(InvokePrivate<bool>("TryParseHex", plainArgs));
        Assert.Equal(0xDEADBEEFu, (uint)plainArgs[1]!);

        var invalidArgs = new object?[] { "not_hex", 0u };
        Assert.False(InvokePrivate<bool>("TryParseHex", invalidArgs));
        Assert.Equal(0u, (uint)invalidArgs[1]!);

        var callLabelArgs = new object?[] { "call_ctr_80001000_80001004", "call_ctr_", 0u, 0u };
        Assert.True(InvokePrivate<bool>("TryParseCallLabel", callLabelArgs));
        Assert.Equal(0x80001000u, (uint)callLabelArgs[2]!);
        Assert.Equal(0x80001004u, (uint)callLabelArgs[3]!);

        var shortLabelArgs = new object?[] { "call_ctr_80001000", "call_ctr_", 0u, 0u };
        Assert.False(InvokePrivate<bool>("TryParseCallLabel", shortLabelArgs));

        var wrongPrefixArgs = new object?[] { "indirect_ctr_80001000", "call_ctr_", 0u, 0u };
        Assert.False(InvokePrivate<bool>("TryParseCallLabel", wrongPrefixArgs));

        var abiArgs = InvokePrivate<IReadOnlyList<IrValue>>("BuildAbiCallArgs");
        Assert.Equal(21, abiArgs.Count);
        Assert.Equal("r3", abiArgs[0].RegisterName);
        Assert.Equal("r10", abiArgs[7].RegisterName);
        Assert.Equal("f1", abiArgs[8].RegisterName);
        Assert.Equal("f13", abiArgs[^1].RegisterName);
    }

    private static readonly HashSet<string> NoLocalLabels = new();

    [Fact]
    public void TryBuildSyntheticBlock_CoversIndirectAndCallForms()
    {
        var indirectArgs = new object?[] { "indirect_ctr_80001234", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", indirectArgs));
        var indirect = Assert.IsType<IrBasicBlock>(indirectArgs[2]);
        Assert.Equal("indirect_ctr_80001234", indirect.Label);
        Assert.Equal("ctr", Assert.IsType<IrIndirectJump>(Assert.Single(indirect.Instructions)).Target.RegisterName);

        var callCtrArgs = new object?[] { "call_ctr_80001000_80001004", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", callCtrArgs));
        var callCtr = Assert.IsType<IrBasicBlock>(callCtrArgs[2]);
        Assert.Collection(
            callCtr.Instructions,
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("lr", assign.Destination);
                Assert.Equal(unchecked((int)0x80001004u), assign.Value.Constant);
            },
            ins =>
            {
                var call = Assert.IsType<IrIndirectCall>(ins);
                Assert.Equal("ctr", call.Target.RegisterName);
                Assert.Equal(21, call.Arguments.Count);
            },
            ins => Assert.Equal("0x80001004", Assert.IsType<IrJump>(ins).TargetLabel));

        var callLrArgs = new object?[] { "call_lr_80002000_80002004", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", callLrArgs));
        var callLr = Assert.IsType<IrBasicBlock>(callLrArgs[2]);
        Assert.Collection(
            callLr.Instructions,
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("addr_bclrl_80002000_loc", assign.Destination);
                Assert.Equal("lr", assign.Value.RegisterName);
            },
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("lr", assign.Destination);
                Assert.Equal(unchecked((int)0x80002004u), assign.Value.Constant);
            },
            ins =>
            {
                var call = Assert.IsType<IrIndirectCall>(ins);
                Assert.Equal("addr_bclrl_80002000_loc", call.Target.RegisterName);
                Assert.Equal(21, call.Arguments.Count);
            },
            ins => Assert.Equal("0x80002004", Assert.IsType<IrJump>(ins).TargetLabel));

        var notTakenArgs = new object?[] { "link_branch_80002004_80002004", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", notTakenArgs));
        var notTaken = Assert.IsType<IrBasicBlock>(notTakenArgs[2]);
        Assert.Collection(
            notTaken.Instructions,
            ins =>
            {
                var assign = Assert.IsType<IrAssign>(ins);
                Assert.Equal("lr", assign.Destination);
                Assert.Equal(unchecked((int)0x80002004u), assign.Value.Constant);
            },
            ins => Assert.Equal("0x80002004", Assert.IsType<IrJump>(ins).TargetLabel));

        var linkedCallArgs = new object?[] { "link_branch_80002004_800D7FCC", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", linkedCallArgs));
        var linkedCall = Assert.IsType<IrBasicBlock>(linkedCallArgs[2]);
        Assert.Collection(
            linkedCall.Instructions,
            ins => Assert.Equal(unchecked((int)0x80002004u), Assert.IsType<IrAssign>(ins).Value.Constant),
            ins => Assert.Equal("0x800D7FCC", Assert.IsType<IrCall>(ins).Target),
            ins => Assert.Equal("0x80002004", Assert.IsType<IrJump>(ins).TargetLabel));

        var localLinkedArgs = new object?[] { "link_branch_80002004_80002000", new HashSet<string> { "0x80002000" }, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", localLinkedArgs));
        var localLinked = Assert.IsType<IrBasicBlock>(localLinkedArgs[2]);
        Assert.Collection(
            localLinked.Instructions,
            ins => Assert.Equal(unchecked((int)0x80002004u), Assert.IsType<IrAssign>(ins).Value.Constant),
            ins => Assert.Equal("0x80002000", Assert.IsType<IrJump>(ins).TargetLabel));

        var absoluteArgs = new object?[] { "0x800D7FCC", NoLocalLabels, null! };
        Assert.True(InvokePrivate<bool>("TryBuildSyntheticBlock", absoluteArgs));
        var absolute = Assert.IsType<IrBasicBlock>(absoluteArgs[2]);
        Assert.Equal("0x800D7FCC", absolute.Label);
        Assert.Collection(
            absolute.Instructions,
            ins =>
            {
                var call = Assert.IsType<IrCall>(ins);
                Assert.Equal("0x800D7FCC", call.Target);
                Assert.Equal(21, call.Arguments.Count);
            },
            ins => Assert.Null(Assert.IsType<IrReturn>(ins).Value));

        var invalidArgs = new object?[] { "plain_label", NoLocalLabels, null! };
        Assert.False(InvokePrivate<bool>("TryBuildSyntheticBlock", invalidArgs));
        Assert.Null(invalidArgs[2]);
    }

    [Fact]
    public void PublicRecordsExposeStoredValues()
    {
        var metrics = new TranslationMetrics();
        Assert.Same(TranslationOptions.Default, TranslationOptions.Default);

        var instructions = new[]
        {
            Translator.Core.Disassembly.PpcInstruction.Synthetic(
                0x800060A4,
                0x4E800020,
                "blr",
                Array.Empty<Translator.Core.Disassembly.PpcOperand>())
        };
        var blocks = new[] { new Translator.Core.Analysis.BasicBlocks.BasicBlock(0x800060A4, instructions) };
        var irFunction = new IrFunction(
            "coverage_result",
            "entry",
            new[] { new IrBasicBlock("entry", new IrInstruction[] { new IrReturn(IrValue.Register("r3")) }) });
        var cfg = new IrCfg(
            new Dictionary<string, IrBasicBlock>(StringComparer.OrdinalIgnoreCase) { ["entry"] = irFunction.Blocks[0] },
            "entry",
            new Dictionary<string, List<string>>(StringComparer.OrdinalIgnoreCase) { ["entry"] = new() });
        var ssa = new SsaResult(irFunction, cfg);
        var types = new RepresentationEnvironment();
        var signature = new FunctionAbiClassification("coverage_result", ValueRepresentation.Void);
        var result = new FunctionTranslationResult(
            0x800060A4,
            "coverage_result",
            instructions,
            blocks,
            irFunction,
            ssa,
            types,
            signature,
            "// generated",
            metrics);

        Assert.Equal(0x800060A4u, result.EntryPoint);
        Assert.Equal("coverage_result", result.Name);
        Assert.Same(instructions, result.Instructions);
        Assert.Same(blocks, result.Blocks);
        Assert.Same(irFunction, result.LinearIr);
        Assert.Same(ssa, result.Ssa);
        Assert.Same(types, result.Representations);
        Assert.Same(signature, result.AbiClassification);
        Assert.Equal("// generated", result.CxxCode);
        Assert.Same(metrics, result.Metrics);
    }
}
