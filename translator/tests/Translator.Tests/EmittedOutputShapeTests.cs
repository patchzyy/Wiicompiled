using System.Collections.Generic;
using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

/// <summary>
/// Shape of the emitted text. These assertions are about readability of the
/// generated C++; each one is semantics-preserving by construction.
/// </summary>
public class EmittedOutputShapeTests
{
    private static string Emit(IrFunction function, RepresentationEnvironment types, uint entryPoint = 0x80001000u) =>
        new CxxLinearCodeGenerator().Emit(
            entryPoint,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types);

    private static RepresentationEnvironment UInt32Registers(params string[] names)
    {
        var map = new Dictionary<string, ValueRepresentation>();
        foreach (var name in names) map[name] = ValueRepresentation.UInt32;
        return new RepresentationEnvironment(map);
    }

    [Fact]
    public void BranchWithAPureFallthroughFalseEdgeOmitsTheElseBlock()
    {
        var function = new IrFunction("branch_fallthrough", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "next", "cr0")
            }),
            new IrBasicBlock("next", new IrInstruction[]
            {
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(1), "add"),
                new IrReturn(null)
            }),
            new IrBasicBlock("taken", new IrInstruction[] { new IrReturn(null) })
        });

        var code = Emit(function, UInt32Registers("r3"));

        Assert.Contains("goto loc_taken;", code, StringComparison.Ordinal);
        // The false edge falls through to the next emitted block and needs no
        // representation normalization, so there is nothing to put in an else.
        Assert.DoesNotContain("} else {", code, StringComparison.Ordinal);
    }

    [Fact]
    public void BranchWithARealFalseEdgeKeepsTheElseBlock()
    {
        var function = new IrFunction("branch_two_gotos", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrSetCrField(0, IrValue.Register("r3"), IrValue.Imm(0), false),
                new IrBranch("beq", "taken", "other", "cr0")
            }),
            new IrBasicBlock("filler", new IrInstruction[] { new IrReturn(null) }),
            new IrBasicBlock("taken", new IrInstruction[] { new IrReturn(null) }),
            new IrBasicBlock("other", new IrInstruction[] { new IrReturn(null) })
        });

        var code = Emit(function, UInt32Registers("r3"));

        Assert.Contains("} else {", code, StringComparison.Ordinal);
        Assert.Contains("goto loc_other;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void AddressConstantsAssignedToUnsignedRegistersPrintAsHex()
    {
        var function = new IrFunction("address_constants", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                // 0x801B4ABC as a signed int immediate.
                new IrAssign("lr", IrValue.Imm(unchecked((int)0x801B4ABCu))),
                new IrAssign("r4", IrValue.Imm(unchecked((int)0x80000000u))),
                // Not an address: a genuinely signed small immediate, and a
                // negative value outside the guest address window.
                new IrBinary("r3", IrValue.Register("r3"), IrValue.Imm(-16), "add"),
                new IrAssign("r5", IrValue.Imm(-1)),
                new IrReturn(null)
            })
        });

        var code = Emit(function, UInt32Registers("r3", "r4", "r5"));

        Assert.Contains("ctx->lr = 0x801B4ABCu;", code, StringComparison.Ordinal);
        Assert.Contains("r4 = 0x80000000u;", code, StringComparison.Ordinal);
        Assert.Contains("r3 = (r3 + -16);", code, StringComparison.Ordinal);
        Assert.Contains("r5 = -1;", code, StringComparison.Ordinal);
    }

    [Fact]
    public void SubWordStoresCastToTheExactMemoryParameterType()
    {
        var function = new IrFunction("sub_word_stores", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrStore(new IrAddress("r4", 0), IrValue.Register("r3"), 1),
                new IrStore(new IrAddress("r4", 4), IrValue.Register("r3"), 2),
                new IrStore(new IrAddress("r4", 8), IrValue.Register("r3"), 4),
                new IrReturn(null)
            })
        });

        var code = Emit(function, UInt32Registers("r3", "r4"));

        Assert.Contains("MemoryInline::FlatWrite8(r4, static_cast<uint8_t>(r3));", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWrite16((r4 + 4), static_cast<uint16_t>(r3));", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWrite32((r4 + 8), r3);", code, StringComparison.Ordinal);
        Assert.DoesNotContain("static_cast<uint32_t>(r3)", code, StringComparison.Ordinal);
    }

    [Fact]
    public void FloatFlatHelpersPreserveTheWidthMapping()
    {
        var function = new IrFunction("float_width_helpers", "entry", new[]
        {
            new IrBasicBlock("entry", new IrInstruction[]
            {
                new IrStore(new IrAddress("r4", 0), IrValue.Register("f1"), 4),
                new IrStore(new IrAddress("r4", 8), IrValue.Register("f2"), 8),
                new IrLoad("f3", new IrAddress("r4", 16), 4),
                new IrLoad("f4", new IrAddress("r4", 24), 8),
                new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["r4"] = ValueRepresentation.UInt32,
            ["f1"] = ValueRepresentation.Float64,
            ["f2"] = ValueRepresentation.Float64,
            ["f3"] = ValueRepresentation.Float64,
            ["f4"] = ValueRepresentation.Float64
        });

        var code = Emit(function, types);

        Assert.Contains("MemoryInline::FlatWriteFloat32(r4, f1.d);", code, StringComparison.Ordinal);
        Assert.Contains("MemoryInline::FlatWriteFloat64((r4 + 8), f2.d);", code, StringComparison.Ordinal);
        Assert.Contains("f3.d = MemoryInline::FlatReadFloat32((r4 + 16));", code, StringComparison.Ordinal);
        Assert.Contains("f4.d = MemoryInline::FlatReadFloat64((r4 + 24));", code, StringComparison.Ordinal);
    }

    [Fact]
    public void ContinuationLabelAtBlockEndEmitsValidCxx17Statement()
    {
        var function = new IrFunction("continuation_at_block_end", "0x800E7798", new[]
        {
            new IrBasicBlock("0x800E7798", new IrInstruction[]
            {
                new IrCall(string.Empty, "0x8179B000", System.Array.Empty<IrValue>()),
                new IrTracePpc(0x800E77A0u, "nop", "0x60000000"),
                new IrJump("0x800E77A4")
            }),
            new IrBasicBlock("0x800E77A4", new IrInstruction[]
            {
                new IrReturn(null)
            })
        });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>());
        var code = new CxxLinearCodeGenerator().Emit(
            0x800E7798,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179B000u });

        // In C++17, a label before a closing brace is invalid without an intervening statement.
        Assert.Contains("loc_800E77A0: ;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("loc_800E77A0:\n}", code.Replace("\r\n", "\n"), StringComparison.Ordinal);
    }
}

