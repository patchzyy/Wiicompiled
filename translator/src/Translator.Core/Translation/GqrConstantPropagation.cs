using System;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Analysis.Ssa;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

public sealed record GqrConstantFlow(
    IReadOnlyDictionary<string, IReadOnlyDictionary<string, uint>> In);

/// <summary>Forward must-constant propagation used only to specialize PSQ helpers.</summary>
public static class GqrConstantPropagation
{
    private static readonly StringComparer Comparer = StringComparer.OrdinalIgnoreCase;

    public static GqrConstantFlow Analyze(
        IrFunction function,
        IReadOnlyDictionary<string, uint>? entryConstants = null,
        IReadOnlyDictionary<uint, byte>? calleeWriteMasks = null)
    {
        var cfg = IrCfg.Build(function);
        var input = function.Blocks.ToDictionary(b => b.Label, _ => new Dictionary<string, uint>(Comparer), Comparer);
        var output = function.Blocks.ToDictionary(b => b.Label, _ => new Dictionary<string, uint>(Comparer), Comparer);
        var outputInitialized = function.Blocks.ToDictionary(b => b.Label, _ => false, Comparer);
        var changed = true;
        var iterations = 0;
        while (changed)
        {
            var iterationLimit = Math.Max(256, function.Blocks.Count * 4 + 16);
            if (++iterations > iterationLimit)
                return UnknownFlow(function);
            changed = false;
            foreach (var block in function.Blocks)
            {
                var isEntry = Comparer.Equals(block.Label, function.EntryLabel);
                var predecessors = cfg.Predecessors(block.Label)
                    .Where(p => outputInitialized[p]).ToArray();
                // An uninitialized predecessor is lattice top, not an empty
                // constant map. Do not manufacture a bottom state for blocks
                // that have not become reachable from the entry yet.
                if (!isEntry && predecessors.Length == 0)
                    continue;
                var state = isEntry
                    ? Intersect(predecessors.Select(p => output[p]).Prepend(
                        new Dictionary<string, uint>(entryConstants ?? new Dictionary<string, uint>(), Comparer)))
                    : Intersect(predecessors.Select(p => output[p]));
                var newInput = new Dictionary<string, uint>(state, Comparer);
                foreach (var instruction in block.Instructions)
                    Transfer(instruction, state, output, calleeWriteMasks);
                if (!Same(input[block.Label], newInput)) { input[block.Label] = newInput; changed = true; }
                if (!Same(output[block.Label], state)) { output[block.Label] = state; changed = true; }
                if (!outputInitialized[block.Label]) { outputInitialized[block.Label] = true; changed = true; }
            }
        }
        return new GqrConstantFlow(
            input.ToDictionary(p => p.Key, p => (IReadOnlyDictionary<string, uint>)p.Value, Comparer));
    }

    public static IrFunction Specialize(
        IrFunction function,
        IReadOnlyDictionary<string, uint>? entryConstants = null,
        IReadOnlyDictionary<uint, byte>? calleeWriteMasks = null,
        bool requireRuntimeGuard = false,
        bool resolvedOnly = false)
    {
        var flow = Analyze(function, entryConstants, calleeWriteMasks);
        var blocks = new List<IrBasicBlock>(function.Blocks.Count);
        foreach (var block in function.Blocks)
        {
            var state = new Dictionary<string, uint>(flow.In[block.Label], Comparer);
            var instructions = new List<IrInstruction>(block.Instructions.Count);
            foreach (var instruction in block.Instructions)
            {
                var rewritten = TrySpecializePsq(instruction, state, requireRuntimeGuard, resolvedOnly);
                instructions.Add(rewritten);
                Transfer(instruction, state, null, calleeWriteMasks);
            }
            blocks.Add(new IrBasicBlock(block.Label, instructions));
        }
        return new IrFunction(function.Name, function.EntryLabel, blocks);
    }

    private static IrInstruction TrySpecializePsq(
        IrInstruction instruction,
        IReadOnlyDictionary<string, uint> state,
        bool requireRuntimeGuard,
        bool resolvedOnly)
    {
        if (instruction is IrResolvedPsqLoad load && load.KnownGqr is null &&
            state.TryGetValue($"gqr{load.I}", out var loadGqr))
            return load with { KnownGqr = loadGqr, GuardKnownGqr = requireRuntimeGuard };
        if (instruction is IrResolvedPsqStore store && store.KnownGqr is null &&
            state.TryGetValue($"gqr{store.I}", out var storeGqr))
            return store with { KnownGqr = storeGqr, GuardKnownGqr = requireRuntimeGuard };
        if (resolvedOnly) return instruction;
        if (instruction is not IrCall call) return instruction;
        var isLoad = call.Target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase);
        var isStore = call.Target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase);
        var indexArgument = isLoad ? 2 : isStore ? 3 : -1;
        if (indexArgument < 0 || call.Arguments.Count <= indexArgument ||
            !TryValue(call.Arguments[indexArgument], state, out var index) || index >= 8 ||
            !state.TryGetValue($"gqr{index}", out var gqr)) return instruction;
        var suffix = requireRuntimeGuard ? "KnownGuarded" : "Known";
        return call with { Target = $"{call.Target}{suffix}_{gqr:X8}" };
    }

    private static void Transfer(IrInstruction instruction, Dictionary<string, uint> state,
        IReadOnlyDictionary<string, Dictionary<string, uint>>? predecessorOut,
        IReadOnlyDictionary<uint, byte>? calleeWriteMasks)
    {
        switch (instruction)
        {
            case IrAssign assign:
                SetOrKill(assign.Destination, assign.Value, state);
                break;
            case IrBinary binary:
                if (TryValue(binary.Left, state, out var left) && TryValue(binary.Right, state, out var right) &&
                    TryBinary(binary.Op, left, right, out var result)) state[binary.Destination] = result;
                else state.Remove(binary.Destination);
                break;
            case IrPhi phi:
                if (predecessorOut != null && phi.Sources.Count != 0)
                {
                    uint? value = null;
                    var valid = true;
                    foreach (var source in phi.Sources)
                    {
                        if (!predecessorOut.TryGetValue(source.Key, out var pred) || !pred.TryGetValue(source.Value, out var incoming) ||
                            (value.HasValue && value.Value != incoming)) { valid = false; break; }
                        value = incoming;
                    }
                    if (valid && value.HasValue) state[phi.Destination] = value.Value; else state.Remove(phi.Destination);
                }
                else state.Remove(phi.Destination);
                break;
            case IrLoad load:
                state.Remove(load.Destination);
                break;
            case IrCall call when IsGuestCall(call.Target):
                PreserveUnmodifiedGqrsAcrossCall(call.Target, state, calleeWriteMasks);
                break;
            case IrCall call when !string.IsNullOrWhiteSpace(call.Destination):
                state.Remove(call.Destination);
                break;
            case IrIndirectCall:
                state.Clear();
                break;
        }
    }

    private static void SetOrKill(string destination, IrValue value, Dictionary<string, uint> state)
    {
        var key = GqrKey(destination) ?? destination;
        if (TryValue(value, state, out var constant)) state[key] = constant;
        else state.Remove(key);
    }

    private static bool TryValue(IrValue value, IReadOnlyDictionary<string, uint> state, out uint constant)
    {
        if (value is { Kind: "const", Constant: { } immediate }) { constant = unchecked((uint)immediate); return true; }
        if (value is { Kind: "register", RegisterName: { } name } && state.TryGetValue(name, out constant)) return true;
        constant = 0; return false;
    }

    private static bool TryBinary(string op, uint left, uint right, out uint value)
    {
        value = op.ToLowerInvariant() switch
        {
            "add" or "add_u" => unchecked(left + right),
            "sub" or "sub_u" => unchecked(left - right),
            "or" => left | right,
            "and" => left & right,
            "xor" => left ^ right,
            "shl" => left << ((int)right & 31),
            "rotl" => (left << ((int)right & 31)) | (left >> ((32 - (int)right) & 31)),
            "shr" or "shr_u" => left >> ((int)right & 31),
            _ => 0
        };
        return op.ToLowerInvariant() is "add" or "add_u" or "sub" or "sub_u" or "or" or "and" or "xor" or "shl" or "shr" or "shr_u" or "rotl";
    }

    private static Dictionary<string, uint> Intersect(IEnumerable<Dictionary<string, uint>> maps)
    {
        var list = maps.ToArray();
        var result = new Dictionary<string, uint>(list[0], Comparer);
        foreach (var key in result.Keys.ToArray())
            if (list.Skip(1).Any(map => !map.TryGetValue(key, out var value) || value != result[key])) result.Remove(key);
        return result;
    }
    private static bool Same(IReadOnlyDictionary<string, uint> a, IReadOnlyDictionary<string, uint> b) =>
        a.Count == b.Count && a.All(p => b.TryGetValue(p.Key, out var value) && value == p.Value);
    private static bool IsGuestCall(string target) => target.StartsWith("func_", StringComparison.OrdinalIgnoreCase) ||
        target.StartsWith("0x", StringComparison.OrdinalIgnoreCase) || uint.TryParse(target, out _);

    private static void PreserveUnmodifiedGqrsAcrossCall(
        string target,
        Dictionary<string, uint> state,
        IReadOnlyDictionary<uint, byte>? calleeWriteMasks)
    {
        var writeMask = TryGuestAddress(target, out var address) &&
                        calleeWriteMasks is not null &&
                        calleeWriteMasks.TryGetValue(address, out var knownMask)
            ? knownMask
            : (byte)0xFF;
        var preserved = state
            .Where(pair => GqrKey(pair.Key) is { } key &&
                           (writeMask & (1 << (key[3] - '0'))) == 0)
            .ToArray();
        state.Clear();
        foreach (var pair in preserved)
            state[GqrKey(pair.Key)!] = pair.Value;
    }

    public static bool TryGuestAddress(string target, out uint address) =>
        GuestTargetParser.TryParseAddressOrDecimal(target, out address);

    internal static string? TryGetGqrKey(string name) => GqrKey(name);
    private static GqrConstantFlow UnknownFlow(IrFunction function)
    {
        var unknown = function.Blocks.ToDictionary(
            block => block.Label,
            _ => (IReadOnlyDictionary<string, uint>)new Dictionary<string, uint>(Comparer),
            Comparer);
        return new GqrConstantFlow(unknown);
    }
    private static string? GqrKey(string name)
    {
        var baseName = RegisterNameUtils.StripNumericSuffix(name);
        return baseName.Length == 4 && baseName.StartsWith("gqr", StringComparison.OrdinalIgnoreCase) &&
               baseName[3] is >= '0' and <= '7' ? baseName.ToLowerInvariant() : null;
    }
}
