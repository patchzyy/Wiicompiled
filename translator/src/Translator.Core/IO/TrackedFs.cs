using System.Security.Cryptography;
using System.Text;

namespace Translator.Core.IO;

/// <summary>
/// The translator's only door to the file system and the process environment. Every
/// read, probe, directory scan, write, move and delete in Translator.Core and
/// Translator.Cli goes through here (a source scan in the test suite enforces it), so a
/// <see cref="FileAccessRecorder"/> sees everything a command touched. With no recorder
/// installed every member is a straight pass-through to the System.IO call it names.
///
/// The <c>Output*</c> members exist for output management only: comparing a target before
/// rewriting it, carrying a timestamp over, listing an output tree to prune it. They are
/// never recorded as inputs, so their result must only decide whether or how an output is
/// written, never what it contains. Anything else uses the plain members.
/// </summary>
public static class TrackedFs
{
    private static volatile FileAccessRecorder? _recorder;

    /// <summary>The recorder every access is reported to, or null when nothing records.</summary>
    public static FileAccessRecorder? Recorder => _recorder;

    /// <summary>Installs (or with null removes) the process-wide recorder.</summary>
    public static void Install(FileAccessRecorder? recorder) => _recorder = recorder;

    // ---------------------------------------------------------------- inputs

    public static byte[] ReadAllBytes(string path)
    {
        var bytes = File.ReadAllBytes(path);
        _recorder?.OnRead(path, bytes);
        return bytes;
    }

    /// <summary>Same decoding as <see cref="File.ReadAllText(string)"/>: UTF-8 with byte-order-mark detection.</summary>
    public static string ReadAllText(string path)
    {
        if (_recorder is not { } recorder) return File.ReadAllText(path);
        var bytes = File.ReadAllBytes(path);
        recorder.OnRead(path, bytes);
        return DecodeText(bytes);
    }

    /// <summary>Same decoding as <see cref="File.ReadLines(string)"/>.</summary>
    public static IEnumerable<string> ReadLines(string path)
    {
        if (_recorder is not { } recorder) return File.ReadLines(path);
        var bytes = File.ReadAllBytes(path);
        recorder.OnRead(path, bytes);
        return DecodeLines(bytes);
    }

    // File.ReadAllText/ReadLines are a StreamReader over the file with UTF-8 and BOM detection;
    // decoding the hashed bytes the same way keeps a recorded run's text identical to an unrecorded one.
    internal static string DecodeText(byte[] content)
    {
        using var reader = new StreamReader(new MemoryStream(content, writable: false), Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        return reader.ReadToEnd();
    }

    internal static IEnumerable<string> DecodeLines(byte[] content)
    {
        using var reader = new StreamReader(new MemoryStream(content, writable: false), Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
        while (reader.ReadLine() is { } line) yield return line;
    }

    /// <summary><see cref="File.OpenRead"/>; when recording, the content is hashed through the returned stream first.</summary>
    public static FileStream OpenRead(string path)
    {
        var stream = File.OpenRead(path);
        if (_recorder is { } recorder && recorder.WantsContent(path))
        {
            try
            {
                var hash = SHA256.HashData(stream);
                stream.Seek(0, SeekOrigin.Begin);
                recorder.OnReadHash(path, hash);
            }
            catch
            {
                stream.Dispose();
                throw;
            }
        }
        return stream;
    }

    /// <summary><c>new FileInfo(path).Length</c>, recorded as reading the file.</summary>
    public static long FileLength(string path)
    {
        var length = new FileInfo(path).Length;
        if (_recorder is { } recorder && recorder.WantsContent(path))
        {
            using var stream = File.OpenRead(path);
            recorder.OnReadHash(path, SHA256.HashData(stream));
        }
        return length;
    }

    public static bool FileExists(string? path)
    {
        var exists = File.Exists(path);
        if (!string.IsNullOrEmpty(path)) _recorder?.OnFileProbe(path, exists);
        return exists;
    }

    public static bool DirectoryExists(string? path)
    {
        var exists = Directory.Exists(path);
        if (!string.IsNullOrEmpty(path)) _recorder?.OnDirectoryProbe(path, exists);
        return exists;
    }

    public static IEnumerable<string> EnumerateFiles(string path, string searchPattern, SearchOption searchOption)
    {
        if (_recorder is not { } recorder) return Directory.EnumerateFiles(path, searchPattern, searchOption);
        var entries = Directory.EnumerateFiles(path, searchPattern, searchOption).ToArray();
        recorder.OnScan(new FileScanQuery(path, searchPattern, searchOption, FileScanKind.Files), entries);
        return entries;
    }

    public static IEnumerable<string> EnumerateFiles(string path, string searchPattern) =>
        EnumerateFiles(path, searchPattern, SearchOption.TopDirectoryOnly);

    public static IEnumerable<string> EnumerateDirectories(string path)
    {
        if (_recorder is not { } recorder) return Directory.EnumerateDirectories(path);
        var entries = Directory.EnumerateDirectories(path).ToArray();
        recorder.OnScan(new FileScanQuery(path, "*", SearchOption.TopDirectoryOnly, FileScanKind.Directories), entries);
        return entries;
    }

    public static string GetCurrentDirectory()
    {
        var directory = Directory.GetCurrentDirectory();
        _recorder?.OnCurrentDirectory(directory);
        return directory;
    }

    public static string ExpandEnvironmentVariables(string value)
    {
        var expanded = Environment.ExpandEnvironmentVariables(value);
        _recorder?.OnEnvironmentExpansion(value, expanded);
        return expanded;
    }

    /// <summary>Marks the running command as depending on something no file record can capture (a download).</summary>
    public static void NoteUntrackedInput(string description) => _recorder?.OnUntrackedInput(description);

    // ---------------------------------------------------------------- outputs

    public static void WriteAllBytes(string path, byte[] bytes)
    {
        File.WriteAllBytes(path, bytes);
        _recorder?.OnWrite(path);
    }

    public static void WriteAllText(string path, string? contents)
    {
        File.WriteAllText(path, contents);
        _recorder?.OnWrite(path);
    }

    /// <summary><see cref="File.Create(string)"/>.</summary>
    public static FileStream Create(string path)
    {
        var stream = File.Create(path);
        _recorder?.OnWrite(path);
        return stream;
    }

    /// <summary><c>new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, bufferSize)</c>.</summary>
    public static FileStream CreateNew(string path, int bufferSize)
    {
        var stream = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None, bufferSize);
        _recorder?.OnWrite(path);
        return stream;
    }

    public static void Move(string sourceFileName, string destFileName, bool overwrite)
    {
        File.Move(sourceFileName, destFileName, overwrite);
        if (_recorder is { } recorder)
        {
            recorder.OnDeleteFile(sourceFileName);
            recorder.OnWrite(destFileName);
        }
    }

    public static void Copy(string sourceFileName, string destFileName, bool overwrite)
    {
        if (_recorder is { } recorder && recorder.WantsContent(sourceFileName))
        {
            using var stream = File.OpenRead(sourceFileName);
            recorder.OnReadHash(sourceFileName, SHA256.HashData(stream));
        }
        File.Copy(sourceFileName, destFileName, overwrite);
        _recorder?.OnWrite(destFileName);
    }

    public static void Delete(string path)
    {
        File.Delete(path);
        _recorder?.OnDeleteFile(path);
    }

    public static void CreateDirectory(string path)
    {
        if (_recorder is not { } recorder)
        {
            Directory.CreateDirectory(path);
            return;
        }
        // The topmost directory this call brings into existence; everything below it is the command's own.
        var full = Path.GetFullPath(path);
        string? created = null;
        for (var current = full; current is not null && !Directory.Exists(current); current = Path.GetDirectoryName(current))
            created = current;
        Directory.CreateDirectory(path);
        if (created is not null) recorder.OnCreateDirectory(created);
    }

    public static void DeleteDirectory(string path, bool recursive)
    {
        Directory.Delete(path, recursive);
        _recorder?.OnDeleteDirectory(path);
    }

    public static void MoveDirectory(string sourceDirName, string destDirName)
    {
        Directory.Move(sourceDirName, destDirName);
        _recorder?.OnMoveDirectory(sourceDirName, destDirName);
    }

    // ---------------------------------------------------------------- output management (never inputs)

    /// <summary>Declares an existing file this command decided to keep as one of its outputs (a content-gated write that found nothing to change).</summary>
    public static void KeepOutput(string path) => _recorder?.OnWrite(path);

    public static bool OutputFileExists(string? path) => File.Exists(path);

    public static bool OutputDirectoryExists(string? path) => Directory.Exists(path);

    /// <summary><c>new FileInfo(path).Length</c> of an output; throws like FileInfo when it is missing.</summary>
    public static long OutputFileLength(string path) => new FileInfo(path).Length;

    /// <summary>A read-only stream over an existing output, for comparing it with what is about to be written.</summary>
    public static FileStream OpenOutputForCompare(string path, int bufferSize) =>
        new(path, FileMode.Open, FileAccess.Read, FileShare.Read, bufferSize, FileOptions.SequentialScan);

    public static DateTime GetOutputLastWriteTimeUtc(string path) => File.GetLastWriteTimeUtc(path);

    public static void SetLastWriteTimeUtc(string path, DateTime lastWriteTimeUtc) =>
        File.SetLastWriteTimeUtc(path, lastWriteTimeUtc);

    /// <summary>
    /// Lists an output tree for pruning. Recorded as the listing the tree must still have
    /// once the command finished, so a file added there later still invalidates the command.
    /// </summary>
    public static IEnumerable<string> EnumerateOutputFiles(string path, string searchPattern, SearchOption searchOption)
    {
        if (_recorder is not { } recorder) return Directory.EnumerateFiles(path, searchPattern, searchOption);
        var entries = Directory.EnumerateFiles(path, searchPattern, searchOption).ToArray();
        recorder.OnOutputScan(new FileScanQuery(path, searchPattern, searchOption, FileScanKind.Files));
        return entries;
    }

    public static IEnumerable<string> EnumerateOutputFiles(string path, string searchPattern) =>
        EnumerateOutputFiles(path, searchPattern, SearchOption.TopDirectoryOnly);

    public static IEnumerable<string> EnumerateOutputDirectories(string path)
    {
        if (_recorder is not { } recorder) return Directory.EnumerateDirectories(path);
        var entries = Directory.EnumerateDirectories(path).ToArray();
        recorder.OnOutputScan(new FileScanQuery(path, "*", SearchOption.TopDirectoryOnly, FileScanKind.Directories));
        return entries;
    }
}
