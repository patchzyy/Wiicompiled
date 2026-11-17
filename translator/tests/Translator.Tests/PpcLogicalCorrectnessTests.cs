using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Disassembly;
using Translator.Core.Ir;
using Translator.Core.Lifting;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class PpcLogicalCorrectnessTests
{
    [Fact]
    public void OnlyTheFullStoreConditionalOpcodeWithRecordBitIsAccepted()
    {
        for (uint xo = 0; xo < 1024; ++xo)
        foreach (var rc in new[] { false, true })
        {
            var word = (31u << 26) | (3u << 21) | (4u << 16) | (5u << 11) |
                       (xo << 1) | (rc ? 1u : 0u);
            var decoded = PpcDecoder.Decode(0x80001000, word);
            Assert.Equal(xo == 150 && rc, decoded.Mnemonic == "stwcx.");
        }

        var instruction = PpcDecoder.Decode(0x80001000, 0x7C64292Du);
        Assert.Equal("stwcx.", instruction.Mnemonic);
        Assert.Collection(instruction.Operands,
            op => Assert.Equal("r3", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r4", Assert.IsType<PpcRegisterOperand>(op).Name),
            op => Assert.Equal("r5", Assert.IsType<PpcRegisterOperand>(op).Name));
        var lifted = Assert.Single(new PpcLifter().Lift([instruction])).Ir;
        var store = Assert.Single(lifted.OfType<IrCall>());
        Assert.Equal("PPC_Stwcx", store.Target);
        Assert.Equal(IrValue.Register("r3"), store.Arguments[1]);
    }

    [Theory]
    [InlineData(false, 3u)]
    [InlineData(false, 4u)]
    [InlineData(false, 5u)]
    [InlineData(true, 3u)]
    [InlineData(true, 4u)]
    [InlineData(true, 5u)]
    public void NorRecordsItsResultAsASignedCr0ComparisonOnlyForRc(bool rc, uint destination)
    {
        var word = (31u << 26) | (3u << 21) | (destination << 16) | (5u << 11) |
                   (124u << 1) | (rc ? 1u : 0u);
        var instruction = PpcDecoder.Decode(0x80001000, word);
        Assert.Equal(rc ? "nor." : "nor", instruction.Mnemonic);
        var lifted = Assert.Single(new PpcLifter().Lift([instruction])).Ir;
        var register = $"r{destination}";
        Assert.Equal(new IrBinary(register, IrValue.Register("r3"), IrValue.Register("r5"), "nor"), lifted[0]);
        if (rc)
            Assert.Equal(new IrSetCrField(0, IrValue.Register(register), IrValue.Imm(0), false), lifted[1]);
        else
            Assert.Single(lifted);

        var function = new IrFunction("nor_record", "entry",
        [
            new IrBasicBlock("entry", lifted.Append<IrInstruction>(new IrBranch("bso", "taken", "exit")).ToArray()),
            new IrBasicBlock("taken", [new IrReturn(null)]),
            new IrBasicBlock("exit", [new IrReturn(null)])
        ]);
        var code = Emit(function);
        Assert.Contains("~(r3 | r5)", code, StringComparison.Ordinal);
        if (rc)
            Assert.Contains($"SetCRResident(cr, xer, 0, static_cast<int32_t>({register}), static_cast<int32_t>(0))", code, StringComparison.Ordinal);
        else
            Assert.DoesNotContain("SetCRResident(", code, StringComparison.Ordinal);
    }

    [Fact]
    public void CrorcPreservesOperandOrderThroughLiftingAndBranchEmission()
    {
        var instruction = PpcDecoder.Decode(0x80001000,
            (19u << 26) | (2u << 21) | (3u << 16) | (4u << 11) | (417u << 1));
        Assert.Equal("crorc", instruction.Mnemonic);
        var call = Assert.IsType<IrCall>(Assert.Single(Assert.Single(new PpcLifter().Lift([instruction])).Ir));
        Assert.Equal(new[] { IrValue.Imm(6), IrValue.Imm(2), IrValue.Imm(3), IrValue.Imm(4) }, call.Arguments);
        var function = new IrFunction("crorc_branch", "entry",
        [
            new IrBasicBlock("entry", [call, new IrBranch("beq", "taken", "exit")]),
            new IrBasicBlock("taken", [new IrReturn(null)]),
            new IrBasicBlock("exit", [new IrReturn(null)])
        ]);
        var code = Emit(function);
        Assert.Contains("PpcCrLogicalResident(cr, static_cast<uint32_t>(6), static_cast<uint32_t>(2), static_cast<uint32_t>(3), static_cast<uint32_t>(4))", code, StringComparison.Ordinal);
        Assert.Contains("& 0x2", code, StringComparison.Ordinal);
    }

    private static string Emit(IrFunction function) => new CxxLinearCodeGenerator().Emit(
        0x80001000, new SsaTransformer().Convert(function),
        new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
        new RepresentationEnvironment());
}
