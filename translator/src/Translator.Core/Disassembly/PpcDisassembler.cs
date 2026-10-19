using System;
using System.Buffers;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Linq;
using Translator.Core.Loading;

namespace Translator.Core.Disassembly;

/// <summary>
/// Simple disassembler that walks reachable instructions using the in-process PPC decoder.
/// Optimized for demand-driven decoding to avoid decoding unreachable instructions.
/// </summary>
public sealed class PpcDisassemblyLimitExceededException : InvalidOperationException
{
    public PpcDisassemblyLimitExceededException(
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        int decodedInstructions,
        uint blockedAddress)
        : base(
            $"Disassembly budget exhausted for 0x{entryPoint:X8} after {decodedInstructions} reachable instructions; " +
            $"next reachable address was 0x{blockedAddress:X8} (maxInstructions={maxInstructions}, maxBytes=0x{maxBytes:X}). " +
            "Increase the disassembly budget before trusting the generated control flow.")
    {
        EntryPoint = entryPoint;
        MaxInstructions = maxInstructions;
        MaxBytes = maxBytes;
        DecodedInstructions = decodedInstructions;
        BlockedAddress = blockedAddress;
    }

    public uint EntryPoint { get; }
    public int MaxInstructions { get; }
    public int MaxBytes { get; }
    public int DecodedInstructions { get; }
    public uint BlockedAddress { get; }
}

/// <summary>
/// Records every membership question asked of the known-function entry point set.
/// A cached decode is replayable only while all of these still answer the same way.
/// </summary>
public sealed class BoundaryProbeLog
{
    private List<uint>? _absent;

    /// <summary>Addresses that were asked about and were not boundaries.</summary>
    public IReadOnlyList<uint> AbsentBoundaries => (IReadOnlyList<uint>?)_absent ?? Array.Empty<uint>();

    public void RecordAbsent(uint address)
    {
        _absent ??= new List<uint>();
        if (!_absent.Contains(address))
        {
            _absent.Add(address);
        }
    }

    /// <summary>
    /// True while every recorded question still has the same answer. Boundary
    /// sets only ever grow during a run, so only the negative answers can flip.
    /// </summary>
    public bool IsStillValid(IReadOnlySet<uint>? knownFunctionEntryPoints)
    {
        if (_absent is null || knownFunctionEntryPoints is null)
        {
            return true;
        }

        for (var i = 0; i < _absent.Count; i++)
        {
            if (knownFunctionEntryPoints.Contains(_absent[i]))
            {
                return false;
            }
        }

        return true;
    }
}

public sealed class PpcDisassembler : IDisposable
{
    public IReadOnlyList<PpcInstruction> DisassembleFunction(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions = 256,
        int maxBytes = 0x800,
        IReadOnlySet<uint>? knownFunctionEntryPoints = null,
        BoundaryProbeLog? boundaryProbes = null)
    {
        var instructions = DisassembleFunctionCore(
            image, entryPoint, maxInstructions, maxBytes, knownFunctionEntryPoints, boundaryProbes, throwOnBudget: true);
        return instructions!;
    }

    /// <summary>
    /// Budget-tolerant variant for speculative decoding (leaf inlining), where an
    /// oversized body is a normal "not a candidate" answer, not an error to throw and swallow.
    /// </summary>
    public IReadOnlyList<PpcInstruction>? TryDisassembleFunction(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        IReadOnlySet<uint>? knownFunctionEntryPoints = null,
        BoundaryProbeLog? boundaryProbes = null)
        => DisassembleFunctionCore(
            image, entryPoint, maxInstructions, maxBytes, knownFunctionEntryPoints, boundaryProbes, throwOnBudget: false);

    private static IReadOnlyList<PpcInstruction>? DisassembleFunctionCore(
        ProgramImage image,
        uint entryPoint,
        int maxInstructions,
        int maxBytes,
        IReadOnlySet<uint>? knownFunctionEntryPoints,
        BoundaryProbeLog? boundaryProbes,
        bool throwOnBudget)
    {
        if (!image.Contains(entryPoint, sizeof(uint)))
        {
            throw new ArgumentOutOfRangeException(nameof(entryPoint), $"Address 0x{entryPoint:X8} is outside the loaded RAM image.");
        }
        var offset = image.GetOffset(entryPoint, sizeof(uint));

        var available = Math.Min(maxBytes, image.Memory.Length - offset);

        // Demand-driven decoding: decode only the instructions we actually visit
        var instructionMap = new Dictionary<uint, PpcInstruction>();
        var reachable = new HashSet<uint>();
        var localControlFlowTargets = new HashSet<uint>();
        var worklist = new Queue<uint>();
        worklist.Enqueue(entryPoint);

        var endAddress = entryPoint + (uint)available;

        // Ordered view used by jump-table recognition, rebuilt lazily when the decoded set grows.
        List<PpcInstruction>? orderedCache = null;
        Dictionary<uint, int>? indexByAddressCache = null;
        var orderedCacheCount = -1;

        while (worklist.Count > 0)
        {
            if (reachable.Count >= maxInstructions)
            {
                if (!throwOnBudget)
                {
                    return null;
                }

                throw new PpcDisassemblyLimitExceededException(
                    entryPoint,
                    maxInstructions,
                    maxBytes,
                    reachable.Count,
                    worklist.Peek());
            }

            var cursor = worklist.Dequeue();
            while (cursor >= entryPoint && cursor < endAddress)
            {
                if (reachable.Count >= maxInstructions)
                {
                    if (!throwOnBudget)
                    {
                        return null;
                    }

                    throw new PpcDisassemblyLimitExceededException(
                        entryPoint,
                        maxInstructions,
                        maxBytes,
                        reachable.Count,
                        cursor);
                }

                // Decode on demand
                if (!instructionMap.TryGetValue(cursor, out var ins))
                {
                    if (!image.Contains(cursor, sizeof(uint)))
                    {
                        break;
                    }
                    var cursorOffset = image.GetOffset(cursor, sizeof(uint));
                    var word = BinaryPrimitives.ReadUInt32BigEndian(image.Memory.AsSpan(cursorOffset, 4));
                    ins = PpcDecoder.Decode(cursor, word);
                    instructionMap[cursor] = ins;
                }

                if (!reachable.Add(ins.Address))
                {
                    break;
                }

                var recognizedJumpTable = false;
                if (ins.BranchTargets.Count == 0 && string.Equals(ins.Mnemonic, "bctr", StringComparison.OrdinalIgnoreCase))
                {
                    // For jump tables, we need the ordered list - build it lazily
                    if (orderedCache is null || indexByAddressCache is null || orderedCacheCount != instructionMap.Count)
                    {
                        orderedCache = instructionMap.Values.OrderBy(i => i.Address).ToList();
                        indexByAddressCache = new Dictionary<uint, int>(orderedCache.Count);
                        for (var idx = 0; idx < orderedCache.Count; idx++)
                        {
                            indexByAddressCache[orderedCache[idx].Address] = idx;
                        }

                        orderedCacheCount = instructionMap.Count;
                    }

                    if (JumpTableDetector.TryRecognize(orderedCache, indexByAddressCache, ins.Address, entryPoint, endAddress, image, out var jtTargets))
                    {
                        ins = new PpcInstruction(ins.Address, ins.Word, ins.Mnemonic, ins.Operands, jtTargets, isReturn: false, ins.IsCall, ins.IsConditionalBranch);
                        instructionMap[ins.Address] = ins;
                        // Same address, so the ordered view's count is unchanged but its content is stale; drop it.
                        orderedCache = null;
                        indexByAddressCache = null;
                        orderedCacheCount = -1;
                        foreach (var target in jtTargets)
                        {
                            localControlFlowTargets.Add(target);
                        }
                        recognizedJumpTable = true;
                    }
                }

                if (ins.IsConditionalBranch)
                {
                    foreach (var target in ins.BranchTargets)
                    {
                        // A conditional call to another function returns to its fallthrough.
                        if (ins.IsCall && IsKnownExternalFunctionEntry(target))
                        {
                            continue;
                        }
                        localControlFlowTargets.Add(target);
                        if (!reachable.Contains(target))
                        {
                            worklist.Enqueue(target);
                        }
                    }
                    cursor = ins.EndAddress;
                    continue;
                }

                if (ins.IsUnconditionalBranch)
                {
                    foreach (var target in ins.BranchTargets)
                    {
                        if ((recognizedJumpTable || !IsKnownExternalFunctionEntry(target)) && !reachable.Contains(target))
                        {
                            worklist.Enqueue(target);
                        }
                    }
                    break; // no fallthrough
                }

                if (ins.IsReturn)
                {
                    break;
                }

                cursor = ins.EndAddress;
            }
        }

        var addresses = new uint[reachable.Count];
        reachable.CopyTo(addresses);
        Array.Sort(addresses);
        var ordered = new List<PpcInstruction>(addresses.Length);
        foreach (var address in addresses)
        {
            ordered.Add(instructionMap[address]);
        }

        return ordered;

        bool IsKnownExternalFunctionEntry(uint target)
        {
            if (target == entryPoint || localControlFlowTargets.Contains(target))
            {
                return false;
            }

            if (knownFunctionEntryPoints is null)
            {
                return false;
            }

            var known = knownFunctionEntryPoints.Contains(target);
            if (!known)
            {
                boundaryProbes?.RecordAbsent(target);
            }

            return known;
        }
    }

    public void Dispose()
    {
        // nothing to dispose currently
    }
}
