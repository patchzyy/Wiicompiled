using System;
using System.Collections.Generic;
using System.Linq;
using System.Globalization;
using Translator.Core.Ir;
using Translator.Core.Analysis;
using Translator.Core.Analysis.Ssa;

namespace Translator.Core.CodeGen;

/// <summary>
/// Converts repeated accesses through one SSA base into an explicit resolved
/// guest-memory range. Lifetimes are deliberately bounded by calls and block
/// edges; widening across CFG edges is a separate dominance proof.
/// </summary>
public static class GuestMemoryRangeLowering
{
    private const int MaxRangeLength = 4096;

    // Hardware register window (gather pipe, CP/PE/PI registers, EFB aperture below 0xCE000000).
    // Nothing here is backed by a host mapping, so hoisting it would only feed the cold unresolved
    // chain, and it would also rewrite gather-pipe stores out of the IrStore shape that
    // TryEmitKnownGpuFifoStore needs for its direct GX_HLE_FIFO_Write* fast path. Keep it unhoisted.
    private const uint HardwareRegisterWindowStart = 0xCC000000u;
    private const uint HardwareRegisterWindowEnd = 0xCE000000u;

    public static IrFunction Lower(IrFunction function, int minimumAccesses = 8,
        bool ignoreDisabledPcTraces = false,
        bool scalarOnly = false)
    {
        var nextRange = 0;
        var hardwareConstants = CxxLinearCodeGenerator.BuildKnownUIntConstants(function);
        function = LowerFunctionLiveInRanges(
            function, ref nextRange, minimumAccesses, ignoreDisabledPcTraces,
            hardwareConstants);
        var blocks = function.Blocks.Select(block =>
            block with { Instructions = CombineAdjacentLoads(
                LowerBlock(block.Instructions, ref nextRange, minimumAccesses,
                    scalarOnly, hardwareConstants),
                ignoreDisabledPcTraces) }).ToArray();
        return function with { Blocks = blocks };
    }

    /// <summary>
    /// True when the instruction's effective address is a compile-time constant inside the
    /// hardware register window. Must use the same constant map as <see cref="CxxLinearCodeGenerator.BuildKnownUIntConstants"/>
    /// so every gather-pipe write the emitter would classify is excluded here too.
    /// </summary>
    internal static bool IsProvenHardwareRegisterAccess(
        IrInstruction instruction, IReadOnlyDictionary<string, uint>? constants)
    {
        if (constants is null || constants.Count == 0)
            return false;
        var address = instruction switch
        {
            IrLoad load => load.Address,
            IrStore store => store.Address,
            IrCall call when TryParsePsq(call, out _) &&
                             call.Arguments[0].RegisterName is { } psqBase =>
                new IrAddress(psqBase, 0),
            _ => null
        };
        if (address is null || !constants.TryGetValue(address.Base, out var baseAddress))
            return false;
        var effective = unchecked(baseAddress + (uint)address.Offset);
        return effective >= HardwareRegisterWindowStart && effective < HardwareRegisterWindowEnd;
    }

    /// <summary>
    /// Reuses scalar ranges across CFG edges only when the first access dominates every consumer
    /// and no invalidating call can occur on any path between them (the cross-CFG memory epoch proof).
    /// </summary>
    private static IrFunction LowerFunctionLiveInRanges(
        IrFunction function, ref int nextRange, int minimumAccesses,
        bool ignoreDisabledPcTraces,
        IReadOnlyDictionary<string, uint>? hardwareConstants = null)
    {
        var definitions = new Dictionary<string, List<DefinitionSite>>(StringComparer.OrdinalIgnoreCase);
        for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
        for (var instructionIndex = 0;
             instructionIndex < function.Blocks[blockIndex].Instructions.Count;
             instructionIndex++)
        {
            foreach (var destination in DefinedDestinations(
                         function.Blocks[blockIndex].Instructions[instructionIndex]))
            {
                if (!definitions.TryGetValue(destination, out var sites))
                    definitions[destination] = sites = new List<DefinitionSite>();
                sites.Add(new DefinitionSite(blockIndex, instructionIndex));
            }
        }
        var candidates = new List<FunctionAccess>();
        for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
        {
            foreach (var access in BuildAccesses(function.Blocks[blockIndex].Instructions,
                         normalizeArchitecturalRegisters: false, hardwareConstants))
                if (access.Psq is null)
                    candidates.Add(new FunctionAccess(blockIndex, access));
        }
        if (!candidates.GroupBy(candidate => new AccessClass(
                candidate.Access.Address.Base, candidate.Access.IsStore))
            .Any(group => group.Count() >= minimumAccesses))
            return function;

        var graph = BlockGraph.Build(function);

        var plans = new Dictionary<(int Block, int Instruction), Plan>();
        var resolves = new Dictionary<int, List<PlacedResolve>>();
        foreach (var group in candidates.GroupBy(candidate => new AccessClass(
                     candidate.Access.Address.Base, candidate.Access.IsStore)))
        {
            var remaining = group.ToList();
            if (remaining.Count < minimumAccesses) continue;
            if (remaining.Any(candidate => HasOverlappingOppositeAccess(
                    candidate.Access, candidates.Select(static item => item.Access))))
                continue;
            var invalidators = CollectInvalidators(function, group.Key.Base);
            DefinitionSite? definition = null;
            if (definitions.TryGetValue(group.Key.Base, out var definitionSites))
            {
                // The SSA value must have one authoritative definition and it
                // must dominate every consumer.  Reused version names on
                // mutually-exclusive edges are conservatively rejected.
                if (definitionSites.Count != 1) continue;
                definition = definitionSites[0];
                var defined = definitionSites[0];
                if (remaining.Any(candidate =>
                        !graph.Dominates(defined.BlockIndex, candidate.BlockIndex) ||
                        candidate.BlockIndex == defined.BlockIndex &&
                        candidate.Access.Index <= defined.InstructionIndex))
                    continue;
            }
            // The pair predicate is invariant across rounds, so memoize it.
            var groupIndex = new Dictionary<FunctionAccess, int>(ReferenceEqualityComparer.Instance);
            foreach (var access in remaining) groupIndex.Add(access, groupIndex.Count);
            var compatible = remaining.Count <= 4096 ? new sbyte[remaining.Count * remaining.Count] : null;
            bool Compatible(FunctionAccess anchor, FunctionAccess candidate)
            {
                if (compatible is null)
                    return DominatesAccess(graph, anchor, candidate) &&
                        !HasInvalidatingBoundaryOnPath(graph, invalidators, anchor, candidate);
                var slot = groupIndex[anchor] * groupIndex.Count + groupIndex[candidate];
                if (compatible[slot] != 0) return compatible[slot] > 0;
                var value = DominatesAccess(graph, anchor, candidate) &&
                    !HasInvalidatingBoundaryOnPath(graph, invalidators, anchor, candidate);
                compatible[slot] = value ? (sbyte)1 : (sbyte)-1;
                return value;
            }
            while (remaining.Count >= minimumAccesses)
            {
                FunctionAccess? bestAnchor = null;
                FunctionAccess[] bestMembers = [];
                foreach (var anchor in remaining)
                {
                    var members = remaining.Where(candidate => Compatible(anchor, candidate))
                        .OrderBy(candidate => candidate.Access.Index)
                        .ToList();
                    // Keep a bounded affine window around the real anchor. A
                    // distant outlier must not suppress a useful safe fragment.
                    var selected = new List<FunctionAccess>();
                    long windowMin = anchor.Access.Address.Offset;
                    long windowMax = windowMin + anchor.Access.SizeBytes;
                    foreach (var candidate in members.OrderBy(candidate =>
                                 Math.Abs((long)candidate.Access.Address.Offset - anchor.Access.Address.Offset)))
                    {
                        var nextMin = Math.Min(windowMin, candidate.Access.Address.Offset);
                        var nextMax = Math.Max(windowMax,
                            (long)candidate.Access.Address.Offset + candidate.Access.SizeBytes);
                        if (nextMax - nextMin > MaxRangeLength) continue;
                        windowMin = nextMin;
                        windowMax = nextMax;
                        selected.Add(candidate);
                    }
                    if (selected.Count > bestMembers.Length)
                    {
                        bestAnchor = anchor;
                        bestMembers = selected.ToArray();
                    }
                }
                if (bestAnchor is null || bestMembers.Length < minimumAccesses) break;

                var min = bestMembers.Min(static candidate => (long)candidate.Access.Address.Offset);
                var max = bestMembers.Max(static candidate =>
                    (long)candidate.Access.Address.Offset + candidate.Access.SizeBytes);
                var range = $"guest_range_{nextRange++}";
                var plan = new Plan(range, (int)min, (int)(max - min),
                    NeedsRead: !group.Key.IsStore, NeedsWrite: group.Key.IsStore);
                var resolve = new IrResolveGuestMemoryRange(range, IrValue.Register(group.Key.Base),
                    plan.MinOffset, plan.Length, plan.NeedsRead, plan.NeedsWrite);
                if (!resolves.TryGetValue(bestAnchor.BlockIndex, out var placed))
                    resolves[bestAnchor.BlockIndex] = placed = new List<PlacedResolve>();
                // Placement is immediately before an access that executes on
                // every path reaching the remaining consumers.
                placed.Add(new PlacedResolve(bestAnchor.Access.Index - 1, resolve));
                foreach (var candidate in bestMembers)
                {
                    plans[(candidate.BlockIndex, candidate.Access.Index)] = plan;
                    remaining.Remove(candidate);
                }
            }
        }
        if (resolves.Count == 0) return function;

        var blocks = new IrBasicBlock[function.Blocks.Count];
        for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
        {
            var source = function.Blocks[blockIndex].Instructions;
            var accessMap = BuildAccesses(source, normalizeArchitecturalRegisters: false, hardwareConstants)
                .ToDictionary(static access => access.Index);
            resolves.TryGetValue(blockIndex, out var blockResolves);
            var output = new List<IrInstruction>(source.Count + (blockResolves?.Count ?? 0));
            // LLVM requires the phi set to stay a contiguous block prefix, so any resolve placed
            // at or before a leading phi must be emitted after the whole phi set instead.
            var leadingPhiEnd = -1;
            var sawMaterialInstruction = false;
            for (var index = 0; index < source.Count; ++index)
            {
                if (source[index] is IrPhi && !sawMaterialInstruction)
                {
                    leadingPhiEnd = index;
                    continue;
                }
                if (source[index] is IrComment or IrTracePpc) continue;
                sawMaterialInstruction = true;
            }
            var placedAfter = blockResolves?.GroupBy(placed =>
                placed.AfterInstructionIndex < 0 ||
                placed.AfterInstructionIndex <= leadingPhiEnd &&
                source[placed.AfterInstructionIndex] is IrPhi
                    ? leadingPhiEnd
                    : placed.AfterInstructionIndex)
                .ToDictionary(static group => group.Key,
                    static group => group.Select(placed => placed.Resolve).ToArray());
            if (placedAfter is not null && leadingPhiEnd < 0 && placedAfter.Remove(-1, out var entryResolves))
                output.AddRange(entryResolves);
            for (var instructionIndex = 0; instructionIndex < source.Count; instructionIndex++)
            {
                if (!plans.TryGetValue((blockIndex, instructionIndex), out var plan))
                {
                    output.Add(source[instructionIndex]);
                    if (placedAfter is not null && placedAfter.TryGetValue(instructionIndex, out var afterUnchanged))
                        output.AddRange(afterUnchanged);
                    continue;
                }
                var access = accessMap[instructionIndex];
                output.Add(ToResolved(source[instructionIndex], access, plan));
                if (placedAfter is not null && placedAfter.TryGetValue(instructionIndex, out var afterResolved))
                    output.AddRange(afterResolved);
            }
            blocks[blockIndex] = function.Blocks[blockIndex] with
                { Instructions = CombineAdjacentLoads(output, ignoreDisabledPcTraces) };
        }
        return function with { Blocks = blocks };
    }

    private static bool DominatesAccess(
        BlockGraph graph,
        FunctionAccess anchor,
        FunctionAccess consumer)
    {
        if (anchor.BlockIndex == consumer.BlockIndex)
            return anchor.Access.Index <= consumer.Access.Index;
        return graph.Dominates(anchor.BlockIndex, consumer.BlockIndex);
    }

    private static bool HasOverlappingOppositeAccess(
        Access access, IEnumerable<Access> candidates)
    {
        var accessStart = (long)access.Address.Offset;
        var accessEnd = accessStart + access.SizeBytes;
        return candidates.Any(candidate =>
            candidate.IsStore != access.IsStore &&
            candidate.Address.Base.Equals(
                access.Address.Base, StringComparison.OrdinalIgnoreCase) &&
            accessStart < (long)candidate.Address.Offset + candidate.SizeBytes &&
            candidate.Address.Offset < accessEnd);
    }

    private static bool HasInvalidatingBoundaryOnPath(
        BlockGraph graph,
        IReadOnlyList<InstructionSite> invalidators,
        FunctionAccess anchor,
        FunctionAccess consumer)
    {
        var anchorBlock = anchor.BlockIndex;
        var consumerBlock = consumer.BlockIndex;
        var consumerReentered = anchorBlock != consumerBlock && graph.Cyclic[consumerBlock];
        foreach (var invalidator in invalidators)
        {
            var boundaryBlock = invalidator.BlockIndex;
            var afterAnchor = boundaryBlock == anchorBlock
                ? invalidator.InstructionIndex > anchor.Access.Index
                : graph.Reaches(anchorBlock, boundaryBlock);
            if (!afterAnchor) continue;
            var beforeConsumer = boundaryBlock == consumerBlock
                ? invalidator.InstructionIndex < consumer.Access.Index || consumerReentered
                : graph.Reaches(boundaryBlock, consumerBlock);
            if (beforeConsumer) return true;
        }
        return false;
    }

    private static IReadOnlyList<InstructionSite> CollectInvalidators(
        IrFunction function, string baseName)
    {
        var result = new List<InstructionSite>();
        for (var blockIndex = 0; blockIndex < function.Blocks.Count; blockIndex++)
        for (var instructionIndex = 0;
             instructionIndex < function.Blocks[blockIndex].Instructions.Count;
             instructionIndex++)
        {
            var instruction = function.Blocks[blockIndex].Instructions[instructionIndex];
            if (IsMemoryEpochBoundary(instruction) ||
                Defines(instruction, baseName, normalizeArchitecturalRegisters: true))
                result.Add(new InstructionSite(blockIndex, instructionIndex));
        }
        return result;
    }

    /// <summary>Dominator and reachability sets as bitsets over block ordinals.</summary>
    private sealed class BlockGraph
    {
        private readonly ulong[][] _dominators;
        private readonly ulong[][] _reach;

        private BlockGraph(ulong[][] dominators, ulong[][] reach, bool[] cyclic)
        {
            _dominators = dominators;
            _reach = reach;
            Cyclic = cyclic;
        }

        /// <summary>Blocks that can reach themselves again through a successor.</summary>
        public bool[] Cyclic { get; }

        public bool Dominates(int dominator, int block) =>
            (_dominators[block][dominator >> 6] & (1UL << (dominator & 63))) != 0;

        /// <summary>Reflexive: every block reaches itself.</summary>
        public bool Reaches(int from, int to) =>
            (_reach[from][to >> 6] & (1UL << (to & 63))) != 0;

        public static BlockGraph Build(IrFunction function)
        {
            var cfg = IrCfg.Build(function);
            var count = function.Blocks.Count;
            var words = (count + 63) >> 6;
            var index = new Dictionary<string, int>(count, StringComparer.OrdinalIgnoreCase);
            for (var i = 0; i < count; i++) index.Add(function.Blocks[i].Label, i);
            var successors = new int[count][];
            var predecessors = new int[count][];
            for (var i = 0; i < count; i++)
            {
                var label = function.Blocks[i].Label;
                successors[i] = cfg.Successors(label).Select(successor => index[successor]).ToArray();
                predecessors[i] = cfg.Predecessors(label).Select(predecessor => index[predecessor]).ToArray();
            }

            var reach = new ulong[count][];
            var pending = new Stack<int>();
            for (var start = 0; start < count; start++)
            {
                var row = new ulong[words];
                pending.Push(start);
                while (pending.Count != 0)
                {
                    var current = pending.Pop();
                    ref var word = ref row[current >> 6];
                    var bit = 1UL << (current & 63);
                    if ((word & bit) != 0) continue;
                    word |= bit;
                    foreach (var successor in successors[current]) pending.Push(successor);
                }
                reach[start] = row;
            }

            var cyclic = new bool[count];
            for (var i = 0; i < count; i++)
            {
                foreach (var successor in successors[i])
                {
                    if ((reach[successor][i >> 6] & (1UL << (i & 63))) == 0) continue;
                    cyclic[i] = true;
                    break;
                }
            }

            var entry = index[cfg.Entry];
            var dominators = new ulong[count][];
            for (var i = 0; i < count; i++)
            {
                dominators[i] = new ulong[words];
                if (i == entry)
                {
                    dominators[i][i >> 6] = 1UL << (i & 63);
                    continue;
                }
                Array.Fill(dominators[i], ulong.MaxValue);
                if ((count & 63) != 0) dominators[i][words - 1] = (1UL << (count & 63)) - 1;
            }
            var next = new ulong[words];
            bool changed;
            do
            {
                changed = false;
                for (var i = 0; i < count; i++)
                {
                    if (i == entry) continue;
                    var preds = predecessors[i];
                    if (preds.Length == 0)
                    {
                        Array.Clear(next);
                    }
                    else
                    {
                        Array.Copy(dominators[preds[0]], next, words);
                        for (var p = 1; p < preds.Length; p++)
                        {
                            var other = dominators[preds[p]];
                            for (var w = 0; w < words; w++) next[w] &= other[w];
                        }
                    }
                    next[i >> 6] |= 1UL << (i & 63);
                    if (next.AsSpan().SequenceEqual(dominators[i])) continue;
                    next.AsSpan().CopyTo(dominators[i]);
                    changed = true;
                }
            } while (changed);
            return new BlockGraph(dominators, reach, cyclic);
        }
    }

    private static IrInstruction ToResolved(IrInstruction instruction, Access access, Plan plan)
    {
        var relativeOffset = access.Address.Offset - plan.MinOffset;
        return instruction switch
        {
            IrLoad load => new IrResolvedLoad(load.Destination, plan.Range, load.Address,
                relativeOffset, load.SizeBytes),
            IrStore store => new IrResolvedStore(plan.Range, store.Address, relativeOffset,
                store.Source, store.SizeBytes),
            IrCall call when access.Psq is { IsStore: false } psq =>
                new IrResolvedPsqLoad(call.Destination, plan.Range, call.Arguments[0], relativeOffset,
                    psq.W, psq.I, psq.KnownGqr, psq.GuardKnownGqr),
            IrCall call when access.Psq is { IsStore: true } psq =>
                new IrResolvedPsqStore(plan.Range, call.Arguments[0], relativeOffset, call.Arguments[1],
                    psq.W, psq.I, psq.KnownGqr, psq.GuardKnownGqr),
            _ => instruction
        };
    }

    private static IReadOnlyList<IrInstruction> CombineAdjacentLoads(
        IReadOnlyList<IrInstruction> instructions, bool ignoreDisabledPcTraces)
    {
        var result = new List<IrInstruction>(instructions.Count);
        for (var index = 0; index < instructions.Count; index++)
        {
            var next = index + 1;
            if (instructions[index] is IrResolvedLoad first)
            {
                while (next < instructions.Count &&
                       ((ignoreDisabledPcTraces && instructions[next] is IrTracePpc) ||
                        IsMovableAddressCalculation(instructions[next], first.Destination))) next++;
            }
            else if (instructions[index] is IrResolvedStore)
            {
                while (next < instructions.Count &&
                       ((ignoreDisabledPcTraces && instructions[next] is IrTracePpc) ||
                        IsMovableAddressCalculation(instructions[next], null))) next++;
            }

            if (next < instructions.Count &&
                instructions[index] is IrResolvedLoad firstLoad &&
                instructions[next] is IrResolvedLoad secondLoad &&
                firstLoad.Range.Equals(secondLoad.Range, StringComparison.Ordinal) &&
                firstLoad.SizeBytes == secondLoad.SizeBytes && firstLoad.SizeBytes is 2 or 4 &&
                Math.Abs(secondLoad.RangeOffset - firstLoad.RangeOffset) == firstLoad.SizeBytes)
            {
                for (var movable = index + 1; movable < next; movable++) result.Add(instructions[movable]);
                var descending = secondLoad.RangeOffset < firstLoad.RangeOffset;
                result.Add(new IrResolvedLoadPair(firstLoad.Destination, secondLoad.Destination, firstLoad.Range,
                    firstLoad.OriginalAddress, secondLoad.OriginalAddress,
                    Math.Min(firstLoad.RangeOffset, secondLoad.RangeOffset), firstLoad.SizeBytes, descending));
                index = next;
                continue;
            }
            if (next < instructions.Count &&
                instructions[index] is IrResolvedStore firstStore &&
                instructions[next] is IrResolvedStore secondStore &&
                firstStore.Range.Equals(secondStore.Range, StringComparison.Ordinal) &&
                firstStore.SizeBytes == secondStore.SizeBytes && firstStore.SizeBytes is 2 or 4 &&
                Math.Abs(secondStore.RangeOffset - firstStore.RangeOffset) == firstStore.SizeBytes)
            {
                for (var movable = index + 1; movable < next; movable++) result.Add(instructions[movable]);
                var descending = secondStore.RangeOffset < firstStore.RangeOffset;
                result.Add(new IrResolvedStorePair(firstStore.Range, firstStore.OriginalAddress,
                    secondStore.OriginalAddress, Math.Min(firstStore.RangeOffset, secondStore.RangeOffset),
                    firstStore.Source, secondStore.Source, firstStore.SizeBytes, descending));
                index = next;
                continue;
            }
            result.Add(instructions[index]);
        }
        return result;
    }

    private static bool IsMovableAddressCalculation(IrInstruction instruction, string? forbiddenUse)
    {
        static bool Uses(IrValue value, string name) =>
            value.RegisterName?.Equals(name, StringComparison.OrdinalIgnoreCase) == true;

        return instruction switch
        {
            IrAssign assign when !IsArchitecturalRegister(assign.Destination) =>
                forbiddenUse is null || !Uses(assign.Value, forbiddenUse),
            IrBinary binary when !IsArchitecturalRegister(binary.Destination) =>
                forbiddenUse is null || (!Uses(binary.Left, forbiddenUse) && !Uses(binary.Right, forbiddenUse)),
            _ => false
        };
    }

    private static IReadOnlyList<IrInstruction> LowerBlock(
        IReadOnlyList<IrInstruction> instructions,
        ref int nextRange,
        int minimumAccesses,
        bool scalarOnly,
        IReadOnlyDictionary<string, uint>? hardwareConstants = null)
    {
        var result = new List<IrInstruction>(instructions.Count);
        var segment = new List<IrInstruction>();
        var rangeCounter = nextRange;

        void Flush()
        {
            if (segment.Count == 0) return;
            LowerSegment(segment, result, ref rangeCounter, minimumAccesses,
                scalarOnly, hardwareConstants);
            segment.Clear();
        }

        foreach (var instruction in instructions)
        {
            if (IsOwnershipBoundary(instruction))
            {
                Flush();
                result.Add(instruction);
            }
            else
            {
                segment.Add(instruction);
            }
        }
        Flush();
        nextRange = rangeCounter;
        return result;
    }

    private static void LowerSegment(
        IReadOnlyList<IrInstruction> segment,
        List<IrInstruction> output,
        ref int nextRange,
        int minimumAccesses,
        bool scalarOnly,
        IReadOnlyDictionary<string, uint>? hardwareConstants = null)
    {
        var accesses = BuildAccesses(segment,
                normalizeArchitecturalRegisters: true, hardwareConstants)
            .Where(access => !scalarOnly || access.Psq is null)
            .ToArray();
        var accessesByIndex = accesses.ToDictionary(static access => access.Index);
        var plans = new Dictionary<int, Plan>();

        foreach (var group in accesses.GroupBy(access =>
                     new AccessClass(access.Address.Base, access.IsStore)))
        {
            var grouped = group.OrderBy(static access => access.Index).ToArray();
            if (grouped.Length < 2) continue;
            // Resolving a range costs about as much as one checked access, so require enough
            // consumers to amortize it; a per-function cache for just two accesses measurably
            // increased stack traffic and CPU time.
            if (grouped.Length < minimumAccesses) continue;
            if (grouped.Any(access => HasOverlappingOppositeAccess(access, accesses)))
                continue;
            var min = grouped.Min(static access => (long)access.Address.Offset);
            var max = grouped.Max(static access => (long)access.Address.Offset + access.SizeBytes);
            var length = max - min;
            if (length <= 0 || length > MaxRangeLength) continue;

            var range = $"guest_range_{nextRange++}";
            var plan = new Plan(range, (int)min, (int)length,
                grouped.Any(static access => !access.IsStore), grouped.Any(static access => access.IsStore));
            foreach (var access in grouped) plans[access.Index] = plan;
        }

        var emitted = new HashSet<string>(StringComparer.Ordinal);
        for (var index = 0; index < segment.Count; index++)
        {
            if (!plans.TryGetValue(index, out var plan))
            {
                output.Add(segment[index]);
                continue;
            }

            var access = accessesByIndex[index];
            if (emitted.Add(plan.Range))
            {
                // The anchor's raw base register holds root + BaseDelta at
                // this point, so shifting the canonical minimum back by the
                // delta makes the emitted expression equal root + MinOffset.
                output.Add(new IrResolveGuestMemoryRange(
                    plan.Range, IrValue.Register(access.OriginalBase ?? access.Address.Base),
                    checked(plan.MinOffset - access.BaseDelta),
                    plan.Length, plan.NeedsRead, plan.NeedsWrite));
            }

            output.Add(ToResolved(segment[index], access, plan));
        }
    }

    private static bool IsOwnershipBoundary(IrInstruction instruction) => instruction switch
    {
        IrCall call => IsMemoryEpochBoundary(call),
        IrIndirectCall or IrBranch or IrJump or IrIndirectJump or IrJumpTable or IrReturn or IrUndefined => true,
        _ => false
    };

    // Catalog effects, rather than helper-name prefixes, define transparency.
    // Unknown/complete-context helpers, guest re-entry, suspension, thread
    // switching, and hidden GPR writes all end the pointer lifetime.
    private static bool IsMemoryEpochBoundary(IrInstruction instruction) => instruction switch
    {
        IrCall call => IsMemoryEpochBoundary(GuestHelperEffectCatalog.Analyze(call)),
        IrIndirectCall => true,
        _ => false
    };

    private static bool IsMemoryEpochBoundary(GuestHelperEffect effect)
    {
        const GuestCallBoundaryFlags unsafeFlags =
            GuestCallBoundaryFlags.RequiresCompleteContext |
            GuestCallBoundaryFlags.CanSuspend |
            GuestCallBoundaryFlags.CanSwitchThreads |
            GuestCallBoundaryFlags.InvokesGuestCode;
        return effect.GprWriteMask != 0 || (effect.BoundaryFlags & unsafeFlags) != 0;
    }

    private static bool Defines(IrInstruction instruction, string name,
        bool normalizeArchitecturalRegisters = true)
    {
        foreach (var destination in DefinedDestinations(instruction))
        {
            var matches = normalizeArchitecturalRegisters
                ? NormalizeRegisterIdentity(destination).Equals(
                    NormalizeRegisterIdentity(name), StringComparison.OrdinalIgnoreCase)
                : destination.Equals(name, StringComparison.OrdinalIgnoreCase);
            if (matches) return true;
        }
        return false;
    }

    private static IEnumerable<Access> BuildAccesses(IReadOnlyList<IrInstruction> segment,
        bool normalizeArchitecturalRegisters = true,
        IReadOnlyDictionary<string, uint>? hardwareConstants = null)
    {
        var affine = new Dictionary<string, (string Root, int Offset)>(StringComparer.OrdinalIgnoreCase);
        // psq_stu/stwu-style pointer walks redefine the architectural base on
        // every step. Tracking the redefinition as an affine update keeps the
        // whole chain in one canonical root; an untrackable write instead
        // starts a new generation ("r10#2") so accesses through different
        // runtime values can never share a range proof.
        var generations = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

        (string Root, int Offset) Lookup(string name)
        {
            if (affine.TryGetValue(name, out var known)) return known;
            var normalized = normalizeArchitecturalRegisters
                ? NormalizeRegisterIdentity(name) : name;
            if (normalizeArchitecturalRegisters && IsArchitecturalRegister(name))
            {
                if (!normalized.Equals(name, StringComparison.OrdinalIgnoreCase) &&
                    affine.TryGetValue(normalized, out var knownNormalized))
                    return knownNormalized;
                if (generations.TryGetValue(normalized, out var generation) && generation > 0)
                    return (normalized + "#" + generation, 0);
            }
            return (normalized, 0);
        }

        void InvalidateDestination(string destination)
        {
            if (normalizeArchitecturalRegisters && IsArchitecturalRegister(destination))
            {
                var normalized = NormalizeRegisterIdentity(destination);
                generations[normalized] =
                    generations.TryGetValue(normalized, out var generation) ? generation + 1 : 1;
                affine.Remove(normalized);
                return;
            }
            affine.Remove(destination);
        }

        for (var index = 0; index < segment.Count; index++)
        {
            var instruction = segment[index];
            // A hardware-register access is deliberately not an access at all
            // as far as range proofs are concerned; the affine tracker below
            // still observes the instruction's register writes.
            var raw = IsProvenHardwareRegisterAccess(instruction, hardwareConstants)
                ? null
                : Access.TryCreate(instruction, index);
            if (raw is not null)
            {
                var (root, delta) = Lookup(raw.Address.Base);
                var canonical = new IrAddress(root, checked(delta + raw.Address.Offset));
                yield return raw with
                {
                    Address = canonical,
                    OriginalBase = raw.Address.Base,
                    BaseDelta = delta,
                };
            }

            switch (instruction)
            {
                case IrAssign assign when assign.Value is { Kind: "register", RegisterName: { } source }:
                    if (IsArchitecturalRegister(assign.Destination))
                    {
                        if (normalizeArchitecturalRegisters)
                            affine[NormalizeRegisterIdentity(assign.Destination)] = Lookup(source);
                        else affine.Remove(assign.Destination);
                    }
                    else affine[assign.Destination] = Lookup(source);
                    break;
                case IrBinary binary when binary.Op.Equals("add", StringComparison.OrdinalIgnoreCase):
                    if (TryAffineAdd(binary, Lookup, out var value) &&
                        (normalizeArchitecturalRegisters || !IsArchitecturalRegister(binary.Destination)))
                    {
                        affine[normalizeArchitecturalRegisters &&
                               IsArchitecturalRegister(binary.Destination)
                            ? NormalizeRegisterIdentity(binary.Destination)
                            : binary.Destination] = value;
                    }
                    else InvalidateDestination(binary.Destination);
                    break;
                default:
                    foreach (var destination in DefinedDestinations(instruction))
                        InvalidateDestination(destination);
                    break;
            }
        }
    }

    private static bool TryAffineAdd(IrBinary binary,
        Func<string, (string Root, int Offset)> lookup,
        out (string Root, int Offset) result)
    {
        result = default;
        IrValue register;
        long constant;
        if (binary.Left is { Kind: "register" } && binary.Right is { Kind: "const", Constant: { } right })
        { register = binary.Left; constant = right; }
        else if (binary.Right is { Kind: "register" } && binary.Left is { Kind: "const", Constant: { } left })
        { register = binary.Right; constant = left; }
        else return false;
        if (register.RegisterName is null || constant is < int.MinValue or > int.MaxValue) return false;
        var baseValue = lookup(register.RegisterName);
        var sum = (long)baseValue.Offset + constant;
        if (sum is < int.MinValue or > int.MaxValue) return false;
        result = (baseValue.Root, (int)sum);
        return true;
    }

    /// <summary>Every register this instruction writes. Must delegate to
    /// <see cref="IrRegisterDataFlow.Definitions"/>, not a local node-kind switch: a prior switch missed
    /// resolved-load kinds, which shared one range root across loads and caused the minimap shadow-pane
    /// black-icon bug in CtrlRace2DMapCharacter::calcTransform.</summary>
    private static IEnumerable<string> DefinedDestinations(IrInstruction instruction) =>
        IrRegisterDataFlow.Definitions(instruction);

    private static string NormalizeRegisterIdentity(string name)
    {
        if (!IsArchitecturalRegister(name)) return name;
        var separator = name.IndexOf('_');
        return separator < 0 ? name : name[..separator];
    }

    private static bool IsArchitecturalRegister(string name)
    {
        var separator = name.IndexOf('_');
        var baseName = separator < 0 ? name : name[..separator];
        if (separator >= 0 && !name[(separator + 1)..].All(char.IsDigit)) return false;
        if (baseName.Length < 2) return false;
        if (baseName[0] is not ('r' or 'R' or 'f' or 'F')) return false;
        return int.TryParse(baseName.AsSpan(1), out var index) && index is >= 0 and < 32;
    }

    // Address is the canonical (root-relative) form used for grouping.
    // OriginalBase/BaseDelta reconstruct the anchor-time expression: the raw
    // base register plus the affine delta the tracker had accumulated for it,
    // so a resolve emitted at the anchor can reference a register that is
    // itself walked forward later in the chain.
    private sealed record Access(int Index, IrAddress Address, int SizeBytes, bool IsStore, PsqAccess? Psq = null,
        string? OriginalBase = null, int BaseDelta = 0)
    {
        public static Access? TryCreate(IrInstruction instruction, int index) => instruction switch
        {
            IrLoad load => new Access(index, load.Address, load.SizeBytes, false),
            IrStore store => new Access(index, store.Address, store.SizeBytes, true),
            IrCall call when TryParsePsq(call, out var psq) && call.Arguments[0].RegisterName is { } address =>
                new Access(index, new IrAddress(address, 0), psq.W == 0 ? 8 : 4, psq.IsStore, psq),
            _ => null
        };
    }

    private sealed record PsqAccess(bool IsStore, uint W, uint I, uint? KnownGqr, bool GuardKnownGqr);

    private static bool TryParsePsq(IrCall call, out PsqAccess psq)
    {
        psq = null!;
        var isLoad = call.Target.Equals("PPC_PsqL", StringComparison.OrdinalIgnoreCase) ||
                     call.Target.StartsWith("PPC_PsqLKnown_", StringComparison.OrdinalIgnoreCase) ||
                     call.Target.StartsWith("PPC_PsqLKnownGuarded_", StringComparison.OrdinalIgnoreCase);
        var isStore = call.Target.Equals("PPC_PsqSt", StringComparison.OrdinalIgnoreCase) ||
                      call.Target.StartsWith("PPC_PsqStKnown_", StringComparison.OrdinalIgnoreCase) ||
                      call.Target.StartsWith("PPC_PsqStKnownGuarded_", StringComparison.OrdinalIgnoreCase);
        if ((!isLoad && !isStore) || call.Arguments.Count < (isLoad ? 3 : 4) ||
            call.Arguments[0] is not { Kind: "register", RegisterName: not null }) return false;
        var wArg = call.Arguments[isLoad ? 1 : 2];
        var iArg = call.Arguments[isLoad ? 2 : 3];
        if (wArg.Constant is not (0 or 1) || iArg.Constant is < 0 or > 7) return false;
        uint? known = null;
        var guardedMarker = isLoad ? "PPC_PsqLKnownGuarded_" : "PPC_PsqStKnownGuarded_";
        var knownMarker = isLoad ? "PPC_PsqLKnown_" : "PPC_PsqStKnown_";
        var guarded = call.Target.StartsWith(guardedMarker, StringComparison.OrdinalIgnoreCase);
        var marker = guarded ? guardedMarker : knownMarker;
        if (call.Target.StartsWith(marker, StringComparison.OrdinalIgnoreCase) &&
            uint.TryParse(call.Target.AsSpan(marker.Length), NumberStyles.HexNumber,
                CultureInfo.InvariantCulture, out var parsed)) known = parsed;
        psq = new PsqAccess(isStore, (uint)wArg.Constant.GetValueOrDefault(),
            (uint)iArg.Constant.GetValueOrDefault(), known, guarded);
        return true;
    }

    private sealed record Plan(string Range, int MinOffset, int Length, bool NeedsRead, bool NeedsWrite);
    private sealed record FunctionAccess(int BlockIndex, Access Access)
    {
        public string AccessBlockLabel(IrFunction function) => function.Blocks[BlockIndex].Label;
    }

    private sealed record DefinitionSite(int BlockIndex, int InstructionIndex);
    private sealed record AccessClass(string Base, bool IsStore)
    {
        public bool Equals(AccessClass? other) => other is not null &&
            Base.Equals(other.Base, StringComparison.OrdinalIgnoreCase) &&
            IsStore == other.IsStore;

        public override int GetHashCode() => HashCode.Combine(
            StringComparer.OrdinalIgnoreCase.GetHashCode(Base),
            IsStore);
    }
    private sealed record InstructionSite(int BlockIndex, int InstructionIndex);
    private sealed record PlacedResolve(int AfterInstructionIndex, IrResolveGuestMemoryRange Resolve);
}
