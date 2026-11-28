using System;
using System.Buffers;
using System.IO;
using System.Text;

namespace Translator.Core.IO;

/// <summary>
/// The one generated-file writer. Every write is content-gated (so an unchanged
/// file keeps its modification time and Ninja does not rebuild the world) and
/// atomic (so an interrupted run never leaves a truncated artifact behind).
/// </summary>
public static class FileOutput
{
    private const int CompareBufferSize = 128 * 1024;

    public static bool WriteTextIfChanged(string path, string content) =>
        WriteBytesIfChanged(path, Encoding.UTF8.GetBytes(content));

    public static bool WriteBytesIfChanged(string path, ReadOnlySpan<byte> content)
    {
        var fullPath = Path.GetFullPath(path);
        // The target is this command's output whether or not its bytes change.
        TrackedFs.KeepOutput(fullPath);
        if (ContentEquals(fullPath, content)) return false;
        TrackedFs.CreateDirectory(Path.GetDirectoryName(fullPath)!);
        var temporary = $"{fullPath}.tmp.{Guid.NewGuid():N}";
        using (var stream = TrackedFs.CreateNew(temporary, CompareBufferSize))
        {
            stream.Write(content);
        }
        TrackedFs.Move(temporary, fullPath, overwrite: true);
        return true;
    }

    /// <summary>
    /// Byte-compares an existing file against in-memory content without materializing it, since
    /// generated payloads are hundreds of megabytes in aggregate.
    /// </summary>
    public static bool ContentEquals(string path, ReadOnlySpan<byte> content)
    {
        if (!TrackedFs.OutputFileExists(path) || TrackedFs.OutputFileLength(path) != content.Length) return false;

        using var stream = TrackedFs.OpenOutputForCompare(path, CompareBufferSize);
        var buffer = ArrayPool<byte>.Shared.Rent(CompareBufferSize);
        try
        {
            var offset = 0;
            while (offset < content.Length)
            {
                var read = stream.Read(buffer, 0, Math.Min(buffer.Length, content.Length - offset));
                if (read <= 0) return false;
                if (!buffer.AsSpan(0, read).SequenceEqual(content.Slice(offset, read))) return false;
                offset += read;
            }
            return true;
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(buffer);
        }
    }

    /// <summary>
    /// Byte-compares two files with the same streamed, pooled-buffer strategy as
    /// <see cref="ContentEquals"/>, so publication and gating agree on what
    /// "identical" means and neither materialises a large generated file.
    /// </summary>
    public static bool FilesEqual(string left, string right)
    {
        if (TrackedFs.OutputFileLength(left) != TrackedFs.OutputFileLength(right)) return false;

        using var leftStream = TrackedFs.OpenOutputForCompare(left, CompareBufferSize);
        using var rightStream = TrackedFs.OpenOutputForCompare(right, CompareBufferSize);
        var leftBuffer = ArrayPool<byte>.Shared.Rent(CompareBufferSize);
        var rightBuffer = ArrayPool<byte>.Shared.Rent(CompareBufferSize);
        try
        {
            while (true)
            {
                var leftRead = leftStream.Read(leftBuffer, 0, CompareBufferSize);
                var rightRead = rightStream.Read(rightBuffer, 0, CompareBufferSize);
                if (leftRead != rightRead) return false;
                if (leftRead == 0) return true;
                if (!leftBuffer.AsSpan(0, leftRead).SequenceEqual(rightBuffer.AsSpan(0, rightRead))) return false;
            }
        }
        finally
        {
            ArrayPool<byte>.Shared.Return(leftBuffer);
            ArrayPool<byte>.Shared.Return(rightBuffer);
        }
    }
}
