using System.Buffers.Binary;
using Translator.Core.Disassembly;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods;

public sealed record ModFunctionStart(uint Address, string Reason);

/// <summary>
/// Recovers module entry points from the relocated Kamek image alone. Retro Rewind
/// packs ship no Code.map, so these heuristics are the only source of module
/// function boundaries.
/// </summary>
public static class ModFunctionDiscovery
{
    public static IReadOnlyList<ModFunctionStart> DiscoverKamekFunctions(
        KamekChunk chunk,
        uint moduleGuestBase,
        byte[] relocatedModuleImage)
    {
        var starts = new Dictionary<uint, string>();
        var moduleCodeEnd = checked(moduleGuestBase + chunk.CodeSize);
        var moduleImageEnd = checked(moduleGuestBase + (uint)relocatedModuleImage.Length);

        void Add(uint address, string reason, uint moduleEnd)
        {
            if (address < moduleGuestBase || address >= moduleEnd || (address & 0x3u) != 0)
            {
                return;
            }

            starts.TryAdd(address, reason);
        }

        void AddCodeAddress(uint address, string reason) => Add(address, reason, moduleCodeEnd);

        void AddImageAddress(uint address, string reason) => Add(address, reason, moduleImageEnd);

        if (LooksLikeFunctionPointerTarget(moduleGuestBase, moduleGuestBase, moduleCodeEnd, relocatedModuleImage))
        {
            AddCodeAddress(moduleGuestBase, "module base");
        }

        foreach (var command in chunk.Commands)
        {
            if (command.Arguments.Count == 0)
            {
                continue;
            }

            switch (command.Id)
            {
                case KamekCommandId.Rel24:
                case KamekCommandId.Branch:
                case KamekCommandId.BranchLink:
                    AddImageAddress(KamekAddress.Resolve(command.Arguments[0], moduleGuestBase), command.Id.ToString());
                    break;
            }
        }

        if (chunk.CtorStart <= chunk.CtorEnd && chunk.CtorEnd <= relocatedModuleImage.Length)
        {
            for (var offset = checked((int)chunk.CtorStart); offset + 4 <= chunk.CtorEnd; offset += 4)
            {
                var target = BinaryPrimitives.ReadUInt32BigEndian(relocatedModuleImage.AsSpan(offset, 4));
                AddImageAddress(target, "ctor");
            }
        }

        // Kamek modules frequently register callbacks through static tables or lists that are
        // walked indirectly by module startup code. Seed those native translations ahead of time.
        // The Kamek blob is linked at offset zero, so a word inside it only holds a module address
        // when a pointer relocation wrote one there; anything else is an instruction or data that
        // happens to encode one. Images appended past the blob are not described by chunk commands.
        var pointerRelocations = chunk.Commands
            .Where(command => command.AddressIsRelative &&
                command.Arguments.Count > 0 &&
                command.Id is KamekCommandId.Addr32 or KamekCommandId.CondWritePointer)
            .Select(command => (command.Address, KamekAddress.Resolve(command.Arguments[0], moduleGuestBase)))
            .ToHashSet();
        for (var offset = 0; offset + 4 <= relocatedModuleImage.Length; offset += 4)
        {
            var value = BinaryPrimitives.ReadUInt32BigEndian(relocatedModuleImage.AsSpan(offset, 4));
            // Comparing the value also rejects conditional writes that were skipped.
            if ((uint)offset < chunk.CodeSize && !pointerRelocations.Contains(((uint)offset, value)))
            {
                continue;
            }

            if (LooksLikeFunctionPointerTarget(value, moduleGuestBase, moduleImageEnd, relocatedModuleImage))
            {
                AddImageAddress(value, "module data pointer");
            }
        }

        for (var offset = 0; offset + 4 <= relocatedModuleImage.Length; offset += 4)
        {
            var address = checked(moduleGuestBase + (uint)offset);
            if (LooksLikeFunctionPrologue(address, moduleGuestBase, moduleCodeEnd, relocatedModuleImage))
            {
                AddCodeAddress(address, "module prologue scan");
            }
        }

        foreach (var address in DiscoverSynthesizedFunctionPointers(moduleGuestBase, moduleImageEnd, relocatedModuleImage))
        {
            AddImageAddress(address, "module synthesized pointer");
        }

        foreach (var address in DiscoverLeafBoundaryFunctions(moduleGuestBase, moduleCodeEnd, relocatedModuleImage, starts.Keys))
        {
            AddCodeAddress(address, "module leaf boundary scan");
        }

        foreach (var address in DiscoverTailEntryFunctions(moduleGuestBase, moduleImageEnd, relocatedModuleImage, starts.Keys))
        {
            AddImageAddress(address, "module tail-entry scan");
        }

        return starts
            .OrderBy(kvp => kvp.Key)
            .Select(kvp => new ModFunctionStart(kvp.Key, kvp.Value))
            .ToList();
    }

    private static bool LooksLikeFunctionPrologue(
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        if (address < moduleGuestBase ||
            address >= moduleEnd ||
            (address & 0x3u) != 0)
        {
            return false;
        }

        var offset = checked((int)(address - moduleGuestBase));
        if (offset < 0 || offset + 4 > relocatedModuleImage.Length)
        {
            return false;
        }

        var firstWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
        if (IsStackFramePrologue(firstWord))
        {
            return true;
        }

        if (offset + 8 <= relocatedModuleImage.Length)
        {
            var secondWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset + 4);
            if (firstWord == 0x7C0802A6u && IsStackFramePrologue(secondWord))
            {
                return true;
            }
        }

        return false;
    }

    private static bool LooksLikeFunctionPointerTarget(
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        if (address < moduleGuestBase || address >= moduleEnd || (address & 0x3u) != 0)
        {
            return false;
        }

        var offset = checked((int)(address - moduleGuestBase));
        if (offset < 0 || offset + 4 > relocatedModuleImage.Length)
        {
            return false;
        }

        var firstWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
        if (firstWord == 0 || firstWord == 0xFFFFFFFFu)
        {
            return false;
        }

        return LooksLikeFunctionPrologue(address, moduleGuestBase, moduleEnd, relocatedModuleImage) ||
            LooksLikeBranchTrampoline(address, firstWord, moduleGuestBase, moduleEnd) ||
            PpcControlFlow.IsReturn(firstWord) ||
            LooksLikeLeafFunction(address, moduleGuestBase, moduleEnd, relocatedModuleImage);
    }

    private static bool LooksLikeBranchTrampoline(uint address, uint word, uint moduleGuestBase, uint moduleEnd) =>
        // ASCII data starting with 'H' or 'I' (0x48/0x49) decodes as a forward
        // unconditional branch, so a lone branch word only counts as a
        // trampoline when it lands somewhere code can actually live: base-game
        // MEM1 or the module image itself, which sits above kGuestExecutableLimit
        // now that the overlay is based at 0x81800000.
        PpcControlFlow.IsRelativeUnlinkedBranch(word) &&
        PpcControlFlow.TryDecodeRelativeBranchTarget(address, word, out var target) &&
        (target & 0x3u) == 0 &&
        ((target >= 0x80000000u && target < kGuestExecutableLimit) ||
         (target >= moduleGuestBase && target < moduleEnd));

    private static IEnumerable<uint> DiscoverSynthesizedFunctionPointers(
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        for (var offset = 0; offset + 8 <= relocatedModuleImage.Length; offset += 4)
        {
            var first = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
            if (!PpcInstructionPatterns.TryGetLis(first, out var register, out var high))
            {
                continue;
            }

            var baseValue = high << 16;
            var scanEnd = Math.Min(offset + 36, relocatedModuleImage.Length);
            for (var cursor = offset + 4; cursor + 4 <= scanEnd; cursor += 4)
            {
                var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, cursor);
                if (TryBuildAddress(word, register, baseValue, out var candidate, out var valueRegister))
                {
                    if (LooksLikeSynthesizedFunctionPointerTarget(candidate, moduleGuestBase, moduleEnd, relocatedModuleImage) &&
                        IsPointerValueUsed(relocatedModuleImage, cursor + 4, valueRegister, moduleGuestBase, moduleEnd))
                    {
                        yield return candidate;
                    }

                    if (valueRegister == register)
                    {
                        break;
                    }

                    continue;
                }

                if (WritesRegister(word, register))
                {
                    break;
                }
            }
        }
    }

    private static IEnumerable<uint> DiscoverLeafBoundaryFunctions(
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage,
        IEnumerable<uint> knownStarts)
    {
        var knownStartSet = knownStarts.ToHashSet();
        var sortedKnownStarts = knownStartSet.Order().ToArray();

        for (var offset = 4; offset + 4 <= relocatedModuleImage.Length; offset += 4)
        {
            var address = checked(moduleGuestBase + (uint)offset);
            if (knownStartSet.Contains(address))
            {
                continue;
            }

            var previousAddress = checked(moduleGuestBase + (uint)(offset - 4));
            var previousWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset - 4);
            if (!IsFunctionBoundaryInstruction(previousAddress, previousWord, knownStartSet, moduleGuestBase, moduleEnd))
            {
                continue;
            }

            var nextKnownStart = NextKnownStartAfter(sortedKnownStarts, address);
            if (nextKnownStart is null || nextKnownStart.Value - address > 0x200u)
            {
                continue;
            }

            if (LooksLikeFunctionPrologue(address, moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                continue;
            }

            if (LooksLikeBoundedLeafFunction(address, nextKnownStart.Value, moduleGuestBase, moduleEnd, relocatedModuleImage, knownStartSet))
            {
                yield return address;
            }
        }
    }

    private static uint? NextKnownStartAfter(uint[] sortedKnownStarts, uint address)
    {
        foreach (var start in sortedKnownStarts)
        {
            if (start > address)
            {
                return start;
            }
        }

        return null;
    }

    private static IEnumerable<uint> DiscoverTailEntryFunctions(
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage,
        IEnumerable<uint> knownStarts)
    {
        var knownStartSet = knownStarts.ToHashSet();
        var sortedKnownStarts = knownStartSet.Order().ToArray();

        for (var offset = 4; offset + 4 <= relocatedModuleImage.Length; offset += 4)
        {
            var address = checked(moduleGuestBase + (uint)offset);
            if (knownStartSet.Contains(address) ||
                LooksLikeFunctionPrologue(address, moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                continue;
            }

            var previousWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset - 4);
            if (!IsFunctionTerminator(previousWord) && !PpcControlFlow.IsRelativeUnlinkedBranch(previousWord))
            {
                continue;
            }

            var nextBoundary = NextKnownStartOrPrologueAfter(sortedKnownStarts, address, moduleGuestBase, moduleEnd, relocatedModuleImage);
            if (nextBoundary is null || nextBoundary.Value - address > 0x40u)
            {
                continue;
            }

            if (LooksLikeShortTailEntry(address, nextBoundary.Value, moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                yield return address;
            }
        }
    }

    private static uint? NextKnownStartOrPrologueAfter(
        uint[] sortedKnownStarts,
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        var nextKnownStart = NextKnownStartAfter(sortedKnownStarts, address);
        var scanEnd = nextKnownStart ?? moduleEnd;

        for (var cursor = checked(address + 4); cursor < scanEnd; cursor += 4)
        {
            if (LooksLikeFunctionPrologue(cursor, moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                return cursor;
            }
        }

        return nextKnownStart;
    }

    private static bool LooksLikeShortTailEntry(
        uint address,
        uint nextBoundary,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        if (address >= nextBoundary ||
            address < moduleGuestBase ||
            nextBoundary > moduleEnd ||
            (address & 0x3u) != 0)
        {
            return false;
        }

        var offset = checked((int)(address - moduleGuestBase));
        var endOffset = checked((int)Math.Min(nextBoundary - moduleGuestBase, (uint)relocatedModuleImage.Length));
        if (offset < 0 || offset + 8 > endOffset || LooksLikeInlineAsciiData(relocatedModuleImage, offset, endOffset))
        {
            return false;
        }

        var sawNonBranchInstruction = false;
        for (var cursor = offset; cursor + 4 <= endOffset; cursor += 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, cursor);
            if (!LooksLikeInstruction(word))
            {
                return false;
            }

            var currentAddress = checked(moduleGuestBase + (uint)cursor);
            if (PpcControlFlow.IsRelativeUnlinkedBranch(word) &&
                PpcControlFlow.TryDecodeRelativeBranchTarget(currentAddress, word, out var branchTarget))
            {
                return sawNonBranchInstruction &&
                    (branchTarget < address || branchTarget >= nextBoundary) &&
                    LooksLikeExecutableTailTarget(branchTarget, moduleGuestBase, moduleEnd, relocatedModuleImage);
            }

            if (IsFunctionTerminator(word))
            {
                return false;
            }

            sawNonBranchInstruction = true;
        }

        return false;
    }

    private static bool LooksLikeExecutableTailTarget(
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage) =>
        LooksLikeExternalExecutableBranchTarget(address, moduleGuestBase, moduleEnd) ||
        LooksLikeFunctionPointerTarget(address, moduleGuestBase, moduleEnd, relocatedModuleImage);

    private static bool TryBuildAddress(uint word, int baseRegister, uint baseValue, out uint address, out int valueRegister)
    {
        if (PpcInstructionPatterns.TryGetAddi(word, out var destination, out var source, out var immediate))
        {
            if (source != baseRegister)
            {
                address = 0;
                valueRegister = 0;
                return false;
            }

            address = unchecked(baseValue + (uint)immediate);
            valueRegister = destination;
            return true;
        }

        if (PpcInstructionPatterns.TryGetOri(word, out var oriSource, out var oriDestination, out var oriImmediate))
        {
            if (oriSource != baseRegister)
            {
                address = 0;
                valueRegister = 0;
                return false;
            }

            address = baseValue | oriImmediate;
            valueRegister = oriDestination;
            return true;
        }

        address = 0;
        valueRegister = 0;
        return false;
    }

    private static bool IsPointerValueUsed(
        byte[] relocatedModuleImage,
        int startOffset,
        int register,
        uint moduleGuestBase,
        uint moduleEnd)
    {
        // Bounded to cover typical constructor prologue/setup before the value is stored or used;
        // a write to the value register still ends the search so a later unrelated store can't fake liveness.
        var scanEnd = Math.Min(startOffset + 128, relocatedModuleImage.Length);
        for (var offset = startOffset; offset + 4 <= scanEnd; offset += 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
            if (PpcRegisterEffects.MayWriteGpr(word, register))
            {
                return false;
            }

            if (StoresRegister(word, register) || MovesRegisterToCtr(word, register))
            {
                return true;
            }

            if (IsBranchWithLink(word))
            {
                // A call passes r3-r10 into the callee and clobbers the other
                // volatile registers, so the pointer either escapes here as an
                // argument or dies with the call. Callee-saved values survive.
                if (IsArgumentRegister(register))
                {
                    return true;
                }
                if (!IsCalleeSavedRegister(register))
                {
                    return false;
                }
                continue;
            }

            if (PpcControlFlow.IsRelativeUnlinkedBranch(word))
            {
                // An unconditional branch out of the module is a tail call into
                // base code: argument registers flow into the callee unchanged.
                if (PpcControlFlow.TryDecodeRelativeBranchTarget(checked(moduleGuestBase + (uint)offset), word, out var branchTarget) &&
                    LooksLikeExternalExecutableBranchTarget(branchTarget, moduleGuestBase, moduleEnd))
                {
                    return IsArgumentRegister(register);
                }
                continue;
            }

            if (IsFunctionTerminator(word))
            {
                // bctr is an indirect tail call, so argument registers escape;
                // blr only publishes the pointer through the r3 return value.
                // Either way the linear scan must not run into the next function.
                return word == 0x4E800420u ? IsArgumentRegister(register) : register == 3;
            }
        }

        return false;
    }

    private static bool IsArgumentRegister(int register) => register is >= 3 and <= 10;

    private static bool IsCalleeSavedRegister(int register) => register is 1 or 2 or >= 13;

    private static bool IsBranchWithLink(uint word)
    {
        var fields = new PpcWordFields(word);
        return (fields.PrimaryOpcode == 18 && (word & 0x1u) != 0) || // bl / bla
            word == 0x4E800021u || // blrl
            word == 0x4E800421u;   // bctrl
    }

    private static bool LooksLikeSynthesizedFunctionPointerTarget(
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        if (!LooksLikeFunctionPointerTarget(address, moduleGuestBase, moduleEnd, relocatedModuleImage))
        {
            return LooksLikeLeafFunction(address, moduleGuestBase, moduleEnd, relocatedModuleImage);
        }

        return true;
    }

    private static bool LooksLikeLeafFunction(
        uint address,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage)
    {
        if (address < moduleGuestBase || address >= moduleEnd || (address & 0x3u) != 0)
        {
            return false;
        }

        var offset = checked((int)(address - moduleGuestBase));
        if (offset < 0 || offset + 4 > relocatedModuleImage.Length)
        {
            return false;
        }

        var firstWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
        if (firstWord == 0 || firstWord == 0xFFFFFFFFu)
        {
            return false;
        }

        var scanEnd = Math.Min(offset + 128, relocatedModuleImage.Length);
        for (var cursor = offset; cursor + 4 <= scanEnd; cursor += 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, cursor);
            if (IsFunctionTerminator(word))
            {
                return true;
            }

            if (cursor != offset && LooksLikeFunctionPrologue(checked(moduleGuestBase + (uint)cursor), moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                return false;
            }
        }

        return false;
    }

    private static bool LooksLikeBoundedLeafFunction(
        uint address,
        uint nextKnownStart,
        uint moduleGuestBase,
        uint moduleEnd,
        byte[] relocatedModuleImage,
        IReadOnlySet<uint> knownStarts)
    {
        if (address >= nextKnownStart ||
            address < moduleGuestBase ||
            address >= moduleEnd ||
            (address & 0x3u) != 0)
        {
            return false;
        }

        var offset = checked((int)(address - moduleGuestBase));
        var endOffset = checked((int)Math.Min(nextKnownStart - moduleGuestBase, (uint)relocatedModuleImage.Length));
        if (offset < 0 || offset + 4 > endOffset)
        {
            return false;
        }

        var firstWord = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, offset);
        if (!LooksLikeInstruction(firstWord) || IsFunctionTerminator(firstWord) || LooksLikeInlineAsciiData(relocatedModuleImage, offset, endOffset))
        {
            return false;
        }

        var sawNonBranchInstruction = false;
        for (var cursor = offset; cursor + 4 <= endOffset; cursor += 4)
        {
            var word = PpcWordFields.ReadBigEndianWord(relocatedModuleImage, cursor);
            if (!LooksLikeInstruction(word))
            {
                return false;
            }

            if (IsFunctionTerminator(word))
            {
                return sawNonBranchInstruction;
            }

            var currentAddress = checked(moduleGuestBase + (uint)cursor);
            if (PpcControlFlow.TryDecodeConditionalRelativeBranchTarget(currentAddress, word, out var conditionalTarget) &&
                (conditionalTarget < address || conditionalTarget >= nextKnownStart))
            {
                return false;
            }

            if (PpcControlFlow.IsRelativeUnlinkedBranch(word) &&
                PpcControlFlow.TryDecodeRelativeBranchTarget(currentAddress, word, out var branchTarget) &&
                (branchTarget < address || branchTarget >= nextKnownStart))
            {
                if (knownStarts.Contains(branchTarget) ||
                    LooksLikeExternalExecutableBranchTarget(branchTarget, moduleGuestBase, moduleEnd))
                {
                    return sawNonBranchInstruction;
                }
            }

            if (!PpcControlFlow.IsRelativeUnlinkedBranch(word))
            {
                sawNonBranchInstruction = true;
            }

            if (cursor != offset && LooksLikeFunctionPrologue(checked(moduleGuestBase + (uint)cursor), moduleGuestBase, moduleEnd, relocatedModuleImage))
            {
                return false;
            }
        }

        return false;
    }

    private static bool LooksLikeInlineAsciiData(byte[] relocatedModuleImage, int offset, int endOffset)
    {
        var scanEnd = Math.Min(offset + 16, endOffset);
        var examined = 0;
        var printableOrZero = 0;

        for (var cursor = offset; cursor < scanEnd; cursor++)
        {
            var value = relocatedModuleImage[cursor];
            examined++;
            if (value == 0 || (value >= 0x20 && value <= 0x7E))
            {
                printableOrZero++;
            }
        }

        return examined >= 8 && printableOrZero == examined;
    }

    private static bool WritesRegister(uint word, int register)
        => PpcRegisterEffects.MayWriteGpr(word, register);

    private static bool StoresRegister(uint word, int register)
    {
        var fields = new PpcWordFields(word);
        var opcode = fields.PrimaryOpcode;
        var rs = fields.GprField0;
        return (opcode is 36 or 37 or 38 or 44 or 45 or 46 or 52 or 53 or 54) &&
            rs == register;
    }

    private static bool MovesRegisterToCtr(uint word, int register)
    {
        // mtctr rS is mtspr 9,rS. The split SPR field encodes CTR as 0x120.
        return PpcInstructionPatterns.TryGetMtspr(word, 9, out var source) && source == register;
    }

    private static bool IsStackFramePrologue(uint word) =>
        (word & 0xFFFF0000u) == 0x94210000u &&
        (short)(word & 0xFFFFu) < 0;

    private static bool IsFunctionBoundaryInstruction(
        uint address,
        uint word,
        IReadOnlySet<uint> knownStarts,
        uint moduleGuestBase,
        uint moduleEnd)
    {
        if (IsFunctionTerminator(word))
        {
            return true;
        }

        return PpcControlFlow.IsRelativeUnlinkedBranch(word) &&
            PpcControlFlow.TryDecodeRelativeBranchTarget(address, word, out var branchTarget) &&
            (knownStarts.Contains(branchTarget) ||
             LooksLikeExternalExecutableBranchTarget(branchTarget, moduleGuestBase, moduleEnd));
    }

    // End of cached MEM1 (0x80000000-0x817FFFFF); guest code never executes
    // above it, so branch targets beyond this are data misread as instructions.
    private const uint kGuestExecutableLimit = 0x81800000u;

    private static bool LooksLikeExternalExecutableBranchTarget(uint address, uint moduleGuestBase, uint moduleEnd) =>
        address >= 0x80000000u &&
        address < kGuestExecutableLimit &&
        (address & 0x3u) == 0 &&
        (address < moduleGuestBase || address >= moduleEnd);

    private static bool LooksLikeInstruction(uint word)
    {
        if (word == 0 || word == 0xFFFFFFFFu)
        {
            return false;
        }

        var opcode = new PpcWordFields(word).PrimaryOpcode;
        return opcode is
            3 or 4 or
            7 or 8 or 10 or 11 or 12 or 13 or 14 or 15 or 16 or 17 or 18 or 19 or
            20 or 21 or 23 or 24 or 26 or 28 or 29 or 31 or
            32 or 33 or 34 or 35 or 36 or 37 or 38 or 39 or
            40 or 41 or 42 or 43 or 44 or 45 or 46 or 47 or
            48 or 49 or 50 or 51 or 52 or 53 or 54 or 55 or
            56 or 57 or 58 or 59 or 60 or 61 or 62 or 63;
    }

    private static bool IsFunctionTerminator(uint word) =>
        PpcControlFlow.IsBctr(word) ||
        PpcControlFlow.IsReturn(word);
}
