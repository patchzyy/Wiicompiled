using Translator.Core.Mods;

namespace Translator.Tests;

public sealed class ModDataPatchWriterTests
{
    [Fact]
    public void WriteEmitsBinaryBlobsAndStableAssemblyWithoutHexArrays()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-mod-data-" + Guid.NewGuid().ToString("N"));
        try
        {
            var cppPath = Path.Combine(root, "cpp", "mod_data_patches.cpp");
            var moduleImage = Enumerable.Range(0, 257).Select(i => (byte)(i * 37)).ToArray();
            var digest = Enumerable.Range(0, 20).Select(i => (byte)(0xA0 + i)).ToArray();

            Write(cppPath, moduleImage, digest);

            var blobRoot = Path.Combine(root, "cpp", "mod_data_patches_blobs");
            Assert.Equal(moduleImage, File.ReadAllBytes(Path.Combine(blobRoot, "module_image.bin")));
            Assert.Equal(digest, File.ReadAllBytes(Path.Combine(blobRoot, "kamek_code_sha1.bin")));

            var cpp = File.ReadAllText(cppPath);
            Assert.Contains("extern const uint8_t kModuleImage[];", cpp, StringComparison.Ordinal);
            Assert.Contains("Memory::GetPointer(kModuleGuestBase, kModuleImageSize), kModuleImage, kModuleImageSize", cpp, StringComparison.Ordinal);
            Assert.DoesNotContain("alignas(16) const uint8_t", cpp, StringComparison.Ordinal);
            Assert.DoesNotContain("0x00, 0x25,", cpp, StringComparison.Ordinal);

            var assemblyPath = Path.Combine(root, "cpp", "mod_data_patches_blobs.S");
            var assembly = File.ReadAllText(assemblyPath);
            var symbolPrefix = OperatingSystem.IsMacOS() ? "_" : "";
            Assert.Contains($".globl {symbolPrefix}kModuleImage", assembly, StringComparison.Ordinal);
            Assert.Contains($".globl {symbolPrefix}kKamekCodeSha1Digest", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain("__APPLE__", assembly, StringComparison.Ordinal);
            Assert.Contains(".incbin", assembly, StringComparison.Ordinal);

            var timestamps = new[] { cppPath, assemblyPath }
                .Concat(Directory.GetFiles(blobRoot, "*.bin"))
                .ToDictionary(path => path, File.GetLastWriteTimeUtc);
            Write(cppPath, moduleImage, digest);
            foreach (var (path, timestamp) in timestamps)
            {
                Assert.Equal(timestamp, File.GetLastWriteTimeUtc(path));
            }
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [Fact]
    public void WritePrunesOptionalDigestWhenItDisappears()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-mod-data-" + Guid.NewGuid().ToString("N"));
        try
        {
            var cppPath = Path.Combine(root, "mod_data_patches.cpp");
            Write(cppPath, [1, 2, 3, 4], new byte[20]);
            Write(cppPath, [1, 2, 3, 4], null);

            var blobRoot = Path.Combine(root, "mod_data_patches_blobs");
            Assert.Equal(["module_image.bin"], Directory.GetFiles(blobRoot, "*.bin")
                .Select(path => Path.GetFileName(path)!)
                .ToArray());
            var assembly = File.ReadAllText(Path.Combine(root, "mod_data_patches_blobs.S"));
            Assert.DoesNotContain("kKamekCodeSha1Digest", assembly, StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [Fact]
    public void WriteCanReferencePublishedBlobsFromTransactionalStaging()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-mod-data-" + Guid.NewGuid().ToString("N"));
        try
        {
            var stagingCpp = Path.Combine(root, ".staging", "cpp", "mod_data_patches.cpp");
            var publishedBlobs = Path.Combine(root, "published", "cpp", "mod_data_patches_blobs");
            Write(stagingCpp, [1, 2, 3, 4], null, publishedBlobDirectory: publishedBlobs);

            var assembly = File.ReadAllText(Path.Combine(root, ".staging", "cpp", "mod_data_patches_blobs.S"));
            var expected = Path.GetFullPath(Path.Combine(publishedBlobs, "module_image.bin")).Replace('\\', '/');
            Assert.Contains($".incbin \"{expected}\"", assembly, StringComparison.Ordinal);
            Assert.DoesNotContain(".staging", assembly, StringComparison.OrdinalIgnoreCase);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    [Fact]
    public void WriteChangesAssemblyWhenSameSizeBlobContentChanges()
    {
        var root = Path.Combine(Path.GetTempPath(), "mkw-mod-data-" + Guid.NewGuid().ToString("N"));
        try
        {
            var cppPath = Path.Combine(root, "mod_data_patches.cpp");
            Write(cppPath, [1, 2, 3, 4], null);
            var assemblyPath = Path.Combine(root, "mod_data_patches_blobs.S");
            var before = File.ReadAllText(assemblyPath);

            Write(cppPath, [1, 2, 3, 5], null);
            var after = File.ReadAllText(assemblyPath);

            Assert.NotEqual(before, after);
            Assert.Contains("sha256=", after, StringComparison.Ordinal);
        }
        finally
        {
            if (Directory.Exists(root))
            {
                Directory.Delete(root, recursive: true);
            }
        }
    }

    private static void Write(
        string path,
        byte[] moduleImage,
        byte[]? digest,
        string? publishedBlobDirectory = null)
    {
        var plan = new KamekPatchPlan { ModuleGuestBase = 0x81700000u, Classifications = [] };
        var manifest = new BaseManifest("test", 1, "RMCP01", "P", "hash", 0, [], [], "ranges.json");
        ModDataPatchWriter.Write(
            path,
            plan,
            0x803992E0u,
            manifest,
            [],
            moduleImage,
            digest,
            null,
            (uint)moduleImage.Length,
            0x40u,
            0x81700000u,
            0x81700000u,
            false,
            publishedBlobDirectory: publishedBlobDirectory);
    }
}
