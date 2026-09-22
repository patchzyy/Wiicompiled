using System.Buffers.Binary;
using System.Text.Json;
using Translator.Cli.Configuration;
using Translator.Core.Build;
using Translator.Core.Mods;
using Translator.Core.Parsing.Kamek;

namespace Translator.Tests;

public sealed class KamekLrContinuationIntegrationTests
{
    private const uint Caller = 0x80004000;
    private const uint Module = 0x80010000;
    private const uint Hook = Module + 0x40;

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    public void SkipReturnThroughKnownTailCallsRetainsCallerDispatch(int tailDepth)
    {
        var bundle = Translate(tailDepth, SkipReturn(20));
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
        Assert.Contains(bundle.Entries, entry => entry.EntryPoint == Caller + 24 &&
            entry.VirtualPath.Contains("rr_continue_"));
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    [InlineData(2)]
    public void OrdinaryReturnsThroughKnownTailCallsStayLightweight(int tailDepth)
    {
        var bundle = Translate(tailDepth, [0x38630001u, 0x4E800020u]); // addi r3,r3,1; blr
        var caller = Source(bundle, Caller);
        Assert.DoesNotContain("switch (ctx->lr)", caller);
        Assert.DoesNotContain("if (ctx->lr !=", caller);
        Assert.DoesNotContain(bundle.Entries, entry => entry.VirtualPath.Contains("rr_continue_"));
    }

    [Fact]
    public void DirectSkipReturnRegistersTheAdjustedBaseAddress()
    {
        var bundle = Translate(0, SkipReturn(20));
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
        Assert.Contains(bundle.Entries, entry => entry.EntryPoint == Caller + 24 &&
            entry.VirtualPath.Contains("rr_continue_"));
    }

    [Fact]
    public void CtrSkipRetainsDispatchAndRegistersItsContinuation()
    {
        var bundle = Translate(0,
        [
            0x7D8802A6u, // mflr r12
            0x398C0014u, // addi r12,r12,20
            0x7D8903A6u, // mtctr r12
            0x4E800420u // bctr
        ]);
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
        Assert.Contains(bundle.Entries, entry => entry.EntryPoint == Caller + 24 &&
            entry.VirtualPath.Contains("rr_continue_"));
    }

    [Fact]
    public void NormalReturnArmDoesNotHideTailCalledSkipReturn()
    {
        var bundle = Translate(1, SkipReturn(20), conditionalWrapper: true);
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void SharedHookRetainsResumeDispatchAtEveryCallSite(int tailDepth)
    {
        var bundle = Translate(tailDepth, SkipReturn(20), sharedTarget: true);
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
        AssertResumeDispatch(Source(bundle, Caller + 0x40), Caller + 0x40, Caller + 0x58);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(1)]
    public void SkipReturnHandlingDoesNotSpreadToAnOrdinaryPatchedCaller(int tailDepth)
    {
        var bundle = Translate(tailDepth, SkipReturn(20), companionBody: [0x38630001u, 0x4E800020u]);
        var ordinaryCaller = Source(bundle, Caller + 0x40);
        Assert.DoesNotContain("switch (ctx->lr)", ordinaryCaller);
        Assert.DoesNotContain("if (ctx->lr !=", ordinaryCaller);
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
    }

    [Theory]
    [InlineData(0)]
    [InlineData(20)]
    public void SavedAndRestoredLrDistinguishesOrdinaryAndSkipReturns(int offset)
    {
        var bundle = Translate(0,
        [
            0x7D8802A6u, // mflr r12
            0x91810004u, // stw r12,4(r1)
            0x81810004u, // lwz r12,4(r1)
            0x398C0000u | (ushort)offset, // addi r12,r12,offset
            0x7D8803A6u, // mtlr r12
            0x4E800020u // blr
        ]);
        var caller = Source(bundle, Caller);
        if (offset == 0)
            Assert.DoesNotContain("if (ctx->lr !=", caller);
        else
            AssertResumeDispatch(caller, Caller, Caller + 4 + (uint)offset);
    }

    [Fact]
    public void CappedAnalysisDoesNotClassifyAHookAsAnOrdinaryReturn()
    {
        var bundle = Translate(0, SyntheticLrHookFactory.ManyOrdinaryReturnPaths());
        AssertResumeDispatch(Source(bundle, Caller), Caller, Caller + 24);
        Assert.Contains(bundle.Entries, entry => entry.EntryPoint == Caller + 24 &&
            entry.VirtualPath.Contains("rr_continue_"));
    }

    private static void AssertResumeDispatch(string source, uint callSite, uint continuation)
    {
        var call = source.IndexOf($"InvokeDirectCpu<0x{Hook:X8}u>(ctx);", StringComparison.Ordinal);
        Assert.True(call >= 0, source);
        var guard = source.IndexOf($"if (ctx->lr != 0x{callSite + 4:X8}u)", call, StringComparison.Ordinal);
        Assert.True(guard > call, $"The hook must dispatch its adjusted return address.\n{source}");
        Assert.Contains($"case 0x{continuation:X8}u:", source[guard..]);
        Assert.Contains($"goto loc_{continuation:X8};", source[guard..]);
        Assert.Contains($"loc_{continuation:X8}:", source);
    }

    private static uint[] SkipReturn(int offset) =>
    [
        0x7D8802A6u, // mflr r12
        0x398C0000u | (ushort)offset, // addi r12,r12,offset
        0x7D8803A6u, // mtlr r12
        0x4E800020u // blr
    ];

    private static string Source(TranslationSourceBundle bundle, uint address) =>
        Assert.Single(bundle.Entries, entry => entry.EntryPoint == address &&
            entry.VirtualPath.StartsWith("overlays/", StringComparison.Ordinal)).Source;

    private static TranslationSourceBundle Translate(int tailDepth, uint[] body,
        bool conditionalWrapper = false, bool sharedTarget = false, uint[]? companionBody = null)
    {
        var root = Path.Combine(Path.GetTempPath(), $"kamek-lr-integration-{Guid.NewGuid():N}");
        Directory.CreateDirectory(root);
        try
        {
            uint[] caller = [0x60000000u, 0x38630001u, 0x38630001u, 0x38630001u,
                0x38630001u, 0x38630001u, 0x60000000u, 0x4E800020u];
            var hasSecondCaller = sharedTarget || companionBody is not null;
            var baseWords = Enumerable.Repeat(0x4E800020u, hasSecondCaller ? 24 : 8).ToArray();
            caller.CopyTo(baseWords, 0);
            if (hasSecondCaller) caller.CopyTo(baseWords, 16);
            var baseBytes = Words(baseWords);
            File.WriteAllBytes(Path.Combine(root, "main.dol"), SyntheticDolFactory.CreateBytes(
                Caller, sections: [SyntheticDolFactory.Text(0, Caller, baseWords)]));
            File.WriteAllBytes(Path.Combine(root, "base.bin"), baseBytes);
            // translate-mod requires a REL; this fixture has no REL code or relocations.
            File.WriteAllBytes(Path.Combine(root, "empty.rel"), new byte[0x48]);
            var functions = new List<BaseFunctionRangeMetadata>
            {
                new(Caller, Caller + 32, "caller", ".text", 0, "synthetic", ["Executable"])
            };
            if (hasSecondCaller)
                functions.Add(new(Caller + 0x40, Caller + 0x60, "second_caller", ".text", 0x40,
                    "synthetic", ["Executable"]));
            var manifest = new BaseManifest("synthetic", 1, "TEST01", "P", "", 0,
                [new BaseSectionMetadata(".text", "main.dol", Caller, Caller + (uint)baseBytes.Length,
                    true, false, "base.bin", 0)],
                functions, "ranges.json");
            var manifestPath = Path.Combine(root, "base.json");
            File.WriteAllText(manifestPath, JsonSerializer.Serialize(manifest));
            var projectPath = Path.Combine(root, "recomp.yml");
            Directory.CreateDirectory(Path.Combine(root, "native"));
            Directory.CreateDirectory(Path.Combine(root, "generated", "functions"));
            File.WriteAllText(projectPath, """
                schema_version: 1
                project:
                  id: kamek-lr-test
                memory:
                  base: 0x80000000
                  size: 0x00020000
                  sda_base: 0x80002000
                  sda2_base: 0x80003000
                inputs:
                  dol:
                    path: main.dol
                  rel:
                    path: empty.rel
                    load_address: 0x80008000
                runtime:
                  native_registration_root: native
                output:
                  root: generated
                """);

            var moduleSize = companionBody is null ? 0x40 * (tailDepth + 1) + body.Length * 4
                : 0x300 + companionBody.Length * 4;
            var moduleWords = Enumerable.Repeat(0x4E800020u, moduleSize / 4).ToArray();
            body.CopyTo(moduleWords, (0x40 * (tailDepth + 1)) / 4);
            var commands = new List<uint> { ((uint)KamekCommandId.BranchLink << 24) | 0x00FFFFFEu, Caller, Hook - Module };
            if (hasSecondCaller)
                commands.AddRange([((uint)KamekCommandId.BranchLink << 24) | 0x00FFFFFEu,
                    Caller + 0x40, sharedTarget ? Hook - Module : 0x300u]);
            companionBody?.CopyTo(moduleWords, 0x300 / 4);
            for (var depth = 0; depth < tailDepth; depth++)
            {
                var offset = 0x40u * (uint)(depth + 1);
                var branchOffset = offset;
                if (conditionalWrapper && depth == 0)
                {
                    moduleWords[offset / 4] = 0x2C030000u; // cmpwi r3,0
                    moduleWords[offset / 4 + 1] = 0x4D820020u; // beqlr
                    branchOffset += 8;
                }
                // Explicit Kamek branch targets make every helper a known function boundary.
                commands.Add(((uint)KamekCommandId.Branch << 24) | branchOffset);
                commands.Add(offset + 0x40);
            }
            var code = Words(moduleWords);
            var commandBytes = Words(commands.ToArray());
            var pul = new byte[KamekChunk.HeaderSize + code.Length + commandBytes.Length];
            Write(pul, 0, KamekChunk.Magic0);
            Write(pul, 4, KamekChunk.Magic1);
            Write(pul, 12, (uint)code.Length);
            Write(pul, 24, (uint)pul.Length);
            code.CopyTo(pul, KamekChunk.HeaderSize);
            commandBytes.CopyTo(pul, KamekChunk.HeaderSize + code.Length);
            var pulPath = Path.Combine(root, "Code.pul");
            File.WriteAllBytes(pulPath, pul);
            var output = Path.Combine(root, "mod");
            string[] args = ["translate-mod", "--project", projectPath, "--code-pul", pulPath,
                "--base-manifest", manifestPath, "--out", output, "--module-guest-base", $"0x{Module:X8}",
                "--module-link-base", $"0x{Module:X8}", "--skip-retro-wfc", "--emit-cpp", "--threads", "1"];
            var entryPoint = typeof(TranslationProjectConfig).Assembly.EntryPoint!;
            var exitCode = Assert.IsType<int>(entryPoint.Invoke(null, [args]));
            Assert.Equal(0, exitCode);
            return TranslationSourceBundle.Read(Path.Combine(output, "translated_sources.bin"));
        }
        finally
        {
            Directory.Delete(root, recursive: true);
        }
    }

    private static byte[] Words(uint[] words)
    {
        var bytes = new byte[words.Length * 4];
        for (var index = 0; index < words.Length; index++) Write(bytes, index * 4, words[index]);
        return bytes;
    }

    private static void Write(byte[] bytes, int offset, uint value) =>
        BinaryPrimitives.WriteUInt32BigEndian(bytes.AsSpan(offset, 4), value);
}
