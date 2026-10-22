using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;
using Translator.Core.Analysis.Representation;
using Translator.Core.Translation;
using Translator.Core.Representation;

namespace Translator.Core.CodeGen;

public sealed partial class CxxLinearCodeGenerator
{
    internal const int MinimumGuardedGqrVersioningSites = 3;

    private readonly record struct GqrEntryGuard(uint Index, uint Value);

    private static string GqrEntryGuardName(uint index, uint value) =>
        $"gqr_entry_{index}_{value:X8}";

    private static IReadOnlyList<GqrEntryGuard> CollectGqrEntryGuards(IrFunction function)
    {
        var guards = new HashSet<GqrEntryGuard>();
        foreach (var instruction in function.Blocks.SelectMany(static block => block.Instructions))
        {
            switch (instruction)
            {
                case IrResolvedPsqLoad load when load.GuardKnownGqr && load.KnownGqr.HasValue:
                    guards.Add(new GqrEntryGuard(load.I, load.KnownGqr.Value));
                    break;
                case IrResolvedPsqStore store when store.GuardKnownGqr && store.KnownGqr.HasValue:
                    guards.Add(new GqrEntryGuard(store.I, store.KnownGqr.Value));
                    break;
                case IrCall call when TryGetGuardedGqr(call, out var guard):
                    guards.Add(guard);
                    break;
            }
        }
        return guards.OrderBy(static guard => guard.Index).ThenBy(static guard => guard.Value).ToArray();
    }

    private static bool TryGetGuardedGqr(IrCall call, out GqrEntryGuard guard)
    {
        guard = default;
        const string loadPrefix = "PPC_PsqLKnownGuarded_";
        const string storePrefix = "PPC_PsqStKnownGuarded_";
        var isLoad = call.Target.StartsWith(loadPrefix, StringComparison.OrdinalIgnoreCase);
        var prefix = isLoad ? loadPrefix : call.Target.StartsWith(storePrefix, StringComparison.OrdinalIgnoreCase)
            ? storePrefix : null;
        var indexArgument = isLoad ? 2 : 3;
        var index = call.Arguments.Count > indexArgument
            ? call.Arguments[indexArgument].Constant : null;
        if (prefix is null || call.Arguments.Count <= indexArgument ||
            index is not (>= 0 and < 8) ||
            !uint.TryParse(call.Target.AsSpan(prefix.Length), System.Globalization.NumberStyles.HexNumber,
                System.Globalization.CultureInfo.InvariantCulture, out var value))
            return false;
        guard = new GqrEntryGuard((uint)index.Value, value);
        return true;
    }

    private static bool CanVersionGuardedGqrFunction(
        IrFunction function,
        IReadOnlyList<GqrEntryGuard> guards)
    {
        // Versioning trades cold code size for a branch-free hot body, so it only pays off with enough
        // repeated PSQ work to amortize the second instantiation. Three sites already outweighs that
        // cost; a higher threshold missed matrix helpers like PSMTXMultVec/PSMTXConcat that dominate
        // the paired-single profile but only clear it after inlining.
        if (guards.Count == 0 ||
            guards.GroupBy(static guard => guard.Index).Any(static group => group.Count() != 1))
        {
            return false;
        }

        var guardedIndices = guards.Select(static guard => guard.Index).ToHashSet();
        var guardedSites = 0;
        foreach (var instruction in function.Blocks.SelectMany(static block => block.Instructions))
        {
            switch (instruction)
            {
                case IrAssign assign when GqrConstantPropagation.TryGetGqrKey(assign.Destination) is { } key &&
                                          guardedIndices.Contains((uint)(key[3] - '0')):
                    return false;
                case IrResolvedPsqLoad load when load.GuardKnownGqr && load.KnownGqr.HasValue:
                case IrResolvedPsqStore store when store.GuardKnownGqr && store.KnownGqr.HasValue:
                    guardedSites++;
                    break;
                case IrCall call when TryGetGuardedGqr(call, out _):
                    guardedSites++;
                    break;
            }
        }

        return guardedSites >= MinimumGuardedGqrVersioningSites;
    }

    private static string ApplyGuardedGqrFunctionVersioning(
        string code,
        string functionName,
        IReadOnlyList<GqrEntryGuard> guards)
    {
        const string parameters = CpuContextDefinitionParameter;
        const string callArguments = "ctx";
        // Match the unqualified definition emitted before leaf-cache rewriting.
        // The inline qualifier belongs on the rewritten definition below; if
        // it is present here the cache pass cannot find the function opening.
        var functionDecl = FunctionDefinitionSignature(functionName);
        var implementationName = $"{functionName}_gqr_impl";
        var lines = code.Replace("\r\n", "\n", StringComparison.Ordinal).Split('\n');
        var rewritten = new StringBuilder(code.Length * 2);
        var pendingFunctionOpen = false;
        var inFunction = false;
        var braceDepth = 0;

        foreach (var originalLine in lines)
        {
            var line = originalLine;
            if (!inFunction && string.Equals(line, functionDecl, StringComparison.Ordinal))
            {
                rewritten.AppendLine("template <bool gqr_entry_profile>");
                rewritten.AppendLine($"MKW_PPC_NO_INLINE static void {implementationName}({parameters})");
                pendingFunctionOpen = true;
                continue;
            }

            if (pendingFunctionOpen && string.Equals(line, "{", StringComparison.Ordinal))
            {
                rewritten.AppendLine(line);
                pendingFunctionOpen = false;
                inFunction = true;
                braceDepth = 1;
                continue;
            }

            if (inFunction)
            {
                if (guards.Any(guard => line.Contains(
                        $"const bool {GqrEntryGuardName(guard.Index, guard.Value)} =",
                        StringComparison.Ordinal)))
                {
                    continue;
                }

                foreach (var guard in guards)
                {
                    line = line.Replace(
                        GqrEntryGuardName(guard.Index, guard.Value),
                        "gqr_entry_profile",
                        StringComparison.Ordinal);
                }
                line = line.Replace(
                    "if (gqr_entry_profile)",
                    "if constexpr (gqr_entry_profile)",
                    StringComparison.Ordinal);
            }

            rewritten.AppendLine(line);

            if (!inFunction)
            {
                continue;
            }

            braceDepth += CountChar(line, '{') - CountChar(line, '}');
            if (braceDepth > 0)
            {
                continue;
            }

            inFunction = false;
            var condition = string.Join(" && ", guards.Select(static guard =>
                $"ctx->gqr[{guard.Index}u] == 0x{guard.Value:X8}u"));
            rewritten.AppendLine();
            rewritten.AppendLine(FunctionDefinitionSignature(functionName));
            rewritten.AppendLine("{");
            rewritten.AppendLine($"    if ({condition})");
            rewritten.AppendLine($"        {implementationName}<true>({callArguments});");
            rewritten.AppendLine("    else");
            rewritten.AppendLine($"        {implementationName}<false>({callArguments});");
            rewritten.AppendLine("}");
        }

        return rewritten.ToString();
    }

    private sealed record PairedFlowStateMaps(
        Dictionary<string, HashSet<string>> In,
        Dictionary<string, HashSet<string>> Out);

    private static HashSet<string> CollectNonRegisterLocals(IrFunction function)
    {
        var locals = new HashSet<string>(StringComparer.OrdinalIgnoreCase);

        void Add(string name)
        {
            // Only add if it's NOT a CPU register that uses direct ctx access.
            // CR fields need local variables because direct CR field writes are complex.
            if (!IsCpuRegisterNoLocalNeeded(name))
            {
                locals.Add(name);
            }
        }

        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                // Range destinations are host pointers declared separately in
                // the function prologue. IrSetCrField writes CR through a
                // helper and has no C++ destination expression. Everything
                // else follows the canonical syntactic IR use/def walker.
                if (instruction is not (IrResolveGuestMemoryRange or IrSetCrField))
                {
                    foreach (var definition in IrRegisterDataFlow.Definitions(instruction))
                    {
                        Add(definition);
                    }
                }

                foreach (var use in IrRegisterDataFlow.Uses(instruction))
                {
                    Add(use);
                }
            }
        }

        return locals;
    }

    private static IEnumerable<string> CollectCallTargets(IrFunction function)
    {
        foreach (var block in function.Blocks)
        {
            foreach (var ins in block.Instructions)
            {
                switch (ins)
                {
                    case IrCall call when !string.IsNullOrWhiteSpace(call.Target):
                        yield return call.Target;
                        break;
                }
            }
        }
    }

    /// <summary>
    /// Bit position of a float register in a paired-state mask, or -1 if not a float register.
    /// </summary>
    private static int PairedFloatRegisterBit(string baseName) =>
        ClassifyRegisterBase(RegisterBaseSpan(baseName), out var index) == GuestRegisterClass.Fpr
            ? index
            : -1;

    private static uint ClearPairedBit(uint mask, string destination)
    {
        var bit = PairedFloatRegisterBit(destination);
        return bit >= 0 ? mask & ~(1u << bit) : mask;
    }

    private static HashSet<string> MaterializePairedFloatSet(uint mask, StringComparer comparer)
    {
        var set = new HashSet<string>(comparer);
        for (var bit = 0; bit < 32; bit++)
        {
            if ((mask & (1u << bit)) != 0)
            {
                set.Add(FprRegisterName(bit));
            }
        }

        return set;
    }

    private static PairedFlowStateMaps ComputePairedFlowStates(
        IrFunction func,
        IrCfg cfg,
        IGuestFunctionAbiProvider guestAbiProvider)
    {
        var comparer = StringComparer.OrdinalIgnoreCase;
        // f0..f31 occupy bits 0..31. The fixpoint used to seed a fresh
        // HashSet<string> holding all 32 register names for every block and to
        // rebuild two more sets per block per iteration; the mask form has the
        // same lattice, the same initial state and the same transfer.
        const uint allFloatRegisters = uint.MaxValue;
        const uint f1Bit = 1u << 1;
        var blocks = func.Blocks;
        var blockCount = blocks.Count;
        var blockIndices = new Dictionary<string, int>(blockCount, comparer);
        for (var i = 0; i < blockCount; i++)
        {
            blockIndices[blocks[i].Label] = i;
        }

        var isEntryBlock = new bool[blockCount];
        var inMasks = new uint[blockCount];
        var outMasks = new uint[blockCount];
        // This is a forward must-analysis: non-entry blocks start at top so loop latches
        // can converge on paired registers that are preserved through the cycle.
        for (var i = 0; i < blockCount; i++)
        {
            isEntryBlock[i] = blocks[i].Label.Equals(func.EntryLabel, StringComparison.OrdinalIgnoreCase);
            inMasks[i] = isEntryBlock[i] ? 0u : allFloatRegisters;
            outMasks[i] = isEntryBlock[i] ? 0u : allFloatRegisters;
        }

        // The CFG is immutable for the whole fixpoint, so the filtered
        // predecessor lists are built once instead of once per iteration.
        var predecessors = new List<string>[blockCount];
        for (var i = 0; i < blockCount; i++)
        {
            var label = blocks[i].Label;
            predecessors[i] = cfg.Predecessors(label)
                .Where(p => !p.Equals(label, StringComparison.OrdinalIgnoreCase))
                .ToList();
        }

        var changed = true;
        while (changed)
        {
            changed = false;
            for (var b = 0; b < blockCount; b++)
            {
                var block = blocks[b];
                var preds = predecessors[b];
                uint newIn;
                // A translated entry can have CFG backedges, but it also has an external
                // predecessor whose live FPRs use the scalar CpuContext representation.
                if (isEntryBlock[b] || preds.Count == 0)
                {
                    newIn = 0u;
                }
                else
                {
                    newIn = outMasks[blockIndices[preds[0]]];
                    for (var p = 1; p < preds.Count; p++)
                    {
                        newIn &= outMasks[blockIndices[preds[p]]];
                    }
                }

                var newOut = newIn;
                foreach (var ins in block.Instructions)
                {
                    switch (ins)
                    {
                        case IrPhi phi:
                            {
                                var destBit = PairedFloatRegisterBit(phi.Destination);
                                if (destBit < 0)
                                {
                                    break;
                                }

                                var allPaired = true;
                                foreach (var (predLabel, src) in phi.Sources)
                                {
                                    var srcBit = PairedFloatRegisterBit(src);
                                    if (srcBit < 0)
                                    {
                                        allPaired = false;
                                        break;
                                    }

                                    if (!blockIndices.TryGetValue(predLabel, out var predIndex) ||
                                        (outMasks[predIndex] & (1u << srcBit)) == 0)
                                    {
                                        allPaired = false;
                                        break;
                                    }
                                }

                                if (allPaired)
                                {
                                    newOut |= 1u << destBit;
                                }
                                else
                                {
                                    newOut &= ~(1u << destBit);
                                }
                                break;
                            }
                        case IrCall call:
                            {
                                if (IsGuestCallTarget(call.Target))
                                {
                                    // Normalize proven scalar arguments and treat f1 as the
                                    // conventional scalar return. Preserve f2-f13 representation
                                    // tags when the callee leaves their payloads untouched.
                                    newOut = ClearPairedGuestScalarFloatArguments(
                                        call.Target, call.Arguments, newOut, guestAbiProvider);
                                    newOut &= ~f1Bit;
                                }

                                if (!string.IsNullOrWhiteSpace(call.Destination))
                                {
                                    var destBit = PairedFloatRegisterBit(call.Destination);
                                    if (destBit < 0)
                                    {
                                        break;
                                    }
                                    if (IsPairedProducerTarget(call.Target))
                                    {
                                        newOut |= 1u << destBit;
                                    }
                                    else
                                    {
                                        newOut &= ~(1u << destBit);
                                    }
                                }
                                break;
                            }
                        case IrIndirectCall icall when !string.IsNullOrWhiteSpace(icall.Destination):
                            {
                                newOut &= ~f1Bit;
                                var destBit = PairedFloatRegisterBit(icall.Destination);
                                if (destBit >= 0)
                                {
                                    newOut &= ~(1u << destBit);
                                }
                                break;
                            }
                        case IrIndirectCall icall:
                            newOut &= ~f1Bit;
                            break;
                        case IrAssign assign:
                            {
                                var destBit = PairedFloatRegisterBit(assign.Destination);
                                if (destBit < 0)
                                {
                                    break;
                                }

                                if (assign.Value.Kind == "register" && assign.Value.RegisterName != null)
                                {
                                    var srcBit = PairedFloatRegisterBit(assign.Value.RegisterName);
                                    if (srcBit >= 0 && (newOut & (1u << srcBit)) != 0)
                                    {
                                        newOut |= 1u << destBit;
                                    }
                                    else
                                    {
                                        newOut &= ~(1u << destBit);
                                    }
                                }
                                else
                                {
                                    newOut &= ~(1u << destBit);
                                }
                                break;
                            }
                        case IrBinary bin:
                            {
                                var destBit = PairedFloatRegisterBit(bin.Destination);
                                if (destBit >= 0)
                                {
                                    newOut &= ~(1u << destBit);
                                }
                                break;
                            }
                        case IrLoad load:
                            newOut = ClearPairedBit(newOut, load.Destination);
                            break;
                        case IrResolvedLoad load:
                            newOut = ClearPairedBit(newOut, load.Destination);
                            break;
                        case IrResolvedLoadPair pair:
                            newOut = ClearPairedBit(newOut, pair.FirstDestination);
                            newOut = ClearPairedBit(newOut, pair.SecondDestination);
                            break;
                        case IrResolvedPsqLoad load:
                            {
                                var destBit = PairedFloatRegisterBit(load.Destination);
                                if (destBit >= 0)
                                {
                                    newOut |= 1u << destBit;
                                }
                                break;
                            }
                    }
                }

                if (newIn != inMasks[b])
                {
                    inMasks[b] = newIn;
                    changed = true;
                }

                if (newOut != outMasks[b])
                {
                    outMasks[b] = newOut;
                    changed = true;
                }
            }
        }

        var inMap = new Dictionary<string, HashSet<string>>(blockCount, comparer);
        var outMap = new Dictionary<string, HashSet<string>>(blockCount, comparer);
        for (var i = 0; i < blockCount; i++)
        {
            inMap[blocks[i].Label] = MaterializePairedFloatSet(inMasks[i], comparer);
            outMap[blocks[i].Label] = MaterializePairedFloatSet(outMasks[i], comparer);
        }

        return new PairedFlowStateMaps(inMap, outMap);
    }

    private static uint ClearPairedGuestScalarFloatArguments(
        string target,
        IReadOnlyList<IrValue> arguments,
        uint pairedMask,
        IGuestFunctionAbiProvider guestAbiProvider)
    {
        foreach (var arg in arguments)
        {
            if (arg.Kind != "register" || arg.RegisterName == null)
            {
                continue;
            }

            var bit = PairedFloatRegisterBit(arg.RegisterName);
            if (bit < 0 || (pairedMask & (1u << bit)) == 0)
            {
                continue;
            }

            if (IsGuestScalarFloatArgument(guestAbiProvider, target, GetRegisterBaseName(arg.RegisterName)))
            {
                pairedMask &= ~(1u << bit);
            }
        }

        return pairedMask;
    }
}
