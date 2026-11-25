using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class FusedFloatLoadCodeGenTests
{
    [Theory]
    [InlineData(false, true, true)]
    [InlineData(true, true, true)]
    [InlineData(false, true, false)]
    [InlineData(true, true, false)]
    [InlineData(false, false, true)]
    [InlineData(true, false, true)]
    public void FusedFloatLoadsWidenBitsWithoutHostFloatConversionAndKeepFallbacks(
        bool descending, bool firstFloat, bool secondFloat)
    {
        var code = Emit(descending, firstFloat, secondFloat, 4);
        Assert.Contains("MemoryInline::ReadResolvedPair32(range, 0u)", code, StringComparison.Ordinal);
        Assert.Contains("if (resolved_pair.valid)", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PpcBitCastToFloatInline(resolved_pair.", code, StringComparison.Ordinal);

        CheckMember(firstFloat, descending ? "second" : "first", descending ? 4 : 0);
        CheckMember(secondFloat, descending ? "first" : "second", descending ? 0 : 4);

        void CheckMember(bool isFloat, string member, int offset)
        {
            var value = isFloat
                ? $"PpcBitCastToDoubleInline(PpcConvertToDoubleBitsInline(resolved_pair.{member}))"
                : $"resolved_pair.{member}";
            Assert.Contains($"= {value};", code, StringComparison.Ordinal);
            var helper = isFloat ? "ReadResolvedFloat32" : "ReadResolved32";
            var fallback = code.IndexOf("} else {", code.IndexOf("if (resolved_pair.valid)", StringComparison.Ordinal), StringComparison.Ordinal);
            Assert.True(fallback > 0, code);
            Assert.Contains($"MemoryInline::{helper}(range, {offset}u,", code[fallback..], StringComparison.Ordinal);
        }
    }

    [Theory]
    [InlineData(false, 2)]
    [InlineData(true, 2)]
    [InlineData(false, 4)]
    [InlineData(true, 4)]
    public void FusedIntegerLoadsKeepRawMembers(bool descending, int width)
    {
        var code = Emit(descending, false, false, width);
        Assert.Contains($"MemoryInline::ReadResolvedPair{width * 8}(range, 0u)", code, StringComparison.Ordinal);
        Assert.Contains("= resolved_pair.first;", code, StringComparison.Ordinal);
        Assert.Contains("= resolved_pair.second;", code, StringComparison.Ordinal);
        Assert.DoesNotContain("PpcConvertToDoubleBitsInline(resolved_pair.", code, StringComparison.Ordinal);
    }

    private static string Emit(bool descending, bool firstFloat, bool secondFloat, int width)
    {
        var function = new IrFunction("fused_load", "entry", [new IrBasicBlock("entry",
        [
            new IrResolveGuestMemoryRange("range", IrValue.Register("r3"), 8, width * 2, true, false),
            new IrResolvedLoadPair(firstFloat ? "f1" : "r4", secondFloat ? "f2" : "r5", "range",
                new IrAddress("r3", descending ? 8 + width : 8),
                new IrAddress("r3", descending ? 8 : 8 + width), 0, width, descending),
            new IrReturn(null)
        ])]);
        return new CxxLinearCodeGenerator().Emit(0x80001000, new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void), new RepresentationEnvironment());
    }
}
