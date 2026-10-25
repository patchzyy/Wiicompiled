using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;

namespace Translator.Core.Translation;

public sealed record GqrInterproceduralResult(
    IReadOnlyDictionary<uint, IReadOnlyDictionary<string, uint>> EntryConstants,
    IReadOnlyDictionary<uint, byte> WriteMasks,
    IReadOnlySet<uint> ReachableFunctions);

/// <summary>
/// Computes closed-world GQR effects and must-constant entry facts for direct
/// guest calls. Unknown and indirect calls conservatively write every GQR.
/// </summary>
public static class GqrInterproceduralAnalysis
{
    private const int GqrCount = 8;

    /// <summary>Minimum dirty-caller count before a round is farmed out to the thread pool.</summary>
    private const int ParallelRoundThreshold = 8;

    private static readonly StringComparer Comparer = StringComparer.OrdinalIgnoreCase;

    private static readonly string[] GqrKeys =
        ["gqr0", "gqr1", "gqr2", "gqr3", "gqr4", "gqr5", "gqr6", "gqr7"];

    /// <summary>Bulk-translation path: same closed-world fixpoint as the naive formulation, but over
    /// compact GQR projections and a caller-side worklist that only re-evaluates movers. The per-callee
    /// merge is a commutative/associative intersection, so visit order never affects the result.</summary>
    public static GqrInterproceduralResult Analyze(
        IReadOnlyDictionary<uint, GqrFunctionSummary> functions,
        IEnumerable<uint> roots)
    {
        var writeMasks = ComputeCompactWriteMasks(functions);
        var rootSet = roots.Where(functions.ContainsKey).ToHashSet();
        var reachable = ComputeCompactReachable(functions, rootSet);

        // Dense, deterministic indexing over the reachable set. Reachability and
        // the callee write masks are round-invariant, so both are computed once.
        var count = reachable.Count;
        var addresses = new uint[count];
        reachable.CopyTo(addresses);
        Array.Sort(addresses);
        var indices = new Dictionary<uint, int>(count);
        for (var i = 0; i < count; i++) indices[addresses[i]] = i;

        var summaries = new GqrFunctionSummary[count];
        var isRoot = new bool[count];
        for (var i = 0; i < count; i++)
        {
            summaries[i] = functions[addresses[i]];
            isRoot[i] = rootSet.Contains(addresses[i]);
        }

        // Entry facts as a bitmask plus one value per GQR slot; the string keys
        // only exist at the public boundary.
        var entryMasks = new byte[count];
        var entryValues = new uint[count * GqrCount];

        // Call-site shape (which sites exist and where they point) is invariant;
        // only the constants flowing into them change between rounds.
        var contributions = BuildCompactCallSites(summaries, indices);

        // Reverse index (callee -> contributing call sites) in CSR form. A callee
        // only has to be re-merged when one of its callers was re-evaluated, and
        // every other callee keeps the entry fact it already agreed with.
        var edgeCount = 0;
        for (var caller = 0; caller < count; caller++)
            edgeCount += contributions[caller].Targets.Length;
        var targetStart = new int[count + 1];
        for (var caller = 0; caller < count; caller++)
        {
            var targets = contributions[caller].Targets;
            for (var site = 0; site < targets.Length; site++) targetStart[targets[site] + 1]++;
        }
        for (var i = 0; i < count; i++) targetStart[i + 1] += targetStart[i];
        var edgeCaller = new int[edgeCount];
        var edgeSite = new int[edgeCount];
        var edgeFill = new int[count];
        Array.Copy(targetStart, edgeFill, count);
        for (var caller = 0; caller < count; caller++)
        {
            var targets = contributions[caller].Targets;
            for (var site = 0; site < targets.Length; site++)
            {
                var position = edgeFill[targets[site]]++;
                edgeCaller[position] = caller;
                edgeSite[position] = site;
            }
        }

        var mergedValues = new uint[GqrCount];
        var affected = new int[count];
        var affectedFlag = new bool[count];

        // Callers without a single reachable outgoing call site can never
        // contribute anything, so they never need their flow analysed.
        var dirty = new int[count];
        var dirtyCount = 0;
        for (var i = 0; i < count; i++)
            if (contributions[i].Targets.Length != 0) dirty[dirtyCount++] = i;

        var changed = true;
        var iterations = 0;
        while (changed)
        {
            if (++iterations > Math.Max(256, functions.Count * 2 + 16))
                throw new InvalidOperationException("Interprocedural compact GQR entry propagation did not converge.");

            EvaluateCompactCallers(
                summaries, entryMasks, entryValues, writeMasks, indices, contributions, dirty, dirtyCount);

            var affectedCount = 0;
            for (var d = 0; d < dirtyCount; d++)
            {
                var targets = contributions[dirty[d]].Targets;
                for (var site = 0; site < targets.Length; site++)
                {
                    var target = targets[site];
                    if (affectedFlag[target]) continue;
                    affectedFlag[target] = true;
                    affected[affectedCount++] = target;
                }
            }

            changed = false;
            dirtyCount = 0;
            for (var a = 0; a < affectedCount; a++)
            {
                var target = affected[a];
                affectedFlag[target] = false;

                // Intersection of the must-constant facts of every call site that
                // targets this callee; commutative and associative, so the merged
                // value never depends on the caller visit order.
                byte mask = 0;
                var first = true;
                for (var edge = targetStart[target]; edge < targetStart[target + 1]; edge++)
                {
                    var sites = contributions[edgeCaller[edge]];
                    var site = edgeSite[edge];
                    var sourceBase = site * GqrCount;
                    var siteMask = sites.Masks[site];
                    if (first)
                    {
                        first = false;
                        mask = siteMask;
                        for (var slot = 0; slot < GqrCount; slot++)
                            if ((mask & (1 << slot)) != 0)
                                mergedValues[slot] = sites.Values[sourceBase + slot];
                        continue;
                    }

                    var combined = (byte)(mask & siteMask);
                    for (var slot = 0; slot < GqrCount; slot++)
                    {
                        var bit = 1 << slot;
                        if ((combined & bit) == 0) continue;
                        if (mergedValues[slot] != sites.Values[sourceBase + slot])
                            combined &= (byte)~bit;
                    }
                    mask = combined;
                    if (mask == 0) break;
                }

                var nextMask = isRoot[target] ? (byte)0 : mask;
                var entryBase = target * GqrCount;
                var same = entryMasks[target] == nextMask;
                for (var slot = 0; slot < GqrCount && same; slot++)
                    if ((nextMask & (1 << slot)) != 0 &&
                        entryValues[entryBase + slot] != mergedValues[slot])
                        same = false;
                if (same) continue;

                entryMasks[target] = nextMask;
                for (var slot = 0; slot < GqrCount; slot++)
                    if ((nextMask & (1 << slot)) != 0)
                        entryValues[entryBase + slot] = mergedValues[slot];
                changed = true;
                if (contributions[target].Targets.Length != 0) dirty[dirtyCount++] = target;
            }
        }

        var entries = new Dictionary<uint, IReadOnlyDictionary<string, uint>>(functions.Count);
        foreach (var address in functions.Keys)
        {
            var constants = new Dictionary<string, uint>(Comparer);
            if (indices.TryGetValue(address, out var index))
            {
                var mask = entryMasks[index];
                var entryBase = index * GqrCount;
                for (var slot = 0; slot < GqrCount; slot++)
                    if ((mask & (1 << slot)) != 0)
                        constants[GqrKeys[slot]] = entryValues[entryBase + slot];
            }
            entries[address] = constants;
        }

        return new GqrInterproceduralResult(entries, writeMasks, reachable);
    }

    /// <summary>
    /// Per-caller outgoing call sites. <see cref="Targets"/> is round-invariant;
    /// the mask/value payload is overwritten in place each time the caller is
    /// re-evaluated.
    /// </summary>
    private sealed class CompactCallSites
    {
        public CompactCallSites(int[] targets)
        {
            Targets = targets;
            Masks = new byte[targets.Length];
            Values = new uint[targets.Length * GqrCount];
        }

        public int[] Targets { get; }
        public byte[] Masks { get; }
        public uint[] Values { get; }
    }

    private static CompactCallSites[] BuildCompactCallSites(
        GqrFunctionSummary[] summaries,
        Dictionary<uint, int> indices)
    {
        var result = new CompactCallSites[summaries.Length];
        void Build(int index)
        {
            var function = summaries[index];
            var targets = new List<int>();
            for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
            {
                var instructions = function.Blocks[blockIndex].Instructions;
                for (var i = 0; i < instructions.Length; i++)
                {
                    ref readonly var instruction = ref instructions[i];
                    if (instruction.Operation == GqrSummaryOperation.GuestCall &&
                        instruction.CallTargetKnown &&
                        indices.TryGetValue(instruction.CallTarget, out var target))
                        targets.Add(target);
                }
            }
            result[index] = new CompactCallSites(targets.ToArray());
        }

        if (summaries.Length >= ParallelRoundThreshold)
            Parallel.For(0, summaries.Length, CreateParallelOptions(), Build);
        else
            for (var i = 0; i < summaries.Length; i++) Build(i);
        return result;
    }

    private static void EvaluateCompactCallers(
        GqrFunctionSummary[] summaries,
        byte[] entryMasks,
        uint[] entryValues,
        Dictionary<uint, byte> calleeWriteMasks,
        Dictionary<uint, int> indices,
        CompactCallSites[] contributions,
        int[] dirty,
        int dirtyCount)
    {
        if (dirtyCount <= 0) return;

        void Evaluate(int slot)
        {
            var index = dirty[slot];
            EvaluateCompactCaller(
                summaries[index],
                entryMasks[index],
                entryValues,
                index * GqrCount,
                calleeWriteMasks,
                indices,
                contributions[index]);
        }

        // Re-evaluating a caller only reads that caller's summary and the frozen
        // entry snapshot, and only writes that caller's own contribution buffer.
        if (dirtyCount >= ParallelRoundThreshold)
            Parallel.For(0, dirtyCount, CreateParallelOptions(), Evaluate);
        else
            for (var slot = 0; slot < dirtyCount; slot++) Evaluate(slot);
    }

    private static ParallelOptions CreateParallelOptions() =>
        new() { MaxDegreeOfParallelism = Environment.ProcessorCount };

    /// <summary>
    /// Runs the per-block must-constant fixpoint for a single function and records
    /// the constants reaching each outgoing direct call site.
    /// </summary>
    private static void EvaluateCompactCaller(
        GqrFunctionSummary function,
        byte entryMask,
        uint[] entryValues,
        int entryBase,
        Dictionary<uint, byte> calleeWriteMasks,
        Dictionary<uint, int> indices,
        CompactCallSites sites)
    {
        var blockCount = function.Blocks.Count;
        var input = new Dictionary<int, uint>[blockCount];
        var output = new Dictionary<int, uint>[blockCount];
        var maxPredecessors = 0;
        for (var i = 0; i < blockCount; i++)
        {
            input[i] = new Dictionary<int, uint>();
            output[i] = new Dictionary<int, uint>();
            var predecessorCount = function.Blocks[i].Predecessors.Length;
            if (predecessorCount > maxPredecessors) maxPredecessors = predecessorCount;
        }

        var outputInitialized = new bool[blockCount];
        var predecessors = new int[maxPredecessors];
        var state = new Dictionary<int, uint>();
        var iterationLimit = Math.Max(256, blockCount * 4 + 16);
        var converged = true;

        var changed = true;
        var iterations = 0;
        while (changed)
        {
            if (++iterations > iterationLimit)
            {
                converged = false;
                break;
            }

            changed = false;
            for (var blockIndex = 0; blockIndex < blockCount; blockIndex++)
            {
                var block = function.Blocks[blockIndex];
                var isEntry = blockIndex == function.EntryBlock;
                var predecessorCount = 0;
                var blockPredecessors = block.Predecessors;
                for (var i = 0; i < blockPredecessors.Length; i++)
                {
                    var predecessor = blockPredecessors[i];
                    if (predecessor != blockIndex && outputInitialized[predecessor])
                        predecessors[predecessorCount++] = predecessor;
                }
                // An uninitialized predecessor is lattice top, not an empty
                // constant map, so blocks that are not reachable from the entry
                // yet must not be given a bottom state.
                if (!isEntry && predecessorCount == 0) continue;

                state.Clear();
                if (isEntry)
                {
                    for (var slot = 0; slot < GqrCount; slot++)
                        if ((entryMask & (1 << slot)) != 0)
                            state[slot] = entryValues[entryBase + slot];
                }
                else
                {
                    CompactIntersectInto(output, predecessors, predecessorCount, state);
                }

                // The block input is the pre-transfer state; compare before the
                // transfer functions mutate it so the copy is only made when the
                // fact actually moved.
                if (!CompactSame(input[blockIndex], state))
                {
                    input[blockIndex] = new Dictionary<int, uint>(state);
                    changed = true;
                }

                var instructions = block.Instructions;
                for (var i = 0; i < instructions.Length; i++)
                    TransferCompact(function, in instructions[i], state, output, calleeWriteMasks);

                if (!CompactSame(output[blockIndex], state))
                {
                    output[blockIndex] = new Dictionary<int, uint>(state);
                    changed = true;
                }
                if (!outputInitialized[blockIndex])
                {
                    outputInitialized[blockIndex] = true;
                    changed = true;
                }
            }
        }

        if (!converged)
            for (var i = 0; i < blockCount; i++) input[i] = new Dictionary<int, uint>();

        var site = 0;
        for (var blockIndex = 0; blockIndex < blockCount; blockIndex++)
        {
            state.Clear();
            foreach (var pair in input[blockIndex]) state[pair.Key] = pair.Value;
            var instructions = function.Blocks[blockIndex].Instructions;
            for (var i = 0; i < instructions.Length; i++)
            {
                ref readonly var instruction = ref instructions[i];
                if (instruction.Operation == GqrSummaryOperation.GuestCall &&
                    instruction.CallTargetKnown &&
                    indices.ContainsKey(instruction.CallTarget))
                {
                    byte mask = 0;
                    var valueBase = site * GqrCount;
                    foreach (var pair in state)
                    {
                        var affinity = function.GetGqrAffinity(pair.Key);
                        if (affinity < 0) continue;
                        mask |= (byte)(1 << affinity);
                        sites.Values[valueBase + affinity] = pair.Value;
                    }
                    sites.Masks[site] = mask;
                    site++;
                }
                TransferCompact(function, in instruction, state, null, calleeWriteMasks);
            }
        }
    }

    private static Dictionary<uint, byte> ComputeCompactWriteMasks(
        IReadOnlyDictionary<uint, GqrFunctionSummary> functions)
    {
        var count = functions.Count;
        var addresses = new uint[count];
        var indices = new Dictionary<uint, int>(count);
        var position = 0;
        foreach (var address in functions.Keys)
        {
            addresses[position] = address;
            indices[address] = position;
            position++;
        }

        var local = new byte[count];
        var callees = new int[count][];
        var callerCounts = new int[count];
        var known = new List<int>();
        for (var i = 0; i < count; i++)
        {
            var function = functions[addresses[i]];
            var mask = function.HasUnknownGuestOrIndirectCall ? (byte)0xFF : function.LocalWriteMask;
            var targets = function.DirectGuestTargets;
            known.Clear();
            for (var t = 0; t < targets.Count; t++)
            {
                if (indices.TryGetValue(targets[t], out var target)) known.Add(target);
                else mask = 0xFF;
            }
            local[i] = mask;
            var edges = known.ToArray();
            callees[i] = edges;
            for (var e = 0; e < edges.Length; e++) callerCounts[edges[e]]++;
        }

        // Reverse (callee -> caller) edges in CSR form so the union fixpoint can
        // run as a worklist instead of a global round-robin sweep. Both compute
        // the same least fixpoint of a monotone union.
        var callerStart = new int[count + 1];
        for (var i = 0; i < count; i++) callerStart[i + 1] = callerStart[i] + callerCounts[i];
        var callerEdges = new int[callerStart[count]];
        var fill = new int[count];
        Array.Copy(callerStart, fill, count);
        for (var i = 0; i < count; i++)
        {
            var edges = callees[i];
            for (var e = 0; e < edges.Length; e++) callerEdges[fill[edges[e]]++] = i;
        }

        var result = new byte[count];
        Array.Copy(local, result, count);
        var queued = new bool[count];
        var queue = new Queue<int>(count);
        for (var i = 0; i < count; i++)
        {
            queue.Enqueue(i);
            queued[i] = true;
        }
        while (queue.TryDequeue(out var index))
        {
            queued[index] = false;
            var mask = local[index];
            var edges = callees[index];
            for (var e = 0; e < edges.Length; e++) mask |= result[edges[e]];
            if (mask == result[index]) continue;
            result[index] = mask;
            for (var e = callerStart[index]; e < callerStart[index + 1]; e++)
            {
                var caller = callerEdges[e];
                if (queued[caller]) continue;
                queued[caller] = true;
                queue.Enqueue(caller);
            }
        }

        var masks = new Dictionary<uint, byte>(count);
        for (var i = 0; i < count; i++) masks[addresses[i]] = result[i];
        return masks;
    }

    /// <summary>
    /// Unknown-entry roots that depend only on the final call graph, never on discovery order:
    /// <paramref name="fixedRoots"/>, every candidate without a direct caller, then (ascending) any
    /// candidate the roots still don't reach, so no candidate is left without an entry fact source.
    /// </summary>
    public static HashSet<uint> SelectUnknownEntryRoots(
        IReadOnlyDictionary<uint, GqrFunctionSummary> functions,
        IEnumerable<uint> fixedRoots,
        IEnumerable<uint> candidates)
    {
        var called = new HashSet<uint>();
        foreach (var summary in functions.Values)
            foreach (var target in summary.DirectGuestTargets)
                called.Add(target);
        var ordered = candidates.Where(functions.ContainsKey).Distinct().OrderBy(static a => a).ToArray();
        var roots = fixedRoots.Where(functions.ContainsKey).ToHashSet();
        foreach (var address in ordered)
            if (!called.Contains(address))
                roots.Add(address);
        var reachable = ComputeCompactReachable(functions, roots);
        foreach (var address in ordered)
        {
            if (reachable.Contains(address)) continue;
            roots.Add(address);
            ExtendCompactReachable(functions, reachable, address);
        }
        return roots;
    }

    private static HashSet<uint> ComputeCompactReachable(
        IReadOnlyDictionary<uint, GqrFunctionSummary> functions,
        IReadOnlySet<uint> roots)
    {
        var reachable = new HashSet<uint>();
        foreach (var root in roots)
            ExtendCompactReachable(functions, reachable, root);
        return reachable;
    }

    private static void ExtendCompactReachable(
        IReadOnlyDictionary<uint, GqrFunctionSummary> functions,
        HashSet<uint> reachable,
        uint root)
    {
        var queue = new Queue<uint>();
        queue.Enqueue(root);
        while (queue.TryDequeue(out var address))
        {
            if (!reachable.Add(address)) continue;
            foreach (var target in functions[address].DirectGuestTargets)
                if (functions.ContainsKey(target) && !reachable.Contains(target))
                    queue.Enqueue(target);
        }
    }

    private static void TransferCompact(
        GqrFunctionSummary function,
        in GqrSummaryInstruction instruction,
        Dictionary<int, uint> state,
        Dictionary<int, uint>[]? predecessorOut,
        Dictionary<uint, byte> calleeWriteMasks)
    {
        switch (instruction.Operation)
        {
            case GqrSummaryOperation.Assign:
                SetOrKillCompact(instruction.Destination, instruction.Left, state);
                break;
            case GqrSummaryOperation.Binary:
                if (TryCompactValue(instruction.Left, state, out var left) &&
                    TryCompactValue(instruction.Right, state, out var right) &&
                    TryCompactBinary(instruction.BinaryOperation, left, right, out var result))
                    state[instruction.Destination] = result;
                else
                    state.Remove(instruction.Destination);
                break;
            case GqrSummaryOperation.Phi:
            {
                var phiSources = instruction.PhiSources;
                if (predecessorOut is not null && phiSources is { Length: > 0 })
                {
                    uint? value = null;
                    var valid = true;
                    foreach (var source in phiSources)
                    {
                        if (source.Predecessor < 0 ||
                            !predecessorOut[source.Predecessor].TryGetValue(source.Variable, out var incoming) ||
                            (value.HasValue && value.Value != incoming))
                        {
                            valid = false;
                            break;
                        }
                        value = incoming;
                    }
                    if (valid && value.HasValue) state[instruction.Destination] = value.Value;
                    else state.Remove(instruction.Destination);
                }
                else
                    state.Remove(instruction.Destination);
                break;
            }
            case GqrSummaryOperation.Kill:
                state.Remove(instruction.Destination);
                break;
            case GqrSummaryOperation.GuestCall:
            {
                var writeMask = instruction.CallTargetKnown &&
                                calleeWriteMasks.TryGetValue(instruction.CallTarget, out var knownMask)
                    ? knownMask
                    : (byte)0xFF;
                if (writeMask == 0xFF)
                {
                    state.Clear();
                    break;
                }

                byte preservedMask = 0;
                Span<uint> preserved = stackalloc uint[GqrCount];
                foreach (var pair in state)
                {
                    var affinity = function.GetGqrAffinity(pair.Key);
                    if (affinity < 0 || (writeMask & (1 << affinity)) != 0) continue;
                    preservedMask |= (byte)(1 << affinity);
                    preserved[affinity] = pair.Value;
                }
                state.Clear();
                for (var slot = 0; slot < GqrCount; slot++)
                    if ((preservedMask & (1 << slot)) != 0)
                        state[slot] = preserved[slot];
                break;
            }
            case GqrSummaryOperation.IndirectCall:
                state.Clear();
                break;
        }
    }

    private static void SetOrKillCompact(
        int destination,
        GqrSummaryValue value,
        Dictionary<int, uint> state)
    {
        if (TryCompactValue(value, state, out var constant)) state[destination] = constant;
        else state.Remove(destination);
    }

    private static bool TryCompactValue(
        GqrSummaryValue value,
        Dictionary<int, uint> state,
        out uint constant)
    {
        if (value.Kind == 2) { constant = value.Constant; return true; }
        if (value.Kind == 1 && state.TryGetValue(value.Variable, out constant)) return true;
        constant = 0;
        return false;
    }

    private static bool TryCompactBinary(
        GqrBinaryOperation operation,
        uint left,
        uint right,
        out uint value)
    {
        value = operation switch
        {
            GqrBinaryOperation.Add => unchecked(left + right),
            GqrBinaryOperation.Subtract => unchecked(left - right),
            GqrBinaryOperation.Or => left | right,
            GqrBinaryOperation.And => left & right,
            GqrBinaryOperation.Xor => left ^ right,
            GqrBinaryOperation.ShiftLeft => left << ((int)right & 31),
            GqrBinaryOperation.RotateLeft => (left << ((int)right & 31)) | (left >> ((32 - (int)right) & 31)),
            GqrBinaryOperation.ShiftRight => left >> ((int)right & 31),
            _ => 0
        };
        return operation != GqrBinaryOperation.Unknown;
    }

    /// <summary>
    /// Writes the must-constant intersection of the selected predecessor outputs
    /// into <paramref name="destination"/>, preserving the first predecessor's
    /// key order exactly as the copy-then-prune formulation did.
    /// </summary>
    private static void CompactIntersectInto(
        Dictionary<int, uint>[] output,
        int[] predecessors,
        int predecessorCount,
        Dictionary<int, uint> destination)
    {
        var first = output[predecessors[0]];
        foreach (var pair in first)
        {
            var keep = true;
            for (var i = 1; i < predecessorCount; i++)
            {
                if (output[predecessors[i]].TryGetValue(pair.Key, out var value) && value == pair.Value) continue;
                keep = false;
                break;
            }
            if (keep) destination[pair.Key] = pair.Value;
        }
    }

    private static bool CompactSame(
        Dictionary<int, uint> left,
        Dictionary<int, uint> right)
    {
        if (left.Count != right.Count) return false;
        foreach (var pair in left)
            if (!right.TryGetValue(pair.Key, out var value) || value != pair.Value)
                return false;
        return true;
    }

}
