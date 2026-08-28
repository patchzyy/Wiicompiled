using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using Translator.Core.IO;
using Translator.Core.Loading;

namespace Translator.Core.Parsing.Rel;

public sealed class RelFile
{
    private readonly byte[] _raw;

    private RelFile(byte[] raw,
        IReadOnlyList<RelSection> sections,
        IReadOnlyList<RelImportEntry> imports,
        uint totalSize)
    {
        _raw = raw;
        Sections = sections;
        _imports = imports;
        _totalSize = totalSize;
    }

    private readonly IReadOnlyList<RelImportEntry> _imports;
    private readonly uint _totalSize;

    /// <summary>The only REL header surface any caller outside this file reads.</summary>
    public IReadOnlyList<RelSection> Sections { get; }

    public static RelFile Load(string path)
    {
        var raw = File.ReadAllBytes(path);
        using var stream = new MemoryStream(raw, writable: false);
        using var reader = new BigEndianBinaryReader(stream, leaveOpen: true);

        reader.Seek(0x0C, SeekOrigin.Begin);
        var sectionCount = reader.ReadUInt32();
        var sectionTableOffset = reader.ReadUInt32();

        reader.Seek(0x1C, SeekOrigin.Begin);
        _ = reader.ReadUInt32(); // version (unused for now)
        var bssSize = reader.ReadUInt32();

        reader.Seek(0x28, SeekOrigin.Begin);
        var importTableOffset = reader.ReadUInt32();
        var importTableSize = reader.ReadUInt32();

        _ = reader.ReadByte(); // prolog section (0x30)
        _ = reader.ReadByte(); // epilog section (0x31)
        _ = reader.ReadByte(); // unresolved section (0x32)
        var bssSectionIndex = reader.ReadByte(); // 0x33

        // Offsets used by the runtime loader (kept for debugging / layout checks)
        _ = reader.ReadUInt32(); // prolog offset within prolog section (0x34)
        _ = reader.ReadUInt32(); // epilog offset within epilog section (0x38)
        _ = reader.ReadUInt32(); // unresolved offset within unresolved section (0x3C)

        _ = reader.ReadUInt32(); // align (0x40)
        var bssAlign = reader.ReadUInt32(); // 0x44

        // Basic validation
        if (sectionTableOffset + sectionCount * 8 > raw.Length)
        {
            throw new InvalidDataException("Section table extends beyond REL bounds");
        }

        if (importTableOffset + importTableSize > raw.Length)
        {
            throw new InvalidDataException("Import table extends beyond REL bounds");
        }

        var sections = ParseSections(raw, sectionTableOffset, sectionCount);
        if (bssAlign == 0)
        {
            bssAlign = 0x20;
        }
        var bssOffset = AlignUp(CalculateBssOffset(raw.Length), bssAlign);
        var totalSize = checked(bssOffset + bssSize);
        var adjustedSections = ApplyBssLayout(sections, bssSectionIndex, bssOffset, bssSize);
        var imports = ParseImports(raw, importTableOffset, importTableSize);

        return new RelFile(
            raw,
            new ReadOnlyCollection<RelSection>(adjustedSections),
            new ReadOnlyCollection<RelImportEntry>(imports),
            totalSize);
    }

    private static List<RelSection> ParseSections(byte[] raw, uint offset, uint count)
    {
        var sections = new List<RelSection>((int)count);
        for (var i = 0; i < count; i++)
        {
            var entryOffset = checked((int)(offset + (uint)(i * 8)));
            var offRaw = ReadUInt32(raw, entryOffset);
            var size = ReadUInt32(raw, entryOffset + 4);
            var executable = (offRaw & 1) != 0;
            var fileOffset = offRaw & ~1u;
            sections.Add(new RelSection(i, fileOffset, size, executable));
        }
        return sections;
    }

    private static List<RelSection> ApplyBssLayout(List<RelSection> sections, byte bssSectionIndex, uint bssOffset, uint bssSize)
    {
        // REL BSS sections have fileOffset==0; the runtime loader fills the real address in at load
        // time. We place BSS right after the loaded REL bytes in our flat image so relocations that
        // target or write into BSS resolve in-bounds.
        var bssCursor = bssOffset;
        var bssEnd = checked(bssOffset + bssSize);

        var adjusted = new List<RelSection>(sections.Count);
        foreach (var section in sections)
        {
            var isBss = section.FileOffset == 0 && (section.Size != 0 || section.Index == bssSectionIndex);
            if (!isBss)
            {
                adjusted.Add(section);
                continue;
            }

            if (bssCursor > bssEnd)
            {
                throw new InvalidDataException("BSS layout exceeded declared BSS size");
            }

            adjusted.Add(new RelSection(section.Index, bssCursor, section.Size, section.Executable));
            bssCursor = checked(bssCursor + section.Size);
        }

        if (bssCursor > bssEnd)
        {
            throw new InvalidDataException($"BSS section sizes exceed header bssSize (cursor=0x{bssCursor:X8}, end=0x{bssEnd:X8})");
        }

        return adjusted;
    }

    private static List<RelImportEntry> ParseImports(byte[] raw, uint offset, uint size)
    {
        var imports = new List<RelImportEntry>((int)(size / 8));
        for (var cursor = offset; cursor < offset + size; cursor += 8)
        {
            var moduleId = ReadUInt32(raw, (int)cursor);
            var relocationOffset = ReadUInt32(raw, (int)cursor + 4);
            imports.Add(new RelImportEntry(moduleId, relocationOffset));
        }
        return imports;
    }

    private static uint CalculateBssOffset(int fileSize)
    {
        // BSS placement must be deterministic for offline relocation. We assume the common
        // OSAllocFromHeap layout, alignUp(size + 0x20 header, 0x20) = (size + 0x20 + 0x1F) & ~0x1F,
        // and place BSS right after the heap-rounded REL allocation; a future REL consumer with a
        // different allocator would need its own placement policy.
        const uint heapAlign = 0x20;
        const uint heapHeaderSize = 0x20;
        return AlignUp(checked((uint)fileSize + heapHeaderSize), heapAlign);
    }

    private static uint AlignUp(uint value, uint alignment)
    {
        if (alignment == 0)
        {
            return value;
        }

        if ((alignment & (alignment - 1)) != 0)
        {
            throw new InvalidDataException($"Alignment must be a power of two (got 0x{alignment:X})");
        }

        var mask = alignment - 1;
        return checked((value + mask) & ~mask);
    }

    public RelImage BuildImage(uint baseAddress,
        uint dolBaseAddress = MemoryLayout.DolBaseAddress,
        bool applyRelocations = true)
    {
        var buffer = new byte[checked((int)_totalSize)];
        Buffer.BlockCopy(_raw, 0, buffer, 0, _raw.Length);

        if (applyRelocations)
        {
            ApplyRelocations(buffer, baseAddress, dolBaseAddress);
        }

        return new RelImage(buffer, baseAddress);
    }
    
    private void ApplyRelocations(byte[] memory, uint baseAddress, uint dolBaseAddress)
    {
        foreach (var import in _imports)
        {
            uint currentOffset = 0;
            byte currentSection = 0;
            var cursor = import.RelocationOffset;

            while (true)
            {
                var delta = ReadUInt16(_raw, (int)cursor);
                var type = (RelocationType)ReadByte(_raw, (int)cursor + 2);
                var symbolSection = ReadByte(_raw, (int)cursor + 3);
                var addend = ReadUInt32(_raw, (int)cursor + 4);
                cursor += 8;

                if (type == RelocationType.R_RVL_STOP)
                {
                    break;
                }

                if (type == RelocationType.R_RVL_SECT)
                {
                    currentSection = symbolSection;
                    currentOffset = 0;
                    continue;
                }

                currentOffset = checked(currentOffset + delta);
                if (currentSection >= Sections.Count)
                {
                    throw new InvalidDataException($"Relocation referenced unknown section index {currentSection}");
                }

                var dstSection = Sections[currentSection];
                var dst = baseAddress + dstSection.FileOffset + currentOffset;

                uint target;
                if (import.ModuleId == 0)
                {
                    target = addend;
                    if (target < MemoryLayout.RamBase)
                    {
                        target = dolBaseAddress + target;
                    }
                }
                else
                {
                    if (symbolSection >= Sections.Count)
                    {
                        throw new InvalidDataException($"Relocation referenced unknown symbol section {symbolSection}");
                    }

                    var symbol = Sections[symbolSection];
                    target = baseAddress + symbol.FileOffset + addend;
                }

                var memIndex = checked((int)(dst - baseAddress));
                var orig = ReadUInt32(memory, memIndex);

                switch (type)
                {
                    // StaticR.rel uses exactly these five relocation types; any
                    // other encoding reaches the default arm and fails loudly.
                    case RelocationType.R_RVL_NONE:
                        break;
                    case RelocationType.R_PPC_ADDR32:
                        WriteUInt32(memory, memIndex, target);
                        break;
                    case RelocationType.R_PPC_ADDR16_LO:
                        WriteUInt16(memory, memIndex, (ushort)(target & 0xFFFF));
                        break;
                    case RelocationType.R_PPC_ADDR16_HA:
                        WriteUInt16(memory, memIndex, (ushort)(((target + 0x8000) >> 16) & 0xFFFF));
                        break;
                    case RelocationType.R_PPC_REL24:
                        {
                            var disp = (target - dst) & 0x03FFFFFC;
                            var patched = (orig & 0xFC000003) | disp;
                            WriteUInt32(memory, memIndex, patched);
                            break;
                        }
                    default:
                        throw new NotSupportedException($"Unhandled relocation type {type} at 0x{dst:X8}");
                }
            }
        }
    }

    private static byte ReadByte(IReadOnlyList<byte> data, int offset)
    {
        if (offset < 0 || offset >= data.Count)
        {
            throw new InvalidDataException("Attempted to read past end of REL payload");
        }
        return data[offset];
    }

    private static ushort ReadUInt16(IReadOnlyList<byte> data, int offset)
    {
        // "offset + 2 > data.Count" can itself overflow when offset is near int.MaxValue (reachable
        // from a crafted RelocationOffset via the uint->int cast in ApplyRelocations), wrapping to a
        // negative value that would wrongly pass this check. Subtracting from data.Count instead
        // cannot overflow the same way.
        if (offset < 0 || offset > data.Count - 2)
        {
            throw new InvalidDataException("Attempted to read past end of REL payload");
        }
        Span<byte> buffer = stackalloc byte[2];
        buffer[0] = data[offset];
        buffer[1] = data[offset + 1];
        return BinaryPrimitives.ReadUInt16BigEndian(buffer);
    }

    private static uint ReadUInt32(IReadOnlyList<byte> data, int offset)
    {
        // See ReadUInt16 above: compare via subtraction, not addition, to avoid an int overflow
        // near int.MaxValue letting an out-of-range offset slip past this check.
        if (offset < 0 || offset > data.Count - 4)
        {
            throw new InvalidDataException("Attempted to read past end of REL payload");
        }
        Span<byte> buffer = stackalloc byte[4];
        buffer[0] = data[offset];
        buffer[1] = data[offset + 1];
        buffer[2] = data[offset + 2];
        buffer[3] = data[offset + 3];
        return BinaryPrimitives.ReadUInt32BigEndian(buffer);
    }

    private static void WriteUInt16(IList<byte> data, int offset, ushort value)
    {
        if (offset < 0 || offset + 2 > data.Count)
        {
            throw new InvalidDataException("Attempted to write past end of REL memory");
        }
        Span<byte> buffer = stackalloc byte[2];
        BinaryPrimitives.WriteUInt16BigEndian(buffer, value);
        data[offset] = buffer[0];
        data[offset + 1] = buffer[1];
    }

    private static void WriteUInt32(IList<byte> data, int offset, uint value)
    {
        if (offset < 0 || offset + 4 > data.Count)
        {
            throw new InvalidDataException("Attempted to write past end of REL memory");
        }
        Span<byte> buffer = stackalloc byte[4];
        BinaryPrimitives.WriteUInt32BigEndian(buffer, value);
        data[offset] = buffer[0];
        data[offset + 1] = buffer[1];
        data[offset + 2] = buffer[2];
        data[offset + 3] = buffer[3];
    }
}
