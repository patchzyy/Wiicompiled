using Translator.Core.IO;
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text.Json;
using Translator.Core.Parsing.Kamek;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Loading;

namespace Translator.Core.Mods;

public sealed record PatchApplicationRecord(
    uint Address,
    KamekCommandId CommandId,
    string Target,
    string Status,
    string Detail);

public sealed record OverlayFunctionImage(
    uint Start,
    uint End,
    string Name,
    string SectionName,
    string ImageFile,
    string Sha256,
    int PatchCount,
    IReadOnlyList<PatchApplicationRecord> Patches);

public sealed record OverlayBuildResult(
    uint ModuleGuestBase,
    string ModuleImageFile,
    uint ModuleImageSize,
    string ModuleImageSha256,
    int ModulePatchCount,
    IReadOnlyList<OverlayFunctionImage> OverlayFunctions,
    IReadOnlyList<PatchApplicationRecord> Diagnostics);

public static class OverlayFunctionBuilder
{
    public static OverlayBuildResult BuildAndWrite(
        KamekChunk chunk,
        BaseManifest baseManifest,
        KamekPatchPlan patchPlan,
        string baseManifestDirectory,
        string outputDirectory,
        IReadOnlyCollection<uint>? additionalOverlayFunctionStarts = null,
        byte[]? additionalModuleImage = null,
        uint? additionalModuleImageOffset = null,
        IReadOnlyCollection<RetroWfcStaticBytePatchPlan>? retroWfcStaticBytePatches = null,
        IReadOnlyCollection<RetroWfcExecutableHookPlan>? retroWfcExecutableHooks = null)
    {
        var overlayDir = Path.Combine(outputDirectory, "overlay_images");
        TrackedFs.CreateDirectory(overlayDir);

        var diagnostics = new List<PatchApplicationRecord>();
        var moduleImage = BuildModuleImage(chunk.CodeBlob, additionalModuleImage, additionalModuleImageOffset);
        var modulePatchCount = 0;
        foreach (var command in chunk.Commands.Where(c => c.AddressIsRelative))
        {
            var target = patchPlan.ModuleGuestBase + command.Address;
            if (TryApplyPatch(
                    moduleImage,
                    patchPlan.ModuleGuestBase,
                    target,
                    command,
                    patchPlan.ModuleGuestBase,
                    out var record))
            {
                modulePatchCount++;
            }
            diagnostics.Add(record);
        }

        var moduleImageFile = $"kamek_module_{patchPlan.ModuleGuestBase:X8}.bin";
        TrackedFs.WriteAllBytes(Path.Combine(overlayDir, moduleImageFile), moduleImage);

        var sectionsByName = baseManifest.Sections.ToDictionary(s => s.Name, StringComparer.OrdinalIgnoreCase);
        var imageCache = new Dictionary<string, byte[]>(StringComparer.OrdinalIgnoreCase);
        var functionIndex = new BaseFunctionIndex(baseManifest.Functions);
        var overlays = new List<OverlayFunctionImage>();

        var executablePatchGroups = patchPlan.ExecutablePatches
            .Where(c => c.ContainingFunctionStart.HasValue)
            .GroupBy(c => c.ContainingFunctionStart!.Value)
            .ToDictionary(g => g.Key, g => g.ToList());
        var retroWfcStaticBytePatchSource = retroWfcStaticBytePatches is null
            ? Enumerable.Empty<RetroWfcStaticBytePatchPlan>()
            : retroWfcStaticBytePatches;
        var retroWfcStaticBytePatchGroups = retroWfcStaticBytePatchSource
            .Where(p => p.SectionExecutable && p.ContainingFunctionStart.HasValue)
            .GroupBy(p => p.ContainingFunctionStart!.Value)
            .ToDictionary(g => g.Key, g => g.ToList());
        var retroWfcExecutableHookSource = retroWfcExecutableHooks is null
            ? Enumerable.Empty<RetroWfcExecutableHookPlan>()
            : retroWfcExecutableHooks;
        var retroWfcExecutableHookGroups = retroWfcExecutableHookSource
            .Where(h => h.TargetAddress.HasValue)
            .GroupBy(h => h.ContainingFunctionStart)
            .ToDictionary(g => g.Key, g => g.ToList());
        var overlayStarts = new SortedSet<uint>(executablePatchGroups.Keys);
        foreach (var start in retroWfcStaticBytePatchGroups.Keys)
        {
            overlayStarts.Add(start);
        }
        foreach (var start in retroWfcExecutableHookGroups.Keys)
        {
            overlayStarts.Add(start);
        }
        if (additionalOverlayFunctionStarts is not null)
        {
            foreach (var start in additionalOverlayFunctionStarts)
            {
                overlayStarts.Add(start);
            }
        }

        foreach (var functionStart in overlayStarts)
        {
            var function = functionIndex.FindContaining(functionStart)
                ?? throw new InvalidDataException($"No base function found for overlay 0x{functionStart:X8}.");

            if (!sectionsByName.TryGetValue(function.SourceSection, out var section))
            {
                throw new InvalidDataException($"Base function 0x{function.Start:X8} references unknown section '{function.SourceSection}'.");
            }
            if (string.IsNullOrWhiteSpace(section.ImageFile))
            {
                throw new InvalidDataException($"Base section '{section.Name}' has no executable image file.");
            }

            var sourceImage = LoadImage(baseManifestDirectory, section.ImageFile, imageCache);
            var sourceOffset = checked((int)function.SourceImageOffset);
            var functionSize = checked((int)(function.End - function.Start));
            if (sourceOffset < 0 || sourceOffset + functionSize > sourceImage.Length)
            {
                throw new InvalidDataException(
                    $"Base function 0x{function.Start:X8}-0x{function.End:X8} exceeds source image '{section.ImageFile}'.");
            }

            var bytes = new byte[functionSize];
            Array.Copy(sourceImage, sourceOffset, bytes, 0, functionSize);

            var patchRecords = new List<PatchApplicationRecord>();
            executablePatchGroups.TryGetValue(function.Start, out var group);
            foreach (var classification in (group ?? new List<KamekPatchClassification>()).OrderBy(c => c.CommandAddress))
            {
                var command = FindMatchingCommand(chunk, classification);
                if (TryApplyPatch(
                        bytes,
                        function.Start,
                        classification.CommandAddress,
                        command,
                        patchPlan.ModuleGuestBase,
                        out var record))
                {
                    patchRecords.Add(record);
                }
                else
                {
                    patchRecords.Add(record);
                    diagnostics.Add(record);
                }
            }
            retroWfcExecutableHookGroups.TryGetValue(function.Start, out var executableHookGroup);
            var executableHooks = executableHookGroup is null
                ? Enumerable.Empty<RetroWfcExecutableHookPlan>()
                : executableHookGroup;
            foreach (var hook in executableHooks.OrderBy(h => h.Address).ThenBy(h => h.RecordIndex))
            {
                if (TryApplyRetroWfcExecutableHookPatch(
                        bytes,
                        function.Start,
                        hook,
                        out var record))
                {
                    patchRecords.Add(record);
                }
                else
                {
                    patchRecords.Add(record);
                    diagnostics.Add(record);
                }
            }
            retroWfcStaticBytePatchGroups.TryGetValue(function.Start, out var staticByteGroup);
            var staticBytePatches = staticByteGroup is null
                ? Enumerable.Empty<RetroWfcStaticBytePatchPlan>()
                : staticByteGroup;
            foreach (var patch in staticBytePatches.OrderBy(p => p.Address).ThenBy(p => p.RecordIndex))
            {
                if (TryApplyRetroWfcStaticBytePatch(
                        bytes,
                        function.Start,
                        patch,
                        out var record))
                {
                    patchRecords.Add(record);
                }
                else
                {
                    patchRecords.Add(record);
                    diagnostics.Add(record);
                }
            }

            var imageFile = $"overlay_{function.Start:X8}.bin";
            TrackedFs.WriteAllBytes(Path.Combine(overlayDir, imageFile), bytes);

            overlays.Add(new OverlayFunctionImage(
                function.Start,
                function.End,
                function.Name,
                function.SourceSection,
                imageFile,
                ChecksumUtilities.Sha256Hex(bytes),
                patchRecords.Count(p => p.Status == "applied"),
                patchRecords));
        }

        var result = new OverlayBuildResult(
            patchPlan.ModuleGuestBase,
            moduleImageFile,
            checked((uint)moduleImage.Length),
            ChecksumUtilities.Sha256Hex(moduleImage),
            modulePatchCount,
            overlays,
            diagnostics.Where(d => d.Status != "applied").ToList());

        return result;
    }

    private const uint kPpcNop = 0x60000000;

    private static byte[] BuildModuleImage(byte[] baseImage, byte[]? additionalImage, uint? additionalOffset)
    {
        if (additionalImage is not { Length: > 0 })
        {
            return baseImage.ToArray();
        }
        if (!additionalOffset.HasValue)
        {
            throw new InvalidDataException("Additional module image bytes require an explicit module offset.");
        }
        if (additionalOffset.Value < baseImage.Length)
        {
            throw new InvalidDataException(
                $"Additional module image offset 0x{additionalOffset.Value:X} overlaps base module image length 0x{baseImage.Length:X}.");
        }

        var totalSize = checked((int)additionalOffset.Value + additionalImage.Length);
        var result = new byte[totalSize];
        Array.Copy(baseImage, result, baseImage.Length);
        Array.Copy(additionalImage, 0, result, checked((int)additionalOffset.Value), additionalImage.Length);
        return result;
    }

    private static bool RetroWfcHookUsesLink(RetroWfcExecutableHookPlan hook) =>
        hook.TypeName is "call" or "branchCtrLink" ||
        hook.Intent.Contains("Call", StringComparison.Ordinal);

    private static KamekCommand FindMatchingCommand(KamekChunk chunk, KamekPatchClassification classification)
    {
        return chunk.Commands.FirstOrDefault(command =>
            command.Id == classification.CommandId &&
            command.Address == classification.CommandAddress &&
            command.AddressIsAbsolute == classification.CommandAddressIsAbsolute &&
            command.Arguments.SequenceEqual(classification.Arguments))
            ?? throw new InvalidDataException(
                $"Could not match classified command {classification.CommandId} at 0x{classification.CommandAddress:X8} back to Code.pul command stream.");
    }

    private static byte[] LoadImage(string baseManifestDirectory, string imageFile, Dictionary<string, byte[]> cache)
    {
        if (cache.TryGetValue(imageFile, out var bytes))
        {
            return bytes;
        }

        var path = Path.Combine(baseManifestDirectory, imageFile);
        bytes = TrackedFs.ReadAllBytes(path);
        cache[imageFile] = bytes;
        return bytes;
    }

    private static bool TryApplyPatch(
        byte[] image,
        uint imageGuestStart,
        uint patchAddress,
        KamekCommand command,
        uint moduleGuestBase,
        out PatchApplicationRecord record)
    {
        try
        {
            var applied = ApplyPatch(image, imageGuestStart, patchAddress, command, moduleGuestBase, out var detail);
            record = new PatchApplicationRecord(patchAddress, command.Id, FormatTarget(command, moduleGuestBase), applied ? "applied" : "skipped", detail);
            return applied;
        }
        catch (Exception ex) when (ex is InvalidDataException or ArgumentOutOfRangeException or OverflowException)
        {
            record = new PatchApplicationRecord(patchAddress, command.Id, FormatTarget(command, moduleGuestBase), "failed", ex.Message);
            return false;
        }
    }

    private static bool TryApplyRetroWfcExecutableHookPatch(
        byte[] image,
        uint imageGuestStart,
        RetroWfcExecutableHookPlan hook,
        out PatchApplicationRecord record)
    {
        try
        {
            if (!hook.TargetAddress.HasValue)
            {
                throw new InvalidDataException($"Retro WFC executable hook #{hook.RecordIndex} has no resolved semantic target.");
            }
            if (hook.InstructionCount <= 0)
            {
                throw new InvalidDataException($"Retro WFC executable hook #{hook.RecordIndex} has invalid instruction count {hook.InstructionCount}.");
            }

            var offset = checked((int)(hook.Address - imageGuestStart));
            EnsureRange(image, offset, checked(hook.InstructionCount * sizeof(uint)));
            var link = RetroWfcHookUsesLink(hook);
            var branch = KamekPpcEncoding.EncodeBranch(hook.Address, hook.TargetAddress.Value, link);
            WriteUInt32(image, offset, branch);
            for (var i = 1; i < hook.InstructionCount; i++)
            {
                WriteUInt32(image, offset + i * sizeof(uint), kPpcNop);
            }

            var action = hook.TargetActionId ?? string.Join(",", hook.SemanticActionIds);
            record = new PatchApplicationRecord(
                hook.Address,
                link ? KamekCommandId.BranchLink : KamekCommandId.Branch,
                $"RetroWFC#{hook.RecordIndex}",
                "applied",
                $"retro wfc semantic hook {action} -> 0x{hook.TargetAddress.Value:X8}, continuation=0x{hook.ContinuationAddress:X8}, instructions={hook.InstructionCount:N0}");
            return true;
        }
        catch (Exception ex) when (ex is InvalidDataException or ArgumentOutOfRangeException or OverflowException)
        {
            record = new PatchApplicationRecord(
                hook.Address,
                RetroWfcHookUsesLink(hook) ? KamekCommandId.BranchLink : KamekCommandId.Branch,
                $"RetroWFC#{hook.RecordIndex}",
                "failed",
                ex.Message);
            return false;
        }
    }

    private static bool TryApplyRetroWfcStaticBytePatch(
        byte[] image,
        uint imageGuestStart,
        RetroWfcStaticBytePatchPlan patch,
        out PatchApplicationRecord record)
    {
        try
        {
            var patchBytes = Convert.FromHexString(patch.BytesHex);
            if (patchBytes.Length != patch.Size)
            {
                throw new InvalidDataException(
                    $"Retro WFC static byte patch #{patch.RecordIndex} declares {patch.Size:N0} byte(s) but contains {patchBytes.Length:N0} byte(s).");
            }

            var offset = checked((int)(patch.Address - imageGuestStart));
            EnsureRange(image, offset, patchBytes.Length);
            patchBytes.CopyTo(image.AsSpan(offset, patchBytes.Length));

            record = new PatchApplicationRecord(
                patch.Address,
                KamekCommandId.Write8,
                $"RetroWFC#{patch.RecordIndex}",
                "applied",
                $"retro wfc static byte patch {patchBytes.Length:N0} byte(s) sha256={patch.SourceSha256}");
            return true;
        }
        catch (Exception ex) when (ex is FormatException or InvalidDataException or ArgumentOutOfRangeException or OverflowException)
        {
            record = new PatchApplicationRecord(
                patch.Address,
                KamekCommandId.Write8,
                $"RetroWFC#{patch.RecordIndex}",
                "failed",
                ex.Message);
            return false;
        }
    }

    private static bool ApplyPatch(
        byte[] image,
        uint imageGuestStart,
        uint patchAddress,
        KamekCommand command,
        uint moduleGuestBase,
        out string detail)
    {
        var offset = checked((int)(patchAddress - imageGuestStart));
        switch (command.Id)
        {
            case KamekCommandId.Addr32:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                WriteUInt32(image, offset, target);
                detail = $"write pointer 0x{target:X8}";
                return true;
            }
            case KamekCommandId.Addr16Lo:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                WriteUInt16(image, offset, (ushort)(target & 0xFFFFu));
                detail = $"write lo16(0x{target:X8})";
                return true;
            }
            case KamekCommandId.Addr16Hi:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                WriteUInt16(image, offset, (ushort)(target >> 16));
                detail = $"write hi16(0x{target:X8})";
                return true;
            }
            case KamekCommandId.Addr16Ha:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                WriteUInt16(image, offset, (ushort)((target + 0x8000u) >> 16));
                detail = $"write ha16(0x{target:X8})";
                return true;
            }
            case KamekCommandId.Rel24:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                var original = ReadUInt32(image, offset);
                var delta = unchecked((int)target - (int)patchAddress);
                if ((delta & 0x3) != 0 || delta < -0x02000000 || delta > 0x01FFFFFC)
                {
                    throw new ArgumentOutOfRangeException(nameof(command), $"rel24 target 0x{target:X8} is out of range from 0x{patchAddress:X8}.");
                }

                var patched = (original & 0xFC000003u) | (unchecked((uint)delta) & 0x03FFFFFCu);
                WriteUInt32(image, offset, patched);
                detail = $"rel24 0x{target:X8} encoded 0x{patched:X8}";
                return true;
            }
            case KamekCommandId.Write32:
                WriteUInt32(image, offset, command.Arguments[0]);
                detail = $"write32 0x{command.Arguments[0]:X8}";
                return true;
            case KamekCommandId.Write16:
                WriteUInt16(image, offset, checked((ushort)command.Arguments[0]));
                detail = $"write16 0x{command.Arguments[0] & 0xFFFFu:X4}";
                return true;
            case KamekCommandId.Write8:
                WriteByte(image, offset, checked((byte)command.Arguments[0]));
                detail = $"write8 0x{command.Arguments[0] & 0xFFu:X2}";
                return true;
            case KamekCommandId.CondWritePointer:
                return ApplyConditionalWrite32(image, offset, command.Arguments[1], KamekAddress.Resolve(command.Arguments[0], moduleGuestBase), out detail);
            case KamekCommandId.CondWrite32:
                return ApplyConditionalWrite32(image, offset, command.Arguments[1], command.Arguments[0], out detail);
            case KamekCommandId.CondWrite16:
                return ApplyConditionalWrite16(image, offset, checked((ushort)command.Arguments[1]), checked((ushort)command.Arguments[0]), out detail);
            case KamekCommandId.CondWrite8:
                return ApplyConditionalWrite8(image, offset, checked((byte)command.Arguments[1]), checked((byte)command.Arguments[0]), out detail);
            case KamekCommandId.Branch:
            case KamekCommandId.BranchLink:
            {
                var target = KamekAddress.Resolve(command.Arguments[0], moduleGuestBase);
                var patched = KamekPpcEncoding.EncodeBranch(patchAddress, target, command.Id == KamekCommandId.BranchLink);
                WriteUInt32(image, offset, patched);
                detail = $"{command.Id} 0x{target:X8} encoded 0x{patched:X8}";
                return true;
            }
            default:
                throw new InvalidDataException($"Unsupported Code.pul command id {command.Id}.");
        }
    }

    private static bool ApplyConditionalWrite32(byte[] image, int offset, uint expected, uint value, out string detail)
    {
        var actual = ReadUInt32(image, offset);
        if (actual != expected)
        {
            detail = $"conditional write32 skipped; expected 0x{expected:X8}, actual 0x{actual:X8}";
            return false;
        }

        WriteUInt32(image, offset, value);
        detail = $"conditional write32 applied 0x{value:X8}";
        return true;
    }

    private static bool ApplyConditionalWrite16(byte[] image, int offset, ushort expected, ushort value, out string detail)
    {
        var actual = ReadUInt16(image, offset);
        if (actual != expected)
        {
            detail = $"conditional write16 skipped; expected 0x{expected:X4}, actual 0x{actual:X4}";
            return false;
        }

        WriteUInt16(image, offset, value);
        detail = $"conditional write16 applied 0x{value:X4}";
        return true;
    }

    private static bool ApplyConditionalWrite8(byte[] image, int offset, byte expected, byte value, out string detail)
    {
        var actual = ReadByte(image, offset);
        if (actual != expected)
        {
            detail = $"conditional write8 skipped; expected 0x{expected:X2}, actual 0x{actual:X2}";
            return false;
        }

        WriteByte(image, offset, value);
        detail = $"conditional write8 applied 0x{value:X2}";
        return true;
    }

    private static string FormatTarget(KamekCommand command, uint moduleGuestBase)
    {
        if (command.Arguments.Count == 0)
        {
            return string.Empty;
        }

        return command.Id switch
        {
            KamekCommandId.Addr32 or
            KamekCommandId.Addr16Lo or
            KamekCommandId.Addr16Hi or
            KamekCommandId.Addr16Ha or
            KamekCommandId.Rel24 or
            KamekCommandId.Branch or
            KamekCommandId.BranchLink or
            KamekCommandId.CondWritePointer => $"0x{KamekAddress.Resolve(command.Arguments[0], moduleGuestBase):X8}",
            _ => string.Join(", ", command.Arguments.Select(arg => $"0x{arg:X8}"))
        };
    }

    private static uint ReadUInt32(byte[] image, int offset)
    {
        EnsureRange(image, offset, sizeof(uint));
        return BinaryPrimitives.ReadUInt32BigEndian(image.AsSpan(offset, sizeof(uint)));
    }

    private static ushort ReadUInt16(byte[] image, int offset)
    {
        EnsureRange(image, offset, sizeof(ushort));
        return BinaryPrimitives.ReadUInt16BigEndian(image.AsSpan(offset, sizeof(ushort)));
    }

    private static byte ReadByte(byte[] image, int offset)
    {
        EnsureRange(image, offset, sizeof(byte));
        return image[offset];
    }

    private static void WriteUInt32(byte[] image, int offset, uint value)
    {
        EnsureRange(image, offset, sizeof(uint));
        BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(offset, sizeof(uint)), value);
    }

    private static void WriteUInt16(byte[] image, int offset, ushort value)
    {
        EnsureRange(image, offset, sizeof(ushort));
        BinaryPrimitives.WriteUInt16BigEndian(image.AsSpan(offset, sizeof(ushort)), value);
    }

    private static void WriteByte(byte[] image, int offset, byte value)
    {
        EnsureRange(image, offset, sizeof(byte));
        image[offset] = value;
    }

    private static void EnsureRange(byte[] image, int offset, int size)
    {
        if (offset < 0 || offset + size > image.Length)
        {
            throw new ArgumentOutOfRangeException(nameof(offset), $"Patch offset 0x{offset:X} size {size} exceeds image length 0x{image.Length:X}.");
        }
    }



}
