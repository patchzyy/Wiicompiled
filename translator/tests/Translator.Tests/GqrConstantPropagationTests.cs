using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Translation;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class GqrConstantPropagationTests
{
    [Fact]
    public void SpecializesKnownGqrAfterConstantWrite()
    {
        var function = Function(new IrInstruction[]
        {
            new IrAssign("r3", IrValue.Imm(0x00040004)),
            new IrAssign("gqr2", IrValue.Register("r3")),
            PsqLoad(2),
            new IrReturn(null)
        });
        var specialized = GqrConstantPropagation.Specialize(function);
        var call = Assert.IsType<IrCall>(specialized.Blocks[0].Instructions[2]);
        Assert.Equal("PPC_PsqLKnown_00040004", call.Target);
    }

    [Fact]
    public void MergeKeepsOnlyEqualMustConstants()
    {
        var equal = Diamond(0x00040004, 0x00040004);
        var unequal = Diamond(0x00040004, 0x00050005);
        Assert.Contains(GqrConstantPropagation.Specialize(equal).Blocks.Single(b => b.Label == "join").Instructions,
            i => i is IrCall { Target: "PPC_PsqLKnown_00040004" });
        Assert.Contains(GqrConstantPropagation.Specialize(unequal).Blocks.Single(b => b.Label == "join").Instructions,
            i => i is IrCall { Target: "PPC_PsqL" });
    }

    [Fact]
    public void LoopAndOutOfOrderBlocksDoNotEraseEstablishedConstants()
    {
        var function = new IrFunction("gqr_loop", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrAssign("gqr5", IrValue.Imm(0x00070007)),
                new IrJump("loop")
            }),
            // Deliberately precedes its only reachable predecessor in storage order.
            new IrBasicBlock("exit", new IrInstruction[] { PsqLoad(5), new IrReturn(null) }),
            new IrBasicBlock("loop", new IrInstruction[] { new IrBranch("beq", "loop", "exit") })
        });

        var specialized = GqrConstantPropagation.Specialize(function);

        Assert.Contains(specialized.Blocks.Single(block => block.Label == "exit").Instructions,
            instruction => instruction is IrCall { Target: "PPC_PsqLKnown_00070007" });
    }

    [Fact]
    public void UnknownWriteAndGuestCallsInvalidateGqrConstants()
    {
        var unknownWrite = Function(new IrInstruction[]
        {
            new IrAssign("gqr2", IrValue.Imm(0x00040004)),
            new IrLoad("r4", new IrAddress("r3", 0), 4),
            new IrAssign("gqr2", IrValue.Register("r4")),
            PsqLoad(2), new IrReturn(null)
        });
        var guestCall = Function(new IrInstruction[]
        {
            new IrAssign("gqr2", IrValue.Imm(0x00040004)),
            new IrCall(string.Empty, "func_80001000", Array.Empty<IrValue>()),
            PsqLoad(2), new IrReturn(null)
        });
        Assert.Contains(GqrConstantPropagation.Specialize(unknownWrite).Blocks[0].Instructions,
            i => i is IrCall { Target: "PPC_PsqL" });
        Assert.Contains(GqrConstantPropagation.Specialize(guestCall).Blocks[0].Instructions,
            i => i is IrCall { Target: "PPC_PsqL" });
    }

    [Fact]
    public void CodegenEmitsKnownGqrTemplateWithoutContextRead()
    {
        var function = Function(new IrInstruction[]
        {
            new IrAssign("r3", IrValue.Imm(0x3D043D04)),
            new IrAssign("gqr2", IrValue.Register("r3")),
            new IrCall(string.Empty, "PPC_PsqSt", new[] { IrValue.Register("r4"), IrValue.Register("f1"), IrValue.Imm(0), IrValue.Imm(2) }),
            new IrReturn(null)
        });
        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80001000, ssa,
            new FunctionAbiClassification("known_gqr", ValueRepresentation.Void),
            new RepresentationEnvironment());
        Assert.Contains("PPC_PsqStKnownInline<0u, 2u, 0x3D043D04u>", code, StringComparison.Ordinal);
    }

    [Fact]
    public void GuardedEntryConstantEmitsKnownFastPathAndDynamicFallback()
    {
        var function = Function(new IrInstruction[] { PsqLoad(5), new IrReturn(null) });
        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80001000, ssa,
            new FunctionAbiClassification("guarded_gqr", ValueRepresentation.Void),
            new RepresentationEnvironment(),
            gqrEntryConstants: new Dictionary<string, uint> { ["gqr5"] = 0x00070007u },
            gqrConstantsRequireRuntimeGuard: true);

        Assert.Contains("ctx->gqr[5u] == 0x00070007u", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqLKnownInline<0u, 5u, 0x00070007u>", code, StringComparison.Ordinal);
        // The guard's dynamic arm is the generic helper, reading the GQR value
        // hoisted into the prologue rather than ctx->gqr[5] per access.
        Assert.Contains("[[maybe_unused]] uint32_t mkw_gqr5 = ctx->gqr[5];", code, StringComparison.Ordinal);
        Assert.Contains("PPC_PsqLGqrInline<0u, 5u>(ctx, mkw_gqr5,", code, StringComparison.Ordinal);
    }

    [Fact]
    public void RepeatedGuardedPsqSitesUseOneFunctionVersionGuard()
    {
        var instructions = Enumerable.Range(0, 8)
            .Select(_ => (IrInstruction)PsqLoad(5))
            .Append(new IrReturn(null))
            .ToArray();
        var function = Function(instructions);
        var code = new CxxLinearCodeGenerator().Emit(0x80001000,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("versioned_gqr", ValueRepresentation.Void),
            new RepresentationEnvironment(),
            gqrEntryConstants: new Dictionary<string, uint> { ["gqr5"] = 0x00070007u },
            gqrConstantsRequireRuntimeGuard: true);

        Assert.Contains("template <bool gqr_entry_profile>", code, StringComparison.Ordinal);
        Assert.Contains("versioned_gqr_gqr_impl<true>(ctx);", code, StringComparison.Ordinal);
        Assert.Contains("versioned_gqr_gqr_impl<false>(ctx);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("const bool gqr_entry_5_00070007", code, StringComparison.Ordinal);
    }

    [Fact]
    public void InterproceduralAnalysisPropagatesConstantsThroughDirectCalls()
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80001000] = Function(new IrInstruction[]
            {
                new IrAssign("gqr5", IrValue.Imm(0x00070007)),
                new IrAssign("gqr6", IrValue.Imm(0x3D043D04)),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }),
            [0x80002000] = Function(new IrInstruction[]
            {
                new IrCall(string.Empty, "0x80003000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }),
            [0x80003000] = Function(new IrInstruction[] { PsqLoad(5), new IrReturn(null) })
        };

        var result = GqrInterproceduralAnalysis.Analyze(
            functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value)),
            new[] { 0x80001000u });

        Assert.Equal(0x00070007u, result.EntryConstants[0x80002000]["gqr5"]);
        Assert.Equal(0x3D043D04u, result.EntryConstants[0x80003000]["gqr6"]);
        var specialized = GqrConstantPropagation.Specialize(
            functions[0x80003000], result.EntryConstants[0x80003000], result.WriteMasks);
        Assert.Contains(specialized.Blocks[0].Instructions,
            instruction => instruction is IrCall { Target: "PPC_PsqLKnown_00070007" });
    }

    [Fact]
    public void UnknownEntryRootsDependOnlyOnTheCallGraph()
    {
        IrFunction Calls(params uint[] targets) => Function(targets
            .Select(target => (IrInstruction)new IrCall(string.Empty, $"0x{target:X8}", Array.Empty<IrValue>()))
            .Append(new IrReturn(null))
            .ToArray());
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80001000] = Calls(),
            [0x80002000] = Calls(0x80003000),
            [0x80003000] = Calls(),
            [0x80004000] = Calls(0x80005000),
            [0x80005000] = Calls(0x80004000)
        };
        var summaries = functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value));
        uint[] candidates = [0x80002000, 0x80003000, 0x80004000, 0x80005000];

        var roots = GqrInterproceduralAnalysis.SelectUnknownEntryRoots(summaries, [0x80001000u], candidates);
        var reversed = GqrInterproceduralAnalysis.SelectUnknownEntryRoots(summaries, [0x80001000u], candidates.Reverse());

        Assert.Equal(new uint[] { 0x80001000, 0x80002000, 0x80004000 }, roots.Order());
        Assert.Equal(roots.Order(), reversed.Order());
    }

    [Fact]
    public void InterproceduralAnalysisIntersectsConflictingCallers()
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80001000] = Function(new IrInstruction[]
            {
                new IrAssign("gqr5", IrValue.Imm(0x00070007)),
                new IrCall(string.Empty, "0x80003000", Array.Empty<IrValue>()),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }),
            [0x80002000] = Function(new IrInstruction[]
            {
                new IrAssign("gqr5", IrValue.Imm(0x00050005)),
                new IrCall(string.Empty, "0x80003000", Array.Empty<IrValue>()),
                new IrReturn(null)
            }),
            [0x80003000] = Function(new IrInstruction[] { PsqLoad(5), new IrReturn(null) })
        };

        var result = GqrInterproceduralAnalysis.Analyze(
            functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value)),
            new[] { 0x80001000u });

        Assert.False(result.EntryConstants[0x80003000].ContainsKey("gqr5"));
    }

    [Fact]
    public void CalleeWriteSummaryPreservesOnlyUnmodifiedGqrs()
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80001000] = Function(new IrInstruction[]
            {
                new IrAssign("gqr5", IrValue.Imm(0x00070007)),
                new IrAssign("gqr6", IrValue.Imm(0x3D043D04)),
                new IrCall(string.Empty, "0x80002000", Array.Empty<IrValue>()),
                PsqLoad(5),
                PsqLoad(6),
                new IrReturn(null)
            }),
            [0x80002000] = Function(new IrInstruction[]
            {
                new IrAssign("gqr6", IrValue.Register("r3")),
                new IrReturn(null)
            })
        };
        var result = GqrInterproceduralAnalysis.Analyze(
            functions.ToDictionary(pair => pair.Key, pair => GqrFunctionSummary.Create(pair.Value)),
            new[] { 0x80001000u });
        var specialized = GqrConstantPropagation.Specialize(
            functions[0x80001000], result.EntryConstants[0x80001000], result.WriteMasks);

        Assert.Equal(1 << 6, result.WriteMasks[0x80002000]);
        Assert.Contains(specialized.Blocks[0].Instructions,
            instruction => instruction is IrCall { Target: "PPC_PsqLKnown_00070007" });
        Assert.Contains(specialized.Blocks[0].Instructions,
            instruction => instruction is IrCall { Target: "PPC_PsqL" } call &&
                           call.Arguments[2].Constant == 6);
    }

    private static IrCall PsqLoad(int index) => new("f1", "PPC_PsqL", new[] { IrValue.Register("r4"), IrValue.Imm(0), IrValue.Imm(index) });
    private static IrFunction Function(IReadOnlyList<IrInstruction> instructions) =>
        new("gqr_test", "entry", new[] { new IrBasicBlock("entry", instructions) });
    private static IrFunction Diamond(uint left, uint right) => new("gqr_diamond", "entry", new[]
    {
        new IrBasicBlock("entry", new IrInstruction[] { new IrBranch("beq", "left", "right") }),
        new IrBasicBlock("left", new IrInstruction[] { new IrAssign("gqr2", IrValue.Imm(left)), new IrJump("join") }),
        new IrBasicBlock("right", new IrInstruction[] { new IrAssign("gqr2", IrValue.Imm(right)), new IrJump("join") }),
        new IrBasicBlock("join", new IrInstruction[] { PsqLoad(2), new IrReturn(null) })
    });
}
