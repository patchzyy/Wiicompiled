using System.Buffers.Binary;
using System.Collections.Immutable;
using System.Text.Json;
using Translator.Core.Disassembly;
using Translator.Core.Parsing.Kamek;
using Translator.Core.Mods.Mkwii;

namespace Translator.Core.Mods;

public sealed record ContinuationEntry(
    uint Address,
    uint ContainingFunctionStart,
    uint ContainingFunctionEnd,
    string SectionName,
    uint SourceCommandAddress,
    KamekCommandId SourceCommandId,
    string Reason);

public sealed class ContinuationPlan
{
    public required IReadOnlyList<ContinuationEntry> Entries { get; init; }
}

public static class ContinuationPlanner
{
    private const uint BctrInstruction = 0x4E800420u;
    private const uint BlrInstruction = 0x4E800020u;
    private const int MaxTailJumpConstantLookbackBytes = 64;

    public static ContinuationPlan Build(KamekChunk chunk, BaseManifest baseManifest, uint moduleGuestBase)
    {
        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);
        var entries = new Dictionary<uint, ContinuationEntry>();

        foreach (var command in chunk.Commands)
        {
            if (!IsBranchLikeTarget(command.Id) || command.Arguments.Count == 0)
            {
                continue;
            }

            var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
            var section = FindSection(baseManifest, target);
            if (section is null || !section.Executable)
            {
                continue;
            }

            var function = functionIndex.FindContaining(target);
            if (function is null || function.Start == target)
            {
                continue;
            }

            entries.TryAdd(target, new ContinuationEntry(
                target,
                function.Start,
                function.End,
                section.Name,
                command.AddressIsRelative ? checked(moduleGuestBase + command.Address) : command.Address,
                command.Id,
                "branch-like Code.pul target lands inside a base function"));
        }

        return new ContinuationPlan
        {
            Entries = entries.Values.OrderBy(e => e.Address).ToList()
        };
    }

    public static ContinuationPlan AddModuleTailJumpContinuations(
        ContinuationPlan plan,
        BaseManifest baseManifest,
        uint moduleGuestBase,
        byte[] relocatedModuleImage)
    {
        if (relocatedModuleImage.Length < 4)
        {
            return plan;
        }

        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);
        var entries = plan.Entries.ToDictionary(e => e.Address);

        foreach (var tailJump in DiscoverModuleTailJumps(moduleGuestBase, relocatedModuleImage))
        {
            var section = FindSection(baseManifest, tailJump.TargetAddress);
            if (section is null || !section.Executable)
            {
                continue;
            }

            var function = functionIndex.FindContaining(tailJump.TargetAddress);
            if (function is null || function.Start == tailJump.TargetAddress)
            {
                continue;
            }

            entries.TryAdd(tailJump.TargetAddress, new ContinuationEntry(
                tailJump.TargetAddress,
                function.Start,
                function.End,
                section.Name,
                tailJump.SourceAddress,
                KamekCommandId.Branch,
                "Kamek module tail jump lands inside a base function"));
        }

        return new ContinuationPlan
        {
            Entries = entries.Values.OrderBy(e => e.Address).ToList()
        };
    }

    public static ContinuationPlan AddRetroWfcExecutableHookContinuations(
        ContinuationPlan plan,
        BaseManifest baseManifest,
        IEnumerable<RetroWfcExecutableHookPlan> hooks)
    {
        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);
        var entries = plan.Entries.ToDictionary(e => e.Address);

        foreach (var hook in hooks)
        {
            var target = hook.ContinuationAddress;
            var section = FindSection(baseManifest, target);
            if (section is null || !section.Executable)
            {
                continue;
            }

            var function = functionIndex.FindContaining(target);
            if (function is null || function.Start == target)
            {
                continue;
            }

            var action = hook.TargetActionId ?? string.Join(",", hook.SemanticActionIds);
            entries.TryAdd(target, new ContinuationEntry(
                target,
                function.Start,
                function.End,
                section.Name,
                hook.Address,
                KamekCommandId.Branch,
                $"Retro WFC executable hook continuation {action}"));
        }

        return new ContinuationPlan
        {
            Entries = entries.Values.OrderBy(e => e.Address).ToList()
        };
    }


    private static bool IsBranchLikeTarget(KamekCommandId id) =>
        id is KamekCommandId.Rel24 or KamekCommandId.Branch or KamekCommandId.BranchLink;

    private static BaseSectionMetadata? FindSection(BaseManifest manifest, uint address) =>
        manifest.Sections.FirstOrDefault(section => address >= section.GuestStart && address < section.GuestEnd);

    private static IEnumerable<ModuleTailJump> DiscoverModuleTailJumps(
        uint moduleGuestBase,
        byte[] relocatedModuleImage)
    {
        for (var offset = 0; offset + 4 <= relocatedModuleImage.Length; offset += 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
            if (word != BctrInstruction && word != BlrInstruction)
            {
                continue;
            }

            var sprWriteOffset = offset - 4;
            if (sprWriteOffset < 0)
            {
                continue;
            }

            var sprWrite = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, sprWriteOffset);
            int sourceRegister;
            var hasRegisterSource = word == BctrInstruction
                ? PpcInstructionPatterns.TryGetMtspr(sprWrite, 9, out sourceRegister)
                : PpcInstructionPatterns.TryGetMtspr(sprWrite, 8, out sourceRegister);
            if (!hasRegisterSource)
            {
                continue;
            }

            if (!TryResolveConstantRegisterValue(
                    relocatedModuleImage,
                    sprWriteOffset,
                    sourceRegister,
                    out var targetAddress))
            {
                continue;
            }

            yield return new ModuleTailJump(
                checked(moduleGuestBase + (uint)offset),
                targetAddress);
        }
    }

    private static bool TryResolveConstantRegisterValue(
        byte[] relocatedModuleImage,
        int beforeOffset,
        int register,
        out uint value)
    {
        var lowOperation = LowImmediateOperation.None;
        var lowImmediate = 0u;
        var scanStart = Math.Max(0, beforeOffset - MaxTailJumpConstantLookbackBytes);

        for (var offset = beforeOffset - 4; offset >= scanStart; offset -= 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);

            if (PpcInstructionPatterns.TryGetOri(word, out var oriSource, out var oriDestination, out var oriImmediate) &&
                oriDestination == register)
            {
                if (oriSource != register || lowOperation != LowImmediateOperation.None)
                {
                    break;
                }

                lowOperation = LowImmediateOperation.Or;
                lowImmediate = oriImmediate;
                continue;
            }

            if (PpcInstructionPatterns.TryGetAddi(word, out var addiDestination, out var addiSource, out var addiImmediate) &&
                addiDestination == register)
            {
                if (addiSource != register || lowOperation != LowImmediateOperation.None)
                {
                    break;
                }

                lowOperation = LowImmediateOperation.AddSigned;
                lowImmediate = unchecked((uint)addiImmediate);
                continue;
            }

            if (PpcInstructionPatterns.TryGetLis(word, out var lisDestination, out var highImmediate) &&
                lisDestination == register)
            {
                var baseValue = highImmediate << 16;
                value = lowOperation switch
                {
                    LowImmediateOperation.None => baseValue,
                    LowImmediateOperation.Or => baseValue | lowImmediate,
                    LowImmediateOperation.AddSigned => unchecked(baseValue + (uint)(short)lowImmediate),
                    _ => baseValue
                };
                return true;
            }

            if (PpcRegisterEffects.MayWriteGpr(word, register))
            {
                break;
            }
        }

        value = 0;
        return false;
    }

    private sealed record ModuleTailJump(uint SourceAddress, uint TargetAddress);

    private enum LowImmediateOperation
    {
        None,
        Or,
        AddSigned
    }

    /// <summary>
    /// Per-instruction cap on distinct explored path states. Raised well past what any
    /// observed function needs (a handful of states per branch/loop join) so the cap is a
    /// backstop against pathological blowup, not a limit routine code is expected to hit -
    /// callers that need a trustworthy "no offsets" result should still treat an
    /// onStateCapExceeded callback as "this run could not prove that" (see EmitModCpp's
    /// TargetExhibitsLrSkipReturn), since a state dropped here can never contribute its
    /// bctr/return offset to the result.
    /// </summary>
    private const int MaxStatesPerInstruction = 512;

    public static IEnumerable<int> DiscoverLrRelativeIndirectJumpOffsets(
        IReadOnlyList<PpcInstruction> instructions,
        Action? onStateCapExceeded = null)
    {
        if (instructions.Count == 0)
        {
            yield break;
        }

        var indexByAddress = new Dictionary<uint, int>(instructions.Count);
        for (var i = 0; i < instructions.Count; i++)
        {
            indexByAddress.TryAdd(instructions[i].Address, i);
        }

        var visited = new HashSet<PathState>[instructions.Count];
        for (var i = 0; i < instructions.Count; i++)
        {
            visited[i] = new HashSet<PathState>();
        }

        var seenOffsets = new HashSet<int>();
        var worklist = new Queue<(int Index, PathState State)>();

        void Enqueue(int targetIndex, PathState stateToEnqueue)
        {
            worklist.Enqueue((targetIndex, stateToEnqueue));
        }

        int? GetFallthroughIndex(PpcInstruction instruction)
        {
            if (indexByAddress.TryGetValue(instruction.EndAddress, out var nextIndex))
            {
                return nextIndex;
            }

            return null;
        }

        Enqueue(0, PathState.Empty);

        while (worklist.Count > 0)
        {
            var (idx, state) = worklist.Dequeue();
            if (!visited[idx].Add(state))
            {
                continue;
            }

            if (visited[idx].Count > MaxStatesPerInstruction)
            {
                onStateCapExceeded?.Invoke();
                continue;
            }

            var instruction = instructions[idx];
            var mnemonic = instruction.Mnemonic.ToLowerInvariant();
            var nextState = state;

            if (mnemonic == "mflr" && TryGetInstructionReg(instruction, 0, out var lrDest))
            {
                if (lrDest == "r1")
                {
                    nextState = nextState.WithClearedStackOffsets();
                }

                nextState = nextState.LrReturnOffset.HasValue
                    ? nextState.WithLrOffset(lrDest, nextState.LrReturnOffset.Value)
                    : nextState.WithoutLrOffset(lrDest);
            }
            else if ((mnemonic == "mr" || mnemonic == "or") &&
                TryGetInstructionReg(instruction, 0, out var moveDest) &&
                TryGetInstructionReg(instruction, 1, out var moveSource) &&
                (mnemonic == "mr" ||
                 (instruction.Operands.Count >= 3 &&
                  instruction.Operands[2] is PpcRegisterOperand moveSource2 &&
                  string.Equals(NormalizeInstructionReg(moveSource2.Name), moveSource, StringComparison.OrdinalIgnoreCase))))
            {
                if (moveDest == "r1" && moveSource != "r1")
                {
                    nextState = nextState.WithClearedStackOffsets();
                }
                nextState = nextState.LrOffsets.TryGetValue(moveSource, out var sourceOffset)
                    ? nextState.WithLrOffset(moveDest, sourceOffset)
                    : nextState.WithoutLrOffset(moveDest);
            }
            else if ((mnemonic == "addi" || mnemonic == "addic") &&
                TryGetInstructionReg(instruction, 0, out var addDest) &&
                TryGetInstructionReg(instruction, 1, out var addBase) &&
                TryGetInstructionImm(instruction, 2, out var imm))
            {
                if (addDest == "r1")
                {
                    nextState = addBase == "r1"
                        ? nextState.WithSpDelta(unchecked(nextState.SpDelta + imm))
                        : nextState.WithClearedStackOffsets();
                }
                nextState = nextState.LrOffsets.TryGetValue(addBase, out var baseOffset)
                    ? nextState.WithLrOffset(addDest, unchecked(baseOffset + imm))
                    : nextState.WithoutLrOffset(addDest);
            }
            else if (mnemonic == "mtctr" && TryGetInstructionReg(instruction, 0, out var ctrSource))
            {
                var newCtrOffset = nextState.LrOffsets.TryGetValue(ctrSource, out var sourceOffset) ? sourceOffset : (int?)null;
                nextState = nextState.WithCtrOffset(newCtrOffset);
            }
            else if (mnemonic == "mtlr" && TryGetInstructionReg(instruction, 0, out var lrSource))
            {
                var newLrReturnOffset = nextState.LrOffsets.TryGetValue(lrSource, out var sourceOffset) ? sourceOffset : (int?)null;
                nextState = nextState.WithLrReturnOffset(newLrReturnOffset);
            }
            else if (mnemonic == "stw" &&
                TryGetInstructionReg(instruction, 0, out var storeSrc) &&
                TryGetInstructionDisplacement(instruction, 1, out var storeDisp, out var storeBase, out _))
            {
                if (storeBase == "r1")
                {
                    var targetSlot = nextState.SpDelta + storeDisp;
                    nextState = nextState.LrOffsets.TryGetValue(storeSrc, out var offset)
                        ? nextState.WithStackOffset(targetSlot, offset)
                        : nextState.WithoutStackOffset(targetSlot);
                }
            }
            else if (mnemonic == "stwu" &&
                TryGetInstructionReg(instruction, 0, out var stwuSrc) &&
                TryGetInstructionDisplacement(instruction, 1, out var stwuDisp, out var stwuBase, out _))
            {
                if (stwuBase == "r1")
                {
                    var targetSlot = nextState.SpDelta + stwuDisp;
                    nextState = nextState.LrOffsets.TryGetValue(stwuSrc, out var offset)
                        ? nextState.WithStackOffset(targetSlot, offset)
                        : nextState.WithoutStackOffset(targetSlot);
                    nextState = nextState.WithAdjustedStackPointer(stwuDisp);
                }
                else
                {
                    nextState = nextState.WithoutLrOffset(stwuBase);
                }
            }
            else if (TryGetStackStoreRange(instruction, out var storeOffset, out var storeSize, out var updatesStackPointer))
            {
                nextState = nextState.WithoutStackOffsetsInRange(
                    nextState.SpDelta + storeOffset,
                    storeSize);
                if (updatesStackPointer)
                {
                    nextState = nextState.WithAdjustedStackPointer(storeOffset);
                }
            }
            else if (mnemonic == "lwz" &&
                TryGetInstructionReg(instruction, 0, out var loadDest) &&
                TryGetInstructionDisplacement(instruction, 1, out var loadDisp, out var loadBase, out _))
            {
                if (loadBase == "r1")
                {
                    var targetSlot = nextState.SpDelta + loadDisp;
                    var hasStackOffset = nextState.StackOffsets.TryGetValue(targetSlot, out var offset);

                    if (loadDest == "r1")
                    {
                        nextState = nextState.WithClearedStackOffsets();
                    }

                    nextState = hasStackOffset
                        ? nextState.WithLrOffset(loadDest, offset)
                        : nextState.WithoutLrOffset(loadDest);
                }
                else
                {
                    nextState = nextState.WithoutLrOffset(loadDest);
                    if (loadDest == "r1")
                    {
                        nextState = nextState.WithClearedStackOffsets();
                    }
                }
            }
            else
            {
                if (TryInstructionWritesDest(instruction, out var destinations))
                {
                    foreach (var dest in destinations)
                    {
                        nextState = nextState.WithoutLrOffset(dest);
                        if (dest == "r1")
                        {
                            nextState = nextState.WithClearedStackOffsets();
                        }
                    }
                }
            }

            if (instruction.IsCall || mnemonic == "bl" || mnemonic == "blrl")
            {
                nextState = nextState.WithLrReturnOffset(null).WithCtrOffset(null);
                for (var register = 0; register <= 12; register++)
                {
                    if (register != 1 && register != 2)
                    {
                        nextState = nextState.WithoutLrOffset($"r{register}");
                    }
                }
            }

            if (mnemonic == "bctr")
            {
                if (state.CtrOffset.HasValue && seenOffsets.Add(state.CtrOffset.Value))
                {
                    yield return state.CtrOffset.Value;
                }

                nextState = nextState.WithCtrOffset(null);
                if (instruction.BranchTargets.Count == 0)
                {
                    continue;
                }
            }

            var isReturn = !instruction.IsCall && (instruction.IsReturn || mnemonic == "blr" || mnemonic == "bclr" ||
                (mnemonic.StartsWith("b", StringComparison.Ordinal) && mnemonic.EndsWith("lr", StringComparison.Ordinal)));
            if (isReturn)
            {
                if (state.LrReturnOffset.HasValue && state.LrReturnOffset.Value != 0 && seenOffsets.Add(state.LrReturnOffset.Value))
                {
                    yield return state.LrReturnOffset.Value;
                }

                if (!instruction.IsConditionalBranch)
                {
                    continue;
                }
            }

            if (instruction.IsUnconditionalBranch)
            {
                foreach (var target in instruction.BranchTargets)
                {
                    if (indexByAddress.TryGetValue(target, out var targetIndex))
                    {
                        Enqueue(targetIndex, nextState);
                    }
                }
            }
            else if (instruction.IsConditionalBranch)
            {
                var fallthrough = GetFallthroughIndex(instruction);
                if (fallthrough.HasValue)
                {
                    Enqueue(fallthrough.Value, nextState);
                }

                if (!isReturn)
                {
                    foreach (var target in instruction.BranchTargets)
                    {
                        if (indexByAddress.TryGetValue(target, out var targetIndex))
                        {
                            Enqueue(targetIndex, nextState);
                        }
                    }
                }
            }
            else
            {
                var fallthrough = GetFallthroughIndex(instruction);
                if (fallthrough.HasValue)
                {
                    Enqueue(fallthrough.Value, nextState);
                }
            }
        }

        static bool TryGetInstructionReg(PpcInstruction instruction, int index, out string register)
        {
            if (instruction.Operands.Count > index && instruction.Operands[index] is PpcRegisterOperand operand)
            {
                register = NormalizeInstructionReg(operand.Name);
                return true;
            }

            register = string.Empty;
            return false;
        }

        static bool TryGetInstructionDisplacement(PpcInstruction instruction, int index, out int offset, out string baseRegister, out int baseRegisterNumber)
        {
            if (instruction.Operands.Count > index && instruction.Operands[index] is PpcDisplacementOperand operand)
            {
                offset = operand.Offset;
                baseRegister = NormalizeInstructionReg(operand.BaseRegister);
                baseRegisterNumber = operand.BaseRegisterNumber;
                return true;
            }

            offset = 0;
            baseRegister = string.Empty;
            baseRegisterNumber = -1;
            return false;
        }

        static bool TryGetInstructionImm(PpcInstruction instruction, int index, out int immediate)
        {
            if (instruction.Operands.Count > index && instruction.Operands[index] is PpcImmediateOperand operand)
            {
                immediate = operand.Value;
                return true;
            }

            immediate = 0;
            return false;
        }

        static bool TryInstructionWritesDest(PpcInstruction instruction, out IReadOnlyList<string> destinations)
        {
            if (instruction.Operands.Count == 0 || instruction.Operands[0] is not PpcRegisterOperand operand)
            {
                destinations = Array.Empty<string>();
                return false;
            }

            var mnemonic = instruction.Mnemonic.ToLowerInvariant();
            if (mnemonic.StartsWith("st", StringComparison.Ordinal) ||
                mnemonic.StartsWith("b", StringComparison.Ordinal) ||
                mnemonic.StartsWith("cmp", StringComparison.Ordinal))
            {
                destinations = Array.Empty<string>();
                return false;
            }

            if (mnemonic == "lmw")
            {
                var startReg = Math.Clamp(operand.Number, 0, 31);
                var regs = new string[32 - startReg];
                for (var r = startReg; r <= 31; r++)
                {
                    regs[r - startReg] = $"r{r}";
                }
                destinations = regs;
                return true;
            }

            destinations = [NormalizeInstructionReg(operand.Name)];
            return true;
        }

        static bool TryGetStackStoreRange(PpcInstruction instruction, out int offset, out int size, out bool updatesStackPointer)
        {
            offset = 0;
            size = 0;
            updatesStackPointer = false;
            if (!TryGetInstructionDisplacement(instruction, 1, out offset, out var baseRegister, out _) ||
                baseRegister != "r1")
            {
                return false;
            }

            switch (instruction.Mnemonic.ToLowerInvariant())
            {
                case "stfs":
                    size = 4;
                    return true;
                case "stfsu":
                    size = 4;
                    updatesStackPointer = true;
                    return true;
                case "stfd":
                    size = 8;
                    return true;
                case "stfdu":
                    size = 8;
                    updatesStackPointer = true;
                    return true;
                case "stmw" when instruction.Operands[0] is PpcRegisterOperand register:
                    size = checked((32 - Math.Clamp(register.Number, 0, 31)) * 4);
                    return true;
                default:
                    return false;
            }
        }

        static string NormalizeInstructionReg(string register) => register.ToLowerInvariant();
    }

    private sealed class PathState : IEquatable<PathState>
    {
        public ImmutableDictionary<string, int> LrOffsets { get; }
        public int? CtrOffset { get; }
        public int? LrReturnOffset { get; }
        public int SpDelta { get; }
        public ImmutableDictionary<int, int> StackOffsets { get; }

        public PathState(
            ImmutableDictionary<string, int> lrOffsets,
            int? ctrOffset,
            int? lrReturnOffset,
            int spDelta,
            ImmutableDictionary<int, int> stackOffsets)
        {
            LrOffsets = lrOffsets;
            CtrOffset = ctrOffset;
            LrReturnOffset = lrReturnOffset;
            SpDelta = spDelta;
            StackOffsets = stackOffsets;
        }

        public static readonly PathState Empty = new(
            ImmutableDictionary<string, int>.Empty.WithComparers(StringComparer.OrdinalIgnoreCase),
            null,
            0,
            0,
            ImmutableDictionary<int, int>.Empty);

        public PathState WithLrOffset(string register, int offset) =>
            LrOffsets.TryGetValue(register, out var cur) && cur == offset
                ? this
                : new(LrOffsets.SetItem(register, offset), CtrOffset, LrReturnOffset, SpDelta, StackOffsets);

        public PathState WithoutLrOffset(string register) =>
            LrOffsets.ContainsKey(register)
                ? new(LrOffsets.Remove(register), CtrOffset, LrReturnOffset, SpDelta, StackOffsets)
                : this;

        public PathState WithCtrOffset(int? ctrOffset) =>
            ctrOffset == CtrOffset
                ? this
                : new(LrOffsets, ctrOffset, LrReturnOffset, SpDelta, StackOffsets);

        public PathState WithLrReturnOffset(int? lrReturnOffset) =>
            lrReturnOffset == LrReturnOffset
                ? this
                : new(LrOffsets, CtrOffset, lrReturnOffset, SpDelta, StackOffsets);

        public PathState WithSpDelta(int spDelta) =>
            spDelta == SpDelta
                ? this
                : new(LrOffsets, CtrOffset, LrReturnOffset, spDelta, StackOffsets);

        public PathState WithAdjustedStackPointer(int displacement)
        {
            // r1 can hold an LR-relative address too. Update both relations;
            // guest address arithmetic wraps at 32 bits.
            var updated = WithSpDelta(unchecked(SpDelta + displacement));
            return LrOffsets.TryGetValue("r1", out var offset)
                ? updated.WithLrOffset("r1", unchecked(offset + displacement))
                : updated;
        }

        public PathState WithStackOffset(int slot, int offset) =>
            StackOffsets.TryGetValue(slot, out var cur) && cur == offset
                ? this
                : new(LrOffsets, CtrOffset, LrReturnOffset, SpDelta, StackOffsets.SetItem(slot, offset));

        public PathState WithoutStackOffset(int slot) =>
            StackOffsets.ContainsKey(slot)
                ? new(LrOffsets, CtrOffset, LrReturnOffset, SpDelta, StackOffsets.Remove(slot))
                : this;

        public PathState WithoutStackOffsetsInRange(int start, int size)
        {
            var end = checked(start + size);
            var remaining = StackOffsets;
            foreach (var slot in StackOffsets.Keys)
            {
                if (slot < end && start < checked(slot + 4))
                {
                    remaining = remaining.Remove(slot);
                }
            }

            return remaining.Count == StackOffsets.Count
                ? this
                : new(LrOffsets, CtrOffset, LrReturnOffset, SpDelta, remaining);
        }

        public PathState WithClearedStackOffsets() =>
            StackOffsets.IsEmpty
                ? this
                : new(LrOffsets, CtrOffset, LrReturnOffset, SpDelta, ImmutableDictionary<int, int>.Empty);

        public bool Equals(PathState? other)
        {
            if (ReferenceEquals(this, other)) return true;
            if (other is null) return false;
            if (CtrOffset != other.CtrOffset || LrReturnOffset != other.LrReturnOffset || SpDelta != other.SpDelta) return false;
            if (LrOffsets.Count != other.LrOffsets.Count || StackOffsets.Count != other.StackOffsets.Count) return false;
            foreach (var (k, v) in LrOffsets)
            {
                if (!other.LrOffsets.TryGetValue(k, out var otherV) || v != otherV)
                {
                    return false;
                }
            }
            foreach (var (k, v) in StackOffsets)
            {
                if (!other.StackOffsets.TryGetValue(k, out var otherV) || v != otherV)
                {
                    return false;
                }
            }
            return true;
        }

        public override bool Equals(object? obj) => obj is PathState other && Equals(other);

        public override int GetHashCode()
        {
            var hash = new HashCode();
            hash.Add(CtrOffset);
            hash.Add(LrReturnOffset);
            hash.Add(SpDelta);
            hash.Add(LrOffsets.Count);
            var regHash = 0;
            foreach (var (k, v) in LrOffsets)
            {
                regHash ^= HashCode.Combine(StringComparer.OrdinalIgnoreCase.GetHashCode(k), v);
            }
            hash.Add(regHash);
            hash.Add(StackOffsets.Count);
            var stackHash = 0;
            foreach (var (k, v) in StackOffsets)
            {
                stackHash ^= HashCode.Combine(k, v);
            }
            hash.Add(stackHash);
            return hash.ToHashCode();
        }
    }
}
