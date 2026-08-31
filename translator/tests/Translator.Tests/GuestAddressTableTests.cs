using System.IO;
using Translator.Core;
using Xunit;

namespace Translator.Tests;

public sealed class GuestAddressTableTests
{
    // A region header in the shape tools/region/gen_region_headers.py writes: facts, then one
    // MKW_G_ (address) and MKW_F_ (translated symbol) define per PAL identity.
    private static readonly string[] NtscHeader =
    [
        "// AUTO-GENERATED",
        "#pragma once",
        "#define MKW_REGION_GAME_ID \"RMCE01\"",
        "#define MKW_G_801A9E84 0x801A9DE4u  // OSCreateThread",
        "#define MKW_F_801A9E84 func_801A9DE4",
        "#define MKW_G_803868A0 0x80382520u  // OSAlarmQueue",
        "#define MKW_F_803868A0 func_80382520",
        "#define MKW_G_8016b49c 0x8016B3FCu",
        "#define MKW_F_8016b49c func_8016B3FC",
    ];

    private static GuestAddressTable LoadTable(params string[] lines)
    {
        var path = Path.Combine(Path.GetTempPath(), $"guest-address-table-{Guid.NewGuid():N}.h");
        File.WriteAllLines(path, lines);
        try
        {
            return GuestAddressTable.Load(path);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void LoadsOnlyTheAddressDefines()
    {
        var table = LoadTable(NtscHeader);

        Assert.Equal(3, table.Count);
        Assert.Equal("801A9DE4", table.Resolve("801A9E84"));
        Assert.Equal("80382520", table.Resolve("803868A0"));
    }

    [Fact]
    public void ResolvesIdentitiesRegardlessOfHexCase()
    {
        var table = LoadTable(NtscHeader);

        Assert.Equal("801A9DE4", table.Resolve("801a9e84"));
        Assert.Equal("8016B3FC", table.Resolve("8016B49C"));
    }

    [Fact]
    public void ResolveRejectsAnIdentityTheTableDoesNotCarry()
    {
        var table = LoadTable(NtscHeader);

        var error = Assert.Throws<InvalidDataException>(() => table.Resolve("80000000"));
        Assert.Contains("80000000", error.Message);
    }

    [Fact]
    public void LoadRejectsAHeaderWithoutAddressDefines()
    {
        Assert.Throws<InvalidDataException>(() => LoadTable("#pragma once", "#define MKW_REGION_GAME_ID \"RMCE01\""));
    }

    [Fact]
    public void RewritesEveryAddressCarryingSpelling()
    {
        var table = LoadTable(NtscHeader);
        var source = string.Join('\n',
            "PPC_NATIVE_OVERRIDE(801A9E84, OSCreateThread_HLE, uint32_t, (CpuContext* ctx), (ctx));",
            "PPC_NATIVE_OVERRIDE_VOID(803868A0, Dummy, (CpuContext* ctx), (ctx));",
            "GX_FATAL_STUB(8016b49c, \"__GX__DefaultTexRegionCallback_8016b49c\")",
            "constexpr uint32_t kAlarmQueueAddr = MKW_GADDR(803868A0);",
            "extern \"C\" void MKW_GUEST_FUNC(801A9E84)(CpuContext* ctx);");

        var rewritten = table.Rewrite(source);

        Assert.Equal(string.Join('\n',
            "PPC_NATIVE_OVERRIDE(801A9DE4, OSCreateThread_HLE, uint32_t, (CpuContext* ctx), (ctx));",
            "PPC_NATIVE_OVERRIDE_VOID(80382520, Dummy, (CpuContext* ctx), (ctx));",
            "GX_FATAL_STUB(8016B3FC, \"__GX__DefaultTexRegionCallback_8016b49c\")",
            "constexpr uint32_t kAlarmQueueAddr = 0x80382520;",
            "extern \"C\" void func_801A9DE4(CpuContext* ctx);"), rewritten);
    }

    [Fact]
    public void RewriteLeavesSourcesWithoutAddressMacrosAlone()
    {
        var table = LoadTable(NtscHeader);
        const string source = "REGISTER_NATIVE_FUNCTION(0x801A9E84, OSCreateThread_HLE);\nuint32_t x = 0x803868A0u;";

        Assert.Equal(source, table.Rewrite(source));
    }

    [Fact]
    public void RewriteFailsLoudlyWhenAMacroSpellingSurvives()
    {
        var table = LoadTable(NtscHeader);

        // A spelling the rewrite patterns do not recognise must not slip through as a PAL address.
        var error = Assert.Throws<InvalidDataException>(() => table.Rewrite("uint32_t a = MKW_GADDR(0x801A9E84);"));
        Assert.Contains("MKW_GADDR(", error.Message);
    }

    [Fact]
    public void RewriteSkipsMacroDefinitionsAndTheirContinuationLines()
    {
        var table = LoadTable(NtscHeader);
        var source = string.Join('\n',
            "#define MKW_GADDR(pal_hex) MKW_G_##pal_hex",
            "#define GX_FATAL_STUB(addr, sym) \\",
            "    extern \"C\" void gx_stub_##addr(CpuContext* ctx) { HaltGX(MKW_GADDR(addr), sym); }",
            "constexpr uint32_t k = MKW_GADDR(801A9E84);");

        var rewritten = table.Rewrite(source);

        Assert.Contains("HaltGX(MKW_GADDR(addr), sym)", rewritten);
        Assert.Contains("constexpr uint32_t k = 0x801A9DE4;", rewritten);
    }
}
