using Translator.Core.Analysis.Ssa;
using Translator.Core.Analysis.Representation;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;
using Xunit;

namespace Translator.Tests;

// This binary-free code-generation regression must run in the default suite.
public class LrContinuationCodeGenTests
{
    [Fact]
    public void CodeGenerator_DispatchesGuestCallLrContinuationWithoutMarkingTargetNonReturning()
    {
        var function = new IrFunction(
            "lr_continuation_call",
            "0x800E591C",
            new[]
            {
                new IrBasicBlock("0x800E591C", new IrInstruction[]
                {
                    new IrAssign("lr", IrValue.Imm(unchecked((int)0x800E5920u))),
                    new IrCall(string.Empty, "0x8179AC3C", Array.Empty<IrValue>()),
                    new IrAssign("r3", IrValue.Imm(8)),
                    new IrReturn(null)
                }),
                new IrBasicBlock("0x800E5934", new IrInstruction[]
                {
                    new IrAssign("r3", IrValue.Imm(1)),
                    new IrReturn(null)
                })
            });

        var types = new RepresentationEnvironment(new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r3"] = ValueRepresentation.UInt32
        });
        var signature = new FunctionAbiClassification("lr_continuation_call", ValueRepresentation.Void);

        var ssa = new SsaTransformer().Convert(function);
        var code = new CxxLinearCodeGenerator().Emit(
            0x800E591C,
            ssa,
            signature,
            types,
            lrContinuationCallTargets: new HashSet<uint> { 0x8179AC3Cu });

        var callIndex = code.IndexOf("InvokeDirectCpu<0x8179AC3Cu>(ctx);", StringComparison.Ordinal);
        var fallthroughGuardIndex = code.IndexOf("if (ctx->lr != 0x800E5920u)", callIndex, StringComparison.Ordinal);
        var localCaseIndex = code.IndexOf("case 0x800E5934u:", fallthroughGuardIndex, StringComparison.Ordinal);
        var returnIndex = code.IndexOf("return;", localCaseIndex, StringComparison.Ordinal);
        var fallthroughAssignmentIndex = code.IndexOf("r3 = 8;", callIndex, StringComparison.Ordinal);

        Assert.True(callIndex >= 0);
        Assert.True(fallthroughGuardIndex > callIndex);
        Assert.True(localCaseIndex > fallthroughGuardIndex);
        Assert.True(returnIndex > localCaseIndex);
        Assert.True(fallthroughAssignmentIndex > returnIndex, code);
        Assert.Contains("goto loc_800E5934;", code);
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void CodeGenerator_KeepsPlainAccessesInResumableBodies(bool resumable)
    {
        const uint entry = 0x80001000u;
        var instructions = new List<IrInstruction>
        {
            new IrAssign("lr", IrValue.Imm(unchecked((int)(entry + 4)))),
            new IrCall(string.Empty, "0x8179AC3C", Array.Empty<IrValue>()),
            new IrTracePpc(entry, "bl", "0x48000001")
        };
        var registers = new Dictionary<string, ValueRepresentation>
        {
            ["lr"] = ValueRepresentation.UInt32,
            ["r4"] = ValueRepresentation.UInt32
        };
        for (var i = 0; i < 8; i++)
        {
            var destination = $"r{5 + i}";
            registers[destination] = ValueRepresentation.UInt32;
            instructions.Add(new IrLoad(destination, new IrAddress("r4", i * 4), 4));
            instructions.Add(new IrTracePpc(entry + 4 + (uint)i * 4, "lwz", "0x80000000"));
        }
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("resumable_loads", $"0x{entry:X8}",
            new[] { new IrBasicBlock($"0x{entry:X8}", instructions) });

        var code = new CxxLinearCodeGenerator().Emit(
            entry,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification("resumable_loads", ValueRepresentation.Void),
            new RepresentationEnvironment(registers),
            lrContinuationCallTargets: resumable ? new HashSet<uint> { 0x8179AC3Cu } : new HashSet<uint>());

        if (!resumable)
        {
            // Control: the same body is range-lowered and paired when nothing can resume it mid-block.
            Assert.Contains("ReadResolvedPair32", code);
            return;
        }

        // A resume past the range resolve would read through a missing or stale host pointer.
        Assert.DoesNotContain("guest_range_", code);
        // Resuming at the second load must run that load and not the first one.
        var label = code.IndexOf($"loc_{entry + 8:X8}: ;", StringComparison.Ordinal);
        Assert.True(label >= 0, code);
        var resumed = code[label..];
        Assert.DoesNotContain("r5 = ", resumed);
        Assert.Contains("r6 = ", resumed);
    }
}
