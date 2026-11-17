using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading.Tasks;
using Translator.Core.Ir;

namespace Translator.Core.Analysis;

public sealed record GuestAbiInterproceduralResult(
    IReadOnlyDictionary<uint, GuestAbiContract> Contracts,
    IReadOnlyList<IReadOnlyList<uint>> StronglyConnectedComponents);

/// <summary>
/// Computes architectural effects in guest execution order, so a callee read is suppressed once
/// the caller already defined that register, unlike a transitive union of descendant masks.
/// </summary>
public static class GuestAbiInterproceduralAnalyzer
{
    public static GuestAbiInterproceduralResult Analyze(
        IReadOnlyDictionary<uint, IrFunction> functions,
        IReadOnlyDictionary<uint, GuestAbiContract>? externalContracts = null,
        bool retainBodies = true,
        int? maxDegreeOfParallelism = null)
    {
        externalContracts ??= new Dictionary<uint, GuestAbiContract>();
        var addresses = functions.Keys.OrderBy(static address => address).ToArray();

        // GuestAbiContractAnalyzer.Analyze is pure and the round's contract snapshot is frozen, so
        // per-function analyses within a round can run on any thread. Determinism comes from
        // applying results serially in ascending-ordinal order, matching the old single-threaded loop.
        var parallelOptions = new ParallelOptions
        {
            MaxDegreeOfParallelism = maxDegreeOfParallelism ?? Environment.ProcessorCount
        };

        // Lazy callers must supply stable bodies and support concurrent reads.
        var bodies = retainBodies ? new IrFunction[addresses.Length] : null;
        if (bodies is not null)
        {
            for (var index = 0; index < addresses.Length; ++index) bodies[index] = functions[addresses[index]];
        }
        IrFunction Body(int index) => bodies is not null ? bodies[index] : functions[addresses[index]];

        // Mirror of `contracts` keyed by ordinal.  The fixpoint compares against
        // it from the parallel phase, where the dictionary must stay untouched.
        var current = new GuestAbiContract[addresses.Length];
        Parallel.For(
            0,
            addresses.Length,
            parallelOptions,
            index => current[index] = GuestAbiContractAnalyzer.Analyze(Body(index)));

        var contracts = new Dictionary<uint, GuestAbiContract>(addresses.Length);
        for (var index = 0; index < addresses.Length; ++index) contracts.Add(addresses[index], current[index]);

        var ordinals = new Dictionary<uint, int>(addresses.Length);
        for (var index = 0; index < addresses.Length; ++index) ordinals.Add(addresses[index], index);

        // Callers of each function. A function whose callees all kept their contracts reproduces
        // the same contract and can be skipped for the round; that's the only liberty taken, since
        // rounds still see a snapshot fixed at round start, preserving the original fixed point.
        // The edge scan itself is a read-only IR walk, collected in parallel per ordinal then
        // stitched serially in ascending caller order to match the single-threaded result.
        var directCallees = new int[addresses.Length][];
        Parallel.For(
            0,
            addresses.Length,
            parallelOptions,
            static () => new HashSet<int>(),
            (index, _, scratch) =>
            {
                scratch.Clear();
                CollectDirectCallees(Body(index), ordinals, scratch);
                directCallees[index] = scratch.ToArray();
                return scratch;
            },
            static _ => { });

        var callers = new List<int>?[addresses.Length];
        for (var index = 0; index < addresses.Length; ++index)
        {
            foreach (var callee in directCallees[index]) (callers[callee] ??= new List<int>()).Add(index);
        }

        var visible = new Dictionary<uint, GuestAbiContract>(externalContracts);
        foreach (var (address, contract) in contracts) visible[address] = contract;

        var pending = new bool[addresses.Length];
        var nextRound = new bool[addresses.Length];
        Array.Fill(pending, true);
        // Ascending ordinals of the functions this round re-analyzes, plus the
        // slot each one writes its result into.  Both are reused every round.
        var worklist = new int[addresses.Length];
        var roundResults = new GuestAbiContract?[addresses.Length];
        while (true)
        {
            var worklistCount = 0;
            for (var index = 0; index < addresses.Length; ++index)
            {
                if (pending[index]) worklist[worklistCount++] = index;
            }

            Parallel.For(0, worklistCount, parallelOptions, position =>
            {
                var ordinal = worklist[position];
                var next = GuestAbiContractAnalyzer.Analyze(Body(ordinal), visible);
                roundResults[position] = Equivalent(current[ordinal], next) ? null : next;
            });

            Array.Clear(nextRound);
            var updated = false;
            for (var position = 0; position < worklistCount; ++position)
            {
                var contract = roundResults[position];
                if (contract is null) continue;
                updated = true;
                var ordinal = worklist[position];
                var address = addresses[ordinal];
                current[ordinal] = contract;
                contracts[address] = contract;
                visible[address] = contract;
                var callerList = callers[ordinal];
                if (callerList is null) continue;
                foreach (var caller in callerList) nextRound[caller] = true;
            }

            if (!updated) break;

            (pending, nextRound) = (nextRound, pending);
        }

        return new GuestAbiInterproceduralResult(contracts, BuildComponents(functions, contracts));
    }

    /// <summary>
    /// Ordinals of functions this one calls directly, from raw IR rather than
    /// <see cref="GuestAbiContract.DirectCallTargets"/>, which omits inline save/restore thunks
    /// that the analyzer still consults when they happen to have a contract.
    /// </summary>
    private static void CollectDirectCallees(
        IrFunction function,
        Dictionary<uint, int> ordinals,
        HashSet<int> result)
    {
        foreach (var block in function.Blocks)
        {
            foreach (var instruction in block.Instructions)
            {
                if (instruction is IrCall call &&
                    GuestTargetParser.TryParseAddress(call.Target, out var target) &&
                    ordinals.TryGetValue(target, out var ordinal))
                {
                    result.Add(ordinal);
                }
            }
        }
    }

    private static bool Equivalent(GuestAbiContract left, GuestAbiContract right) =>
        left.GprReadBeforeWriteMask == right.GprReadBeforeWriteMask &&
        left.GprPossibleWriteMask == right.GprPossibleWriteMask &&
        left.GprReturnMask == right.GprReturnMask &&
        left.FprReadBeforeWriteMask == right.FprReadBeforeWriteMask &&
        left.FprPossibleWriteMask == right.FprPossibleWriteMask &&
        left.FprReturnMask == right.FprReturnMask &&
        left.CrReadBeforeWriteMask == right.CrReadBeforeWriteMask &&
        left.CrPossibleWriteMask == right.CrPossibleWriteMask &&
        left.ReadsXerBeforeWrite == right.ReadsXerBeforeWrite &&
        left.MayWriteXer == right.MayWriteXer &&
        left.ReadsCtrBeforeWrite == right.ReadsCtrBeforeWrite &&
        left.MayWriteCtr == right.MayWriteCtr &&
        left.ReadsLrBeforeWrite == right.ReadsLrBeforeWrite &&
        left.MayWriteLr == right.MayWriteLr &&
        left.ReadsFpscrBeforeWrite == right.ReadsFpscrBeforeWrite &&
        left.MayWriteFpscr == right.MayWriteFpscr &&
        left.GqrReadBeforeWriteMask == right.GqrReadBeforeWriteMask &&
        left.GqrPossibleWriteMask == right.GqrPossibleWriteMask &&
        left.HidReadBeforeWriteMask == right.HidReadBeforeWriteMask &&
        left.HidPossibleWriteMask == right.HidPossibleWriteMask &&
        left.GprDefiniteWriteMask == right.GprDefiniteWriteMask &&
        left.FprDefiniteWriteMask == right.FprDefiniteWriteMask &&
        left.CrDefiniteWriteMask == right.CrDefiniteWriteMask &&
        left.DefinitelyWritesXer == right.DefinitelyWritesXer &&
        left.DefinitelyWritesCtr == right.DefinitelyWritesCtr &&
        left.DefinitelyWritesLr == right.DefinitelyWritesLr &&
        left.DefinitelyWritesFpscr == right.DefinitelyWritesFpscr &&
        left.GqrDefiniteWriteMask == right.GqrDefiniteWriteMask &&
        left.HidDefiniteWriteMask == right.HidDefiniteWriteMask &&
        left.BoundaryFlags == right.BoundaryFlags &&
        SameTargets(left.DirectCallTargets, right.DirectCallTargets);

    private static bool SameTargets(IReadOnlyList<uint> left, IReadOnlyList<uint> right)
    {
        if (ReferenceEquals(left, right)) return true;
        if (left.Count != right.Count) return false;
        for (var index = 0; index < left.Count; ++index)
        {
            if (left[index] != right[index]) return false;
        }

        return true;
    }

    private static IReadOnlyList<IReadOnlyList<uint>> BuildComponents(
        IReadOnlyDictionary<uint, IrFunction> functions,
        IReadOnlyDictionary<uint, GuestAbiContract> contracts)
    {
        // Iterative Kosaraju, not recursive Tarjan: production titles have call chains deep enough
        // to exhaust the native .NET stack before a recursive Tarjan discovers a component.
        var addresses = functions.Keys.OrderBy(static address => address).ToArray();
        var adjacency = new Dictionary<uint, uint[]>(addresses.Length);
        var reverse = addresses.ToDictionary(static address => address, static _ => new List<uint>());
        foreach (var address in addresses)
        {
            var targets = contracts[address].DirectCallTargets
                .Where(functions.ContainsKey)
                .Distinct()
                .OrderBy(static target => target)
                .ToArray();
            adjacency[address] = targets;
            foreach (var target in targets) reverse[target].Add(address);
        }

        var visited = new HashSet<uint>();
        var finishOrder = new List<uint>(addresses.Length);
        foreach (var root in addresses)
        {
            if (!visited.Add(root)) continue;
            var traversal = new Stack<(uint Address, int NextTarget)>();
            traversal.Push((root, 0));
            while (traversal.Count != 0)
            {
                var (address, nextTarget) = traversal.Pop();
                var targets = adjacency[address];
                if (nextTarget < targets.Length)
                {
                    traversal.Push((address, nextTarget + 1));
                    var target = targets[nextTarget];
                    if (visited.Add(target)) traversal.Push((target, 0));
                    continue;
                }

                finishOrder.Add(address);
            }
        }

        visited.Clear();
        var result = new List<IReadOnlyList<uint>>();
        for (var orderIndex = finishOrder.Count - 1; orderIndex >= 0; --orderIndex)
        {
            var root = finishOrder[orderIndex];
            if (!visited.Add(root)) continue;
            var component = new List<uint>();
            var traversal = new Stack<uint>();
            traversal.Push(root);
            while (traversal.Count != 0)
            {
                var address = traversal.Pop();
                component.Add(address);
                foreach (var caller in reverse[address])
                    if (visited.Add(caller)) traversal.Push(caller);
            }

            component.Sort();
            result.Add(component);
        }

        result.Sort(static (left, right) => left[0].CompareTo(right[0]));
        return result;
    }
}
