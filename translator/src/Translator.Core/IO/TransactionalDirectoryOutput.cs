namespace Translator.Core.IO;

/// <summary>
/// Generates a complete directory tree away from the live output and publishes it only
/// after the producer succeeds. Byte-identical files retain their live last-write time so
/// incremental build systems do not rebuild unchanged generated sources.
/// </summary>
public static class TransactionalDirectoryOutput
{
    private const int FileSystemRetryCount = 8;

    public sealed record PublishResult(
        int AddedFiles,
        int UpdatedFiles,
        int RemovedFiles,
        int UnchangedFiles);

    /// <summary>
    /// The producer's exit code, the publication counts when one happened, and any
    /// non-fatal cleanup warnings. Warnings are returned rather than printed: this
    /// is Core, and only the CLI writes to the console.
    /// </summary>
    public sealed record GenerateAndPublishResult(
        int ExitCode,
        PublishResult? Publication,
        IReadOnlyList<string> Warnings);

    public static GenerateAndPublishResult GenerateAndPublish(
        string destinationDirectory,
        Func<string, int> producer,
        Action<string, int>? onProducerFailure = null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(destinationDirectory);
        ArgumentNullException.ThrowIfNull(producer);

        var destination = Path.GetFullPath(destinationDirectory);
        var parent = Path.GetDirectoryName(destination)
            ?? throw new InvalidOperationException($"Output directory has no parent: {destination}");
        TrackedFs.CreateDirectory(parent);

        var leaf = Path.GetFileName(destination.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        var staging = Path.Combine(parent, $".{leaf}.translate-mod-staging-{Guid.NewGuid():N}");
        // The same list instance is handed to the result record, so warnings the
        // staging cleanup below adds still reach the caller.
        var warnings = new List<string>();
        TrackedFs.CreateDirectory(staging);

        try
        {
            var producerResult = producer(staging);
            if (producerResult != 0)
            {
                onProducerFailure?.Invoke(staging, producerResult);
                return new GenerateAndPublishResult(producerResult, null, warnings);
            }

            return new GenerateAndPublishResult(0, Publish(staging, destination, warnings), warnings);
        }
        finally
        {
            if (TrackedFs.DirectoryExists(staging))
            {
                TryDeleteDirectory(staging, "staging output", warnings);
            }
        }
    }

    private static PublishResult Publish(string staging, string destination, List<string> warnings)
    {
        var stagedFiles = EnumerateRelativeFiles(staging);
        var liveFiles = TrackedFs.OutputDirectoryExists(destination)
            ? EnumerateRelativeFiles(destination)
            : new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);

        var added = 0;
        var updated = 0;
        var unchanged = 0;
        foreach (var (relativePath, stagedPath) in stagedFiles)
        {
            if (!liveFiles.TryGetValue(relativePath, out var livePath))
            {
                added++;
                continue;
            }

            if (!FilesEqual(stagedPath, livePath))
            {
                updated++;
                continue;
            }

            // Ninja/MSBuild use last-write time to determine whether generated C++ must
            // be rebuilt. The clean stage is authoritative for membership, while the live
            // tree is authoritative for the timestamp of identical content.
            TrackedFs.SetLastWriteTimeUtc(stagedPath, TrackedFs.GetOutputLastWriteTimeUtc(livePath));
            unchanged++;
        }

        var removed = liveFiles.Keys.Count(path => !stagedFiles.ContainsKey(path));
        if (!TrackedFs.OutputDirectoryExists(destination))
        {
            MoveDirectoryWithRetry(staging, destination);
            return new PublishResult(added, updated, removed, unchanged);
        }

        var parent = Path.GetDirectoryName(destination)!;
        var leaf = Path.GetFileName(destination.TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar));
        var backup = Path.Combine(parent, $".{leaf}.translate-mod-backup-{Guid.NewGuid():N}");

        MoveDirectoryWithRetry(destination, backup);
        try
        {
            MoveDirectoryWithRetry(staging, destination);
        }
        catch (Exception publishError)
        {
            if (!TrackedFs.OutputDirectoryExists(destination) && TrackedFs.OutputDirectoryExists(backup))
            {
                try
                {
                    MoveDirectoryWithRetry(backup, destination);
                }
                catch (Exception rollbackError)
                {
                    throw new AggregateException(
                        $"Failed to publish generated output and restore last-known-good directory {destination}.",
                        publishError,
                        rollbackError);
                }
            }
            throw;
        }

        // Publication already succeeded. A stale backup is safe and should not make a
        // correct generated tree appear to have failed.
        TryDeleteDirectory(backup, "prior output backup", warnings);

        return new PublishResult(added, updated, removed, unchanged);
    }

    private static void MoveDirectoryWithRetry(string source, string destination)
    {
        RetryTransientFileSystemAction(
            () => TrackedFs.MoveDirectory(source, destination),
            $"move directory '{source}' to '{destination}'");
    }

    private static void TryDeleteDirectory(string path, string description, List<string> warnings)
    {
        try
        {
            RetryTransientFileSystemAction(
                () => TrackedFs.DeleteDirectory(path, recursive: true),
                $"delete {description} '{path}'");
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            warnings.Add($"failed to delete {description} {path}: {ex.Message}");
        }
    }

    private static void RetryTransientFileSystemAction(Action action, string description)
    {
        for (var attempt = 1; ; attempt++)
        {
            try
            {
                action();
                return;
            }
            catch (Exception ex) when ((ex is IOException || ex is UnauthorizedAccessException) &&
                                       attempt < FileSystemRetryCount)
            {
                // Antivirus/indexer/build-system handles can briefly prevent directory
                // renames on Windows. Use bounded backoff; persistent failures still
                // trigger the last-known-good rollback above.
                Thread.Sleep(50 * attempt);
            }
            catch (Exception ex) when (ex is IOException || ex is UnauthorizedAccessException)
            {
                throw new IOException(
                    $"Unable to {description} after {FileSystemRetryCount} attempts.",
                    ex);
            }
        }
    }

    private static Dictionary<string, string> EnumerateRelativeFiles(string root) =>
        TrackedFs.EnumerateOutputFiles(root, "*", SearchOption.AllDirectories)
            .ToDictionary(
                path => Path.GetRelativePath(root, path),
                path => path,
                StringComparer.OrdinalIgnoreCase);

    private static bool FilesEqual(string left, string right) => FileOutput.FilesEqual(left, right);
}
