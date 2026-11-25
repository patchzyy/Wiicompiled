using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class PairedFlowCodeGenTests
{
    [Fact]
    public void MixedScalarAndPairedPredecessorsNormalizeLiveFprOnTheEdge()
    {
        var function = new IrFunction(
            "mixed_ps_merge",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrBranch("beq", "paired", "scalar")
                }),
                new IrBasicBlock("paired", new IrInstruction[]
                {
                    new IrCall("f5", "PPC_PsMul", new[]
                    {
                        IrValue.Register("f1"), IrValue.Register("f2")
                    }),
                    new IrJump("merge")
                }),
                new IrBasicBlock("scalar", new IrInstruction[]
                {
                    new IrAssign("f5", IrValue.Register("f3")),
                    new IrJump("merge")
                }),
                new IrBasicBlock("merge", new IrInstruction[]
                {
                    new IrCall("f6", "PPC_PsNeg", new[] { IrValue.Register("f5") }),
                    new IrReturn(null)
                })
            });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f5"] = ValueRepresentation.Float64,
            ["f6"] = ValueRepresentation.Float64,
            ["cr0"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("mixed_ps_merge", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006000, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains(
            "f5.d = PPC_PsToScalarInline(f5.d);",
            code,
            StringComparison.Ordinal);
        Assert.Contains(
            "PPC_PsNegInline(PPC_PsFromScalarInline(f5.d))",
            code,
            StringComparison.Ordinal);
    }

    [Fact]
    public void FloatingCrCompareExtractsPs0FromPairedOperands()
    {
        var function = new IrFunction("paired_fcmp", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("f2", "PPC_PsMul", new[] { IrValue.Register("f3"), IrValue.Register("f4") }),
                new IrCall("f5", "PPC_PsAdd", new[] { IrValue.Register("f6"), IrValue.Register("f7") }),
                new IrSetCrField(0, IrValue.Register("f2"), IrValue.Register("f5"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(Enumerable.Range(0, 8)
            .ToDictionary(index => $"f{index}", _ => (ValueRepresentation)ValueRepresentation.Float64));
        var signature = new FunctionAbiClassification("paired_fcmp", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006004, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains(
            "SetCRFloatResident(cr, 0, PPC_PsToScalarInline(f2.d), PPC_PsToScalarInline(f5.d));",
            code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatingCrCompareLeavesScalarOperandsUnchanged()
    {
        var function = new IrFunction("scalar_fcmp", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(3, IrValue.Register("f1"), IrValue.Register("f2"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64
        });
        var signature = new FunctionAbiClassification("scalar_fcmp", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006004, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains("SetCRFloatResident(cr, 3, f1.d, f2.d);", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ExternalEntryFprsStayScalarAcrossBackedge()
    {
        var function = new IrFunction(
            "entry_backedge_ps_state",
            "entry",
            new[]
            {
                new IrBasicBlock("entry", new IrInstruction[]
                {
                    new IrCall(string.Empty, "PPC_Fcmp", new[] { IrValue.Imm(0), IrValue.Register("f31"), IrValue.Register("f1") }),
                    new IrBranch("beq", "loop", "exit")
                }),
                new IrBasicBlock("loop", new IrInstruction[]
                {
                    new IrCall("f31", "PPC_PsMul", new[] { IrValue.Register("f2"), IrValue.Register("f3") }),
                    new IrJump("entry")
                }),
                new IrBasicBlock("exit", new IrInstruction[] { new IrReturn(null) })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f31"] = ValueRepresentation.Float64,
            ["cr0"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("entry_backedge_ps_state", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(0x80006008, ssa, signature, types);

        // A raw "PPC_Fcmp" IrCall isn't normal decoder output, but residency treats it as a
        // safety-net form of fcmpo/fcmpu and fuses it into the resident CR write like IrSetCrField,
        // using the operands directly instead of PPC_PsToScalarInline (CxxLinearCodeGenerator.Inlines.cs "PPC_FCMP").
        Assert.Contains("SetCRFloatResident(cr, 0, f31.d, f1.d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("SetCRFloatResident(cr, 0, PPC_PsToScalarInline(f31.d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ResolvedScalarLoadEndsPairedStateAcrossEdge()
    {
        var function = new IrFunction("resolved_load_clears_paired", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrCall("f5", "PPC_PsMul", new[] { IrValue.Register("f1"), IrValue.Register("f2") }),
                new IrResolveGuestMemoryRange("range", IrValue.Register("r3"), 0, 8, true, false),
                new IrResolvedLoad("f5", "range", new IrAddress("r3", 0), 0, 8),
                new IrJump("next")
            }),
            new IrBasicBlock("next", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("f5"), IrValue.Register("f1"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f5"] = ValueRepresentation.Float64,
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("resolved_load_clears_paired", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x80006008, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains("SetCRFloatResident(cr, 0, f5.d, f1.d);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PPC_PsToScalarInline(f5.d)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ResolvedPsqLoadStaysPairedAcrossEdge()
    {
        var function = new IrFunction("resolved_psq_load_paired", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrResolveGuestMemoryRange("range", IrValue.Register("r3"), 0, 8, true, false),
                new IrResolvedPsqLoad("f5", "range", IrValue.Register("r3"), 0, 0, 0, 0u),
                new IrJump("next")
            }),
            new IrBasicBlock("next", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("f5"), IrValue.Register("f1"), false),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["f1"] = ValueRepresentation.Float64,
            ["f5"] = ValueRepresentation.Float64,
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("resolved_psq_load_paired", ValueRepresentation.Void);

        var code = new CxxLinearCodeGenerator().Emit(
            0x8000600C, new SsaTransformer().Convert(function), signature, types);

        Assert.Contains("SetCRFloatResident(cr, 0, PPC_PsToScalarInline(f5.d), f1.d);", code, StringComparison.Ordinal);
    }
}
