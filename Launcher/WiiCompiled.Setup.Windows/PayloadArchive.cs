using System.IO.Compression;
using System.Text;
using System.Text.Json;

namespace WiiCompiled.Setup.Windows;

internal sealed class PayloadArchive : IDisposable
{
    private static readonly byte[] FooterMagic = Encoding.ASCII.GetBytes("MKWCPAY1");
    private static readonly DateTime NormalizedPayloadTimestampUtc =
        new(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    private const int FooterSize = 24;
    private readonly FileStream _executable;
    private readonly SliceStream _payloadStream;
    private readonly ZipArchive _zip;

    private PayloadArchive(FileStream executable, SliceStream payloadStream, ZipArchive zip)
    {
        _executable = executable;
        _payloadStream = payloadStream;
        _zip = zip;
    }

    public static PayloadArchive OpenCurrent()
    {
        var path = Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate the setup executable.");
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            if (stream.Length < FooterSize)
                throw new InvalidDataException("This setup executable does not contain an installation payload.");

            stream.Position = stream.Length - FooterSize;
            using var reader = new BinaryReader(stream, Encoding.ASCII, leaveOpen: true);
            var magic = reader.ReadBytes(FooterMagic.Length);
            var offset = reader.ReadInt64();
            var length = reader.ReadInt64();
            if (!magic.SequenceEqual(FooterMagic) || offset < 0 || length <= 0 || offset + length != stream.Length - FooterSize)
                throw new InvalidDataException("The installation payload is missing or damaged.");

            var slice = new SliceStream(stream, offset, length);
            var zip = new ZipArchive(slice, ZipArchiveMode.Read, leaveOpen: true);
            return new PayloadArchive(stream, slice, zip);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    public PayloadManifest ReadManifest()
    {
        var entry = FindEntry(InstalledLayout.PayloadManifestFileName)
                    ?? throw new InvalidDataException("The payload manifest is missing.");
        using var stream = entry.Open();
        return ParseManifest(stream);
    }

    internal static PayloadManifest ParseManifest(Stream stream)
    {
        var manifest = JsonSerializer.Deserialize<PayloadManifest>(stream,
            new JsonSerializerOptions { PropertyNameCaseInsensitive = true })
            ?? throw new InvalidDataException("The payload manifest is invalid.");
        ValidateManifest(manifest);
        return manifest;
    }

    internal static void ValidateManifest(PayloadManifest manifest)
    {
        if (manifest.SchemaVersion != PayloadManifest.CurrentSchemaVersion)
            throw new InvalidDataException($"Unsupported payload schema {manifest.SchemaVersion}.");
        // The payload identities are the release-computed replacement for hashing the whole
        // toolkit on the user's machine; a payload without them was packaged incorrectly.
        if (string.IsNullOrWhiteSpace(manifest.ToolkitFingerprint) ||
            string.IsNullOrWhiteSpace(manifest.ToolkitPackageFingerprint) ||
            string.IsNullOrWhiteSpace(manifest.RuntimeAssetsFingerprint))
            throw new InvalidDataException("The payload manifest is missing its content identities.");
        if (manifest.SupportedReleases is not { Count: > 0 })
            throw new InvalidDataException("The payload manifest names no supported game release.");
        foreach (var release in manifest.SupportedReleases)
        {
            if (!RetroWfcPayload.SupportedGameIds.Contains(release.GameId) ||
                !IsSha256(release.DolSha256) || !IsSha256(release.RelSha256))
                throw new InvalidDataException(
                    $"The payload manifest describes an unsupported or incomplete release '{release.GameId}'.");
        }
        if (manifest.SupportedReleases.DistinctBy(release => release.GameId).Count() !=
            manifest.SupportedReleases.Count)
            throw new InvalidDataException("The payload manifest lists a game release twice.");
    }

    private static bool IsSha256(string value) =>
        value.Length == 64 && value.All(char.IsAsciiHexDigit);

    /// <summary>Extracts nodtool and the runtime DLLs it loads; returns the nodtool path.</summary>
    public string ExtractDiscTool(string directory)
    {
        var tool = Path.Combine(directory, "nodtool.exe");
        ExtractEntry(InstalledLayout.ToolkitEntryPrefix + "nodtool.exe", tool);
        ExtractDirectory(InstalledLayout.ToolkitEntryPrefix + "Redist", directory);
        return tool;
    }

    public void ExtractEntry(string entryName, string destination)
    {
        var entry = FindEntry(entryName)
                    ?? throw new InvalidDataException($"Payload entry is missing: {entryName}");
        Directory.CreateDirectory(Path.GetDirectoryName(destination)!);
        using (var input = entry.Open())
        using (var output = new FileStream(destination, FileMode.Create, FileAccess.Write, FileShare.None))
            input.CopyTo(output);
        NormalizeExtractedTimestamp(destination);
    }

    public void ExtractDirectory(string prefix, string destination)
    {
        prefix = NormalizeEntryName(prefix).TrimEnd('/') + "/";
        var destinationRoot = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        foreach (var entry in _zip.Entries)
        {
            var normalizedEntry = NormalizeEntryName(entry.FullName);
            if (!normalizedEntry.StartsWith(prefix, StringComparison.Ordinal) || normalizedEntry.EndsWith('/'))
                continue;
            var relative = normalizedEntry[prefix.Length..].Replace('/', Path.DirectorySeparatorChar);
            var outputPath = Path.GetFullPath(Path.Combine(destinationRoot, relative));
            if (!outputPath.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Unsafe payload path: {entry.FullName}");
            Directory.CreateDirectory(Path.GetDirectoryName(outputPath)!);
            entry.ExtractToFile(outputPath, overwrite: true);
            NormalizeExtractedTimestamp(outputPath);
        }
    }

    internal static void NormalizeExtractedTimestamp(string path) =>
        File.SetLastWriteTimeUtc(path, NormalizedPayloadTimestampUtc);

    private static string NormalizeEntryName(string value) => value.Replace('\\', '/').TrimStart('/');
    private ZipArchiveEntry? FindEntry(string name)
    {
        var normalized = NormalizeEntryName(name);
        return _zip.Entries.FirstOrDefault(entry =>
            NormalizeEntryName(entry.FullName).Equals(normalized, StringComparison.Ordinal));
    }

    public void Dispose()
    {
        _zip.Dispose();
        _payloadStream.Dispose();
        _executable.Dispose();
    }

    private sealed class SliceStream : Stream
    {
        private readonly Stream _inner;
        private readonly long _offset;
        private readonly long _length;
        private long _position;

        public SliceStream(Stream inner, long offset, long length)
        {
            _inner = inner;
            _offset = offset;
            _length = length;
        }

        public override bool CanRead => true;
        public override bool CanSeek => true;
        public override bool CanWrite => false;
        public override long Length => _length;
        public override long Position { get => _position; set => Seek(value, SeekOrigin.Begin); }
        public override void Flush() { }
        public override int Read(byte[] buffer, int offset, int count)
        {
            if (_position >= _length) return 0;
            count = (int)Math.Min(count, _length - _position);
            lock (_inner)
            {
                _inner.Position = _offset + _position;
                var read = _inner.Read(buffer, offset, count);
                _position += read;
                return read;
            }
        }
        public override long Seek(long offset, SeekOrigin origin)
        {
            var next = origin switch
            {
                SeekOrigin.Begin => offset,
                SeekOrigin.Current => _position + offset,
                SeekOrigin.End => _length + offset,
                _ => throw new ArgumentOutOfRangeException(nameof(origin))
            };
            if (next < 0 || next > _length) throw new IOException("Attempted to seek outside the payload.");
            return _position = next;
        }
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }
}
