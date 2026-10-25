using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using Translator.Core;
using Translator.Core.CodeGen;
using Xunit;

namespace Translator.Tests;

public sealed class RuntimeNativeIndexTests
{
    // The Windows setup self-test and Launcher/build-cache.sh pin the same fixture digest.
    [Fact]
    public void SourceDigestMatchesInstallerImplementations()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkw-native-digest-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        try
        {
            File.WriteAllText(Path.Combine(directory, "main.cpp"), "int main();");
            Assert.Equal("50b9779591f4b205751361c8899accd362f96ea5dfd32febed1bedca534c523a",
                RuntimeNativeIndexFile.ComputeSourceDigest(directory));
        }
        finally
        {
            Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void BuildCapturesEveryRuntimeRegistrationKind()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkw-native-kinds-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        try
        {
            File.WriteAllText(Path.Combine(directory, "registrations.cpp"), """
                REGISTER_NATIVE_FUNCTION(0x80000010, Direct);
                REGISTER_NATIVE_FUNCTION_AS(0x80000020, Aliased, "alias");
                REGISTER_TRANSLATED_FUNCTION(0x80000030, Translated);
                PPC_NATIVE_OVERRIDE_VOID(80000040, Stub, (void), ());
                GX_FATAL_STUB(80000050, "Fatal")
                // REGISTER_NATIVE_FUNCTION(0x80000060, CommentedOut);
                REGISTER_NATIVE_WRAPPER_AS(0x80000070, Wrapper, "wrapper");
                REGISTER_NATIVE_REPLACEMENT_AS(0x80000080, Replacement, "replacement");
                """);

            var registrations = RuntimeNativeIndexBuilder.Build(directory).Registrations;
            Assert.Equal(7, registrations.Length);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000010u && entry.ExcludesBaseTranslation &&
                entry.ModOverlay == NativeModOverlay.Unspecified);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000020u && !entry.ExcludesBaseTranslation &&
                entry.ModOverlay == NativeModOverlay.Unspecified);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000070u && entry.Symbol == "Wrapper" && !entry.ExcludesBaseTranslation &&
                entry.ModOverlay == NativeModOverlay.Wrapper);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000080u && entry.Symbol == "Replacement" && !entry.ExcludesBaseTranslation &&
                entry.ModOverlay == NativeModOverlay.Replacement);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000030u && entry.IsTranslatedOverride);
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000040u && entry.Symbol == "Stub");
            Assert.Contains(registrations, static entry =>
                entry.Address == 0x80000050u && entry.Symbol == "GX_FATAL_STUB_80000050");
            Assert.DoesNotContain(registrations, static entry => entry.Address == 0x80000060u);
        }
        finally
        {
            if (Directory.Exists(directory))
                Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void BuildSharesTypedAbiAndEffectDataWithoutCreatingSidecarFiles()
    {
        var directory = Path.Combine(Path.GetTempPath(), $"mkw-native-index-{Guid.NewGuid():N}");
        Directory.CreateDirectory(directory);
        var sourcePath = Path.Combine(directory, "fixture.cpp");
        try
        {
            File.WriteAllText(sourcePath, """
                extern "C" void Typed(float value, uint32_t count) { (void)value; (void)count; }
                PPC_NATIVE_OVERRIDE_VOID(80001234, Typed, (float value, uint32_t count), (value, count));
                """);

            var index = RuntimeNativeIndexBuilder.Build(directory);
            Assert.Single(index.Registrations);
            Assert.Single(index.VoidStubAbis);
            Assert.Single(index.Effects);
            Assert.True(index.ToGuestEffectSet().Contracts.ContainsKey(0x80001234u));

            var provider = RuntimeNativeFunctionAbiProvider.FromIndex(
                index, new HashSet<uint> { 0x80001234u });
            Assert.True(provider.TryGetGuestFunctionAbi("func_80001234", out var abi));
            Assert.Contains("f1", abi.ArgumentRegisters);
            Assert.Contains("r3", abi.ArgumentRegisters);
            Assert.Contains("f1", abi.ScalarFloatArgumentRegisters);

            var outOfScope = RuntimeNativeFunctionAbiProvider.FromIndex(index, new HashSet<uint>());
            Assert.False(outOfScope.TryGetGuestFunctionAbi("func_80001234", out _));

            RuntimeNativeIndexBuilder.Resolve(directory, Path.Combine(directory, "missing-index.json"));
            Assert.Equal([sourcePath], Directory.GetFiles(directory));
        }
        finally
        {
            if (Directory.Exists(directory))
                Directory.Delete(directory, recursive: true);
        }
    }

    [Fact]
    public void SerializedRepositoryIndexRoundTripsExactly()
    {
        var sources = Path.Combine(ProjectPathsForTests.FindRepositoryRoot(), "runtime", "src");
        var built = RuntimeNativeIndexBuilder.Build(sources);
        Assert.NotEmpty(built.Registrations);
        Assert.Contains(built.Registrations, static entry => entry.ModOverlay == NativeModOverlay.Wrapper);
        Assert.Contains(built.Registrations, static entry => entry.ModOverlay == NativeModOverlay.Replacement);
        Assert.NotEmpty(built.VoidStubAbis);
        Assert.NotEmpty(built.Effects);

        var path = Path.Combine(Path.GetTempPath(), $"mkw-native-index-{Guid.NewGuid():N}.json");
        try
        {
            var bytes = RuntimeNativeIndexFile.Serialize(built, "digest");
            File.WriteAllBytes(path, bytes);
            Assert.True(RuntimeNativeIndexFile.TryLoad(path, "digest", out var loaded, out var reason), reason);
            AssertIndexEqual(built, loaded);
            Assert.Equal(bytes, RuntimeNativeIndexFile.Serialize(loaded, "digest"));
        }
        finally
        {
            File.Delete(path);
        }
    }

    // A wrapper that calls through with MKW_GUEST_FUNC would silently drop the mod overlay it promises to run.
    [Fact]
    public void RepositoryWrapperNativesCallThroughToTheNextTranslation()
    {
        var sources = Path.Combine(ProjectPathsForTests.FindRepositoryRoot(), "runtime", "src");
        var wrappers = RuntimeNativeIndexBuilder.Build(sources).Registrations
            .Where(static entry => entry.ModOverlay == NativeModOverlay.Wrapper)
            .ToArray();
        Assert.NotEmpty(wrappers);
        foreach (var wrapper in wrappers)
        {
            var source = File.ReadAllText(Path.Combine(sources, wrapper.SourceFile));
            var pal = $"{wrapper.Address:X8}";
            Assert.True(Regex.IsMatch(source, $@"MKW_GUEST_NEXT\(\s*{pal}\s*\)", RegexOptions.IgnoreCase),
                $"{wrapper.Symbol} is a wrapper but never calls MKW_GUEST_NEXT({pal})");
            Assert.False(Regex.IsMatch(source, $@"MKW_GUEST_FUNC\(\s*{pal}\s*\)", RegexOptions.IgnoreCase),
                $"{wrapper.Symbol} is a wrapper but calls MKW_GUEST_FUNC({pal})");
        }
    }

    [Fact]
    public void ResolveUsesAMatchingIndexAndKeepsWorkingWithoutSources()
    {
        var root = CreateFixture();
        try
        {
            var sources = Path.Combine(root, "src");
            var indexPath = Path.Combine(root, RuntimeNativeIndexFile.DefaultFileName);
            var built = RuntimeNativeIndexBuilder.Build(sources);
            Assert.True(RuntimeNativeIndexFile.Write(
                indexPath, built, RuntimeNativeIndexFile.ComputeSourceDigest(sources)));

            var diagnostics = new StringWriter();
            AssertIndexEqual(built, RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics));
            Assert.Equal("", diagnostics.ToString());
            Assert.Contains(built.Registrations, static entry => entry.SourceFile == "hle/gx/stubs.cpp");
            Assert.Contains(built.Effects, static entry => entry.IsPrecise);
            Assert.Contains(built.Effects, static entry => !entry.IsPrecise);

            Directory.Delete(sources, recursive: true);
            AssertIndexEqual(built, RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics));
            Assert.Equal("", diagnostics.ToString());
            Assert.Empty(RuntimeNativeIndexBuilder.Build(sources).Registrations);

            File.WriteAllText(indexPath, "{}");
            Assert.Throws<InvalidOperationException>(
                () => RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void ResolveRescansWhenTheIndexIsStale()
    {
        var root = CreateFixture();
        try
        {
            var sources = Path.Combine(root, "src");
            var indexPath = Path.Combine(root, RuntimeNativeIndexFile.DefaultFileName);
            RuntimeNativeIndexFile.Write(
                indexPath, RuntimeNativeIndexBuilder.Build(sources), RuntimeNativeIndexFile.ComputeSourceDigest(sources));

            File.AppendAllText(Path.Combine(sources, "natives.cpp"), "\nREGISTER_NATIVE_FUNCTION(0x80009990, Added);\n");
            var diagnostics = new StringWriter();
            var resolved = RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics);
            Assert.Contains(resolved.Registrations, static entry => entry.Address == 0x80009990u);
            var current = RuntimeNativeIndexBuilder.Build(sources);
            AssertIndexEqual(current, resolved);
            Assert.Contains("native sources changed", diagnostics.ToString());

            var json = Encoding.UTF8.GetString(
                    RuntimeNativeIndexFile.Serialize(current, RuntimeNativeIndexFile.ComputeSourceDigest(sources)))
                .Replace($"\"extractorVersion\": {RuntimeNativeIndexFile.ExtractorVersion}", "\"extractorVersion\": 0");
            File.WriteAllText(indexPath, json);
            diagnostics = new StringWriter();
            AssertIndexEqual(current, RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics));
            Assert.Contains("extractor version differs", diagnostics.ToString());

            File.WriteAllText(indexPath, "{ not json");
            diagnostics = new StringWriter();
            AssertIndexEqual(current, RuntimeNativeIndexBuilder.Resolve(sources, indexPath, diagnostics));
            Assert.Contains("unreadable", diagnostics.ToString());
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void SerializationAndDigestAreDeterministicAcrossLocations()
    {
        var first = CreateFixture();
        var second = CreateFixture();
        try
        {
            var firstSources = Path.Combine(first, "src");
            var secondSources = Path.Combine(second, "src");
            var digest = RuntimeNativeIndexFile.ComputeSourceDigest(firstSources);
            Assert.Equal(digest, RuntimeNativeIndexFile.ComputeSourceDigest(secondSources));

            var bytes = RuntimeNativeIndexFile.Serialize(RuntimeNativeIndexBuilder.Build(firstSources), digest);
            Assert.Equal(bytes, RuntimeNativeIndexFile.Serialize(RuntimeNativeIndexBuilder.Build(firstSources), digest));
            Assert.Equal(bytes, RuntimeNativeIndexFile.Serialize(RuntimeNativeIndexBuilder.Build(secondSources), digest));
            Assert.DoesNotContain((byte)'\r', bytes);
            Assert.Equal((byte)'\n', bytes[^1]);

            var expected = new StringBuilder();
            foreach (var relative in new[] { "hle/gx/stubs.cpp", "include/decl.h", "natives.cpp" })
            {
                var fileHash = SHA256.HashData(File.ReadAllBytes(Path.Combine(firstSources, relative)));
                expected.Append(Convert.ToHexString(fileHash).ToLowerInvariant()).Append("  ").Append(relative).Append('\n');
            }
            Assert.Equal(
                Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(expected.ToString()))).ToLowerInvariant(),
                digest);

            File.WriteAllText(Path.Combine(firstSources, "notes.txt"), "not scanned");
            Assert.Equal(digest, RuntimeNativeIndexFile.ComputeSourceDigest(firstSources));
            File.AppendAllText(Path.Combine(firstSources, "include", "decl.h"), "\n");
            Assert.NotEqual(digest, RuntimeNativeIndexFile.ComputeSourceDigest(firstSources));
        }
        finally
        {
            Directory.Delete(first, recursive: true);
            Directory.Delete(second, recursive: true);
        }
    }

    internal static string CreateFixture()
    {
        var root = Path.Combine(Path.GetTempPath(), $"mkw-native-fixture-{Guid.NewGuid():N}");
        var sources = Path.Combine(root, "src");
        Directory.CreateDirectory(Path.Combine(sources, "hle", "gx"));
        Directory.CreateDirectory(Path.Combine(sources, "include"));
        File.WriteAllText(Path.Combine(sources, "natives.cpp"), """
            extern "C" void Precise(CpuContext* ctx) { ctx->gpr[3] = ctx->gpr[4]; ctx->lr = 0; }
            extern "C" void Complete(CpuContext* ctx) { Helper(ctx); }
            REGISTER_NATIVE_FUNCTION(0x80000010, Precise);
            REGISTER_NATIVE_FUNCTION_AS(0x80000020, Complete, "alias");
            REGISTER_TRANSLATED_FUNCTION(0x80000030, Translated);
            GX_FATAL_STUB(80000050, "Fatal")
            """);
        File.WriteAllText(Path.Combine(sources, "hle", "gx", "stubs.cpp"), """
            extern "C" void Typed(float value, uint32_t count) { (void)value; (void)count; }
            PPC_NATIVE_OVERRIDE_VOID(80001234, Typed, (float value, uint32_t count), (value, count));
            """);
        File.WriteAllText(Path.Combine(sources, "include", "decl.h"), """
            REGISTER_NATIVE_FUNCTION(0x80000060, Declared);
            """);
        return root;
    }

    internal static void AssertIndexEqual(RuntimeNativeIndex expected, RuntimeNativeIndex actual)
    {
        Assert.Equal(expected.Registrations, actual.Registrations);
        Assert.Equal(expected.Effects, actual.Effects);
        Assert.Equal(expected.VoidStubAbis.Length, actual.VoidStubAbis.Length);
        foreach (var (left, right) in expected.VoidStubAbis.Zip(actual.VoidStubAbis))
        {
            Assert.Equal(left.Address, right.Address);
            Assert.Equal(left.ArgumentRegisters, right.ArgumentRegisters);
            Assert.Equal(left.ScalarFloatArgumentRegisters, right.ScalarFloatArgumentRegisters);
        }
    }
}
