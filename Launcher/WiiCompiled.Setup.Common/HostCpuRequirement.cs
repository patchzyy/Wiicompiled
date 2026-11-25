using System.Runtime.InteropServices;
using System.Runtime.Intrinsics.X86;

namespace WiiCompiled.Setup.Common;

public enum X86CpuProfile
{
    V2,
    V3
}

// Setup-time copy of runtime/src/host_cpu_baseline.cpp, checked before anything is built.
public static class HostCpuRequirement
{
    private readonly record struct Feature(string Name, uint Leaf, int Register, int Bit, X86CpuProfile Profile);

    // Same table and order as the runtime: register 0..3 = eax, ebx, ecx, edx.
    private static readonly Feature[] Features =
    [
        new("SSE3", 1, 2, 0, X86CpuProfile.V2),
        new("SSSE3", 1, 2, 9, X86CpuProfile.V2),
        new("CMPXCHG16B", 1, 2, 13, X86CpuProfile.V2),
        new("SSE4.1", 1, 2, 19, X86CpuProfile.V2),
        new("SSE4.2", 1, 2, 20, X86CpuProfile.V2),
        new("POPCNT", 1, 2, 23, X86CpuProfile.V2),
        new("LAHF-SAHF", 0x80000001u, 2, 0, X86CpuProfile.V2),
        new("FMA", 1, 2, 12, X86CpuProfile.V3),
        new("MOVBE", 1, 2, 22, X86CpuProfile.V3),
        new("OSXSAVE", 1, 2, 27, X86CpuProfile.V3),
        new("AVX", 1, 2, 28, X86CpuProfile.V3),
        new("F16C", 1, 2, 29, X86CpuProfile.V3),
        new("BMI1", 7, 1, 3, X86CpuProfile.V3),
        new("AVX2", 7, 1, 5, X86CpuProfile.V3),
        new("BMI2", 7, 1, 8, X86CpuProfile.V3),
        new("LZCNT", 0x80000001u, 2, 5, X86CpuProfile.V3),
    ];

    public static void EnsureSupported(X86CpuProfile profile)
    {
        if (RuntimeInformation.ProcessArchitecture != Architecture.X64)
            return;

        // The JIT only reports AVX when the OS also saves the YMM state (XCR0), which is the
        // runtime's last check; there is no managed XGETBV.
        var missing = FindMissingFeatures(profile, CpuId, Avx.IsSupported);
        if (missing.Count > 0)
            throw new PlatformNotSupportedException(Describe(profile, missing));
    }

    // The highest profile this machine runs; builds fall back to v2 when v3 is missing.
    public static X86CpuProfile SelectBuildProfile() =>
        RuntimeInformation.ProcessArchitecture != Architecture.X64
            ? X86CpuProfile.V3
            : SelectBuildProfile(CpuId, Avx.IsSupported);

    public static X86CpuProfile SelectBuildProfile(Func<uint, uint[]> cpuid, bool osSavesAvxState) =>
        FindMissingFeatures(X86CpuProfile.V3, cpuid, osSavesAvxState).Count == 0
            ? X86CpuProfile.V3
            : X86CpuProfile.V2;

    public static IReadOnlyList<string> FindMissingFeatures(X86CpuProfile profile,
        Func<uint, uint[]> cpuid, bool osSavesAvxState)
    {
        var maxBasic = cpuid(0)[0];
        var maxExtended = cpuid(0x80000000u)[0];
        var missing = new List<string>();
        foreach (var feature in Features)
        {
            if (feature.Profile > profile)
                continue;
            var leafAvailable = (feature.Leaf & 0x80000000u) != 0
                ? feature.Leaf <= maxExtended
                : feature.Leaf <= maxBasic;
            if (!leafAvailable || (cpuid(feature.Leaf)[feature.Register] & (1u << feature.Bit)) == 0)
                missing.Add(feature.Name);
        }

        if (missing.Count == 0 && profile >= X86CpuProfile.V3 && !osSavesAvxState)
            missing.Add("operating system support for AVX register state (XCR0 YMM bits)");
        return missing;
    }

    public static string Describe(X86CpuProfile profile, IReadOnlyList<string> missing) =>
        profile == X86CpuProfile.V2
            ? "This build needs a processor that supports the x86-64-v2 instruction set.\n\n" +
              "Missing on this machine: " + string.Join(", ", missing) + "\n\n" +
              "x86-64-v2 covers Intel Core processors from Nehalem (2008) onward and AMD processors " +
              "from Jaguar (2013) onward."
            : "This build needs a processor that supports the x86-64-v3 instruction set.\n\n" +
              "Missing on this machine: " + string.Join(", ", missing) + "\n\n" +
              "x86-64-v3 covers Intel Core processors from Haswell (4th generation, 2013) onward " +
              "and AMD processors from Excavator (2015) onward.";

    private static uint[] CpuId(uint leaf)
    {
        var (eax, ebx, ecx, edx) = X86Base.CpuId(unchecked((int)leaf), 0);
        return [unchecked((uint)eax), unchecked((uint)ebx), unchecked((uint)ecx), unchecked((uint)edx)];
    }
}
