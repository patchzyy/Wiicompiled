namespace WiiCompiled.Setup;

/// <summary>
/// One entry of an exact regular directory tree. Directory topology is part of the content
/// contract everywhere this walker is used: an empty directory can be a runtime-visible asset just
/// as a regular file can be, so it must not disappear from a content identity or a staged copy.
/// </summary>
internal sealed record RegularTreeEntry(string RelativePath, string FullPath, bool IsDirectory,
    bool IsEmptyDirectory, long Length);

internal static class FileSystemUtilities
{
    public static void CopyDirectory(string source, string destination,
        CancellationToken cancellationToken = default)
    {
        Directory.CreateDirectory(destination);
        foreach (var directory in Directory.EnumerateDirectories(source, "*", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            Directory.CreateDirectory(Path.Combine(destination, Path.GetRelativePath(source, directory)));
        }
        foreach (var file in Directory.EnumerateFiles(source, "*", SearchOption.AllDirectories))
        {
            cancellationToken.ThrowIfCancellationRequested();
            var output = Path.Combine(destination, Path.GetRelativePath(source, file));
            Directory.CreateDirectory(Path.GetDirectoryName(output)!);
            File.Copy(file, output, overwrite: true);
        }
    }

    /// <summary>
    /// Enumerates an exact tree without following links, sorted by forward-slash relative path.
    /// Every directory appears exactly once and is flagged when it has no children at all.
    /// </summary>
    public static IReadOnlyList<RegularTreeEntry> EnumerateRegularTree(string root,
        CancellationToken cancellationToken = default, string description = "The directory tree")
    {
        var rootInfo = new DirectoryInfo(Path.GetFullPath(root));
        if (!rootInfo.Exists)
            throw new DirectoryNotFoundException($"{description} is missing: {rootInfo.FullName}");
        RejectReparsePoint(rootInfo, description);

        var entries = new List<RegularTreeEntry>();
        var pending = new Stack<DirectoryInfo>();
        pending.Push(rootInfo);
        while (pending.Count > 0)
        {
            cancellationToken.ThrowIfCancellationRequested();
            var directory = pending.Pop();
            var hasChild = false;
            foreach (var entry in directory.EnumerateFileSystemInfos())
            {
                cancellationToken.ThrowIfCancellationRequested();
                RejectReparsePoint(entry, description);
                hasChild = true;
                switch (entry)
                {
                    case DirectoryInfo child:
                        pending.Push(child);
                        break;
                    case FileInfo file:
                        entries.Add(new RegularTreeEntry(Relative(rootInfo, file.FullName), file.FullName,
                            false, false, file.Length));
                        break;
                    default:
                        throw new InvalidDataException(
                            $"{description} contains an unsupported file-system entry: {entry.FullName}");
                }
            }

            if (directory != rootInfo)
                entries.Add(new RegularTreeEntry(Relative(rootInfo, directory.FullName), directory.FullName,
                    true, !hasChild, 0));
        }

        entries.Sort((left, right) => StringComparer.Ordinal.Compare(left.RelativePath, right.RelativePath));
        return entries;
    }

    /// <summary>Removes exactly this directory, clearing attributes that would refuse deletion.</summary>
    public static void DeleteDirectoryIfExists(string path)
    {
        if (!Directory.Exists(path)) return;
        foreach (var entry in Directory.EnumerateFileSystemEntries(path, "*", SearchOption.AllDirectories))
        {
            try { File.SetAttributes(entry, FileAttributes.Normal); }
            catch { }
        }
        try { File.SetAttributes(path, FileAttributes.Normal); }
        catch { }
        Directory.Delete(path, recursive: true);
    }

    /// <summary>
    /// One comparable spelling of a path. Normalization happens before the trailing separator is
    /// dropped, because <c>Path.GetFullPath("C:")</c> is the current directory on that drive rather
    /// than the drive root.
    /// </summary>
    public static string NormalizePath(string path) =>
        Path.TrimEndingDirectorySeparator(Path.GetFullPath(path));

    /// <summary>Whether two paths name the same location, after normalization.</summary>
    public static bool PathsEqual(string left, string right) =>
        NormalizePath(left).Equals(NormalizePath(right), StringComparison.OrdinalIgnoreCase);

    /// <summary>Whether <paramref name="candidate"/> is <paramref name="container"/> or inside it, using a
    /// normalized prefix check so <c>C:\Games2</c> doesn't match inside <c>C:\Games</c>.</summary>
    public static bool PathContains(string container, string candidate)
    {
        container = NormalizePath(container);
        candidate = NormalizePath(candidate);
        return candidate.Equals(container, StringComparison.OrdinalIgnoreCase) ||
               candidate.StartsWith(container + Path.DirectorySeparatorChar,
                   StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>Whether two paths are equal or one contains the other, after normalization.</summary>
    public static bool PathsOverlap(string left, string right) =>
        PathContains(left, right) || PathContains(right, left);

    /// <summary>Location rules shared by install directories and portable roots (both are trees setup may
    /// replace and uninstall may delete). <paramref name="subject"/> only names what's being rejected.</summary>
    public static void EnsureUsableLocation(string path, string subject)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new InvalidOperationException($"{subject} is required.");
        if (path.StartsWith(@"\\", StringComparison.Ordinal))
            throw new InvalidOperationException($"{subject} must be on a local drive, not a network path.");
        if (path.Length > MaximumLocationLength)
            throw new InvalidOperationException(
                $"{subject} path is too long. Choose a path shorter than {MaximumLocationLength} characters.");

        var full = NormalizePath(path);
        var drive = Path.GetPathRoot(full);
        if (string.IsNullOrWhiteSpace(drive) ||
            full.Equals(NormalizePath(drive), StringComparison.OrdinalIgnoreCase))
        {
            throw new InvalidOperationException(
                $"{subject} must be a subfolder, not the root of a drive. " +
                "Choose a dedicated folder such as D:\\Games\\WiiCompiled\\Install.");
        }

        var windows = Environment.GetFolderPath(Environment.SpecialFolder.Windows);
        if (!string.IsNullOrEmpty(windows) && PathContains(windows, full))
            throw new InvalidOperationException($"{subject} cannot be inside the Windows directory.");

        foreach (var folder in ReservedProgramLocations)
        {
            var reserved = Environment.GetFolderPath(folder);
            // Unlike UserProfile/Documents/AppData below, no subfolder of Program Files or the
            // Windows system directories is ever a reasonable install target either - same
            // containment rule already applied to the Windows directory check above, just missing
            // here previously (only exact equality to the root was rejected).
            if (!string.IsNullOrEmpty(reserved) && PathContains(reserved, full))
                throw new InvalidOperationException(
                    $"{subject} cannot be inside {reserved}. Choose a dedicated folder.");
        }

        foreach (var folder in ReservedExactLocations)
        {
            var reserved = Environment.GetFolderPath(folder);
            if (string.IsNullOrEmpty(reserved)) continue;
            if (full.Equals(NormalizePath(reserved), StringComparison.OrdinalIgnoreCase))
                throw new InvalidOperationException(
                    $"{subject} cannot be {reserved}. Choose a dedicated folder.");
        }
    }

    /// <summary>
    /// The local build nests deep paths (llvm-mingw headers, CMake object directories) below the
    /// installation, so the root itself has to stay well inside the classic path limit.
    /// </summary>
    private const int MaximumLocationLength = 180;

    /// <summary>Well-known folders that must never <em>be</em> an installation or portable root.</summary>
    // No subfolder of any of these is ever a reasonable install target, so containment is rejected.
    private static readonly Environment.SpecialFolder[] ReservedProgramLocations =
    [
        Environment.SpecialFolder.ProgramFiles,
        Environment.SpecialFolder.ProgramFilesX86,
        Environment.SpecialFolder.CommonProgramFiles,
        Environment.SpecialFolder.System,
        Environment.SpecialFolder.SystemX86,
    ];

    // A subfolder of any of these (e.g. "Documents\Games\WiiCompiled") is a completely normal
    // install target, so only the exact special folder itself is rejected.
    private static readonly Environment.SpecialFolder[] ReservedExactLocations =
    [
        Environment.SpecialFolder.UserProfile,
        Environment.SpecialFolder.LocalApplicationData,
        Environment.SpecialFolder.ApplicationData,
        Environment.SpecialFolder.CommonApplicationData,
        Environment.SpecialFolder.DesktopDirectory,
        Environment.SpecialFolder.MyDocuments
    ];

    /// <summary>
    /// Writes a file so that a reader only ever sees the old contents or the new ones: the bytes go
    /// to a sibling temporary and are renamed over the destination, which is atomic on NTFS.
    /// Interrupting the write leaves the temporary behind, never a truncated document.
    /// </summary>
    public static void WriteAtomic(string path, byte[] contents)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(Path.GetFullPath(path))!);
        var temporary = path + $".tmp-{Guid.NewGuid():N}";
        try
        {
            File.WriteAllBytes(temporary, contents);
            File.Move(temporary, path, overwrite: true);
        }
        finally
        {
            try { File.Delete(temporary); } catch { }
        }
    }

    public static void WriteAtomic(string path, string contents) =>
        WriteAtomic(path, System.Text.Encoding.UTF8.GetBytes(contents));

    /// <summary>
    /// Refuses an operation that cannot fit on the destination drive. The caller owns the allowance
    /// it needs; this owns the one message and the one drive-readiness rule both paths report.
    /// </summary>
    public static void EnsureFreeSpace(string path, long required, string operationDescription)
    {
        var root = Path.GetPathRoot(path)
                   ?? throw new InvalidOperationException(
                       "The installation directory is not on a local drive.");
        var drive = new DriveInfo(root);
        if (!drive.IsReady)
            throw new IOException($"The destination drive {root} is not ready.");
        if (drive.AvailableFreeSpace < required)
        {
            throw new IOException(
                $"Not enough free space on {root}. {operationDescription} needs approximately " +
                $"{FormatGiB(required)} free, but only {FormatGiB(drive.AvailableFreeSpace)} is available.");
        }
    }

    private static string FormatGiB(long bytes) => $"{bytes / (1024d * 1024 * 1024):0.0} GiB";

    public static void RejectReparsePoint(FileSystemInfo entry, string description)
    {
        if ((entry.Attributes & FileAttributes.ReparsePoint) != 0)
            throw new InvalidDataException(
                $"{description} contains a reparse point instead of a regular entry: {entry.FullName}");
    }

    private static string Relative(DirectoryInfo root, string fullPath) =>
        Path.GetRelativePath(root.FullName, fullPath).Replace('\\', '/');
}
