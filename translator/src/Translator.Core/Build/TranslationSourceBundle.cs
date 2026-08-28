using System.Security.Cryptography;
using System.Text;
using Translator.Core.Loading;

namespace Translator.Core.Build;

/// <summary>
/// One bundled translation unit. Sha256/SourceByteLength are populated by TranslationSourceBundle.Read
/// from the already-verified on-disk record, so consumers can validate against external metadata
/// without re-hashing; entries built by producers leave them unset.
/// </summary>
public sealed record TranslationSourceBundleEntry(
    uint EntryPoint,
    string VirtualPath,
    string Source,
    string? Sha256 = null,
    long SourceByteLength = -1);

/// <summary>Versioned deterministic source handoff between lowering and aggregate shard emission.</summary>
public sealed class TranslationSourceBundle
{
    private static readonly byte[] Magic = "MKWSRC01"u8.ToArray();
    public const int FormatVersion = 1;
    private readonly IReadOnlyList<TranslationSourceBundleEntry> _entries;
    // A null value marks an address carried by more than one virtual path. Such an
    // address is ambiguous and must not resolve, exactly as the previous linear
    // "exactly one match" probe behaved.
    private readonly Dictionary<uint, TranslationSourceBundleEntry?> _byEntryPoint;

    private TranslationSourceBundle(IReadOnlyList<TranslationSourceBundleEntry> entries)
    {
        _entries = entries;
        _byEntryPoint = new Dictionary<uint, TranslationSourceBundleEntry?>(entries.Count);
        foreach (var entry in entries)
        {
            if (!_byEntryPoint.TryAdd(entry.EntryPoint, entry)) _byEntryPoint[entry.EntryPoint] = null;
        }
    }

    public IReadOnlyList<TranslationSourceBundleEntry> Entries => _entries;
    public bool TryGet(uint entryPoint, out TranslationSourceBundleEntry entry)
    {
        if (_byEntryPoint.TryGetValue(entryPoint, out var match) && match is not null)
        {
            entry = match;
            return true;
        }
        entry = null!;
        return false;
    }

    /// <summary>
    /// Rejects a virtual path that Read() would itself reject, so a bad path fails loudly at write
    /// time instead of silently producing a bundle nothing can read back
    /// (Path.Combine(baseFunctionsDir/modCppDir/cppDirectory, entry.VirtualPath) is the eventual
    /// consumer). Shared by both writers and by Read, so the two directions can never drift apart.
    /// </summary>
    internal static void ValidateVirtualPath(string virtualPath, string context)
    {
        if (string.IsNullOrWhiteSpace(virtualPath))
        {
            throw new InvalidDataException($"Invalid virtual path '{virtualPath}' in translation source bundle '{context}'.");
        }

        // Path.IsPathRooted is host-OS-dependent (e.g. Path.IsPathRooted(@"\foo") is false on
        // Unix), so a writer running on one OS could accept a path that the '/'-normalized
        // serialized form - read back on the same OS, let alone a different one - then rejects as
        // rooted, producing a bundle its own writer can't round-trip. Normalize separators first
        // and check rootedness/drive-prefix explicitly so the rule is identical on every OS.
        var normalized = virtualPath.Replace('\\', '/');
        var hasDrivePrefix = normalized.Length >= 2 && normalized[1] == ':';
        if (normalized.StartsWith('/') || hasDrivePrefix ||
            normalized.Split('/').Any(component => component == ".."))
        {
            throw new InvalidDataException($"Invalid virtual path '{virtualPath}' in translation source bundle '{context}'.");
        }
    }

    public static void Write(string path, IEnumerable<TranslationSourceBundleEntry> entries)
    {
        var ordered = entries.OrderBy(static entry => entry.EntryPoint).ThenBy(static entry => entry.VirtualPath, StringComparer.Ordinal).ToArray();
        if (ordered.Select(static entry => (entry.EntryPoint, entry.VirtualPath)).Distinct().Count() != ordered.Length)
            throw new InvalidDataException("Translation source bundle contains duplicate address/path identities.");
        foreach (var entry in ordered)
            ValidateVirtualPath(entry.VirtualPath, path);
        var fullPath = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(fullPath)!);
        var temporary = $"{fullPath}.tmp.{Guid.NewGuid():N}";
        using (var stream = File.Create(temporary))
        using (var writer = new BinaryWriter(stream, Encoding.UTF8, leaveOpen: false))
        {
            writer.Write(Magic);
            writer.Write(FormatVersion);
            writer.Write(ordered.Length);
            foreach (var entry in ordered)
            {
                var pathBytes = Encoding.UTF8.GetBytes(entry.VirtualPath.Replace('\\', '/'));
                var sourceBytes = Encoding.UTF8.GetBytes(entry.Source);
                writer.Write(entry.EntryPoint);
                writer.Write(pathBytes.Length);
                writer.Write(pathBytes);
                writer.Write(sourceBytes.Length);
                writer.Write(SHA256.HashData(sourceBytes));
                writer.Write(sourceBytes);
            }
        }
        File.Move(temporary, fullPath, overwrite: true);
    }

    public static TranslationSourceBundleWriter CreateWriter(string path, int entryCount) => new(path, entryCount, Magic, FormatVersion);

    public static TranslationSourceBundle Read(string path)
    {
        using var stream = File.OpenRead(path);
        using var reader = new BinaryReader(stream, Encoding.UTF8, leaveOpen: false);
        if (!reader.ReadBytes(Magic.Length).SequenceEqual(Magic) || reader.ReadInt32() != FormatVersion)
            throw new InvalidDataException($"Unsupported translation source bundle '{path}'.");
        var count = reader.ReadInt32();
        if (count < 0) throw new InvalidDataException("Negative translation source bundle entry count.");
        var entries = new List<TranslationSourceBundleEntry>(count);
        var identities = new HashSet<(uint, string)>();
        for (var i = 0; i < count; i++)
        {
            var address = reader.ReadUInt32();
            var virtualPath = Encoding.UTF8.GetString(ReadBoundedBytes(reader, 1 << 20));
            ValidateVirtualPath(virtualPath, path);
            var sourceLength = reader.ReadInt32();
            var hash = reader.ReadBytes(32);
            if (sourceLength < 0) throw new InvalidDataException("Negative translation source length.");
            var sourceBytes = reader.ReadBytes(sourceLength);
            if (sourceBytes.Length != sourceLength || !SHA256.HashData(sourceBytes).SequenceEqual(hash))
                throw new InvalidDataException($"Translation source 0x{address:X8} is truncated or corrupt.");
            if (!identities.Add((address, virtualPath)))
                throw new InvalidDataException($"Duplicate translation source 0x{address:X8} '{virtualPath}'.");
            entries.Add(new TranslationSourceBundleEntry(
                address,
                virtualPath,
                Encoding.UTF8.GetString(sourceBytes),
                ChecksumUtilities.ToHex(hash),
                sourceLength));
        }
        if (stream.Position != stream.Length) throw new InvalidDataException("Translation source bundle has trailing data.");
        return new TranslationSourceBundle(entries);
    }

    private static byte[] ReadBoundedBytes(BinaryReader reader, int maximum)
    {
        var length = reader.ReadInt32();
        if (length < 0 || length > maximum) throw new InvalidDataException("Invalid translation bundle string length.");
        var bytes = reader.ReadBytes(length);
        if (bytes.Length != length) throw new EndOfStreamException();
        return bytes;
    }
}

public sealed class TranslationSourceBundleWriter : IDisposable
{
    private readonly string _path;
    private readonly string _temporary;
    private readonly int _expectedCount;
    private readonly BinaryWriter _writer;
    private int _written;
    private bool _completed;
    private (uint Address, string Path)? _previous;

    internal TranslationSourceBundleWriter(string path, int entryCount, byte[] magic, int version)
    {
        if (entryCount < 0) throw new ArgumentOutOfRangeException(nameof(entryCount));
        _path = Path.GetFullPath(path);
        Directory.CreateDirectory(Path.GetDirectoryName(_path)!);
        SweepAbandonedTemporaries(_path);
        _temporary = $"{_path}.tmp.{Guid.NewGuid():N}";
        _expectedCount = entryCount;
        _writer = new BinaryWriter(File.Create(_temporary), Encoding.UTF8, leaveOpen: false);
        _writer.Write(magic);
        _writer.Write(version);
        _writer.Write(entryCount);
    }

    /// <summary>
    /// A killed run leaves its in-progress temporary behind, and those orphans are
    /// hundreds of megabytes each. Only the exact "target.tmp." prefix is swept, and a
    /// file another process still holds open is left alone.
    /// </summary>
    private static void SweepAbandonedTemporaries(string targetPath)
    {
        var directory = Path.GetDirectoryName(targetPath)!;
        var prefix = Path.GetFileName(targetPath) + ".tmp.";
        // The pattern is a Win32 wildcard, so re-check the prefix explicitly and
        // snapshot the listing before deleting from it.
        foreach (var candidate in Directory.GetFiles(directory, prefix + "*"))
        {
            if (!Path.GetFileName(candidate).StartsWith(prefix, StringComparison.Ordinal)) continue;
            try
            {
                File.Delete(candidate);
            }
            catch (IOException)
            {
            }
            catch (UnauthorizedAccessException)
            {
            }
        }
    }

    public void Write(TranslationSourceBundleEntry entry)
    {
        if (_completed || _written >= _expectedCount) throw new InvalidOperationException("Translation source bundle writer is complete.");
        TranslationSourceBundle.ValidateVirtualPath(entry.VirtualPath, _path);
        var virtualPath = entry.VirtualPath.Replace('\\', '/');
        if (_previous is { } previous &&
            (entry.EntryPoint < previous.Address ||
             (entry.EntryPoint == previous.Address && string.CompareOrdinal(virtualPath, previous.Path) <= 0)))
            throw new InvalidDataException("Translation source bundle entries must be written in deterministic address/path order.");
        var pathBytes = Encoding.UTF8.GetBytes(virtualPath);
        var sourceBytes = Encoding.UTF8.GetBytes(entry.Source);
        _writer.Write(entry.EntryPoint);
        _writer.Write(pathBytes.Length);
        _writer.Write(pathBytes);
        _writer.Write(sourceBytes.Length);
        _writer.Write(SHA256.HashData(sourceBytes));
        _writer.Write(sourceBytes);
        _previous = (entry.EntryPoint, virtualPath);
        _written++;
    }

    public void Complete()
    {
        if (_written != _expectedCount)
            throw new InvalidOperationException($"Translation source bundle expected {_expectedCount} entries but received {_written}.");
        _writer.Dispose();
        File.Move(_temporary, _path, overwrite: true);
        _completed = true;
    }

    public void Dispose()
    {
        _writer.Dispose();
        if (!_completed && File.Exists(_temporary)) File.Delete(_temporary);
    }
}
