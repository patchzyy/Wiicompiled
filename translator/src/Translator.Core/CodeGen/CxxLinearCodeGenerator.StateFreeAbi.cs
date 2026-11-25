using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using System.Text.RegularExpressions;
using Translator.Core.Analysis;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    /// <summary>
    /// Every pattern the state-free rewriter uses, built once. The static <c>Regex.*</c> helpers
    /// cache only 15 patterns by default; this rewriter has more than that and used to build a
    /// fresh pattern per register, thrashing the cache and re-parsing on every call.
    /// </summary>
    private static class StateFreePatterns
    {
        private const RegexOptions Options = RegexOptions.CultureInvariant | RegexOptions.Compiled;

        public static readonly Regex DifferentialBlock = new(
            @"(?s)\s*// MKW_STATE_FREE_DIFFERENTIAL_BEGIN.*?// MKW_STATE_FREE_DIFFERENTIAL_END\s*", Options);

        public static readonly Regex CtxGqrAccess = new(@"ctx->gqr\[(\d+)u?\]", Options);

        public static readonly Regex PsqInlineGqrOperand = new(
            @"PPC_Psq(?:L|St)(?:Stack|Resolved)?Inline<\d+u,\s*(\d+)u>\(ctx,", Options);

        public static readonly Regex CachedGprFlush = new(
            @"(?m)^\s*ctx->gpr\[\d+\]\s*=\s*cached_r\d+;\s*\r?\n?", Options);

        public static readonly Regex CachedSpecialFlush = new(
            @"(?m)^\s*ctx->(?:cr|xer|ctr|lr)\s*=\s*cached_(?:cr|xer|ctr|lr);\s*\r?\n?", Options);

        public static readonly Regex CachedFprFlush = new(
            @"(?m)^\s*ctx->fpr\[\d+\]\s*=\s*cached_f\d+;\s*\r?\n?", Options);

        public static readonly Regex LrStore = new(@"(?m)^\s*ctx->lr\s*=\s*[^;]+;\s*\r?\n?", Options);

        public static readonly Regex PsqLInline = new(
            @"PPC_PsqLInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqLStackInline = new(
            @"PPC_PsqLStackInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqStInline = new(
            @"PPC_PsqStInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqStStackInline = new(
            @"PPC_PsqStStackInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqLResolvedInline = new(
            @"PPC_PsqLResolvedInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqStResolvedInline = new(
            @"PPC_PsqStResolvedInline<(?<w>\d+)u,\s*(?<i>\d+)u>\(ctx,\s*", Options);

        public static readonly Regex PsqKnownInline = new(
            @"(PPC_Psq(?:L|St)Known(?:Resolved|Stack)?Inline<[^>]+>)\(ctx,\s*", Options);

        public static readonly Regex BareReturn = new(@"\breturn\s*;", Options);

        public static readonly Regex AggregateReturn = new(@"\breturn\s*\{", Options);

        public static readonly Regex ContextWord = new(@"\bctx\b", Options);

        public static readonly Regex BoundaryDirectCall = new(
            @"(?m)^(?<pad>\s*)InvokeDirectCpu<0x(?<address>[0-9A-Fa-f]{8})u>\(ctx\);\s*$", Options);

        public static readonly Regex BoundaryDirectCallAddress = new(
            @"\bInvokeDirectCpu<0x([0-9A-Fa-f]{8})u>\(ctx\);", Options);

        /// <summary>
        /// <c>uint32_t cached_&lt;name&gt;</c> declarations, found in a single scan (the old
        /// per-register pattern ending in <c>\b</c> matched the same complete identifiers).
        /// </summary>
        public static readonly Regex CachedIntDeclaration = new(@"\buint32_t\s+(cached_\w+)\b", Options);

        public static readonly Regex CachedFprDeclaration = new(@"\bPPC_FPR\s+(cached_\w+)\b", Options);
    }

    /// <summary>
    /// Names of the <c>cached_*</c> locals declared in one body, collected in a
    /// single pass. See <see cref="StateFreePatterns.CachedIntDeclaration"/>.
    /// </summary>
    private static HashSet<string> CollectStateFreeCachedDeclarations(string body)
    {
        var declared = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in StateFreePatterns.CachedIntDeclaration.Matches(body))
        {
            declared.Add(match.Groups[1].Value);
        }

        foreach (Match match in StateFreePatterns.CachedFprDeclaration.Matches(body))
        {
            declared.Add(match.Groups[1].Value);
        }

        return declared;
    }

    private static bool IsStateFreeCallSafe(GuestAbiContract contract) =>
        !contract.HasFullSynchronizationFence;

    private static bool StateFreeHasOutputs(GuestAbiContract contract) =>
        contract.GprPossibleWriteMask != 0 || contract.FprPossibleWriteMask != 0 ||
        contract.CrPossibleWriteMask != 0 || contract.MayWriteXer ||
        contract.MayWriteCtr || contract.MayWriteLr || contract.MayWriteFpscr ||
        contract.GqrPossibleWriteMask != 0 || contract.HidPossibleWriteMask != 0;

    private static int StateFreeOutputCount(GuestAbiContract contract) =>
        System.Numerics.BitOperations.PopCount(contract.GprPossibleWriteMask) +
        System.Numerics.BitOperations.PopCount(contract.FprPossibleWriteMask) +
        (contract.CrPossibleWriteMask != 0 ? 1 : 0) +
        (contract.MayWriteXer ? 1 : 0) + (contract.MayWriteCtr ? 1 : 0) +
        (contract.MayWriteLr ? 1 : 0) + (contract.MayWriteFpscr ? 1 : 0) +
        System.Numerics.BitOperations.PopCount(contract.GqrPossibleWriteMask) +
        System.Numerics.BitOperations.PopCount(contract.HidPossibleWriteMask);

    private static uint StateFreeGprInputMask(GuestAbiContract contract) =>
        contract.GprReadBeforeWriteMask;

    private static int StateFreeInputCount(GuestAbiContract contract) =>
        System.Numerics.BitOperations.PopCount(StateFreeGprInputMask(contract)) +
        // Scalar writes preserve PS1, so every possible FPR output also needs
        // the incoming packed value at the native boundary.
        System.Numerics.BitOperations.PopCount(
            contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) +
        (contract.CrReadBeforeWriteMask != 0 || contract.CrPossibleWriteMask != 0 ? 1 : 0) +
        (contract.ReadsXerBeforeWrite || contract.MayWriteXer ? 1 : 0) +
        (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr ? 1 : 0) +
        (contract.ReadsLrBeforeWrite || contract.MayWriteLr ? 1 : 0) +
        (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr ? 1 : 0) +
        System.Numerics.BitOperations.PopCount(
            (uint)(contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask)) +
        System.Numerics.BitOperations.PopCount(
            (uint)(contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask));

    private static bool StateFreeFitsNativeRegisterBudget(GuestAbiContract contract) =>
        StateFreeInputCount(contract) <= 4 && StateFreeOutputCount(contract) <= 2;

    // Do not use Clang's __regcall here: a state-free callee using host r12 as scratch once collided
    // with a caller that kept the guest-memory page table live in r12, crashing at address 0x4808. The
    // platform ABI avoids that until regions can be fused before host register allocation.
    private static string StateFreeCallingConvention(GuestAbiContract contract) => string.Empty;

    private static string StateFreeResultType(uint address) => $"MkwStateFreeResult_{address:X8}";

    private static string StateFreeAvailabilityCondition(
        uint root,
        IReadOnlyDictionary<uint, GuestAbiContract> contracts,
        IReadOnlyDictionary<uint, string> symbols)
    {
        var region = new SortedSet<uint>();
        void Visit(uint address)
        {
            if (!region.Add(address) || !contracts.TryGetValue(address, out var contract)) return;
            foreach (var target in contract.DirectCallTargets)
                if (symbols.ContainsKey(target)) Visit(target);
        }
        Visit(root);
        // Ordinary no-inline native calls: on Windows x64, over four argument slots spill to the stack
        // and over two results need an sret object, defeating register residency (first hit in the
        // THP-heavy 0x80054460 region, 36 inputs / 31 outputs). Enable only when the whole transitive
        // region fits the native register ABI; larger regions need region fusion, not a giant prototype.
        if (region.Any(address =>
                !contracts.TryGetValue(address, out var contract) ||
                !StateFreeFitsNativeRegisterBudget(contract)))
            return "false";
        // Base shards are shared by base and Retro Rewind; profile-neutral dispatch can still pick a mod
        // translation for any descendant even with compile-time-constant call traits. Every member must
        // resolve to the base body before skipping CpuContext dispatch, or the region silently drops a
        // Retro Rewind override and corrupts guest state.
        return $"MkwStateFreeAbiEnabled(0x{root:X8}u) && " + string.Join(" && ", region.Select(static address =>
            $"KnownTranslatedCpuCall<0x{address:X8}u>::kAvailable && " +
            $"!KnownTranslatedCpuCall<0x{address:X8}u>::kMustRemainDynamicallyDispatchable && " +
            $"IsBaseTranslatedCpuTargetActive<0x{address:X8}u>()"));
    }

    private static string StateFreeReturnType(uint address, GuestAbiContract contract) =>
        StateFreeOutputCount(contract) switch
        {
            0 => "void",
            1 => "uint64_t",
            2 => "MkwStateFreeResult2",
            _ => StateFreeResultType(address)
        };

    private static void AppendStateFreeResultDeclaration(StringBuilder sb, uint address, GuestAbiContract contract)
    {
        var outputCount = StateFreeOutputCount(contract);
        if (outputCount <= 2) return;
        sb.AppendLine($"#ifndef MKW_STATE_FREE_RESULT_{address:X8}_DEFINED");
        sb.AppendLine($"#define MKW_STATE_FREE_RESULT_{address:X8}_DEFINED 1");
        sb.AppendLine($"struct {StateFreeResultType(address)} {{");
        for (var gpr = 0; gpr < 32; ++gpr)
            if ((contract.GprPossibleWriteMask & (1u << gpr)) != 0) sb.AppendLine($"    uint32_t r{gpr};");
        for (var fpr = 0; fpr < 32; ++fpr)
            if ((contract.FprPossibleWriteMask & (1u << fpr)) != 0) sb.AppendLine($"    PPC_FPR f{fpr};");
        if (contract.CrPossibleWriteMask != 0) sb.AppendLine("    uint32_t cr;");
        if (contract.MayWriteXer) sb.AppendLine("    uint32_t xer;");
        if (contract.MayWriteCtr) sb.AppendLine("    uint32_t ctr;");
        if (contract.MayWriteLr) sb.AppendLine("    uint32_t lr;");
        if (contract.MayWriteFpscr) sb.AppendLine("    uint32_t fpscr;");
        for (var gqr = 0; gqr < 8; ++gqr)
            if ((contract.GqrPossibleWriteMask & (1 << gqr)) != 0) sb.AppendLine($"    uint32_t gqr{gqr};");
        for (var hid = 0; hid < 3; ++hid)
            if ((contract.HidPossibleWriteMask & (1 << hid)) != 0) sb.AppendLine($"    uint32_t hid{hid};");
        sb.AppendLine("};");
        sb.AppendLine("#endif");
    }

    private static string StateFreeParameterList(GuestAbiContract contract, bool includeNames)
    {
        var parameters = new List<string>();
        var gprInputs = StateFreeGprInputMask(contract);
        for (var gpr = 0; gpr < 32; ++gpr)
        {
            if ((gprInputs & (1u << gpr)) != 0)
                parameters.Add(includeNames ? $"uint32_t native_r{gpr}" : "uint32_t");
        }
        for (var fpr = 0; fpr < 32; ++fpr)
        {
            // Scalar writes preserve PS1, so the previous architectural FPR is
            // conservatively an input until representation ownership proves a
            // complete packed overwrite.
            if (((contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) & (1u << fpr)) != 0)
                parameters.Add(includeNames ? $"PPC_FPR native_f{fpr}" : "PPC_FPR");
        }
        if (contract.CrReadBeforeWriteMask != 0 || contract.CrPossibleWriteMask != 0)
            parameters.Add(includeNames ? "uint32_t native_cr" : "uint32_t");
        if (contract.ReadsXerBeforeWrite || contract.MayWriteXer)
            parameters.Add(includeNames ? "uint32_t native_xer" : "uint32_t");
        if (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr)
            parameters.Add(includeNames ? "uint32_t native_ctr" : "uint32_t");
        if (contract.ReadsLrBeforeWrite || contract.MayWriteLr)
            parameters.Add(includeNames ? "uint32_t native_lr" : "uint32_t");
        if (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr)
            parameters.Add(includeNames ? "uint32_t native_fpscr" : "uint32_t");
        for (var gqr = 0; gqr < 8; ++gqr)
            if (((contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask) & (1 << gqr)) != 0)
                parameters.Add(includeNames ? $"uint32_t native_gqr{gqr}" : "uint32_t");
        for (var hid = 0; hid < 3; ++hid)
            if (((contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask) & (1 << hid)) != 0)
                parameters.Add(includeNames ? $"uint32_t native_hid{hid}" : "uint32_t");
        return string.Join(", ", parameters);
    }

    private static string StateFreeCallArguments(GuestAbiContract contract, uint? callSiteLr = null)
    {
        var arguments = new List<string>();
        var gprInputs = StateFreeGprInputMask(contract);
        for (var gpr = 0; gpr < 32; ++gpr)
        {
            if ((gprInputs & (1u << gpr)) != 0)
                arguments.Add($"ctx->gpr[{gpr}]");
        }
        for (var fpr = 0; fpr < 32; ++fpr)
            if (((contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) & (1u << fpr)) != 0)
                arguments.Add($"ctx->fpr[{fpr}]");
        if (contract.CrReadBeforeWriteMask != 0 || contract.CrPossibleWriteMask != 0) arguments.Add("ctx->cr");
        if (contract.ReadsXerBeforeWrite || contract.MayWriteXer) arguments.Add("ctx->xer");
        if (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr) arguments.Add("ctx->ctr");
        if (contract.ReadsLrBeforeWrite || contract.MayWriteLr)
            arguments.Add(callSiteLr.HasValue ? $"0x{callSiteLr.Value:X8}u" : "ctx->lr");
        if (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr) arguments.Add("ctx->fpscr");
        for (var gqr = 0; gqr < 8; ++gqr)
            if (((contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask) & (1 << gqr)) != 0)
                arguments.Add($"ctx->gqr[{gqr}]");
        for (var hid = 0; hid < 3; ++hid)
            if (((contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask) & (1 << hid)) != 0)
                arguments.Add($"ctx->hid{hid}");
        return string.Join(", ", arguments);
    }

    // Resident marshalling: pass/receive the resident locals directly instead of naming ctx, which would
    // force a full CpuContext spill/reload around every call even though residency already holds the
    // values in host registers. The slow arm still flushes/reloads CpuContext; see EmitResidentStateFreeCall.

    private static string StateFreeResidentCallArguments(
        RegisterResidency residency, GuestAbiContract contract, uint? callSiteLr)
    {
        var arguments = new List<string>();
        var gprInputs = StateFreeGprInputMask(contract);
        for (var gpr = 0; gpr < 32; ++gpr)
        {
            if ((gprInputs & (1u << gpr)) != 0)
                arguments.Add(residency.Gpr(gpr, written: false));
        }
        for (var fpr = 0; fpr < 32; ++fpr)
            if (((contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) & (1u << fpr)) != 0)
                arguments.Add(residency.FprStorage(fpr, written: false));
        if (contract.CrReadBeforeWriteMask != 0 || contract.CrPossibleWriteMask != 0)
            arguments.Add(residency.Cr(written: false));
        if (contract.ReadsXerBeforeWrite || contract.MayWriteXer) arguments.Add(residency.Xer(written: false));
        if (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr) arguments.Add(residency.Ctr(written: false));
        // LR, FPSCR, the GQRs and the HID registers are never localized, so they
        // keep their architectural spelling exactly as the ctx form uses.
        if (contract.ReadsLrBeforeWrite || contract.MayWriteLr)
            arguments.Add(callSiteLr.HasValue ? $"0x{callSiteLr.Value:X8}u" : "ctx->lr");
        if (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr) arguments.Add("ctx->fpscr");
        for (var gqr = 0; gqr < 8; ++gqr)
            if (((contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask) & (1 << gqr)) != 0)
                arguments.Add($"ctx->gqr[{gqr}]");
        for (var hid = 0; hid < 3; ++hid)
            if (((contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask) & (1 << hid)) != 0)
                arguments.Add($"ctx->hid{hid}");
        return string.Join(", ", arguments);
    }

    /// <summary>
    /// Result unpacking into the resident locals. Field/value order must match
    /// <see cref="StateFreeReturnExpression"/> exactly, since this is a positional carrier for the
    /// callee's packed one- and two-output shapes.
    /// </summary>
    private static void AppendStateFreeResidentResultStores(
        StringBuilder sb, string pad, RegisterResidency residency,
        GuestAbiContract contract, string resultName)
    {
        var outputCount = StateFreeOutputCount(contract);
        var resultIndex = 0;
        string Value(string field) => outputCount switch
        {
            1 => resultName,
            2 => $"{resultName}[{resultIndex++}]",
            _ => $"{resultName}.{field}"
        };
        for (var gpr = 0; gpr < 32; ++gpr)
            if ((contract.GprPossibleWriteMask & (1u << gpr)) != 0)
            {
                var value = Value($"r{gpr}");
                sb.AppendLine($"{pad}{residency.Gpr(gpr, written: true)} = static_cast<uint32_t>({value});");
            }
        for (var fpr = 0; fpr < 32; ++fpr)
            if ((contract.FprPossibleWriteMask & (1u << fpr)) != 0)
            {
                var value = Value($"f{fpr}");
                if (outputCount > 2) value += ".raw";
                sb.AppendLine($"{pad}{residency.FprStorage(fpr, written: true)}.raw = static_cast<uint64_t>({value});");
            }
        if (contract.CrPossibleWriteMask != 0)
            sb.AppendLine($"{pad}{residency.Cr(written: true)} = static_cast<uint32_t>({Value("cr")});");
        if (contract.MayWriteXer)
            sb.AppendLine($"{pad}{residency.Xer(written: true)} = static_cast<uint32_t>({Value("xer")});");
        if (contract.MayWriteCtr)
            sb.AppendLine($"{pad}{residency.Ctr(written: true)} = static_cast<uint32_t>({Value("ctr")});");
        if (contract.MayWriteLr) sb.AppendLine($"{pad}ctx->lr = static_cast<uint32_t>({Value("lr")});");
        if (contract.MayWriteFpscr) sb.AppendLine($"{pad}ctx->fpscr = static_cast<uint32_t>({Value("fpscr")});");
        for (var gqr = 0; gqr < 8; ++gqr)
            if ((contract.GqrPossibleWriteMask & (1 << gqr)) != 0)
                sb.AppendLine($"{pad}ctx->gqr[{gqr}] = static_cast<uint32_t>({Value($"gqr{gqr}")});");
        for (var hid = 0; hid < 3; ++hid)
            if ((contract.HidPossibleWriteMask & (1 << hid)) != 0)
                sb.AppendLine($"{pad}ctx->hid{hid} = static_cast<uint32_t>({Value($"hid{hid}")});");
    }

    /// <summary>
    /// Emits one state-free call site whose fast arm marshals through the resident locals.
    /// <para>The <c>InvokeDirectCpu</c> fallback arm is live code, not dead code, since a mod overlay
    /// can replace any region member after this body was emitted, so it carries its own full
    /// flush/reload pair; only the fast arm skips the CpuContext boundary.</para>
    /// </summary>
    private static void EmitResidentStateFreeCall(
        StringBuilder sb,
        string pad,
        int bufferBaseLength,
        uint address,
        string symbol,
        GuestAbiContract contract,
        uint? callSiteLr,
        IReadOnlyDictionary<uint, GuestAbiContract> stateFreeAbiContracts,
        IReadOnlyDictionary<uint, string> stateFreeCallSymbols)
    {
        var residency = _activeResidency ?? throw new InvalidOperationException(
            $"Resident state-free marshalling requested for 0x{address:X8} without an active residency.");

        AppendFlush(sb, pad, ResidencyBoundarySync.StateFreeResidentOuter);
        // The fast arm's result stores and the slow arm's flush/reload are alternatives: each starts from
        // the state here and the dirty set afterwards is their join.
        residency.BeginAlternatives();
        var inner = pad + "    ";
        sb.AppendLine($"{pad}if ({StateFreeAvailabilityCondition(address, stateFreeAbiContracts, stateFreeCallSymbols)}) {{");
        var arguments = StateFreeResidentCallArguments(residency, contract, callSiteLr);
        if (StateFreeHasOutputs(contract))
        {
            // Uniquified by the call site's position in the translation unit,
            // exactly like the CpuContext form.
            var resultName = $"state_free_result_{address:X8}_{bufferBaseLength + sb.Length:X}";
            sb.AppendLine($"{inner}const auto {resultName} = {symbol}({arguments});");
            AppendStateFreeResidentResultStores(sb, inner, residency, contract, resultName);
        }
        else
        {
            sb.AppendLine($"{inner}{symbol}({arguments});");
        }
        sb.AppendLine($"{pad}}} else {{");
        residency.NextAlternative();
        AppendFlush(sb, inner, ResidencyBoundarySync.Full);
        RecordEmittedDirectCallTarget(address);
        sb.AppendLine($"{inner}InvokeDirectCpu<0x{address:X8}u>(ctx);");
        AppendReload(sb, inner, ResidencyBoundarySync.Full);
        residency.EndAlternatives();
        sb.AppendLine($"{pad}}}");
    }

    private static void AppendStateFreeResultStores(
        StringBuilder sb, string pad, uint address, GuestAbiContract contract, string resultName)
    {
        var outputCount = StateFreeOutputCount(contract);
        var resultIndex = 0;
        string Value(string field) => outputCount switch
        {
            1 => resultName,
            2 => $"{resultName}[{resultIndex++}]",
            _ => $"{resultName}.{field}"
        };
        for (var gpr = 0; gpr < 32; ++gpr)
            if ((contract.GprPossibleWriteMask & (1u << gpr)) != 0)
            {
                var value = Value($"r{gpr}");
                sb.AppendLine($"{pad}ctx->gpr[{gpr}] = static_cast<uint32_t>({value});");
            }
        for (var fpr = 0; fpr < 32; ++fpr)
            if ((contract.FprPossibleWriteMask & (1u << fpr)) != 0)
            {
                var value = Value($"f{fpr}");
                if (outputCount > 2) value += ".raw";
                sb.AppendLine($"{pad}ctx->fpr[{fpr}].raw = static_cast<uint64_t>({value});");
            }
        if (contract.CrPossibleWriteMask != 0) sb.AppendLine($"{pad}ctx->cr = static_cast<uint32_t>({Value("cr")});");
        if (contract.MayWriteXer) sb.AppendLine($"{pad}ctx->xer = static_cast<uint32_t>({Value("xer")});");
        if (contract.MayWriteCtr) sb.AppendLine($"{pad}ctx->ctr = static_cast<uint32_t>({Value("ctr")});");
        if (contract.MayWriteLr) sb.AppendLine($"{pad}ctx->lr = static_cast<uint32_t>({Value("lr")});");
        if (contract.MayWriteFpscr) sb.AppendLine($"{pad}ctx->fpscr = static_cast<uint32_t>({Value("fpscr")});");
        for (var gqr = 0; gqr < 8; ++gqr)
            if ((contract.GqrPossibleWriteMask & (1 << gqr)) != 0) sb.AppendLine($"{pad}ctx->gqr[{gqr}] = static_cast<uint32_t>({Value($"gqr{gqr}")});");
        for (var hid = 0; hid < 3; ++hid)
            if ((contract.HidPossibleWriteMask & (1 << hid)) != 0) sb.AppendLine($"{pad}ctx->hid{hid} = static_cast<uint32_t>({Value($"hid{hid}")});");
    }

    private static string StateFreeReturnExpression(GuestAbiContract contract, string body)
    {
        var values = new List<string>();
        var packed = StateFreeOutputCount(contract) <= 2;
        var declared = CollectStateFreeCachedDeclarations(body);
        for (var gpr = 0; gpr < 32; ++gpr)
            if ((contract.GprPossibleWriteMask & (1u << gpr)) != 0)
            {
                var cached = $"cached_r{gpr}";
                var value = declared.Contains(cached) ? cached : $"native_r{gpr}";
                values.Add(packed ? $"static_cast<uint64_t>({value})" : value);
            }
        for (var fpr = 0; fpr < 32; ++fpr)
            if ((contract.FprPossibleWriteMask & (1u << fpr)) != 0)
            {
                var cached = $"cached_f{fpr}";
                var value = declared.Contains(cached) ? cached : $"native_f{fpr}";
                values.Add(packed ? $"{value}.raw" : value);
            }
        if (contract.CrPossibleWriteMask != 0)
        {
            var value = declared.Contains("cached_cr") ? "cached_cr" : "native_cr";
            values.Add(packed ? $"static_cast<uint64_t>({value})" : value);
        }
        if (contract.MayWriteXer)
        {
            var value = declared.Contains("cached_xer") ? "cached_xer" : "native_xer";
            values.Add(packed ? $"static_cast<uint64_t>({value})" : value);
        }
        if (contract.MayWriteCtr)
        {
            var value = declared.Contains("cached_ctr") ? "cached_ctr" : "native_ctr";
            values.Add(packed ? $"static_cast<uint64_t>({value})" : value);
        }
        if (contract.MayWriteLr) values.Add(packed ? "static_cast<uint64_t>(native_lr)" : "native_lr");
        if (contract.MayWriteFpscr) values.Add(packed ? "static_cast<uint64_t>(native_fpscr)" : "native_fpscr");
        for (var gqr = 0; gqr < 8; ++gqr)
            if ((contract.GqrPossibleWriteMask & (1 << gqr)) != 0)
                values.Add(packed ? $"static_cast<uint64_t>(native_gqr{gqr})" : $"native_gqr{gqr}");
        for (var hid = 0; hid < 3; ++hid)
            if ((contract.HidPossibleWriteMask & (1 << hid)) != 0)
                values.Add(packed ? $"static_cast<uint64_t>(native_hid{hid})" : $"native_hid{hid}");
        return values.Count == 1
            ? $"return {values[0]};"
            : $"return {{ {string.Join(", ", values)} }};";
    }

    private static string RemoveStateFreeDirectCallFallbacks(string body)
    {
        const string prefix = "if (MkwStateFreeAbiEnabled(";
        var search = 0;
        while ((search = body.IndexOf(prefix, search, StringComparison.Ordinal)) >= 0)
        {
            var open = body.IndexOf('{', search);
            if (open < 0) break;
            var trueClose = FindMatchingBrace(body, open);
            if (trueClose < 0) break;
            var cursor = trueClose + 1;
            while (cursor < body.Length && char.IsWhiteSpace(body[cursor])) ++cursor;
            if (!body.AsSpan(cursor).StartsWith("else", StringComparison.Ordinal))
            {
                search = trueClose + 1;
                continue;
            }
            cursor += 4;
            while (cursor < body.Length && char.IsWhiteSpace(body[cursor])) ++cursor;
            if (cursor >= body.Length || body[cursor] != '{')
            {
                search = trueClose + 1;
                continue;
            }
            var falseClose = FindMatchingBrace(body, cursor);
            if (falseClose < 0) break;
            var fastBody = body[(open + 1)..trueClose];
            body = body[..search] + fastBody + body[(falseClose + 1)..];
            search += fastBody.Length;
        }
        return body;

        static int FindMatchingBrace(string text, int open)
        {
            var depth = 0;
            for (var index = open; index < text.Length; ++index)
            {
                if (text[index] == '{') ++depth;
                else if (text[index] == '}' && --depth == 0) return index;
            }
            return -1;
        }
    }

    private static string ExtractFunctionBody(string code, string implementationName)
    {
        var declaration = FunctionDefinitionSignature(implementationName);
        var declarationStart = code.IndexOf(declaration, StringComparison.Ordinal);
        if (declarationStart < 0)
            throw new InvalidOperationException($"Could not find ordinary body for {implementationName}.");
        var openBrace = code.IndexOf('{', declarationStart + declaration.Length);
        if (openBrace < 0)
            throw new InvalidOperationException($"Could not find opening brace for {implementationName}.");
        var closeBrace = FindFunctionCloseBrace(code, openBrace, implementationName);
        return code[openBrace..(closeBrace + 1)];
    }

    private static string ReplaceFunctionBody(string code, string implementationName, string replacementBody)
    {
        var declaration = FunctionDefinitionSignature(implementationName);
        var declarationStart = code.IndexOf(declaration, StringComparison.Ordinal);
        if (declarationStart < 0)
            throw new InvalidOperationException($"Could not find cached body for {implementationName}.");
        var openBrace = code.IndexOf('{', declarationStart + declaration.Length);
        if (openBrace < 0)
            throw new InvalidOperationException($"Could not find cached opening brace for {implementationName}.");
        var closeBrace = FindFunctionCloseBrace(code, openBrace, implementationName);
        return code[..openBrace] + replacementBody + code[(closeBrace + 1)..];
    }

    private static int FindFunctionCloseBrace(string code, int openBrace, string implementationName)
    {
        var depth = 0;
        for (var index = openBrace; index < code.Length; ++index)
        {
            if (code[index] == '{') ++depth;
            else if (code[index] == '}' && --depth == 0) return index;
        }
        throw new InvalidOperationException($"Found unterminated body for {implementationName}.");
    }

    /// <summary>
    /// Appends one explicit-state clone. <paramref name="facts"/> reports the interface the clone
    /// actually published, since the contract is refined while the body is rewritten (body-derived
    /// GQR inputs) and the caller cannot predict it upfront.
    /// </summary>
    private static string AppendStateFreeLeafVariant(
        string code,
        string implementationName,
        string stateFreeSymbol,
        uint entryPoint,
        GuestAbiContract contract,
        out GuestStateFreeEmissionFacts facts,
        bool preferInline = false,
        IReadOnlySet<uint>? boundaryContextTargets = null,
        IReadOnlyDictionary<uint, GuestAbiContract>? guestAbiContracts = null)
    {
        if (!IsStateFreeCallSafe(contract))
            throw new InvalidOperationException($"State-free ABI at 0x{entryPoint:X8} crosses a full-context boundary.");

        var declaration = FunctionDefinitionSignature(implementationName);
        var declarationStart = code.IndexOf(declaration, StringComparison.Ordinal);
        if (declarationStart < 0)
            throw new InvalidOperationException($"State-free ABI could not find the leaf body for 0x{entryPoint:X8}.");
        var openBrace = code.IndexOf('{', declarationStart + declaration.Length);
        if (openBrace < 0)
            throw new InvalidOperationException($"State-free ABI found no body for 0x{entryPoint:X8}.");

        var depth = 0;
        var closeBrace = -1;
        for (var index = openBrace; index < code.Length; ++index)
        {
            if (code[index] == '{') ++depth;
            else if (code[index] == '}' && --depth == 0)
            {
                closeBrace = index;
                break;
            }
        }
        if (closeBrace < 0)
            throw new InvalidOperationException($"State-free ABI found an unterminated body for 0x{entryPoint:X8}.");

        var body = code[(openBrace + 1)..closeBrace];
        body = StateFreePatterns.DifferentialBlock.Replace(body, Environment.NewLine);
        body = RemoveStateFreeDirectCallFallbacks(body);

        // One scan per shape instead of one rebuilt pattern per GQR. The
        // captured index text is compared literally, so a hypothetical
        // zero-padded spelling stays a miss exactly as it was before.
        var referencedGqrIndices = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in StateFreePatterns.CtxGqrAccess.Matches(body))
        {
            referencedGqrIndices.Add(match.Groups[1].Value);
        }

        foreach (Match match in StateFreePatterns.PsqInlineGqrOperand.Matches(body))
        {
            referencedGqrIndices.Add(match.Groups[1].Value);
        }

        byte bodyGqrInputs = 0;
        for (var gqr = 0; gqr < 8; ++gqr)
        {
            if (referencedGqrIndices.Contains(gqr.ToString(System.Globalization.CultureInfo.InvariantCulture)))
                bodyGqrInputs |= (byte)(1 << gqr);
        }
        contract = contract with
        {
            GqrReadBeforeWriteMask = (byte)(contract.GqrReadBeforeWriteMask | bodyGqrInputs)
        };

        // Public PPC ABI flushes are replaced by explicit native results.
        body = StateFreePatterns.CachedGprFlush.Replace(body, string.Empty);
        body = StateFreePatterns.CachedSpecialFlush.Replace(body, string.Empty);
        body = StateFreePatterns.CachedFprFlush.Replace(body, string.Empty);

        // The per-register replacements below are order sensitive and stay a
        // sequential chain. These guards only skip scans that provably cannot
        // match: nothing in the chain ever introduces one of these markers.
        var containsCachedGpr = body.Contains("cached_r", StringComparison.Ordinal);
        var containsCtxGpr = body.Contains("ctx->gpr[", StringComparison.Ordinal);
        var containsCachedFpr = body.Contains("cached_f", StringComparison.Ordinal);
        var containsCtxFpr = body.Contains("ctx->fpr[", StringComparison.Ordinal);
        for (var gpr = 0; gpr < 32 && (containsCachedGpr || containsCtxGpr); ++gpr)
        {
            // A function can have several return blocks, and later formatting
            // may place a flush beside a label instead of at the start of a
            // physical line. Remove the exact architectural write wherever it
            // occurs; the cached value remains authoritative inside this clone.
            if (containsCachedGpr && containsCtxGpr)
                body = body.Replace($"ctx->gpr[{gpr}] = cached_r{gpr};", string.Empty, StringComparison.Ordinal);
            if ((StateFreeGprInputMask(contract) & (1u << gpr)) != 0)
            {
                if (containsCtxGpr)
                    body = body.Replace($"ctx->gpr[{gpr}]", $"native_r{gpr}", StringComparison.Ordinal);
            }
            else if (containsCachedGpr && containsCtxGpr)
            {
                body = body.Replace(
                    $"uint32_t cached_r{gpr} = ctx->gpr[{gpr}];",
                    $"uint32_t cached_r{gpr}{{}};",
                    StringComparison.Ordinal);
            }
        }
        for (var fpr = 0; fpr < 32 && (containsCachedFpr || containsCtxFpr); ++fpr)
        {
            if (containsCachedFpr && containsCtxFpr)
                body = body.Replace($"ctx->fpr[{fpr}] = cached_f{fpr};", string.Empty, StringComparison.Ordinal);
            if (((contract.FprReadBeforeWriteMask | contract.FprPossibleWriteMask) & (1u << fpr)) != 0)
            {
                if (containsCtxFpr)
                    body = body.Replace($"ctx->fpr[{fpr}]", $"native_f{fpr}", StringComparison.Ordinal);
            }
            else if (containsCachedFpr && containsCtxFpr)
            {
                body = body.Replace(
                    $"PPC_FPR cached_f{fpr} = ctx->fpr[{fpr}];",
                    $"PPC_FPR cached_f{fpr}{{}};",
                    StringComparison.Ordinal);
            }
        }
        body = body.Replace("ctx->cr = cached_cr;", string.Empty, StringComparison.Ordinal);
        body = body.Replace("ctx->xer = cached_xer;", string.Empty, StringComparison.Ordinal);
        body = body.Replace("ctx->ctr = cached_ctr;", string.Empty, StringComparison.Ordinal);
        body = body.Replace("ctx->lr = cached_lr;", string.Empty, StringComparison.Ordinal);
        if (contract.CrReadBeforeWriteMask != 0 || contract.CrPossibleWriteMask != 0)
            body = body.Replace("ctx->cr", "native_cr", StringComparison.Ordinal);
        if (contract.ReadsXerBeforeWrite || contract.MayWriteXer)
            body = body.Replace("ctx->xer", "native_xer", StringComparison.Ordinal);
        if (contract.ReadsCtrBeforeWrite || contract.MayWriteCtr)
            body = body.Replace("ctx->ctr", "native_ctr", StringComparison.Ordinal);
        if (contract.ReadsLrBeforeWrite || contract.MayWriteLr)
            body = body.Replace("ctx->lr", "native_lr", StringComparison.Ordinal);
        else
            body = StateFreePatterns.LrStore.Replace(body, string.Empty);
        if (contract.ReadsFpscrBeforeWrite || contract.MayWriteFpscr)
            body = body.Replace("ctx->fpscr", "native_fpscr", StringComparison.Ordinal);
        for (var gqr = 0; gqr < 8; ++gqr)
            if (((contract.GqrReadBeforeWriteMask | contract.GqrPossibleWriteMask) & (1 << gqr)) != 0)
            {
                body = body.Replace($"ctx->gqr[{gqr}]", $"native_gqr{gqr}", StringComparison.Ordinal);
                body = body.Replace($"ctx->gqr[{gqr}u]", $"native_gqr{gqr}", StringComparison.Ordinal);
            }
        for (var hid = 0; hid < 3; ++hid)
            if (((contract.HidReadBeforeWriteMask | contract.HidPossibleWriteMask) & (1 << hid)) != 0)
                body = body.Replace($"ctx->hid{hid}", $"native_hid{hid}", StringComparison.Ordinal);

        if (boundaryContextTargets is { Count: > 0 })
        {
            var boundaryContracts = guestAbiContracts ?? throw new InvalidOperationException(
                $"State-free boundary contracts are missing for 0x{entryPoint:X8}.");
            body = EnsureStateFreeBoundarySpecialLocals(
                body, entryPoint, boundaryContextTargets, boundaryContracts);
            body = RewriteStateFreeBoundaryCalls(
                body, entryPoint, boundaryContextTargets,
                boundaryContracts);
        }

        // Generic PSQ helpers need only the selected GQR value. Keep that value
        // in the native signature instead of reintroducing CpuContext solely to
        // read one array element inside an otherwise direct memory operation.
        body = StateFreePatterns.PsqLInline.Replace(body,
            static match => $"PPC_PsqLStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u, false>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqLStackInline.Replace(body,
            static match => $"PPC_PsqLStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u, true>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqStInline.Replace(body,
            static match => $"PPC_PsqStStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u, false>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqStStackInline.Replace(body,
            static match => $"PPC_PsqStStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u, true>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqLResolvedInline.Replace(body,
            static match => $"PPC_PsqLResolvedStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqStResolvedInline.Replace(body,
            static match => $"PPC_PsqStResolvedStateInline<{match.Groups["w"].Value}u, {match.Groups["i"].Value}u>(native_gqr{match.Groups["i"].Value}, ");
        body = StateFreePatterns.PsqKnownInline.Replace(body, "$1(nullptr, ");

        // The process-wide A/B gate belongs only at CpuContext-to-explicit-state
        // entry boundaries. Once execution is inside a context-free region its
        // direct descendants must remain context-free as well.
        if (contract.CrReadBeforeWriteMask == 0 && contract.CrPossibleWriteMask == 0)
            body = body.Replace("uint32_t cached_cr = ctx->cr;", "uint32_t cached_cr = 0;", StringComparison.Ordinal);
        if (!contract.ReadsXerBeforeWrite && !contract.MayWriteXer)
            body = body.Replace("uint32_t cached_xer = ctx->xer;", "uint32_t cached_xer = 0;", StringComparison.Ordinal);
        if (!contract.ReadsCtrBeforeWrite && !contract.MayWriteCtr)
            body = body.Replace("uint32_t cached_ctr = ctx->ctr;", "uint32_t cached_ctr = 0;", StringComparison.Ordinal);

        if (StateFreeHasOutputs(contract))
        {
            var result = StateFreeReturnExpression(contract, body);
            body = StateFreePatterns.BareReturn.Replace(body, result);
            if (!StateFreePatterns.AggregateReturn.IsMatch(body))
                body += Environment.NewLine + "    " + result + Environment.NewLine;
        }

        if (StateFreePatterns.ContextWord.IsMatch(body))
        {
            var retained = string.Join(" | ", body
                .Split(new[] { "\r\n", "\n" }, StringSplitOptions.None)
                .Where(static line => StateFreePatterns.ContextWord.IsMatch(line))
                .Select(static line => line.Trim()));
            throw new InvalidOperationException(
                $"State-free ABI at 0x{entryPoint:X8} retained CpuContext access: {retained}");
        }

        var variant = new StringBuilder(body.Length + 512);
        variant.AppendLine();
        AppendStateFreeResultDeclaration(variant, entryPoint, contract);
        var bodyAttribute = preferInline ? "MKW_PPC_ALWAYS_INLINE_BODY" : "MKW_PPC_NO_INLINE";
        variant.AppendLine($"extern \"C\" {bodyAttribute} {StateFreeReturnType(entryPoint, contract)} {StateFreeCallingConvention(contract)}{stateFreeSymbol}({StateFreeParameterList(contract, includeNames: true)})");
        variant.AppendLine("{");
        variant.Append(body);
        if (!body.EndsWith('\n')) variant.AppendLine();
        variant.AppendLine("}");
        facts = new GuestStateFreeEmissionFacts(
            entryPoint, stateFreeSymbol, contract, StateFreeGprInputMask(contract));
        variant.AppendLine($"// RECOMP_STATE_FREE_ABI address=0x{entryPoint:X8} symbol={stateFreeSymbol} " +
                           $"gpr_in=0x{StateFreeGprInputMask(contract):X8} gpr_out=0x{contract.GprPossibleWriteMask:X8} " +
                           $"fpr_in=0x{contract.FprReadBeforeWriteMask:X8} fpr_out=0x{contract.FprPossibleWriteMask:X8} " +
                           $"cr_in=0x{contract.CrReadBeforeWriteMask:X2} cr_out=0x{contract.CrPossibleWriteMask:X2} " +
                           $"gqr_in=0x{contract.GqrReadBeforeWriteMask:X2} hid_in=0x{contract.HidReadBeforeWriteMask:X2} hid_out=0x{contract.HidPossibleWriteMask:X2} " +
                           $"xer_in={(contract.ReadsXerBeforeWrite ? 1 : 0)} ctr_in={(contract.ReadsCtrBeforeWrite ? 1 : 0)} lr_in={(contract.ReadsLrBeforeWrite ? 1 : 0)}");
        return code.Insert(closeBrace + 1, variant.ToString());
    }

    private static string RewriteStateFreeBoundaryCalls(
        string body,
        uint entryPoint,
        IReadOnlySet<uint> boundaryTargets,
        IReadOnlyDictionary<uint, GuestAbiContract> contracts)
    {
        // The body is fixed for the whole rewrite, so the cached-local inventory
        // is collected once instead of once per register per matched call site.
        var declared = CollectStateFreeCachedDeclarations(body);
        return StateFreePatterns.BoundaryDirectCall.Replace(body, match =>
        {
            var address = uint.Parse(match.Groups["address"].Value,
                System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture);
            if (!boundaryTargets.Contains(address))
                return match.Value;
            if (!contracts.TryGetValue(address, out var boundary) || boundary.HasFullSynchronizationFence)
                throw new InvalidOperationException(
                    $"Fused state-free boundary 0x{entryPoint:X8} -> 0x{address:X8} has no precise contract.");

            var pad = match.Groups["pad"].Value;
            var replacement = new StringBuilder();
            string CurrentGpr(int index) =>
                declared.Contains($"cached_r{index}") ? $"cached_r{index}" : $"native_r{index}";
            string CurrentFpr(int index) =>
                declared.Contains($"cached_f{index}") ? $"cached_f{index}" : $"native_f{index}";
            var inputGprs = boundary.GprReadBeforeWriteMask | boundary.GprPossibleWriteMask;
            var inputFprs = boundary.FprReadBeforeWriteMask | boundary.FprPossibleWriteMask;
            for (var gpr = 0; gpr < 32; ++gpr)
                if ((inputGprs & (1u << gpr)) != 0)
                    replacement.AppendLine($"{pad}fused_boundary_ctx->gpr[{gpr}] = {CurrentGpr(gpr)};");
            for (var fpr = 0; fpr < 32; ++fpr)
                if ((inputFprs & (1u << fpr)) != 0)
                    replacement.AppendLine($"{pad}fused_boundary_ctx->fpr[{fpr}] = {CurrentFpr(fpr)};");
            if (boundary.CrReadBeforeWriteMask != 0 || boundary.CrPossibleWriteMask != 0)
                replacement.AppendLine($"{pad}fused_boundary_ctx->cr = cached_cr;");
            if (boundary.ReadsXerBeforeWrite || boundary.MayWriteXer)
                replacement.AppendLine($"{pad}fused_boundary_ctx->xer = cached_xer;");
            if (boundary.ReadsCtrBeforeWrite || boundary.MayWriteCtr)
                replacement.AppendLine($"{pad}fused_boundary_ctx->ctr = cached_ctr;");
            if (boundary.ReadsLrBeforeWrite || boundary.MayWriteLr)
                replacement.AppendLine($"{pad}fused_boundary_ctx->lr = native_lr;");
            if (boundary.ReadsFpscrBeforeWrite || boundary.MayWriteFpscr)
                replacement.AppendLine($"{pad}fused_boundary_ctx->fpscr = native_fpscr;");
            for (var gqr = 0; gqr < 8; ++gqr)
                if (((boundary.GqrReadBeforeWriteMask | boundary.GqrPossibleWriteMask) & (1 << gqr)) != 0)
                    replacement.AppendLine($"{pad}fused_boundary_ctx->gqr[{gqr}] = native_gqr{gqr};");
            for (var hid = 0; hid < 3; ++hid)
                if (((boundary.HidReadBeforeWriteMask | boundary.HidPossibleWriteMask) & (1 << hid)) != 0)
                    replacement.AppendLine($"{pad}fused_boundary_ctx->hid{hid} = native_hid{hid};");

            replacement.AppendLine($"{pad}InvokeDirectCpu<0x{address:X8}u>(fused_boundary_ctx);");

            for (var gpr = 0; gpr < 32; ++gpr)
                if ((boundary.GprPossibleWriteMask & (1u << gpr)) != 0)
                    replacement.AppendLine($"{pad}{CurrentGpr(gpr)} = fused_boundary_ctx->gpr[{gpr}];");
            for (var fpr = 0; fpr < 32; ++fpr)
                if ((boundary.FprPossibleWriteMask & (1u << fpr)) != 0)
                    replacement.AppendLine($"{pad}{CurrentFpr(fpr)} = fused_boundary_ctx->fpr[{fpr}];");
            if (boundary.CrPossibleWriteMask != 0)
                replacement.AppendLine($"{pad}cached_cr = fused_boundary_ctx->cr;");
            if (boundary.MayWriteXer)
                replacement.AppendLine($"{pad}cached_xer = fused_boundary_ctx->xer;");
            if (boundary.MayWriteCtr)
                replacement.AppendLine($"{pad}cached_ctr = fused_boundary_ctx->ctr;");
            if (boundary.MayWriteLr)
                replacement.AppendLine($"{pad}native_lr = fused_boundary_ctx->lr;");
            if (boundary.MayWriteFpscr)
                replacement.AppendLine($"{pad}native_fpscr = fused_boundary_ctx->fpscr;");
            for (var gqr = 0; gqr < 8; ++gqr)
                if ((boundary.GqrPossibleWriteMask & (1 << gqr)) != 0)
                    replacement.AppendLine($"{pad}native_gqr{gqr} = fused_boundary_ctx->gqr[{gqr}];");
            for (var hid = 0; hid < 3; ++hid)
                if ((boundary.HidPossibleWriteMask & (1 << hid)) != 0)
                    replacement.AppendLine($"{pad}native_hid{hid} = fused_boundary_ctx->hid{hid};");
            return replacement.ToString().TrimEnd();
        });
    }

    private static string EnsureStateFreeBoundarySpecialLocals(
        string body,
        uint entryPoint,
        IReadOnlySet<uint> boundaryTargets,
        IReadOnlyDictionary<uint, GuestAbiContract> contracts)
    {
        var needsCr = false;
        var needsXer = false;
        var needsCtr = false;
        // Collected in one pass; the per-target patterns only differed in the
        // literal address, and the address text they matched is captured here.
        var presentBoundaryAddresses = new HashSet<string>(StringComparer.Ordinal);
        foreach (Match match in StateFreePatterns.BoundaryDirectCallAddress.Matches(body))
        {
            presentBoundaryAddresses.Add(match.Groups[1].Value);
        }

        foreach (var address in boundaryTargets)
        {
            // The selector owns one boundary set for the whole fused region,
            // while this method is producing one member clone. Only calls that
            // actually occur in this member contribute to its local residency
            // and signature requirements.
            if (!presentBoundaryAddresses.Contains(address.ToString("X8", System.Globalization.CultureInfo.InvariantCulture)))
                continue;
            if (!contracts.TryGetValue(address, out var boundary) || boundary.HasFullSynchronizationFence)
                throw new InvalidOperationException(
                    $"Fused state-free boundary 0x{entryPoint:X8} -> 0x{address:X8} has no precise contract.");
            needsCr |= boundary.CrReadBeforeWriteMask != 0 || boundary.CrPossibleWriteMask != 0;
            needsXer |= boundary.ReadsXerBeforeWrite || boundary.MayWriteXer;
            needsCtr |= boundary.ReadsCtrBeforeWrite || boundary.MayWriteCtr;
        }

        // A region's interprocedural contract can make special state live solely because an outlined
        // descendant observes it, even when the root body never created the usual cached local. Derive
        // these locals from the precise boundary contracts rather than incidental root-body instructions.
        var declarations = new StringBuilder();
        var declared = needsCr || needsXer || needsCtr
            ? CollectStateFreeCachedDeclarations(body)
            : null;
        if (needsCr && !declared!.Contains("cached_cr"))
            declarations.AppendLine("    uint32_t cached_cr = native_cr;");
        if (needsXer && !declared!.Contains("cached_xer"))
            declarations.AppendLine("    uint32_t cached_xer = native_xer;");
        if (needsCtr && !declared!.Contains("cached_ctr"))
            declarations.AppendLine("    uint32_t cached_ctr = native_ctr;");
        return declarations.Length == 0 ? body : declarations + body;
    }

}
