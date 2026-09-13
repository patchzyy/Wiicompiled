using Translator.Core.Disassembly;

namespace Translator.Core.Mods;

// Opaque exits prevent classification; truncated exploration may also hide offsets.
public sealed record LrContinuationAnalysis(IReadOnlyList<int> Offsets, bool IsComplete, bool WasTruncated)
{
    public bool MaySkipReturn => !IsComplete || Offsets.Count != 0;

    public static LrContinuationAnalysis Analyze(
        uint entryPoint,
        Func<uint, IReadOnlyList<PpcInstruction>> discover)
    {
        const int maxFunctions = 256;
        const int maxInstructions = 65536;
        var instructions = new Dictionary<uint, PpcInstruction>();
        var pending = new Queue<uint>();
        var visited = new HashSet<uint>();
        var complete = true;
        var truncated = false;
        pending.Enqueue(entryPoint);

        while (pending.TryDequeue(out var entry))
        {
            if (instructions.ContainsKey(entry) || !visited.Add(entry))
                continue;
            if (visited.Count > maxFunctions)
            {
                complete = false;
                truncated = true;
                break;
            }

            IReadOnlyList<PpcInstruction> body;
            try
            {
                body = discover(entry);
            }
            catch (Exception ex) when (ex is InvalidOperationException or ArgumentException
                or IndexOutOfRangeException or NotSupportedException or OverflowException)
            {
                complete = false;
                truncated = true;
                continue;
            }
            if (!body.Any(instruction => instruction.Address == entry) ||
                instructions.Count + body.Count > maxInstructions)
            {
                complete = false;
                truncated = true;
                continue;
            }

            foreach (var instruction in body)
                instructions.TryAdd(instruction.Address, instruction);
            foreach (var instruction in body)
            {
                if (!instruction.IsCall)
                {
                    foreach (var target in instruction.BranchTargets)
                        if (!instructions.ContainsKey(target)) pending.Enqueue(target);
                }
                if ((!instruction.IsReturn && !instruction.IsUnconditionalBranch) ||
                    instruction.IsConditionalBranch)
                {
                    if (!instructions.ContainsKey(instruction.EndAddress))
                        pending.Enqueue(instruction.EndAddress);
                }
            }
        }

        if (!instructions.TryGetValue(entryPoint, out var first))
            return new LrContinuationAnalysis([], false, true);

        // Keep the original entry first, including when a tail target precedes it.
        var ordered = new[] { first }.Concat(instructions.Values
            .Where(instruction => instruction.Address != entryPoint)
            .OrderBy(instruction => instruction.Address)).ToArray();
        var offsets = ContinuationPlanner.DiscoverLrRelativeIndirectJumpOffsets(
            ordered, () => { complete = false; truncated = true; }, () => complete = false).ToArray();
        return new LrContinuationAnalysis(offsets, complete, truncated);
    }
}
