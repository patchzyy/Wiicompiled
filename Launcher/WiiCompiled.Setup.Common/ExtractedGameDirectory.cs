using System.Text;

namespace WiiCompiled.Setup.Common;

/// <summary>The disc identity read from an extracted game's <c>sys/boot.bin</c>.</summary>
public sealed record ExtractedGameHeader(string GameId, int Revision, string InternalName);

/// <summary>
/// A game Wheel Wizard already extracted, in the layout Dolphin's "Extract Entire Disc" writes:
/// <c>&lt;root&gt;/DATA/{sys,files}</c>. WiiCompiled reads it in place (<c>dvd_root</c>) instead of extracting a
/// copy of its own, so it is only ever read, never written or deleted.
/// </summary>
public static class ExtractedGameDirectory
{
    private const int BootSize = 0x440;
    private const uint WiiMagic = 0x5D1C9EA3;

    /// <summary>
    /// The <c>DATA</c> folder of an extracted game: <paramref name="path"/> may be the game root holding
    /// <c>DATA/</c>, or the <c>DATA</c> folder itself.
    /// </summary>
    public static string ResolveDataDirectory(string path)
    {
        if (string.IsNullOrWhiteSpace(path))
            throw new ArgumentException("--game-dir needs the folder of the extracted game.");
        var full = FileSystemUtilities.NormalizePath(path);
        if (!Directory.Exists(full))
            throw new DirectoryNotFoundException($"The extracted game folder does not exist: {full}");

        foreach (var candidate in new[] { Path.Combine(full, "DATA"), full })
        {
            if (File.Exists(Path.Combine(candidate, "sys", "main.dol")) && File.Exists(Path.Combine(candidate, "sys", "boot.bin")))
                return FileSystemUtilities.NormalizePath(candidate);
        }

        throw new InvalidDataException(
            $"{full} is not an extracted Wii game: expected DATA/sys/main.dol and DATA/sys/boot.bin.");
    }

    /// <summary>Reads the disc header from <c>sys/boot.bin</c>, which starts with the same bytes as the disc.</summary>
    public static ExtractedGameHeader ReadHeader(string dataDirectory)
    {
        var boot = File.ReadAllBytes(Path.Combine(dataDirectory, "sys", "boot.bin"));
        if (boot.Length < BootSize)
            throw new InvalidDataException("The extracted game's sys/boot.bin is incomplete.");
        var magic = (uint)(boot[0x18] << 24 | boot[0x19] << 16 | boot[0x1A] << 8 | boot[0x1B]);
        if (magic != WiiMagic)
            throw new InvalidDataException("The extracted game is not a Wii game.");

        var gameId = Encoding.ASCII.GetString(boot, 0, 6).TrimEnd('\0', ' ');
        var nameBytes = boot.AsSpan(0x20, 0x40);
        var end = nameBytes.IndexOf((byte)0);
        var name = Encoding.ASCII.GetString(end < 0 ? nameBytes : nameBytes[..end]).Trim();
        return new ExtractedGameHeader(gameId, boot[7], name);
    }
}
