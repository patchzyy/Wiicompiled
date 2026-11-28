using System.Buffers;
using System.Collections.Concurrent;
using System.IO.Compression;
using System.Runtime.ExceptionServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace WiiCompiled.Setup.Windows;

internal sealed class PayloadArchive : IDisposable
{
    private static readonly byte[] FooterMagic = Encoding.ASCII.GetBytes("MKWCPAY1");
    private static readonly DateTime NormalizedPayloadTimestampUtc =
        new(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc);
    private const int FooterSize = 24;
    // Below this many files a directory is extracted on the calling thread; the per-worker
    // archive handles cost more than they save.
    private const int ParallelExtractionThreshold = 256;
    private readonly FileStream _executable;
    private readonly SliceStream _payloadStream;
    private readonly ZipArchive _zip;
    private readonly string _path;
    private readonly long _offset;
    private readonly long _length;

    private PayloadArchive(FileStream executable, SliceStream payloadStream, ZipArchive zip, string path,
        long offset, long length)
    {
        _executable = executable;
        _payloadStream = payloadStream;
        _zip = zip;
        _path = path;
        _offset = offset;
        _length = length;
    }

    public static PayloadArchive OpenCurrent() =>
        OpenAppended(Environment.ProcessPath ?? throw new InvalidOperationException("Cannot locate the setup executable."));

    /// <summary>Opens the payload appended to <paramref name="path"/> (the setup executable) behind its footer.</summary>
    internal static PayloadArchive OpenAppended(string path)
    {
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
            return new PayloadArchive(stream, slice, zip, path, offset, length);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    /// <summary>Opens a bare payload ZIP; used by self-tests that build a payload on disk.</summary>
    internal static PayloadArchive OpenZip(string path)
    {
        var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read);
        try
        {
            var slice = new SliceStream(stream, 0, stream.Length);
            var zip = new ZipArchive(slice, ZipArchiveMode.Read, leaveOpen: true);
            return new PayloadArchive(stream, slice, zip, path, 0, stream.Length);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    /// <summary>
    /// The shipped per-file inventory, or null when this payload has none or it does not describe
    /// the manifest's toolkit. Without it every install falls back to extracting and re-hashing.
    /// </summary>
    public PayloadInventory? TryReadInventory(PayloadManifest manifest, Action<string>? diagnostic = null)
    {
        var entry = FindEntry(PayloadInventory.FileName);
        if (entry is null)
        {
            diagnostic?.Invoke("This payload has no file inventory; installed files are re-hashed instead.");
            return null;
        }
        try
        {
            using var stream = entry.Open();
            var inventory = PayloadInventory.Parse(stream);
            if (!inventory.ToolkitFingerprint.Equals(manifest.ToolkitFingerprint, StringComparison.Ordinal) ||
                !inventory.ToolkitPackageFingerprint.Equals(manifest.ToolkitPackageFingerprint,
                    StringComparison.Ordinal))
            {
                diagnostic?.Invoke("The payload inventory does not describe this release; it is ignored.");
                return null;
            }
            return inventory;
        }
        catch (Exception ex) when (ex is InvalidDataException or JsonException or IOException)
        {
            diagnostic?.Invoke("The payload inventory is unreadable and is ignored: " + ex.Message);
            return null;
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

    /// <summary>
    /// Extracts every entry under <paramref name="prefix"/>. Each file's SHA-256 and size are computed
    /// from the bytes as they stream out of the archive (never trusting the ZIP CRC) and, when
    /// <paramref name="identities"/> is given, recorded under the entry's payload path so the caller
    /// can verify them against the shipped inventory. Large directories are extracted by several
    /// workers, each reading through its own handle on the payload: the cost is per file (create,
    /// write, close and the antivirus scan on close), not bandwidth.
    /// </summary>
    public void ExtractDirectory(string prefix, string destination,
        IDictionary<string, PayloadFileIdentity>? identities = null)
    {
        prefix = NormalizeEntryName(prefix).TrimEnd('/') + "/";
        var destinationRoot = Path.GetFullPath(destination).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        var work = new List<(int Index, string Name, string Output)>();
        var outputs = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var entries = _zip.Entries;
        for (var index = 0; index < entries.Count; index++)
        {
            var normalizedEntry = NormalizeEntryName(entries[index].FullName);
            if (!normalizedEntry.StartsWith(prefix, StringComparison.Ordinal) || normalizedEntry.EndsWith('/'))
                continue;
            var relative = normalizedEntry[prefix.Length..].Replace('/', Path.DirectorySeparatorChar);
            var outputPath = Path.GetFullPath(Path.Combine(destinationRoot, relative));
            if (!outputPath.StartsWith(destinationRoot, StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException($"Unsafe payload path: {entries[index].FullName}");
            // Two workers must never write one file; a release built from a directory has no such pair.
            if (!outputs.Add(outputPath))
                throw new InvalidDataException($"The payload lists {entries[index].FullName} twice.");
            work.Add((index, normalizedEntry, outputPath));
        }
        foreach (var directory in work.Select(item => Path.GetDirectoryName(item.Output)!)
                     .Distinct(StringComparer.OrdinalIgnoreCase))
            Directory.CreateDirectory(directory);

        var extracted = new PayloadFileIdentity[work.Count];
        var workers = Math.Min(Environment.ProcessorCount, 8);
        if (work.Count < ParallelExtractionThreshold || workers < 2)
        {
            for (var i = 0; i < work.Count; i++)
                extracted[i] = ExtractHashed(entries[work[i].Index], work[i].Output);
        }
        else
        {
            try
            {
                // Contiguous ranges keep each worker reading the payload mostly forward.
                Parallel.ForEach(Partitioner.Create(0, work.Count, 32),
                    new ParallelOptions { MaxDegreeOfParallelism = workers },
                    OpenWorkerArchive,
                    (range, state, archive) =>
                    {
                        var workerEntries = archive.Zip.Entries;
                        for (var i = range.Item1; i < range.Item2 && !state.ShouldExitCurrentIteration; i++)
                        {
                            var entry = workerEntries[work[i].Index];
                            if (!string.Equals(NormalizeEntryName(entry.FullName), work[i].Name, StringComparison.Ordinal))
                                throw new InvalidDataException("The installation payload changed while it was being extracted.");
                            extracted[i] = ExtractHashed(entry, work[i].Output);
                        }
                        return archive;
                    },
                    archive => archive.Dispose());
            }
            catch (AggregateException ex)
            {
                // Callers and the user see the first failure itself (a full disk, a locked file), as
                // they did when extraction ran on this thread.
                ExceptionDispatchInfo.Capture(ex.Flatten().InnerExceptions[0]).Throw();
            }
        }
        if (identities is null) return;
        for (var i = 0; i < work.Count; i++)
            identities[work[i].Name] = extracted[i];
    }

    private sealed record WorkerArchive(FileStream Stream, ZipArchive Zip) : IDisposable
    {
        public void Dispose()
        {
            Zip.Dispose();
            Stream.Dispose();
        }
    }

    /// <summary>A second, independent reader of this payload for one extraction worker.</summary>
    private WorkerArchive OpenWorkerArchive()
    {
        var stream = new FileStream(_path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        try
        {
            var zip = new ZipArchive(new SliceStream(stream, _offset, _length), ZipArchiveMode.Read, leaveOpen: true);
            return new WorkerArchive(stream, zip);
        }
        catch
        {
            stream.Dispose();
            throw;
        }
    }

    /// <summary>
    /// The identity of every file entry under <paramref name="prefix"/>, hashed exactly as
    /// <see cref="ExtractDirectory"/> hashes it but without writing anything, keyed the same way.
    /// </summary>
    public Dictionary<string, PayloadFileIdentity> HashDirectory(string prefix)
    {
        prefix = NormalizeEntryName(prefix).TrimEnd('/') + "/";
        var identities = new Dictionary<string, PayloadFileIdentity>(StringComparer.Ordinal);
        foreach (var entry in _zip.Entries)
        {
            var normalizedEntry = NormalizeEntryName(entry.FullName);
            if (!normalizedEntry.StartsWith(prefix, StringComparison.Ordinal) || normalizedEntry.EndsWith('/'))
                continue;
            identities[normalizedEntry] = CopyHashed(entry, output: null);
        }
        return identities;
    }

    private static PayloadFileIdentity ExtractHashed(ZipArchiveEntry entry, string outputPath)
    {
        using var output = new FileStream(outputPath, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 16);
        var identity = CopyHashed(entry, output);
        // Stamped through the open handle once every byte is written, instead of reopening the
        // file by path (a second open, and a second antivirus scan, per file).
        output.Flush();
        File.SetLastWriteTimeUtc(output.SafeFileHandle, NormalizedPayloadTimestampUtc);
        return identity;
    }

    private static PayloadFileIdentity CopyHashed(ZipArchiveEntry entry, Stream? output)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long size = 0;
        var buffer = ArrayPool<byte>.Shared.Rent(1 << 16);
        try
        {
            using var input = entry.Open();
            int read;
            while ((read = input.Read(buffer, 0, buffer.Length)) > 0)
            {
                hash.AppendData(buffer, 0, read);
                output?.Write(buffer, 0, read);
                size += read;
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
        return new PayloadFileIdentity(size, Convert.ToHexString(hash.GetHashAndReset()).ToLowerInvariant());
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
