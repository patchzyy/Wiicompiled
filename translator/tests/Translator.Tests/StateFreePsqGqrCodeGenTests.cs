using Translator.Core.Analysis.Representation;
using Translator.Core.Analysis.Ssa;
using Translator.Core.CodeGen;
using Translator.Core.Ir;
using Translator.Core.Representation;

namespace Translator.Tests;

public sealed class StateFreePsqGqrCodeGenTests
{
    public static IEnumerable<object[]> AccessCases()
    {
        for (uint w = 0; w < 2; ++w)
        for (uint i = 0; i < 8; ++i)
        foreach (var route in new[] { "generic", "stack", "resolved" })
        foreach (var update in new[] { false, true })
            yield return [w, i, route, update];
    }

    [Theory]
    [MemberData(nameof(AccessCases))]
    public void StateFreePsqLoadsAndStoresKeepTheirGqrAndTemplateArguments(
        uint w, uint i, string route, bool update)
    {
        var instructions = new List<IrInstruction>();
        if (route == "stack")
            instructions.Add(new IrBinary("r4", IrValue.Register("r1"), IrValue.Imm(32), "add"));
        if (route == "resolved")
        {
            instructions.Add(new IrResolveGuestMemoryRange("range", IrValue.Register("r4"), 0, 16, true, true));
            instructions.Add(new IrResolvedPsqLoad("f1", "range", IrValue.Register("r4"), 0, w, i, null));
        }
        else
            instructions.Add(new IrCall("f1", "PPC_PsqL", [IrValue.Register("r4"), IrValue.Imm(w), IrValue.Imm(i)]));
        if (update)
            instructions.Add(new IrBinary("r4", IrValue.Register("r4"), IrValue.Imm(8), "add"));
        if (route == "resolved")
            instructions.Add(new IrResolvedPsqStore("range", IrValue.Register("r4"), update ? 8 : 0,
                IrValue.Register("f1"), w, i, null));
        else
            instructions.Add(new IrCall(string.Empty, "PPC_PsqSt",
                [IrValue.Register("r4"), IrValue.Register("f1"), IrValue.Imm(w), IrValue.Imm(i)]));
        instructions.Add(new IrReturn(null));
        var function = new IrFunction("psq_gqr", "entry", [new IrBasicBlock("entry", instructions)]);

        var code = new CxxLinearCodeGenerator().Emit(0x80001000,
            new SsaTransformer().Convert(function),
            new FunctionAbiClassification(function.Name, ValueRepresentation.Void),
            new RepresentationEnvironment(),
            emitStateFreeLeafVariant: true,
            stateFreeCallSymbols: new Dictionary<uint, string> { [0x80001000] = "psq_gqr_native" });

        var start = code.IndexOf("psq_gqr_native(", StringComparison.Ordinal);
        Assert.True(start >= 0, code);
        var end = code.IndexOf("RECOMP_STATE_FREE_ABI", start, StringComparison.Ordinal);
        Assert.True(end > start, code);
        var variant = code[start..end];
        Assert.DoesNotContain("ctx", variant, StringComparison.Ordinal);
        Assert.Contains($"native_gqr{i}", variant, StringComparison.Ordinal);
        if (route == "generic")
        {
            Assert.Contains($"PPC_PsqLStateInline<{w}u, {i}u, false>(native_gqr{i},", variant, StringComparison.Ordinal);
            Assert.Contains($"PPC_PsqStStateInline<{w}u, {i}u, false>(native_gqr{i},", variant, StringComparison.Ordinal);
            Assert.DoesNotContain("GqrInline<", variant, StringComparison.Ordinal);
            Assert.DoesNotContain("mkw_gqr", code, StringComparison.Ordinal);
            Assert.Contains($"PPC_PsqLInline<{w}u, {i}u>(ctx,", code, StringComparison.Ordinal);
        }
        else if (route == "stack")
        {
            Assert.Contains($"PPC_PsqLStateInline<{w}u, {i}u, true>(native_gqr{i},", variant, StringComparison.Ordinal);
            Assert.Contains($"PPC_PsqStStateInline<{w}u, {i}u, true>(native_gqr{i},", variant, StringComparison.Ordinal);
        }
        else
        {
            Assert.Contains($"PPC_PsqLResolvedStateInline<{w}u, {i}u>(native_gqr{i},", variant, StringComparison.Ordinal);
            Assert.Contains($"PPC_PsqStResolvedStateInline<{w}u, {i}u>(native_gqr{i},", variant, StringComparison.Ordinal);
        }
        if (update) Assert.Contains("+ 8", variant, StringComparison.Ordinal);
    }
}
