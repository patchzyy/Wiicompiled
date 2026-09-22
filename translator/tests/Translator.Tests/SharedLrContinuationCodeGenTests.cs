using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

public class SharedLrContinuationCodeGenTests
{
    [Theory]
    [InlineData(2)]
    [InlineData(20)]
    public void MultipleHooksShareOneCompleteDispatchAndKeepNormalFallthrough(int callCount)
    {
        var instructions = new List<IrInstruction>();
        for (var i = 0; i < callCount; i++)
        {
            instructions.Add(new IrAssign("lr", IrValue.Imm(unchecked((int)(0x80001004u + (uint)i * 4u)))));
            instructions.Add(new IrCall(string.Empty, "0x81800000", Array.Empty<IrValue>()));
        }
        instructions.Add(new IrAssign("r3", IrValue.Imm(8)));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("shared_lr_continuation", "0x80001000", new[]
        {
            new IrBasicBlock("0x80001000", instructions),
            new IrBasicBlock("0x80001100", new IrInstruction[]
            {
                new IrAssign("r3", IrValue.Imm(1)), new IrReturn(null)
            })
        });
        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32, ["r3"] = ValueRepresentation.UInt32
        });
        var code = new CxxLinearCodeGenerator().Emit(0x80001000,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("shared_lr_continuation", ValueRepresentation.Void), types,
            lrContinuationCallTargets: new HashSet<uint> { 0x81800000u });

        Assert.Equal(1, code.Split("switch (ctx->lr)").Length - 1);
        Assert.Equal(callCount, code.Split("goto lr_continuation_dispatch;").Length - 1);
        Assert.Equal(callCount, code.Split("if (ctx->lr != ").Length - 1);
        Assert.Contains("case 0x80001100u:", code);
        Assert.Contains("goto loc_80001100;", code);
        Assert.Contains("InvokeIndirectCpu(ctx->lr, ctx);", code);
        var sharedLabel = code.IndexOf("lr_continuation_dispatch:", StringComparison.Ordinal);
        Assert.True(code.IndexOf("r3 = 8;", StringComparison.Ordinal) < sharedLabel);
        Assert.True(code.IndexOf("case 0x80001100u:", StringComparison.Ordinal) > sharedLabel);
    }
}
