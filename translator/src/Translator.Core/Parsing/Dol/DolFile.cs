using System;
using System.Collections.Generic;
using System.Collections.ObjectModel;
using System.IO;
using System.Linq;
using Translator.Core.IO;
using Translator.Core.Loading;

namespace Translator.Core.Parsing.Dol;

public sealed class DolFile
{
    private const int TextCount = 7;
    private const int DataCount = 11;
    private static readonly string[] TextNames = {".init", ".text", "text2", "text3", "text4", "text5", "text6"};
    private static readonly string[] DataNames = {"extab", "extabindex", ".ctors", ".dtors", ".rodata", ".data", ".sdata", ".sdata2", "data8", "data9", "data10"};

    private DolFile(IReadOnlyList<DolSection> sections, uint entryPoint, uint bssAddress, uint bssSize)
    {
        Sections = sections;
        EntryPoint = entryPoint;
        BssAddress = bssAddress;
        BssSize = bssSize;
        MemoryRange = CalculateRange(sections, bssAddress, bssSize);
    }

    public IReadOnlyList<DolSection> Sections { get; }
    public uint EntryPoint { get; }
    public uint BssAddress { get; }
    public uint BssSize { get; }
    public AddressRange MemoryRange { get; }

    public IEnumerable<DolSection> ExecutableSections => Sections.Where(s => s.IsExecutable);

    public static DolFile Load(string path)
    {
        using var stream = TrackedFs.OpenRead(path);
        return Load(stream);
    }

    public static DolFile Load(Stream stream)
    {
        if (!stream.CanSeek)
        {
            throw new ArgumentException("DOL parsing requires a seekable stream", nameof(stream));
        }

        using var reader = new BigEndianBinaryReader(stream, leaveOpen: true);

        var textOffsets = ReadArray(reader, offset: 0x00, count: TextCount);
        var dataOffsets = ReadArray(reader, offset: 0x1C, count: DataCount);
        var textAddresses = ReadArray(reader, offset: 0x48, count: TextCount);
        var dataAddresses = ReadArray(reader, offset: 0x64, count: DataCount);
        var textSizes = ReadArray(reader, offset: 0x90, count: TextCount);
        var dataSizes = ReadArray(reader, offset: 0xAC, count: DataCount);

        reader.Seek(0xD8, SeekOrigin.Begin);
        var bssAddress = reader.ReadUInt32();
        var bssSize = reader.ReadUInt32();
        var entryPoint = reader.ReadUInt32();

        var sections = new List<DolSection>();

        for (var i = 0; i < TextCount; i++)
        {
            if (textSizes[i] == 0)
            {
                continue;
            }

            var data = ReadSectionData(stream, textOffsets[i], textSizes[i]);
            sections.Add(new DolSection(
                index: i,
                name: TextNames.ElementAtOrDefault(i) ?? $"text{i}",
                kind: SectionKind.Text,
                fileOffset: textOffsets[i],
                virtualAddress: textAddresses[i],
                size: textSizes[i],
                data: data));
        }

        for (var i = 0; i < DataCount; i++)
        {
            if (dataSizes[i] == 0)
            {
                continue;
            }

            var data = ReadSectionData(stream, dataOffsets[i], dataSizes[i]);
            sections.Add(new DolSection(
                index: i,
                name: DataNames.ElementAtOrDefault(i) ?? $"data{i}",
                kind: SectionKind.Data,
                fileOffset: dataOffsets[i],
                virtualAddress: dataAddresses[i],
                size: dataSizes[i],
                data: data));
        }

        if (bssSize > 0)
        {
            sections.Add(new DolSection(
                index: sections.Count,
                name: ".bss",
                kind: SectionKind.Bss,
                fileOffset: 0,
                virtualAddress: bssAddress,
                size: bssSize,
                data: ReadOnlyMemory<byte>.Empty));
        }

        return new DolFile(new ReadOnlyCollection<DolSection>(sections), entryPoint, bssAddress, bssSize);
    }

    private static AddressRange CalculateRange(IEnumerable<DolSection> sections, uint bssAddress, uint bssSize)
    {
        var hasAny = false;
        uint start = uint.MaxValue;
        uint end = 0;

        foreach (var section in sections)
        {
            if (section.Size == 0)
            {
                continue;
            }

            hasAny = true;
            start = Math.Min(start, section.VirtualAddress);
            end = Math.Max(end, section.Range.End);
        }

        if (bssSize > 0)
        {
            hasAny = true;
            start = Math.Min(start, bssAddress);
            end = Math.Max(end, checked(bssAddress + bssSize));
        }

        if (!hasAny)
        {
            return new AddressRange(0, 0);
        }

        return new AddressRange(start, end);
    }

    private static uint[] ReadArray(BigEndianBinaryReader reader, int offset, int count)
    {
        reader.Seek(offset, SeekOrigin.Begin);
        var values = new uint[count];
        for (var i = 0; i < count; i++)
        {
            values[i] = reader.ReadUInt32();
        }
        return values;
    }

    private static ReadOnlyMemory<byte> ReadSectionData(Stream stream, uint offset, uint size)
    {
        if (size == 0)
        {
            return ReadOnlyMemory<byte>.Empty;
        }

        var length = checked((int)size);
        var data = new byte[length];
        stream.Seek(offset, SeekOrigin.Begin);
        try
        {
            stream.ReadExactly(data);
        }
        catch (EndOfStreamException exception)
        {
            throw new InvalidDataException(
                $"Section at file offset 0x{offset:X} is truncated; expected {data.Length} bytes.",
                exception);
        }

        return data;
    }
}
