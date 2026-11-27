using Translator.Core.IO;
using System.Security.Cryptography;

namespace Translator.Core.Loading;

/// <summary>
/// The single SHA-256 hex spelling used everywhere identities are compared. Lower-case is load-bearing:
/// a divergent casing anywhere would silently invalidate content-addressed reuse.
/// </summary>
public static class ChecksumUtilities
{
    public static string Sha256Hex(ReadOnlySpan<byte> data) =>
        Convert.ToHexString(SHA256.HashData(data)).ToLowerInvariant();

    public static string Sha256Hex(byte[] data) =>
        Sha256Hex((ReadOnlySpan<byte>)data);

    /// <summary>Digest of raw hash bytes that were already computed elsewhere.</summary>
    public static string ToHex(ReadOnlySpan<byte> hash) =>
        Convert.ToHexString(hash).ToLowerInvariant();

    /// <summary>Streams the file rather than materializing it, so hashing a REL costs no copy.</summary>
    public static string Sha256HexOfFile(string path)
    {
        using var stream = TrackedFs.OpenRead(path);
        return ToHex(SHA256.HashData(stream));
    }
}
