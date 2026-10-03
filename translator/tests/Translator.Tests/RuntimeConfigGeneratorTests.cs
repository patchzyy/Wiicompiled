using System.IO;
using Translator.Core.CodeGen;
using Xunit;

namespace Translator.Tests;

public sealed class RuntimeConfigGeneratorTests
{
    private static string Generate(string? guestRegionHeader)
    {
        var path = Path.Combine(Path.GetTempPath(), $"runtime-config-{Guid.NewGuid():N}.h");
        try
        {
            RuntimeConfigGenerator.GenerateConfigHeader(0x80388880u, 0x8038AC20u, path, "Mario Kart Wii NTSC-U",
                guestRegionHeader);
            return File.ReadAllText(path);
        }
        finally
        {
            File.Delete(path);
        }
    }

    [Fact]
    public void RecordsTheProjectsGuestRegionHeader()
    {
        var header = Generate("runtime/include/region/rmce01.h");

        Assert.Contains("#define MKW_GUEST_REGION_HEADER \"runtime/include/region/rmce01.h\"", header);
        Assert.Contains("constexpr uint32_t SDA1_BASE = 0x80388880u;", header);
        Assert.Contains("constexpr uint32_t SDA2_BASE = 0x8038AC20u;", header);
    }

    [Fact]
    public void LeavesTheRegionToTheRuntimeDefaultWhenTheProjectNamesNoTable()
    {
        var header = Generate(null);

        Assert.DoesNotContain("MKW_GUEST_REGION_HEADER", header);
        Assert.Contains("constexpr uint32_t SDA1_BASE = 0x80388880u;", header);
    }
}
