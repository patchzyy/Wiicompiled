using System;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    private static string BinaryExpressionWithOperands(IrBinary bin, string left, string right)
    {
        if (bin.Op == "sext")
        {
            var widthBits = bin.Right.Constant.GetValueOrDefault(8);
            return widthBits switch
            {
                16 => $"(static_cast<int32_t>(static_cast<int16_t>({left})))",
                8 => $"(static_cast<int32_t>(static_cast<int8_t>({left})))",
                _ => $"(static_cast<int32_t>({left}))",
            };
        }

        return bin.Op switch
        {
            "add" => $"({left} + {right})",
            "sub" => $"({left} - {right})",
            "and" => $"({left} & {right})",
            "andc" => $"({left} & ~{right})",
            "or" => $"({left} | {right})",
            "orc" => $"({left} | ~{right})",
            "fdiv" => $"({left} / {right})",
            "nor" => $"~({left} | {right})",
            "nand" => $"~({left} & {right})",
            "eqv" => $"~({left} ^ {right})",
            "xor" => $"({left} ^ {right})",
            "shl" => $"({left} << {right})",
            "rotl" => $"PpcRotl32Inline(static_cast<uint32_t>({left}), static_cast<uint32_t>({right}))",
            "shr" => $"({left} >> {right})",
            "ppc_slw" => $"PPC_Slw(static_cast<uint32_t>({left}), static_cast<uint32_t>({right}))",
            "ppc_srw" => $"PPC_Srw(static_cast<uint32_t>({left}), static_cast<uint32_t>({right}))",
            "ppc_sraw" => $"PPC_Sraw(static_cast<uint32_t>({left}), static_cast<uint32_t>({right}))",
            "fabs" => $"std::fabs({left})",
            "fneg" => $"(-({left}))",
            "frsp" => $"static_cast<double>(PpcForceSingleValueInline({left}))",
            "frsp_noni" => $"static_cast<double>(static_cast<float>({left}))",
            "fsqrt" => $"std::sqrt({left})",
            "fcmp" => $"({left} - {right})",
            "fctiw" => $"PPC_Fctiw({left})",
            "fctiwz" => $"PPC_Fctiwz({left})",
            "fpr_low_word" => $"PPC_FprLowWordInline({left})",
            "sar" => $"(static_cast<int32_t>({left}) >> {right})",
            "mul" => $"({left} * {right})",
            "mulhwu" => $"(static_cast<uint32_t>((static_cast<uint64_t>({left}) * static_cast<uint64_t>({right})) >> 32))",
            "mulhw" => $"(static_cast<int32_t>((static_cast<int64_t>(static_cast<int32_t>({left})) * static_cast<int64_t>(static_cast<int32_t>({right}))) >> 32))",
            "divu" => $"PPC_Divwu(static_cast<uint32_t>({left}), static_cast<uint32_t>({right}))",
            "div" => $"PPC_Divw(static_cast<int32_t>({left}), static_cast<int32_t>({right}))",
            "sext16" => $"(static_cast<int32_t>(static_cast<int16_t>({left})))",
            "not" => $"~({left})",
            "sub_u" => $"(CompareUnsigned(static_cast<uint32_t>({left}), static_cast<uint32_t>({right})))",
            _ => throw new ArgumentOutOfRangeException(nameof(bin.Op), bin.Op, null)
        };
    }

    // Guest physical address window: MEM1 at 0x80000000 and MEM2 up to
    // 0x94000000. A negative IR immediate whose unsigned form lands here is a
    // code or data address that was only negative because IrValue carries a
    // signed int.
    private const uint GuestAddressWindowStart = 0x80000000u;
    private const uint GuestAddressWindowEnd = 0x94000000u;

    /// <summary>
    /// Formats an assignment RHS. Negative <c>int</c> immediates that are really guest addresses (return
    /// addresses, <c>lis/ori</c> constants) print as hex when the destination is an unsigned 32-bit guest
    /// register, for readability only; small signed immediates like <c>-16</c> stay decimal.
    /// </summary>
    private static string ToRegisterAssignmentExpression(
        string destination,
        IrValue value,
        RepresentationEnvironment types)
    {
        if (value.Kind == "const" &&
            value.Constant is { } constant &&
            constant < 0 &&
            IsUnsigned32BitGuestRegister(destination))
        {
            var unsigned = unchecked((uint)constant);
            if (unsigned is >= GuestAddressWindowStart and < GuestAddressWindowEnd)
            {
                return $"0x{unsigned:X8}u";
            }
        }

        return ToExpression(value, types);
    }

    private static bool IsUnsigned32BitGuestRegister(string name)
    {
        if (!IsCpuRegister(name))
        {
            return false;
        }

        var baseName = BaseRegister(name);
        if (baseName.Length > 1 && baseName[0] == 'r' &&
            int.TryParse(baseName.AsSpan(1), out var gpr) && gpr is >= 0 and < 32)
        {
            return true;
        }

        // CR field destinations lower to ordinary locals whose host type comes
        // from representation classification, so they are deliberately excluded.
        return baseName.Equals("lr", StringComparison.OrdinalIgnoreCase) ||
               baseName.Equals("ctr", StringComparison.OrdinalIgnoreCase);
    }

    private static string ToExpression(IrValue val, RepresentationEnvironment types)
    {
        if (val.Kind == "const" && val.Constant.HasValue)
        {
            return val.Constant.Value.ToString();
        }

        if (val.Kind != "register" || val.RegisterName == null)
        {
            return "0";
        }

        var name = val.RegisterName;
        if (IsCpuRegister(name))
        {
            return RegisterToReadExpression(name, types);
        }

        return name;
    }

    private static string RegisterToCtxRead(string name, RepresentationEnvironment types)
    {
        var baseName = BaseRegisterSpan(name);
        var residency = _activeResidency;

        if (baseName.Length > 1 && baseName[0] == 'r' && int.TryParse(baseName[1..], out var gpr) && gpr is >= 0 and < 32)
        {
            return residency is null ? CtxGprName(gpr) : residency.Gpr(gpr, written: false);
        }

        if (baseName.Length > 1 && baseName[0] == 'f' && int.TryParse(baseName[1..], out var fpr) && fpr is >= 0 and < 32)
        {
            return residency is null ? CtxFprScalarName(fpr) : residency.FprScalar(fpr, written: false);
        }

        if (baseName.Length == 3 && baseName.StartsWith("cr".AsSpan(), StringComparison.OrdinalIgnoreCase) && char.IsDigit(baseName[2]))
        {
            var crField = baseName[2] - '0';
            if (crField is >= 0 and <= 7)
            {
                var shift = (7 - crField) * 4;
                var crExpr = residency is null ? "ctx->cr" : residency.Cr(written: false);
                return $"(({crExpr} >> {shift}) & 0xF)";
            }
        }

        if (SpanEqualsIgnoreCase(baseName, "lr")) return "ctx->lr";
        if (SpanEqualsIgnoreCase(baseName, "ctr")) return residency is null ? "ctx->ctr" : residency.Ctr(written: false);
        if (SpanEqualsIgnoreCase(baseName, "cr")) return residency is null ? "ctx->cr" : residency.Cr(written: false);
        if (SpanEqualsIgnoreCase(baseName, "xer")) return residency is null ? "ctx->xer" : residency.Xer(written: false);
        if (SpanEqualsIgnoreCase(baseName, "srr0")) return "ctx->srr0";
        if (SpanEqualsIgnoreCase(baseName, "srr1")) return "ctx->srr1";
        if (SpanEqualsIgnoreCase(baseName, "msr")) return "ctx->msr";
        if (SpanEqualsIgnoreCase(baseName, "hid0")) return "ctx->hid0";
        if (SpanEqualsIgnoreCase(baseName, "hid1")) return "ctx->hid1";
        if (SpanEqualsIgnoreCase(baseName, "hid2")) return "ctx->hid2";
        if (baseName.StartsWith("gqr".AsSpan(), StringComparison.OrdinalIgnoreCase))
        {
            return int.TryParse(baseName[3..], out var gqrIdx) && gqrIdx is >= 0 and < 8
                ? $"ctx->gqr[{gqrIdx}]"
                : "0";
        }

        return "0";
    }

    private static string ToDestination(string name, RepresentationEnvironment types)
    {
        if (IsCpuRegister(name))
        {
            return RegisterToWriteExpression(name, types);
        }

        return name;
    }

    private static string RegisterToCtxWrite(string name, RepresentationEnvironment types)
    {
        var baseName = BaseRegisterSpan(name);
        var residency = _activeResidency;

        if (baseName.Length > 1 && baseName[0] == 'r' && int.TryParse(baseName[1..], out var gpr) && gpr is >= 0 and < 32)
        {
            return residency is null ? CtxGprName(gpr) : residency.Gpr(gpr, written: true);
        }

        if (baseName.Length > 1 && baseName[0] == 'f' && int.TryParse(baseName[1..], out var fpr) && fpr is >= 0 and < 32)
        {
            return residency is null ? CtxFprScalarName(fpr) : residency.FprScalar(fpr, written: true);
        }

        if (baseName.Length == 3 && baseName.StartsWith("cr".AsSpan(), StringComparison.OrdinalIgnoreCase) && char.IsDigit(baseName[2]))
        {
            return name;
        }

        if (SpanEqualsIgnoreCase(baseName, "lr")) return "ctx->lr";
        if (SpanEqualsIgnoreCase(baseName, "ctr")) return residency is null ? "ctx->ctr" : residency.Ctr(written: true);
        if (SpanEqualsIgnoreCase(baseName, "cr")) return residency is null ? "ctx->cr" : residency.Cr(written: true);
        if (SpanEqualsIgnoreCase(baseName, "xer")) return residency is null ? "ctx->xer" : residency.Xer(written: true);
        if (SpanEqualsIgnoreCase(baseName, "srr0")) return "ctx->srr0";
        if (SpanEqualsIgnoreCase(baseName, "srr1")) return "ctx->srr1";
        if (SpanEqualsIgnoreCase(baseName, "msr")) return "ctx->msr";
        if (SpanEqualsIgnoreCase(baseName, "hid0")) return "ctx->hid0";
        if (SpanEqualsIgnoreCase(baseName, "hid1")) return "ctx->hid1";
        if (SpanEqualsIgnoreCase(baseName, "hid2")) return "ctx->hid2";
        if (baseName.StartsWith("gqr".AsSpan(), StringComparison.OrdinalIgnoreCase))
        {
            return int.TryParse(baseName[3..], out var gqrIdx) && gqrIdx is >= 0 and < 8
                ? $"ctx->gqr[{gqrIdx}]"
                : name;
        }

        return name;
    }

    private static (int Mask, bool Set) CrFieldConditionTest(string condition)
    {
        if (EqualsIgnoreCase(condition, "bne")) return (2, false);
        if (EqualsIgnoreCase(condition, "beq")) return (2, true);
        if (EqualsIgnoreCase(condition, "bgt")) return (4, true);
        if (EqualsIgnoreCase(condition, "ble")) return (4, false);
        if (EqualsIgnoreCase(condition, "blt")) return (8, true);
        if (EqualsIgnoreCase(condition, "bge")) return (8, false);
        if (EqualsIgnoreCase(condition, "bso")) return (1, true);
        if (EqualsIgnoreCase(condition, "bns")) return (1, false);
        return (0, false);
    }

    private static bool EqualsIgnoreCase(string value, string candidate) =>
        string.Equals(value, candidate, StringComparison.OrdinalIgnoreCase);

    private static string ToCondition(string condition, string conditionRegister, RepresentationEnvironment types)
    {
        var conditionBase = BaseRegisterSpan(conditionRegister);
        if (conditionBase.Length == 3 &&
            conditionBase.StartsWith("cr".AsSpan(), StringComparison.OrdinalIgnoreCase) &&
            conditionBase[2] is >= '0' and <= '7')
        {
            var field = conditionBase[2] - '0';
            var fieldShift = (7 - field) * 4;
            var (mask, set) = CrFieldConditionTest(condition);
            if (mask != 0)
            {
                var shiftedMask = mask << fieldShift;
                var crExpr = _activeResidency is null
                    ? "ctx->cr"
                    : _activeResidency.Cr(written: false);
                return $"(({crExpr} & 0x{shiftedMask:X8}u) {(set ? "!=" : "==")} 0)";
            }
        }

        var reg = IsCpuRegister(conditionRegister)
            ? RegisterToReadExpression(conditionRegister, types)
            : ResidentRawCondition(conditionRegister);

        if (EqualsIgnoreCase(condition, "bdnz")) return $"({reg} != 0)";
        if (EqualsIgnoreCase(condition, "bdz")) return $"({reg} == 0)";
        if (EqualsIgnoreCase(condition, "bne")) return $"(({reg} & 0x2) == 0)";
        if (EqualsIgnoreCase(condition, "beq")) return $"(({reg} & 0x2) != 0)";
        if (EqualsIgnoreCase(condition, "bgt")) return $"(({reg} & 0x4) != 0)";
        if (EqualsIgnoreCase(condition, "blt")) return $"(({reg} & 0x8) != 0)";
        if (EqualsIgnoreCase(condition, "bge")) return $"(({reg} & 0x8) == 0)";
        if (EqualsIgnoreCase(condition, "ble")) return $"(({reg} & 0x4) == 0)";
        if (EqualsIgnoreCase(condition, "bso")) return $"(({reg} & 0x1) != 0)";
        if (EqualsIgnoreCase(condition, "bns")) return $"(({reg} & 0x1) == 0)";
        return reg;
    }

    /// <summary>
    /// Maps the raw branch-condition text <c>PpcLifter</c> builds for <c>bc</c>/<c>bcctr</c> onto resident
    /// locals. This is the only emitted register access that bypasses the expression layer, so missing a
    /// rewrite here means reading a stale architectural CR/CTR instead of the live local.
    /// </summary>
    private static string ResidentRawCondition(string conditionText)
    {
        var residency = _activeResidency;
        if (residency is null || string.IsNullOrEmpty(conditionText))
        {
            return conditionText;
        }

        var rewritten = conditionText;
        if (rewritten.Contains("ctx->ctr", StringComparison.Ordinal))
        {
            rewritten = rewritten.Replace("ctx->ctr", residency.Ctr(written: false), StringComparison.Ordinal);
        }

        if (rewritten.Contains("GetCRBit(ctx,", StringComparison.Ordinal))
        {
            rewritten = rewritten.Replace(
                "GetCRBit(ctx,",
                $"GetCRBitResident({residency.Cr(written: false)},",
                StringComparison.Ordinal);
        }

        return rewritten;
    }

    private static string Address(IrAddress addr, RepresentationEnvironment types)
    {
        var baseExpr = IsCpuRegister(addr.Base)
            ? RegisterToReadExpression(addr.Base, types)
            : addr.Base;

        return addr.Offset == 0
            ? baseExpr
            : $"({baseExpr} + {addr.Offset})";
    }

    private static string RemapAddress(
        IrAddress addr,
        RepresentationEnvironment types,
        IReadOnlyDictionary<string, uint> localConstants,
        LinkedAddressRemap? remap)
    {
        if (remap is not null &&
            (localConstants.TryGetValue(addr.Base, out var baseValue) ||
             localConstants.TryGetValue(GetRegisterBaseName(addr.Base), out baseValue)))
        {
            var effective = unchecked(baseValue + (uint)addr.Offset);
            var linkEnd = checked(remap.LinkBase + remap.CodeSize);
            if (effective >= remap.LinkBase && effective < linkEnd)
            {
                var relocated = checked(remap.GuestBase + (effective - remap.LinkBase));
                if (!remap.GuardAgainstResume)
                {
                    return $"0x{relocated:X8}u";
                }

                var runtime = Address(addr, types);
                return $"(static_cast<uint32_t>({runtime}) == 0x{effective:X8}u ? 0x{relocated:X8}u : static_cast<uint32_t>({runtime}))";
            }
        }

        return Address(addr, types);
    }

    private static string BaseRegister(string name)
    {
        var idx = name.IndexOf('_');
        return idx < 0 ? name : name[..idx];
    }

    /// <summary>
    /// Allocation-free <see cref="BaseRegister"/>. The expression layer only
    /// inspects the prefix, so the substring it used to build per access was
    /// pure garbage.
    /// </summary>
    private static ReadOnlySpan<char> BaseRegisterSpan(string name)
    {
        var idx = name.IndexOf('_');
        return idx < 0 ? name.AsSpan() : name.AsSpan(0, idx);
    }

    private static int Bits(int sizeBytes) => sizeBytes switch
    {
        1 => 8,
        2 => 16,
        8 => 64,
        _ => 32
    };

    private static string RegisterToReadExpression(string name, RepresentationEnvironment types)
    {
        return RegisterToCtxRead(name, types);
    }

    private static string RegisterToWriteExpression(string name, RepresentationEnvironment types)
    {
        return RegisterToCtxWrite(name, types);
    }
}
