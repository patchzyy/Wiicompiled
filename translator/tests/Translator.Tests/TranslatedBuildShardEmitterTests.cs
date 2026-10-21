using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using Translator.Core;
using Translator.Core.Build;
using Translator.Core.Mods;
using Xunit;

namespace Translator.Tests;

public sealed class TranslatedBuildShardEmitterTests
{
    [Fact]
    public void RemainingWeightPartitionDoesNotCreateOversizedTailBin()
    {
        var weights = Enumerable.Repeat(300L, 10).ToArray();
        var first = TranslatedBuildShardEmitter.PartitionWeightedIndices(weights, 4);
        var second = TranslatedBuildShardEmitter.PartitionWeightedIndices(weights, 4);

        Assert.Equal(new[] { 2, 3, 2, 3 }, first.Select(static group => group.Count));
        Assert.Equal(
            first.Select(static group => string.Join(",", group)),
            second.Select(static group => string.Join(",", group)));
        Assert.All(first, group => Assert.InRange(group.Sum(index => weights[index]), 600, 900));
    }

    [Fact]
    public void EmitsDeterministicSmallTraitShardsWithoutCodeMap()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-build-shards-{Guid.NewGuid():N}");
        var functions = Path.Combine(root, "functions");
        var native = Path.Combine(root, "runtime", "src");
        var mod = Path.Combine(root, "mod", "cpp");
        var output = Path.Combine(root, "out");
        Directory.CreateDirectory(functions);
        Directory.CreateDirectory(native);
        Directory.CreateDirectory(mod);
        try
        {
            const string caller = """
                #include "abi_bridge.h"
                extern "C" void func_80001000(CpuContext* ctx) {
                    InvokeDirectCpu<0x80002000u>(ctx);
                }
                // RECOMP_REGISTRATION base 0x80001000 func_80001000 preserves=true fpr_mask=0x00000000
                """;
            const string target = """
                #include "abi_bridge.h"
                extern "C" void func_80002000(CpuContext*) {}
                // RECOMP_REGISTRATION base 0x80002000 func_80002000 preserves=true fpr_mask=0x00000000
                """;
            const string runtimeOverridden = """
                #include "abi_bridge.h"
                extern "C" void func_80003000(CpuContext*) {}
                // RECOMP_REGISTRATION base 0x80003000 func_80003000 preserves=true fpr_mask=0x00000000
                """;
            const string nativeOverridden = """
                #include "abi_bridge.h"
                extern "C" void func_80004000(CpuContext*) {}
                // RECOMP_REGISTRATION base 0x80004000 func_80004000 preserves=true fpr_mask=0x00000000
                """;
            Write(Path.Combine(functions, "func_80001000.cpp"), caller);
            Write(Path.Combine(functions, "func_80002000.cpp"), target);
            Write(Path.Combine(functions, "func_80003000.cpp"), runtimeOverridden);
            Write(Path.Combine(functions, "func_80004000.cpp"), nativeOverridden);
            Write(Path.Combine(native, "override.cpp"),
                "REGISTER_TRANSLATED_FUNCTION(0x80003000, RuntimeOverride_80003000);\n" +
                "REGISTER_NATIVE_FUNCTION(0x80004000, NativeOverride_80004000);\n");
            const string modSource = """
                #include "abi_bridge.h"
                extern "C" void rr_80002000(CpuContext*) {}
                // RECOMP_REGISTRATION mod 0x80002000 rr_80002000 "rr_80002000" preserves=false fpr_mask=0x00004000 priority=100 module_id=7
                """;
            Write(Path.Combine(mod, "rr_80002000.cpp"), modSource);

            var metadataPath = Path.Combine(root, "base_output.json");
            BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(metadataPath,
                BaseTranslationOutputMetadata.Create([
                    Metadata("func_80001000.cpp", 0x80001000u, caller),
                    Metadata("func_80002000.cpp", 0x80002000u, target),
                    Metadata("func_80003000.cpp", 0x80003000u, runtimeOverridden),
                    Metadata("func_80004000.cpp", 0x80004000u, nativeOverridden)
                ], TranslationQualityMetadata.Clean, "identity"));
            var manifestPath = Path.Combine(root, "base_manifest.json");
            Write(manifestPath, """
                {"Functions":[
                  {"Start":2147487744,"End":2147487748,"Name":"func_80001000"},
                  {"Start":2147491840,"End":2147491844,"Name":"func_80002000"},
                  {"Start":2147495936,"End":2147495940,"Name":"func_80003000"},
                  {"Start":2147500032,"End":2147500036,"Name":"func_80004000"}
                ]}
                """);
            var resolvedPath = Path.Combine(root, "mod", "resolved_dispatch_profile.json");
            Write(resolvedPath, """
                {"Entries":[
                  {"Address":2147487744,"Symbol":"func_80001000","Name":"func_80001000","Kind":"base","Priority":0,"DirectCallAvailable":true,"PreservesNonvolatileFprs":true,"NonvolatileFprWriteMask":0,"MustRemainDynamicallyDispatchable":false},
                  {"Address":2147491840,"Symbol":"rr_80002000","Name":"rr_80002000","Kind":"rr","Priority":100,"DirectCallAvailable":true,"PreservesNonvolatileFprs":false,"NonvolatileFprWriteMask":16384,"MustRemainDynamicallyDispatchable":true}
                ]}
                """);

            var options = new TranslatedBuildShardOptions(
                metadataPath, functions, output, native,
                resolvedPath, mod, BaseShardCount: 2, ModShardCount: 1, RegistrationShardCount: 1,
                Region: GuestRegion.NtscJ);
            var first = TranslatedBuildShardEmitter.Emit(options);
            Assert.Contains("set(MKW_TRANSLATED_GUEST_REGION 2)", File.ReadAllText(first.CMakeManifestPath));
            Assert.Equal(1, first.SharedBaseFunctionCount);
            Assert.Equal(1, first.ProfileSensitiveTargetCount);
            Assert.Equal(1, first.ProfileSensitiveCallerCount);
            Assert.Equal(1, first.ModFunctionCount);
            Assert.False(File.Exists(Path.Combine(root, "mod", "code.map")));

            var shard = Directory.GetFiles(Path.Combine(output, "base_portable_sensitive"), "*.cpp").Single();
            var shardText = File.ReadAllText(shard);
            Assert.Contains("#include \"abi_bridge.h\"", shardText);
            Assert.DoesNotContain("#include \"" + Path.GetFullPath(Path.Combine(functions, "func_80001000.cpp")).Replace('\\', '/'), shardText);
            Assert.Contains("MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);", shardText);
            Assert.Contains("ApplyRuntimeCallOptions(Target, Context)", shardText);

            var portableBaseTraits = Directory.GetFiles(Path.Combine(output, "base_portable_sensitive"), "*_traits.h").Single();
            var portableRetroTraits = Directory.GetFiles(Path.Combine(output, "retro_portable_sensitive"), "*_traits.h").Single();
            Assert.Contains("MKW_TRANSLATED_TRAIT(80002000, func_80002000,", File.ReadAllText(portableBaseTraits));
            Assert.Contains("MKW_TRANSLATED_TRAIT(80002000, rr_80002000,", File.ReadAllText(portableRetroTraits));
            var baseDispatch = Directory.GetFiles(Path.Combine(output, "base_dispatch"), "*.cpp").Single();
            var baseDispatchText = File.ReadAllText(baseDispatch);
            Assert.Contains("const StaticIndirectDispatchSegment kSegments[256]", baseDispatchText);
            Assert.Contains("{0x80001000u, &func_80001000, 0x00000000u, false}", baseDispatchText);
            Assert.Contains("{0x80002000u, &func_80002000, 0x00000000u, false}", baseDispatchText);
            Assert.Contains("{0x80003000u, &RuntimeOverride_80003000, 0xFFFFC000u, false}", baseDispatchText);
            Assert.DoesNotContain("0x80004000u", baseDispatchText);
            var retroDispatch = Directory.GetFiles(Path.Combine(output, "retro_rewind_dispatch"), "*.cpp").Single();
            var retroDispatchText = File.ReadAllText(retroDispatch);
            Assert.Contains("{0x80002000u, &rr_80002000, 0x00004000u, false}", retroDispatchText);
            Assert.DoesNotContain("{0x80002000u, &func_80002000", retroDispatchText);

            // A function claimed by a native registration is excluded from the
            // translated graph entirely rather than emitted and then overridden.
            Assert.DoesNotContain(
                "func_80003000",
                Directory.GetFiles(output, "*.cpp", SearchOption.AllDirectories)
                    .Select(File.ReadAllText)
                    .Aggregate(string.Concat));

            var timestamps = Directory.GetFiles(output, "*", SearchOption.AllDirectories)
                .ToDictionary(Path.GetFullPath, File.GetLastWriteTimeUtc, StringComparer.OrdinalIgnoreCase);
            TranslatedBuildShardEmitter.Emit(options);
            foreach (var (path, timestamp) in timestamps)
                Assert.Equal(timestamp, File.GetLastWriteTimeUtc(path));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void BindsDirectCallsToTheSelectedProfileWinner()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-same-tu-calls-{Guid.NewGuid():N}");
        var functions = Path.Combine(root, "functions");
        var native = Path.Combine(root, "runtime", "src");
        var output = Path.Combine(root, "out");
        Directory.CreateDirectory(functions);
        Directory.CreateDirectory(native);
        try
        {
            // 0x80001000 shares a shard with both of its targets: 0x80002000 has
            // the same winner in every profile, 0x80003000 writes nonvolatile FPRs.
            const string commonCaller = """
                #include "abi_bridge.h"
                extern "C" void func_80001000(CpuContext* ctx) {
                    InvokeDirectCpu<0x80002000u>(ctx);
                    InvokeDirectCpu<0x80003000u>(ctx);
                }
                // RECOMP_REGISTRATION base 0x80001000 func_80001000 preserves=true fpr_mask=0x00000000
                """;
            const string invariantTarget = """
                #include "abi_bridge.h"
                extern "C" void func_80002000(CpuContext*) {}
                // RECOMP_REGISTRATION base 0x80002000 func_80002000 preserves=true fpr_mask=0x00000000
                """;
            const string fprWritingTarget = """
                #include "abi_bridge.h"
                extern "C" void func_80003000(CpuContext*) {}
                // RECOMP_REGISTRATION base 0x80003000 func_80003000 preserves=false fpr_mask=0x00004000
                """;
            // 0x80004000 and 0x80005000 both call the profile-sensitive winner at
            // 0x80005000, so they land in the product-specific partition.
            const string sharedCaller = """
                #include "abi_bridge.h"
                extern "C" void func_80004000(CpuContext* ctx) {
                    InvokeDirectCpu<0x80005000u>(ctx);
                    InvokeDirectCpu<0x80002000u>(ctx);
                }
                // RECOMP_REGISTRATION base 0x80004000 func_80004000 preserves=true fpr_mask=0x00000000
                """;
            const string profileSensitiveTarget = """
                #include "abi_bridge.h"
                extern "C" void func_80005000(CpuContext* ctx) {
                    InvokeDirectCpu<0x80005000u>(ctx);
                }
                // RECOMP_REGISTRATION base 0x80005000 func_80005000 preserves=true fpr_mask=0x00000000
                """;
            Write(Path.Combine(functions, "func_80001000.cpp"), commonCaller);
            Write(Path.Combine(functions, "func_80002000.cpp"), invariantTarget);
            Write(Path.Combine(functions, "func_80003000.cpp"), fprWritingTarget);
            Write(Path.Combine(functions, "func_80004000.cpp"), sharedCaller);
            Write(Path.Combine(functions, "func_80005000.cpp"), profileSensitiveTarget);

            var metadataPath = Path.Combine(root, "base_output.json");
            BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(metadataPath,
                BaseTranslationOutputMetadata.Create([
                    Metadata("func_80001000.cpp", 0x80001000u, commonCaller),
                    Metadata("func_80002000.cpp", 0x80002000u, invariantTarget),
                    Metadata("func_80003000.cpp", 0x80003000u, fprWritingTarget),
                    Metadata("func_80004000.cpp", 0x80004000u, sharedCaller),
                    Metadata("func_80005000.cpp", 0x80005000u, profileSensitiveTarget)
                ], TranslationQualityMetadata.Clean, "identity"));
            var manifestPath = Path.Combine(root, "base_manifest.json");
            Write(manifestPath, """
                {"Functions":[
                  {"Start":2147487744,"End":2147487748,"Name":"func_80001000"},
                  {"Start":2147491840,"End":2147491844,"Name":"func_80002000"},
                  {"Start":2147495936,"End":2147495940,"Name":"func_80003000"},
                  {"Start":2147500032,"End":2147500036,"Name":"func_80004000"},
                  {"Start":2147504128,"End":2147504132,"Name":"func_80005000"}
                ]}
                """);
            // Retro Rewind resolves 0x80005000 to its own translation, so that
            // winner is not profile-invariant even inside one shard.
            var resolvedPath = Path.Combine(root, "resolved_dispatch_profile.json");
            Write(resolvedPath, """
                {"Entries":[
                  {"Address":2147504128,"Symbol":"rr_80005000","Name":"rr_80005000","Kind":"rr","Priority":100,"DirectCallAvailable":true,"PreservesNonvolatileFprs":true,"NonvolatileFprWriteMask":0,"MustRemainDynamicallyDispatchable":false}
                ]}
                """);

            var options = new TranslatedBuildShardOptions(
                metadataPath, functions, output, native,
                resolvedPath, BaseShardCount: 1, ModShardCount: 1, RegistrationShardCount: 1);
            TranslatedBuildShardEmitter.Emit(options);

            var commonShard = File.ReadAllText(
                Directory.GetFiles(Path.Combine(output, "base_common"), "*.cpp").Single());
            // Same shard, same winner in both profiles: call the definition in
            // this translation unit so the host optimizer can still inline it.
            Assert.Contains("MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);", commonShard);
            // A nonvolatile-FPR writer keeps the generic dynamically guarded call.
            Assert.Contains("InvokeDirectCpu<0x80003000u>(ctx);", commonShard);
            var baseSpecific = Directory.GetFiles(Path.Combine(output, "base_portable_sensitive"), "*.cpp")
                .Select(File.ReadAllText)
                .Aggregate(string.Concat);
            Assert.Contains("MKW_STATIC_TRANSLATED_CALL(0x80005000u, func_80005000, ctx);", baseSpecific);
            Assert.Contains("MKW_STATIC_TRANSLATED_CALL(0x80002000u, func_80002000, ctx);", baseSpecific);

            // The lowering choice participates in shard identity, so a rebuild is
            // byte-identical and file identities are stable.
            var timestamps = Directory.GetFiles(output, "*", SearchOption.AllDirectories)
                .ToDictionary(Path.GetFullPath, File.GetLastWriteTimeUtc, StringComparer.OrdinalIgnoreCase);
            TranslatedBuildShardEmitter.Emit(options);
            foreach (var (path, timestamp) in timestamps)
                Assert.Equal(timestamp, File.GetLastWriteTimeUtc(path));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void FrozenBaseCommonBoundariesKeepUnchangedShardIdentityAcrossAFunctionBodyChange()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-frozen-shards-{Guid.NewGuid():N}");
        var functions = Path.Combine(root, "functions");
        var native = Path.Combine(root, "runtime", "src");
        var output = Path.Combine(root, "out");
        Directory.CreateDirectory(functions);
        Directory.CreateDirectory(native);
        try
        {
            // Six equally weighted profile-neutral functions pack 3/3 across two
            // base_common shards.
            var addresses = new uint[] { 0x80001000, 0x80002000, 0x80003000, 0x80004000, 0x80005000, 0x80006000 };
            static string Body(uint address, string filler) =>
                "#include \"abi_bridge.h\"\n" +
                $"extern \"C\" void func_{address:X8}(CpuContext*) {{}}\n" +
                $"// {filler}\n" +
                $"// RECOMP_REGISTRATION base 0x{address:X8} func_{address:X8} preserves=true fpr_mask=0x00000000\n";

            var metadataPath = Path.Combine(root, "base_output.json");
            var manifestPath = Path.Combine(root, "base_manifest.json");
            void WriteInputs(string lastFunctionFiller)
            {
                var entries = new List<BaseTranslationFunctionMetadata>();
                foreach (var address in addresses)
                {
                    var filler = address == addresses[^1] ? lastFunctionFiller : "";
                    var source = Body(address, filler);
                    var relative = $"func_{address:X8}.cpp";
                    Write(Path.Combine(functions, relative), source);
                    entries.Add(Metadata(relative, address, source));
                }
                BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(
                    metadataPath, BaseTranslationOutputMetadata.Create(
                        entries, TranslationQualityMetadata.Clean, "identity"));
                Write(manifestPath,
                    "{\"Functions\":[" +
                    string.Join(",", addresses.Select(address =>
                        $"{{\"Start\":{address},\"End\":{address + 4},\"Name\":\"func_{address:X8}\"}}")) +
                    "]}");
            }

            string[] BaseCommonShards() =>
                Directory.GetFiles(Path.Combine(output, "base_common"), "*.cpp")
                    .Select(static path => Path.GetFileName(path)!)
                    .Order(StringComparer.Ordinal)
                    .ToArray();

            var options = new TranslatedBuildShardOptions(
                metadataPath, functions, output, native,
                BaseShardCount: 2, ModShardCount: 1, RegistrationShardCount: 1);

            WriteInputs("");
            var first = TranslatedBuildShardEmitter.Emit(options);
            Assert.False(first.BaseCommonBoundariesReused);
            Assert.NotNull(first.BaseCommonShardMapPath);
            var mapPath = first.BaseCommonShardMapPath!;
            Assert.Equal("base_common_shard_map.json", Path.GetFileName(mapPath));
            Assert.True(File.Exists(mapPath));
            Assert.NotNull(first.BaseCommonBalance);
            Assert.Equal(2, first.BaseCommonBalance!.ShardCount);
            var frozenMap = File.ReadAllText(mapPath);
            Assert.Contains("\"0x80001000\"", frozenMap);
            Assert.Contains("\"0x80004000\"", frozenMap);
            var before = BaseCommonShards();
            Assert.Equal(2, before.Length);

            // Only the last function's body changes, and it changes enough to
            // move every greedy cut point behind it. Frozen membership keeps the
            // shard that owns the first three functions byte-identical, so its
            // content-addressed name - and its object file - survive.
            WriteInputs(new string('x', 4096));
            var second = TranslatedBuildShardEmitter.Emit(options);
            Assert.True(second.BaseCommonBoundariesReused);
            Assert.Equal(frozenMap, File.ReadAllText(mapPath));
            var after = BaseCommonShards();
            Assert.Equal(2, after.Length);
            Assert.Single(before.Intersect(after, StringComparer.Ordinal));

            // Discarding the recorded table is the only thing that moves a boundary.
            File.Delete(mapPath);
            var repacked = TranslatedBuildShardEmitter.Emit(options);
            Assert.False(repacked.BaseCommonBoundariesReused);
            var repackedMap = File.ReadAllText(mapPath);
            Assert.NotEqual(frozenMap, repackedMap);
            Assert.Contains("\"0x80006000\"", repackedMap);
            Assert.Empty(after.Intersect(BaseCommonShards(), StringComparer.Ordinal));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }
    }

    [Fact]
    public void LoadedNativeIndexEmitsTheSameGraphAsAScan()
    {
        var root = Path.Combine(Path.GetTempPath(), $"translator-native-index-shards-{Guid.NewGuid():N}");
        var functions = Path.Combine(root, "functions");
        var native = Path.Combine(root, "runtime", "src");
        var output = Path.Combine(root, "out");
        Directory.CreateDirectory(functions);
        try
        {
            var sources = new Dictionary<uint, string>();
            foreach (var address in new[] { 0x80001000u, 0x80003000u, 0x80004000u })
            {
                sources[address] = $$"""
                    #include "abi_bridge.h"
                    extern "C" void func_{{address:X8}}(CpuContext*) {}
                    // RECOMP_REGISTRATION base 0x{{address:X8}} func_{{address:X8}} preserves=true fpr_mask=0x00000000
                    """;
                Write(Path.Combine(functions, $"func_{address:X8}.cpp"), sources[address]);
            }
            Write(Path.Combine(native, "hle", "override.cpp"),
                "REGISTER_TRANSLATED_FUNCTION(0x80003000, RuntimeOverride_80003000);\n" +
                "REGISTER_NATIVE_FUNCTION(0x80004000, NativeOverride_80004000);\n");
            var metadataPath = Path.Combine(root, "base_output.json");
            BaseTranslationOutputMetadataFile.WriteIfChangedAtomic(metadataPath,
                BaseTranslationOutputMetadata.Create(
                    sources.Select(item => Metadata($"func_{item.Key:X8}.cpp", item.Key, item.Value)).ToList(),
                    TranslationQualityMetadata.Clean, "identity"));

            var indexPath = Path.Combine(root, "runtime", RuntimeNativeIndexFile.DefaultFileName);
            var scanned = RuntimeNativeIndexBuilder.Resolve(native, indexPath);
            TranslatedBuildShardEmitter.Emit(new TranslatedBuildShardOptions(
                metadataPath, functions, output, native, BaseShardCount: 2, ModShardCount: 1,
                RegistrationShardCount: 1, NativeIndex: scanned));
            var expected = Snapshot(output);
            Assert.Contains(expected.Values, static text => text.Contains("RuntimeOverride_80003000"));

            RuntimeNativeIndexFile.Write(indexPath, scanned, RuntimeNativeIndexFile.ComputeSourceDigest(native));
            Directory.Delete(native, recursive: true);
            Directory.Delete(output, recursive: true);
            var loaded = RuntimeNativeIndexBuilder.Resolve(native, indexPath);
            TranslatedBuildShardEmitter.Emit(new TranslatedBuildShardOptions(
                metadataPath, functions, output, native, BaseShardCount: 2, ModShardCount: 1,
                RegistrationShardCount: 1, NativeIndex: loaded));
            Assert.Equal(expected, Snapshot(output));
        }
        finally
        {
            if (Directory.Exists(root)) Directory.Delete(root, recursive: true);
        }

        static SortedDictionary<string, string> Snapshot(string directory) =>
            new(Directory.GetFiles(directory, "*", SearchOption.AllDirectories)
                .ToDictionary(path => Path.GetRelativePath(directory, path), File.ReadAllText), StringComparer.Ordinal);
    }

    private static BaseTranslationFunctionMetadata Metadata(string path, uint address, string content)
    {
        var bytes = Encoding.UTF8.GetBytes(content);
        return new BaseTranslationFunctionMetadata(
            path, bytes.Length, Convert.ToHexString(SHA256.HashData(bytes)).ToLowerInvariant(), address, []);
    }

    private static void Write(string path, string content)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, content, new UTF8Encoding(false));
    }
}
