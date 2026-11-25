using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Translator.Cli.Configuration;
using Translator.Core.Mods.Mkwii;
using Translator.Core.Parsing.Kamek;
using PayloadFiles = WiiCompiled.Setup.Common.RetroWfcPayload;

namespace Translator.Tests;

public sealed class RegionalRetroRewindTests
{
    [Theory]
    [InlineData("mkwii", "RMCP01", "P", 0x800ED6E8u, 0x80601C60u, 0x4BD977A4u)]
    [InlineData("mkwii-ntsc-u", "RMCE01", "E", 0x800ED648u, 0x805DD380u, 0x4BDB7D04u)]
    [InlineData("mkwii-ntsc-j", "RMCJ01", "J", 0x800ED608u, 0x806013D4u, 0x4BD979B0u)]
    public void RetailProfileUsesMatchingRegionAndSuppressesItsBootstrap(
        string directory, string gameId, string region, uint hook, uint integritySite, uint expectedBranch)
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var source = File.ReadAllText(Path.Combine(root, "projects", directory, "recomp.yml"));
        var expectedDolHash = Regex.Match(source, @"sha256: ([0-9a-f]{64})").Groups[1].Value;
        var scratch = Path.Combine(Path.GetTempPath(), $"retro-profile-{Guid.NewGuid():N}");
        Directory.CreateDirectory(scratch);
        TranslationProjectConfig project;
        try
        {
            // Keep this suite binary-free: substitute only the retail input paths and hashes.
            // The profile block, including its original compatibility pins, is read unchanged.
            var input = Path.Combine(scratch, "input.bin");
            File.WriteAllBytes(input, new byte[0x100]);
            var hash = Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(input))).ToLowerInvariant();
            source = source.Replace("workspace_root: ../..", $"workspace_root: {root}")
                .Replace("path: Assets/main.dol", $"path: {input}")
                .Replace("path: Assets/StaticR.rel", $"path: {input}");
            source = Regex.Replace(source, @"(?m)^    sha256: [0-9a-f]{64}\r?$", $"    sha256: {hash}");
            var manifest = Path.Combine(scratch, "recomp.yml");
            File.WriteAllText(manifest, source);
            project = TranslationProjectConfig.Load(manifest);
        }
        finally { Directory.Delete(scratch, true); }
        var profile = project.Profiles["retro-rewind"];
        Assert.True(profile.Enabled);
        Assert.Equal(gameId, profile.RequiresGameId);
        Assert.Equal(expectedDolHash, profile.RequiresDolSha256);
        Assert.Equal(region, profile.Region);
        Assert.Equal(PayloadFiles.PayloadUri(gameId), profile.RetroWfcPayload);
        Assert.Equal(hook, profile.RetroWfcLegacyBootstrapHook);

        // The pack checks the static branch bytes at Page::IncrementDuration + 0x18.
        // Its hook is module offset 0x124. A PAL link base in an NTSC profile translates
        // successfully but fails this check at boot, so pin the actual expected opcode.
        Assert.Equal(expectedBranch, KamekPpcEncoding.EncodeBranch(
            integritySite, profile.ModuleLinkBase!.Value + 0x124u, link: false));

        // A combined pack has a different absolute hook in each regional chunk.
        // Selecting a region and suppressing a PAL hook in it must not succeed.
        var bootstrap = new KamekCommand(0, 0, KamekCommandId.Branch, true, hook, [0x100u]);
        var chunk = new KamekChunk(0, 0, 0, 4, 0, 0, 4, new byte[4], [bootstrap]);
        Assert.Empty(RetroWfcBootstrapSuppressor.Suppress(chunk, profile.RetroWfcLegacyBootstrapHook!.Value).Commands);
        if (region != "P")
            Assert.Throws<InvalidDataException>(() => RetroWfcBootstrapSuppressor.Suppress(chunk, 0x800ED6E8u));
    }

    [Theory]
    [InlineData("RMCP01")]
    [InlineData("RMCE01")]
    [InlineData("RMCJ01")]
    public void StagedPayloadRequiresValidSignatureAndMatchingGame(string gameId)
    {
        var root = Path.Combine(Path.GetTempPath(), $"retro-region-{Guid.NewGuid():N}");
        try
        {
            var path = Path.Combine(root, PayloadFiles.PayloadRelativePath(gameId));
            Directory.CreateDirectory(Path.GetDirectoryName(path)!);
            using var rsa = RSA.Create(2048);
            var key = rsa.ExportParameters(false);
            var image = new byte[0x150];
            "WWFC/Payload"u8.CopyTo(image);
            BinaryPrimitives.WriteUInt32BigEndian(image.AsSpan(0x0C), (uint)image.Length);
            Encoding.ASCII.GetBytes(PayloadFiles.PayloadGame(gameId)).CopyTo(image, 0x138);
            Sign();
            Assert.Equal(path, PayloadFiles.ResolveRetroWfcPayloadFile(root, gameId, key));

            image[^1] ^= 1;
            File.WriteAllBytes(path, image);
            Assert.Throws<InvalidDataException>(() => PayloadFiles.ResolveRetroWfcPayloadFile(root, gameId, key));

            // Even a correctly signed payload renamed to this region's filename is rejected.
            Encoding.ASCII.GetBytes(gameId == "RMCP01" ? "RMCED00" : "RMCPD00").CopyTo(image, 0x138);
            Sign();
            Assert.Throws<InvalidDataException>(() => PayloadFiles.ResolveRetroWfcPayloadFile(root, gameId, key));

            void Sign()
            {
                rsa.SignData(image.AsSpan(0x110), HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)
                    .CopyTo(image.AsSpan(0x10));
                File.WriteAllBytes(path, image);
            }
        }
        finally { if (Directory.Exists(root)) Directory.Delete(root, true); }
    }

    [Theory]
    [InlineData("https://rwfc.net/api/wfc/payload?g=RMCED00")]
    [InlineData("https://rwfc.net/api/wfc/payload?g=RMCJD00")]
    public void OnlyExactRegionalDownloadEndpointsAreAccepted(string uri)
    {
        PayloadFiles.ValidateRetroWfcPayloadUri(uri);
        Assert.Throws<InvalidDataException>(() => PayloadFiles.ValidateRetroWfcPayloadUri(uri + "&extra=1"));
        Assert.Throws<InvalidDataException>(() => PayloadFiles.ValidateRetroWfcPayloadUri(uri.Replace("https:", "http:")));
    }
}
