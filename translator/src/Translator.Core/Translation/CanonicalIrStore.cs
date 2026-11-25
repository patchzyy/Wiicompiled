using System.Text;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

public interface ICanonicalIrProvider
{
    bool TryGet(uint entryPoint, out IrFunction function);
}

/// <summary>
/// Bounded, deterministic canonical SSA ownership for a translation wave. Each
/// graph is immediately encoded to numeric opcodes and interned string IDs, so
/// the discovery object graph can be collected without repeating front-end work.
/// </summary>
public sealed class CanonicalIrStore : ICanonicalIrProvider
{
    public const int FormatVersion = 1;
    private readonly object _gate = new();
    private readonly Dictionary<uint, byte[]> _entries = new();

    public int Count { get { lock (_gate) return _entries.Count; } }

    public void Put(uint entryPoint, IrFunction ssaFunction)
    {
        var encoded = CanonicalIrBinaryCodec.Encode(ssaFunction);
        lock (_gate)
        {
            _entries[entryPoint] = encoded;
        }
    }

    public void Remove(uint entryPoint)
    {
        lock (_gate)
        {
            _entries.Remove(entryPoint);
        }
    }

    public bool TryGet(uint entryPoint, out IrFunction function)
    {
        byte[]? encoded;
        lock (_gate) _entries.TryGetValue(entryPoint, out encoded);
        if (encoded is null)
        {
            function = null!;
            return false;
        }
        function = CanonicalIrBinaryCodec.Decode(encoded);
        return true;
    }
}

internal static class CanonicalIrBinaryCodec
{
    private static readonly byte[] Magic = "MKWSSA01"u8.ToArray();

    private enum Op : byte
    {
        Assign, Binary,
        Load, Store, Call, IndirectCall, SetCrField, Phi, Branch, Jump,
        IndirectJump, JumpTable, Return, Comment, TracePpc, Undefined
    }

    // Reused per thread so encoding doesn't regrow (and re-fault) a fresh buffer per function.
    [ThreadStatic] private static MemoryStream? _encodeStream;

    public static byte[] Encode(IrFunction function)
    {
        var strings = BuildStringTable(function);
        var ids = strings.Select((value, index) => (value, index))
            .ToDictionary(static pair => pair.value, static pair => pair.index, StringComparer.Ordinal);
        var stream = _encodeStream ??= new MemoryStream();
        stream.SetLength(0);
        using var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: true);
        writer.Write(Magic);
        WriteVarUInt(writer, (uint)CanonicalIrStore.FormatVersion);
        WriteVarUInt(writer, checked((uint)strings.Count));
        foreach (var value in strings)
        {
            var bytes = Encoding.UTF8.GetBytes(value);
            WriteVarUInt(writer, checked((uint)bytes.Length));
            writer.Write(bytes);
        }
        WriteId(writer, ids, function.Name);
        WriteId(writer, ids, function.EntryLabel);
        WriteVarUInt(writer, checked((uint)function.Blocks.Count));
        foreach (var block in function.Blocks)
        {
            WriteId(writer, ids, block.Label);
            WriteVarUInt(writer, checked((uint)block.Instructions.Count));
            foreach (var instruction in block.Instructions)
                WriteInstruction(writer, ids, instruction);
        }
        writer.Flush();
        return stream.ToArray();
    }

    public static IrFunction Decode(ReadOnlySpan<byte> encoded) => Decode(encoded.ToArray());

    public static IrFunction Decode(byte[] encoded)
    {
        using var stream = new MemoryStream(encoded, writable: false);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: true);
        if (!reader.ReadBytes(Magic.Length).SequenceEqual(Magic) ||
            ReadVarUInt(reader) != CanonicalIrStore.FormatVersion)
            throw new InvalidDataException("Unsupported canonical SSA function payload.");
        var stringCount = CheckedCount(ReadVarUInt(reader));
        var strings = new string[stringCount];
        for (var i = 0; i < strings.Length; i++)
        {
            var length = CheckedCount(ReadVarUInt(reader));
            var bytes = reader.ReadBytes(length);
            if (bytes.Length != length) throw new EndOfStreamException();
            strings[i] = Encoding.UTF8.GetString(bytes);
        }
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Length)];
        var name = Id();
        var entry = Id();
        var blocks = new IrBasicBlock[CheckedCount(ReadVarUInt(reader))];
        for (var blockIndex = 0; blockIndex < blocks.Length; blockIndex++)
        {
            var label = Id();
            var instructions = new IrInstruction[CheckedCount(ReadVarUInt(reader))];
            for (var instructionIndex = 0; instructionIndex < instructions.Length; instructionIndex++)
                instructions[instructionIndex] = ReadInstruction(reader, strings);
            blocks[blockIndex] = new IrBasicBlock(label, instructions);
        }
        if (stream.Position != stream.Length)
            throw new InvalidDataException("Canonical SSA function payload has trailing data.");
        return new IrFunction(name, entry, blocks);
    }

    private static List<string> BuildStringTable(IrFunction function)
    {
        var result = new List<string>();
        var seen = new HashSet<string>(StringComparer.Ordinal);
        void Add(string? value) { if (value is not null && seen.Add(value)) result.Add(value); }
        void Value(IrValue value) { Add(value.RegisterName); if (value.Kind is not ("register" or "const")) Add(value.Kind); }
        Add(function.Name); Add(function.EntryLabel);
        foreach (var block in function.Blocks)
        {
            Add(block.Label);
            foreach (var instruction in block.Instructions)
            {
                switch (instruction)
                {
                    case IrAssign x: Add(x.Destination); Value(x.Value); break;
                    case IrBinary x: Add(x.Destination); Value(x.Left); Value(x.Right); Add(x.Op); break;
                    case IrLoad x: Add(x.Destination); Add(x.Address.Base); break;
                    case IrStore x: Add(x.Address.Base); Value(x.Source); break;
                    case IrCall x: Add(x.Destination); Add(x.Target); foreach (var v in x.Arguments) Value(v); break;
                    case IrIndirectCall x: Add(x.Destination); Value(x.Target); foreach (var v in x.Arguments) Value(v); break;
                    case IrSetCrField x: Value(x.Left); Value(x.Right); break;
                    case IrPhi x: Add(x.Destination); foreach (var pair in x.Sources.OrderBy(static p => p.Key, StringComparer.Ordinal)) { Add(pair.Key); Add(pair.Value); } break;
                    case IrBranch x: Add(x.Condition); Add(x.TrueLabel); Add(x.FalseLabel); Add(x.ConditionRegister); break;
                    case IrJump x: Add(x.TargetLabel); break;
                    case IrIndirectJump x: Value(x.Target); break;
                    case IrJumpTable x: Add(x.Selector); foreach (var item in x.Cases) Add(item.TargetLabel); break;
                    case IrReturn x: if (x.Value is not null) Value(x.Value); break;
                    case IrComment x: Add(x.Text); break;
                    case IrTracePpc x: Add(x.Disassembly); Add(x.RawHex); break;
                    case IrUndefined x: Add(x.Disassembly); Add(x.Reason); break;
                    default: throw new InvalidDataException($"Instruction {instruction.GetType().Name} is not valid in canonical front-end SSA.");
                }
            }
        }
        return result;
    }

    private static void WriteInstruction(BinaryWriter writer, IReadOnlyDictionary<string, int> ids, IrInstruction instruction)
    {
        void Id(string value) => WriteId(writer, ids, value);
        void Value(IrValue value) => WriteValue(writer, ids, value);
        void Values(IReadOnlyList<IrValue> values) { WriteVarUInt(writer, checked((uint)values.Count)); foreach (var value in values) Value(value); }
        switch (instruction)
        {
            case IrAssign x: writer.Write((byte)Op.Assign); Id(x.Destination); Value(x.Value); break;
            case IrBinary x: writer.Write((byte)Op.Binary); Id(x.Destination); Value(x.Left); Value(x.Right); Id(x.Op); break;
            case IrLoad x: writer.Write((byte)Op.Load); Id(x.Destination); WriteAddress(writer, ids, x.Address); WriteVarInt(writer, x.SizeBytes); break;
            case IrStore x: writer.Write((byte)Op.Store); WriteAddress(writer, ids, x.Address); Value(x.Source); WriteVarInt(writer, x.SizeBytes); break;
            case IrCall x: writer.Write((byte)Op.Call); Id(x.Destination); Id(x.Target); Values(x.Arguments); break;
            case IrIndirectCall x: writer.Write((byte)Op.IndirectCall); Id(x.Destination); Value(x.Target); Values(x.Arguments); break;
            case IrSetCrField x: writer.Write((byte)Op.SetCrField); WriteVarInt(writer, x.FieldIndex); Value(x.Left); Value(x.Right); writer.Write(x.IsUnsigned); break;
            case IrPhi x:
                writer.Write((byte)Op.Phi); Id(x.Destination);
                var sources = x.Sources.OrderBy(static pair => pair.Key, StringComparer.Ordinal).ToArray();
                WriteVarUInt(writer, checked((uint)sources.Length));
                foreach (var pair in sources) { Id(pair.Key); Id(pair.Value); }
                break;
            case IrBranch x: writer.Write((byte)Op.Branch); Id(x.Condition); Id(x.TrueLabel); Id(x.FalseLabel); Id(x.ConditionRegister); break;
            case IrJump x: writer.Write((byte)Op.Jump); Id(x.TargetLabel); break;
            case IrIndirectJump x: writer.Write((byte)Op.IndirectJump); Value(x.Target); break;
            case IrJumpTable x:
                writer.Write((byte)Op.JumpTable); Id(x.Selector); WriteVarUInt(writer, checked((uint)x.Cases.Count));
                foreach (var item in x.Cases) { writer.Write(item.TargetAddress); Id(item.TargetLabel); }
                break;
            case IrReturn x: writer.Write((byte)Op.Return); writer.Write(x.Value is not null); if (x.Value is not null) Value(x.Value); break;
            case IrComment x: writer.Write((byte)Op.Comment); Id(x.Text); break;
            case IrTracePpc x: writer.Write((byte)Op.TracePpc); writer.Write(x.Address); Id(x.Disassembly); Id(x.RawHex); break;
            case IrUndefined x: writer.Write((byte)Op.Undefined); writer.Write(x.Address); writer.Write(x.RawInstruction); Id(x.Disassembly); writer.Write(x.Reason is not null); if (x.Reason is not null) Id(x.Reason); break;
            default: throw new InvalidDataException($"Instruction {instruction.GetType().Name} is not valid in canonical front-end SSA.");
        }
    }

    private static IrInstruction ReadInstruction(BinaryReader reader, IReadOnlyList<string> strings)
    {
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Count)];
        IrValue Value() => ReadValue(reader, strings);
        IrValue[] Values() { var values = new IrValue[CheckedCount(ReadVarUInt(reader))]; for (var i = 0; i < values.Length; i++) values[i] = Value(); return values; }
        return (Op)reader.ReadByte() switch
        {
            Op.Assign => new IrAssign(Id(), Value()),
            Op.Binary => new IrBinary(Id(), Value(), Value(), Id()),
            Op.Load => new IrLoad(Id(), ReadAddress(reader, strings), ReadVarInt(reader)),
            Op.Store => new IrStore(ReadAddress(reader, strings), Value(), ReadVarInt(reader)),
            Op.Call => new IrCall(Id(), Id(), Values()),
            Op.IndirectCall => new IrIndirectCall(Id(), Value(), Values()),
            Op.SetCrField => new IrSetCrField(ReadVarInt(reader), Value(), Value(), reader.ReadBoolean()),
            Op.Phi => ReadPhi(reader, strings),
            Op.Branch => new IrBranch(Id(), Id(), Id(), Id()),
            Op.Jump => new IrJump(Id()),
            Op.IndirectJump => new IrIndirectJump(Value()),
            Op.JumpTable => ReadJumpTable(reader, strings),
            Op.Return => new IrReturn(reader.ReadBoolean() ? Value() : null),
            Op.Comment => new IrComment(Id()),
            Op.TracePpc => new IrTracePpc(reader.ReadUInt32(), Id(), Id()),
            Op.Undefined => ReadUndefined(reader, strings),
            var op => throw new InvalidDataException($"Unknown canonical SSA opcode {(byte)op}.")
        };
    }

    private static IrPhi ReadPhi(BinaryReader reader, IReadOnlyList<string> strings)
    {
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Count)];
        var destination = Id();
        var count = CheckedCount(ReadVarUInt(reader));
        var sources = new Dictionary<string, string>(count, StringComparer.Ordinal);
        for (var i = 0; i < count; i++) sources.Add(Id(), Id());
        return new IrPhi(destination, sources);
    }

    private static IrJumpTable ReadJumpTable(BinaryReader reader, IReadOnlyList<string> strings)
    {
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Count)];
        var selector = Id();
        var cases = new IrJumpTableCase[CheckedCount(ReadVarUInt(reader))];
        for (var i = 0; i < cases.Length; i++) cases[i] = new IrJumpTableCase(reader.ReadUInt32(), Id());
        return new IrJumpTable(selector, cases);
    }

    private static IrUndefined ReadUndefined(BinaryReader reader, IReadOnlyList<string> strings)
    {
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Count)];
        var address = reader.ReadUInt32();
        var raw = reader.ReadUInt32();
        var disassembly = Id();
        return new IrUndefined(address, raw, disassembly, reader.ReadBoolean() ? Id() : null);
    }

    private static void WriteValue(BinaryWriter writer, IReadOnlyDictionary<string, int> ids, IrValue value)
    {
        if (value.Kind == "register") { writer.Write((byte)0); WriteId(writer, ids, value.RegisterName!); }
        else if (value.Kind == "const") { writer.Write((byte)1); writer.Write(value.Constant!.Value); }
        else { writer.Write((byte)2); WriteId(writer, ids, value.Kind); writer.Write(value.RegisterName is not null); if (value.RegisterName is not null) WriteId(writer, ids, value.RegisterName); writer.Write(value.Constant.HasValue); if (value.Constant.HasValue) writer.Write(value.Constant.Value); }
    }

    private static IrValue ReadValue(BinaryReader reader, IReadOnlyList<string> strings)
    {
        string Id() => strings[CheckedIndex(ReadVarUInt(reader), strings.Count)];
        return reader.ReadByte() switch
        {
            0 => IrValue.Register(Id()),
            1 => IrValue.Imm(reader.ReadInt64()),
            2 => new IrValue(Id(), reader.ReadBoolean() ? Id() : null, reader.ReadBoolean() ? reader.ReadInt64() : null),
            var kind => throw new InvalidDataException($"Unknown canonical SSA value kind {kind}.")
        };
    }

    private static void WriteAddress(BinaryWriter writer, IReadOnlyDictionary<string, int> ids, IrAddress address) { WriteId(writer, ids, address.Base); WriteVarInt(writer, address.Offset); }
    private static IrAddress ReadAddress(BinaryReader reader, IReadOnlyList<string> strings) => new(strings[CheckedIndex(ReadVarUInt(reader), strings.Count)], ReadVarInt(reader));
    private static void WriteId(BinaryWriter writer, IReadOnlyDictionary<string, int> ids, string value) => WriteVarUInt(writer, checked((uint)ids[value]));
    private static int CheckedCount(uint value) => value <= 16_000_000 ? checked((int)value) : throw new InvalidDataException("Canonical SSA count is unreasonable.");
    private static int CheckedIndex(uint value, int count) => value < count ? checked((int)value) : throw new InvalidDataException("Canonical SSA string ID is out of range.");
    private static void WriteVarInt(BinaryWriter writer, int value) => WriteVarUInt(writer, unchecked((uint)((value << 1) ^ (value >> 31))));
    private static int ReadVarInt(BinaryReader reader) { var value = ReadVarUInt(reader); return unchecked((int)((value >> 1) ^ (uint)-(int)(value & 1))); }
    private static void WriteVarUInt(BinaryWriter writer, uint value) { while (value >= 0x80) { writer.Write((byte)(value | 0x80)); value >>= 7; } writer.Write((byte)value); }
    private static uint ReadVarUInt(BinaryReader reader) { uint value = 0; for (var shift = 0; shift <= 28; shift += 7) { var next = reader.ReadByte(); value |= (uint)(next & 0x7F) << shift; if ((next & 0x80) == 0) return value; } throw new InvalidDataException("Canonical SSA varint is too long."); }
}
