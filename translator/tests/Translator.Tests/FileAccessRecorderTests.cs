using System.Text;
using Translator.Core.IO;

namespace Translator.Tests;

/// <summary>
/// The recorder's bookkeeping rules, exercised on a recorder instance directly (never installed
/// process-wide, so parallel tests are unaffected), and the recording-mode text decoding.
/// </summary>
public sealed class FileAccessRecorderTests : IDisposable
{
    private readonly string _root = Path.Combine(Path.GetTempPath(), $"translator-recorder-{Guid.NewGuid():N}");

    public FileAccessRecorderTests() => Directory.CreateDirectory(_root);

    public void Dispose()
    {
        if (Directory.Exists(_root)) Directory.Delete(_root, recursive: true);
    }

    [Fact]
    public void FirstReadKeepsTheHashItSawAndOwnFilesAreNotInputs()
    {
        var input = Write("input.txt", "one");
        var recorder = new FileAccessRecorder();
        recorder.OnRead(input, File.ReadAllBytes(input));
        File.WriteAllText(input, "two");
        recorder.OnRead(input, File.ReadAllBytes(input));
        recorder.OnWrite(input);

        var produced = Write("produced.txt", "mine");
        recorder.OnWrite(produced);
        recorder.OnRead(produced, File.ReadAllBytes(produced));
        recorder.OnFileProbe(produced, exists: true);

        var staging = Path.Combine(_root, "staging");
        recorder.OnCreateDirectory(staging);
        Directory.CreateDirectory(staging);
        var staged = Write(Path.Combine("staging", "deep", "file.txt"), "staged");
        recorder.OnRead(staged, File.ReadAllBytes(staged));
        recorder.OnScan(new FileScanQuery(staging, "*", SearchOption.AllDirectories, FileScanKind.Files), [staged]);

        var snapshot = recorder.Snapshot();
        var only = Assert.Single(snapshot.Inputs);
        Assert.Equal(input, only.Path);
        Assert.Equal(Translator.Core.Loading.ChecksumUtilities.Sha256Hex(Encoding.UTF8.GetBytes("one")), only.Sha256);
        Assert.Empty(snapshot.Probes);
        Assert.Empty(snapshot.Scans);
        Assert.Contains(snapshot.Outputs, output => output.Path == produced && output.Size == 4);
        // A read-then-rewritten file can never match its own record: fail closed.
        Assert.Contains(snapshot.Outputs, output => output.Path == input);
    }

    [Fact]
    public void DeletedPreexistingPathsMustStayDeletedAndTemporariesVanish()
    {
        var stale = Write("stale.cpp", "old");
        var recorder = new FileAccessRecorder();
        File.Delete(stale);
        recorder.OnDeleteFile(stale);

        var temporary = Write("out.tmp", "partial");
        recorder.OnWrite(temporary);
        var final = Path.Combine(_root, "out.bin");
        File.Move(temporary, final);
        recorder.OnDeleteFile(temporary);
        recorder.OnWrite(final);

        var snapshot = recorder.Snapshot();
        Assert.Contains(snapshot.Probes, probe => probe.Path == stale && !probe.IsDirectory && !probe.Exists);
        Assert.DoesNotContain(snapshot.Probes, probe => probe.Path == temporary);
        Assert.Equal(new[] { (final, 7L) }, snapshot.Outputs);
    }

    [Fact]
    public void PublishedTreesAreOutputsAndBackupsAreNot()
    {
        var destination = Path.Combine(_root, "published");
        Directory.CreateDirectory(destination);
        Write(Path.Combine("published", "old.txt"), "old");
        var recorder = new FileAccessRecorder();
        var staging = Path.Combine(_root, ".published.staging");
        recorder.OnCreateDirectory(staging);
        Directory.CreateDirectory(staging);
        Write(Path.Combine(".published.staging", "new.txt"), "new!");
        var backup = Path.Combine(_root, ".published.backup");
        Directory.Move(destination, backup);
        recorder.OnMoveDirectory(destination, backup);
        Directory.Move(staging, destination);
        recorder.OnMoveDirectory(staging, destination);
        Directory.Delete(backup, recursive: true);
        recorder.OnDeleteDirectory(backup);

        var snapshot = recorder.Snapshot();
        Assert.Equal(new[] { (Path.Combine(destination, "new.txt"), 4L) }, snapshot.Outputs);
        var tree = Assert.Single(snapshot.Scans);
        Assert.Equal(destination, tree.Query.Directory);
        Assert.Equal(new[] { "new.txt" }, tree.Entries);
        Assert.DoesNotContain(snapshot.Probes, probe => probe.Path == destination);
    }

    [Fact]
    public void ReadsFromAMovedPreexistingTreeAreInputs()
    {
        var original = Path.Combine(_root, "existing");
        var input = Write(Path.Combine("existing", "input.txt"), "kept");
        var moved = Path.Combine(_root, "moved");
        var recorder = new FileAccessRecorder();
        Directory.Move(original, moved);
        recorder.OnMoveDirectory(original, moved);
        var movedInput = Path.Combine(moved, Path.GetFileName(input));
        recorder.OnRead(movedInput, File.ReadAllBytes(movedInput));

        var snapshot = recorder.Snapshot();
        var only = Assert.Single(snapshot.Inputs);
        Assert.Equal(movedInput, only.Path);
        Assert.Empty(snapshot.Outputs);
    }

    [Fact]
    public void OutputListingsAreTakenWhenTheRunEnds()
    {
        var blobs = Path.Combine(_root, "blobs");
        Directory.CreateDirectory(blobs);
        Write(Path.Combine("blobs", "stale.bin"), "x");
        var recorder = new FileAccessRecorder();
        recorder.OnOutputScan(new FileScanQuery(blobs, "*.bin", SearchOption.TopDirectoryOnly, FileScanKind.Files));
        File.Delete(Path.Combine(blobs, "stale.bin"));
        Write(Path.Combine("blobs", "kept.bin"), "y");

        var scan = Assert.Single(recorder.Snapshot().Scans);
        Assert.Equal(new[] { "kept.bin" }, scan.Entries);
    }

    [Fact]
    public void OutputListingsOfCreatedDirectoriesAreKept()
    {
        var blobs = Path.Combine(_root, "created");
        var recorder = new FileAccessRecorder();
        recorder.OnCreateDirectory(blobs);
        Directory.CreateDirectory(blobs);
        recorder.OnOutputScan(new FileScanQuery(blobs, "*.bin", SearchOption.TopDirectoryOnly, FileScanKind.Files));
        Write(Path.Combine("created", "kept.bin"), "y");

        var scan = Assert.Single(recorder.Snapshot().Scans);
        Assert.Equal(new[] { "kept.bin" }, scan.Entries);
    }

    [Fact]
    public void UntrackedInputsAndEnvironmentExpansionsAreReported()
    {
        var recorder = new FileAccessRecorder();
        recorder.OnEnvironmentExpansion("plain/path", "plain/path");
        recorder.OnEnvironmentExpansion("%HOME%/x", "/home/user/x");
        recorder.OnUntrackedInput("a download of https://example.invalid/payload");
        var snapshot = recorder.Snapshot();
        Assert.Equal(new[] { ("%HOME%/x", "/home/user/x") }, snapshot.EnvironmentExpansions);
        Assert.Single(snapshot.UntrackedInputs);
    }

    [Theory]
    [InlineData("utf8")]
    [InlineData("utf8-bom")]
    [InlineData("utf16le-bom")]
    [InlineData("utf16be-bom")]
    [InlineData("latin1")]
    [InlineData("empty")]
    public void RecordingModeDecodesTextExactlyLikeFile(string encoding)
    {
        const string text = "line one\r\nline two\nµ snowman ☃\rlast";
        byte[] bytes = encoding switch
        {
            "utf8" => Encoding.UTF8.GetBytes(text),
            "utf8-bom" => [.. Encoding.UTF8.GetPreamble(), .. Encoding.UTF8.GetBytes(text)],
            "utf16le-bom" => [.. Encoding.Unicode.GetPreamble(), .. Encoding.Unicode.GetBytes(text)],
            "utf16be-bom" => [.. Encoding.BigEndianUnicode.GetPreamble(), .. Encoding.BigEndianUnicode.GetBytes(text)],
            "latin1" => Encoding.Latin1.GetBytes(text.Replace("☃", "?")),
            _ => [],
        };
        var path = Path.Combine(_root, $"{encoding}.txt");
        File.WriteAllBytes(path, bytes);

        Assert.Equal(File.ReadAllText(path), TrackedFs.DecodeText(bytes));
        Assert.Equal(File.ReadLines(path).ToArray(), TrackedFs.DecodeLines(bytes).ToArray());
    }

    private string Write(string relative, string content)
    {
        var path = Path.Combine(_root, relative);
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content);
        return path;
    }
}
