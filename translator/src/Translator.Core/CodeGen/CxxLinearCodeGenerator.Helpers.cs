using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private enum InlineGuestThunkKind
    {
        SaveGpr,
        RestGpr,
        SaveFpr,
        RestFpr,
    }

    private readonly record struct InlineGuestThunkSpec(
        InlineGuestThunkKind Kind,
        int StartRegister,
        uint Address);

    /// <summary>
    /// Indentation prefixes. <c>EmitInstruction</c> built one of these per
    /// emitted instruction; the table is indexed by nesting depth and produces
    /// exactly the same text.
    /// </summary>
    private static readonly string[] IndentPads = CreateIndentPads();

    private static string[] CreateIndentPads()
    {
        var pads = new string[10];
        for (var i = 0; i < pads.Length; i++)
        {
            pads[i] = new string(' ', i * 4);
        }

        return pads;
    }

    private static string IndentPad(int indent) =>
        (uint)indent < (uint)IndentPads.Length ? IndentPads[indent] : new string(' ', indent * 4);

    /// <summary>
    /// Single entry point for helper-effect lookups from the emitter. Not memoized: the catalog
    /// already resolves a target via one frozen-dictionary probe and hands out shared instances,
    /// so a call-site memo would just duplicate the catalog's own lookup for no benefit.
    /// </summary>
    private static Translator.Core.Analysis.GuestHelperEffect AnalyzeGuestHelperEffect(IrCall call) =>
        Translator.Core.Analysis.GuestHelperEffectCatalog.Analyze(call);

    private static string SanitizeLabel(string label)
    {
        var clean = label.StartsWith("0x", StringComparison.OrdinalIgnoreCase) ? label[2..] : label;
        clean = clean.Replace(":", "_").Replace("-", "_");
        return $"loc_{clean}";
    }

    private static string SanitizeIdentifier(string name)
    {
        if (string.IsNullOrWhiteSpace(name))
        {
            return "_";
        }

        var trimmed = name.StartsWith("0x", StringComparison.OrdinalIgnoreCase)
            ? name[2..]
            : name;

        var sb = new StringBuilder(trimmed.Length + 8);
        foreach (var ch in trimmed)
        {
            sb.Append(char.IsLetterOrDigit(ch) || ch == '_' ? ch : '_');
        }

        var result = sb.ToString();
        if (result.Length == 0 || !(char.IsLetter(result[0]) || result[0] == '_'))
        {
            result = "func_" + result;
        }

        return result;
    }

    private static string FormatSymbol(string target) => SanitizeIdentifier(target);

    private static bool IsGuestCallTarget(string target)
    {
        if (string.IsNullOrWhiteSpace(target))
        {
            return false;
        }

        if (GuestTargetParser.TryParseAddress(target, out _))
        {
            return true;
        }

        var trimmed = target.Trim();
        if (trimmed.StartsWith("0x", StringComparison.OrdinalIgnoreCase))
        {
            return true;
        }

        return trimmed.StartsWith("func_", StringComparison.OrdinalIgnoreCase);
    }

    private static bool TryParseAddress(string target, out uint address) =>
        GuestTargetParser.TryParseAddress(target, out address);

    private static bool IsGuestScalarFloatArgument(
        IGuestFunctionAbiProvider guestAbiProvider,
        string target,
        string registerName)
    {
        var baseName = GetRegisterBaseName(registerName);
        if (!IsAbiFloatArgumentRegister(baseName) ||
            !guestAbiProvider.TryGetGuestFunctionAbi(target, out var abi))
        {
            return false;
        }

        return abi.HasScalarFloatArgument(baseName);
    }

    private static bool IsGuestPairedScalarFloatReturn(
        IGuestFunctionAbiProvider guestAbiProvider,
        string target) =>
        guestAbiProvider.TryGetGuestFunctionAbi(target, out var abi) &&
        abi.ReturnsPairedScalarFloat;

    private static bool IsPairedProducerTarget(string target) =>
        PpcFloatCallSemantics.IsPairedProducerTarget(target);

    private static bool IsPairedConsumerTarget(string target) =>
        PpcFloatCallSemantics.IsPairedConsumerTarget(target);

    private static bool IsSinglePrecisionConsumerTarget(string target) =>
        PpcFloatCallSemantics.IsSinglePrecisionConsumerTarget(target);

    private static bool IsScalarFloatConsumerTarget(string target) =>
        PpcFloatCallSemantics.IsScalarFloatConsumerTarget(target);

    private static bool IsPairedFloatValue(
        IrValue value,
        IReadOnlyDictionary<string, bool> localPaired)
    {
        if (value is not { Kind: "register", RegisterName: { } register } ||
            !IsFloatRegister(register))
        {
            return false;
        }

        return localPaired.TryGetValue(GetRegisterBaseName(register), out var paired) && paired;
    }

    /// <summary>
    /// Materializes the scalar ps0 value when a scalar floating-point consumer
    /// reads an FPR whose current authoritative representation is a packed pair.
    /// </summary>
    private static string ToScalarFloatExpression(
        IrValue value,
        RepresentationEnvironment types,
        IReadOnlyDictionary<string, bool> localPaired)
    {
        var expression = ToExpression(value, types);
        return IsPairedFloatValue(value, localPaired)
            ? $"PPC_PsToScalarInline({expression})"
            : expression;
    }

    /// <summary>
    /// Materializes a packed pair when a paired-single consumer reads a scalar FPR.
    /// </summary>
    private static string ToPairedFloatExpression(
        IrValue value,
        RepresentationEnvironment types,
        IReadOnlyDictionary<string, bool> localPaired,
        bool fpscrNiDisabled = false)
    {
        var expression = ToExpression(value, types);
        return IsPairedFloatValue(value, localPaired)
            ? expression
            : fpscrNiDisabled
                ? $"PPC_PsFromScalarNoNiInline({expression})"
                : $"PPC_PsFromScalarInline({expression})";
    }

    private static string FprStorageExpression(string register)
    {
        var index = ParseFloatRegisterIndex(register);
        if (index is < 0 or >= 32)
            throw new InvalidOperationException($"Invalid FPR register '{register}'.");
        return _activeResidency is null
            ? $"ctx->fpr[{index}]"
            : _activeResidency.FprStorage(index, written: true);
    }

    private static string PairedAssignment(string destination, string expression, RepresentationEnvironment types)
    {
        if (IsFloatRegister(destination))
            return $"PpcSetPairedFprInline({FprStorageExpression(destination)}, {expression});";
        return $"{ToDestination(destination, types)} = {expression};";
    }

    private static bool TryGetInlineGuestThunkSpec(uint address, out InlineGuestThunkSpec spec)
    {
        var thunks = GuestSaveRestoreThunks.Current;
        if (TryGetInlineGuestThunkRangeSpec(address, thunks.SaveGpr, InlineGuestThunkKind.SaveGpr, out spec))
        {
            return true;
        }

        if (TryGetInlineGuestThunkRangeSpec(address, thunks.RestGpr, InlineGuestThunkKind.RestGpr, out spec))
        {
            return true;
        }

        if (TryGetInlineGuestThunkRangeSpec(address, thunks.SaveFpr, InlineGuestThunkKind.SaveFpr, out spec))
        {
            return true;
        }

        if (TryGetInlineGuestThunkRangeSpec(address, thunks.RestFpr, InlineGuestThunkKind.RestFpr, out spec))
        {
            return true;
        }

        spec = default;
        return false;
    }

    private static bool TryGetInlineGuestThunkRangeSpec(
        uint address,
        GuestSaveRestoreThunkRange? range,
        InlineGuestThunkKind kind,
        out InlineGuestThunkSpec spec)
    {
        if (range is null)
        {
            spec = default;
            return false;
        }

        var (baseAddress, startRegister, endRegister) = range;
        var rangeBytes = checked((uint)((endRegister - startRegister) * 4));
        if (address < baseAddress || address > (baseAddress + rangeBytes) || ((address - baseAddress) & 0x3) != 0)
        {
            spec = default;
            return false;
        }

        spec = new InlineGuestThunkSpec(
            kind,
            startRegister + (int)((address - baseAddress) / 4),
            address);
        return true;
    }

    private static string RequireLabel(string? label, Dictionary<string, string> labelNames, string fromLabel, string functionName)
    {
        if (string.IsNullOrWhiteSpace(label))
        {
            throw new InvalidOperationException($"Block '{fromLabel}' in '{functionName}' references an empty target label.");
        }

        if (!labelNames.TryGetValue(label, out var existing) || string.IsNullOrWhiteSpace(existing))
        {
            throw new InvalidOperationException($"Block '{fromLabel}' in '{functionName}' references missing target '{label}'.");
        }

        return existing;
    }

    private static string EscapeForCxxLiteral(string text) => CxxStringLiteralEscaping.Escape(text);
}
