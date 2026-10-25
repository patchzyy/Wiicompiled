using System.Text.RegularExpressions;
using Translator.Core;
using Translator.Core.Analysis;
using Xunit;

namespace Translator.Tests;

public sealed class GuestAddressTableTests
{
    [Theory]
    [InlineData("RMCP01", GuestRegion.Pal)]
    [InlineData("RMCE01", GuestRegion.NtscU)]
    [InlineData("rmcj01", GuestRegion.NtscJ)]
    [InlineData("RECOMPGEN", GuestRegion.Pal)]
    [InlineData(null, GuestRegion.Pal)]
    public void RegionFollowsTheGameId(string? gameId, GuestRegion expected) =>
        Assert.Equal(expected, GuestRegions.FromGameId(gameId));

    [Theory]
    [InlineData("RMCK01")]
    [InlineData("RMCP02")]
    public void UnsupportedMarioKartReleasesAreRejected(string gameId) =>
        Assert.Throws<InvalidDataException>(() => GuestRegions.FromGameId(gameId));

    [Fact]
    public void MapsRangesAndRejectsUnknownAndOverlappingRanges()
    {
        WithTable("""
            // comment
            MKW_REGION_RANGE(NtscU, 801A9000, 801A9FFF, -0xA0)
            MKW_REGION_RANGE(NtscU, 80386000, 80386FFF, -0x4360)
            MKW_REGION_RANGE(NtscJ, 801A9000, 801A9FFF, -0xE0)
            MKW_REGION_RANGE(NtscJ, 80386000, 80386FFF, -0x680)
            """, table =>
            {
                Assert.Equal(4, table.Count);
                Assert.Equal(0x801A9E84u, table.Map(0x801A9E84, GuestRegion.Pal));
                Assert.Equal(0x801A9DE4u, table.Map(0x801A9E84, GuestRegion.NtscU));
                Assert.Equal(0x80385FE4u, table.Map(0x80386664, GuestRegion.NtscJ));
                Assert.Equal(0x80000000u, table.Map(0x80000000, GuestRegion.Pal));
                Assert.Throws<InvalidDataException>(() => table.Map(0x80000000, GuestRegion.NtscU));
                Assert.Throws<InvalidDataException>(() => table.Map(0x801AA000, GuestRegion.NtscJ));
            });
        WithTable("""
            MKW_REGION_RANGE(NtscU, 801A9000, 801A9FFF, -0xA0)
            MKW_REGION_RANGE(NtscU, 801A9FFF, 801AAFFF, -0xA0)
            """, _ => { }, expectFailure: true);
        WithTable("MKW_REGION_RANGE(Pal, 801A9000, 801A9FFF, +0x0)\n", _ => { }, expectFailure: true);
        WithTable("MKW_REGION_RANGE(1, 801A9000, 801A9FFF, +0x0)\n", _ => { }, expectFailure: true);
        WithTable("MKW_REGION_RANGE(NtscU, 801A9000, 801A9FFF, 0xA0)\n", _ => { }, expectFailure: true);
        WithTable("MKW_REGION_RANGE(NtscU, 801A9000, 801A9FFF, -0x80000000)\n", _ => { }, expectFailure: true);
        WithTable("// no rows\n", _ => { }, expectFailure: true);
    }

    [Fact]
    public void IndexForRegionRekeysEveryEntry()
    {
        var contract = new GuestAbiContract(0, 0, 0, 0, 0, 0, 0, 0, false, false, false, false, false, false,
            GuestCallBoundaryFlags.None, [0x80386664u]);
        var index = new RuntimeNativeIndex(
            [new RuntimeNativeRegistration(0x801A9E84, "OSCreateThread_HLE", "os.cpp", false, true)],
            [new RuntimeNativeAbiEntry(0x801A9E84, ["r3"], [])],
            [new RuntimeNativeEffectEntry(0x801A9E84, contract, true)]);
        WithTable("""
            MKW_REGION_RANGE(NtscU, 801A9000, 801A9FFF, -0xA0)
            MKW_REGION_RANGE(NtscU, 80386000, 80386FFF, -0x4360)
            """, table =>
            {
                var ntscU = index.ForRegion(table, GuestRegion.NtscU);
                Assert.Equal(0x801A9DE4u, ntscU.Registrations.Single().Address);
                Assert.Equal("OSCreateThread_HLE", ntscU.Registrations.Single().Symbol);
                Assert.Equal(0x801A9DE4u, ntscU.VoidStubAbis.Single().Address);
                Assert.Equal(0x801A9DE4u, ntscU.Effects.Single().Address);
                Assert.Equal([0x80382304u], ntscU.Effects.Single().Contract.DirectCallTargets);
                Assert.Equal(0x801A9E84u, index.ForRegion(table, GuestRegion.Pal).Registrations.Single().Address);
            });
    }

    // Every address the runtime registers must have a counterpart, or a regional build cannot place it.
    [Fact]
    public void RepositoryMapCoversEveryRuntimeRegistration()
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var sources = Path.Combine(root, "runtime", "src");
        var table = GuestAddressTable.Load(GuestAddressTable.DefaultPath(sources));
        var index = RuntimeNativeIndexBuilder.Build(sources);
        Assert.NotEmpty(index.Registrations);
        foreach (var region in Enum.GetValues<GuestRegion>())
            Assert.Equal(index.Registrations.Length, index.ForRegion(table, region).Registrations.Length);
    }

    // Every function both MAPs name uniquely, and every verified data address, must port exactly.
    [Theory]
    [InlineData(GuestRegion.NtscU, "mkwii-ntsc-u")]
    [InlineData(GuestRegion.NtscJ, "mkwii-ntsc-j")]
    public void RepositoryMapAgreesWithRegionMapsAndDataAnchors(GuestRegion region, string project)
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var table = GuestAddressTable.Load(GuestAddressTable.DefaultPath(Path.Combine(root, "runtime", "src")));
        var pal = UniqueNames(Path.Combine(root, "projects", "mkwii", "MAP.txt"));
        var target = UniqueNames(Path.Combine(root, "projects", project, "MAP.txt"));
        var compared = 0;
        foreach (var (name, address) in pal)
        {
            if (!target.TryGetValue(name, out var expected))
                continue;
            Assert.True(expected == table.Map(address, region), $"{name} 0x{address:X8} should port to 0x{expected:X8}");
            compared++;
        }
        Assert.True(compared > 15000, $"only {compared} functions compared");

        foreach (var line in File.ReadLines(Path.Combine(root, "projects", project, "data_addresses.txt")))
        {
            var parts = line.Split('#')[0].Split((char[]?)null, StringSplitOptions.RemoveEmptyEntries);
            if (parts.Length < 2 || parts[1] == "?" || parts[0].Length != 8)
                continue;
            var address = Convert.ToUInt32(parts[0], 16);
            Assert.Equal(Convert.ToUInt32(parts[1], 16), table.Map(address, region));
        }
    }

    // Guest functions the runtime calls directly must be real functions in every region.
    [Fact]
    public void RuntimeGuestCallsAreFunctionEntriesInEveryRegion()
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var table = GuestAddressTable.Load(GuestAddressTable.DefaultPath(Path.Combine(root, "runtime", "src")));
        var thirdParty = $"{Path.DirectorySeparatorChar}third_party{Path.DirectorySeparatorChar}";
        var calls = Directory.EnumerateFiles(Path.Combine(root, "runtime"), "*.*", SearchOption.AllDirectories)
            .Where(path => (path.EndsWith(".cpp", StringComparison.Ordinal) || path.EndsWith(".h", StringComparison.Ordinal)) &&
                           !path.Contains(thirdParty, StringComparison.Ordinal))
            .SelectMany(static path => Regex.Matches(File.ReadAllText(path), @"MKW_GUEST_(?:FUNC|NEXT)\(\s*([0-9A-Fa-f]{8})\s*\)")
                .Select(static match => Convert.ToUInt32(match.Groups[1].Value, 16)))
            .Distinct()
            .ToArray();
        Assert.NotEmpty(calls);
        var entries = new Dictionary<GuestRegion, HashSet<uint>>
        {
            [GuestRegion.Pal] = MapEntries(Path.Combine(root, "projects", "mkwii", "MAP.txt")),
            [GuestRegion.NtscU] = MapEntries(Path.Combine(root, "projects", "mkwii-ntsc-u", "MAP.txt")),
            [GuestRegion.NtscJ] = MapEntries(Path.Combine(root, "projects", "mkwii-ntsc-j", "MAP.txt")),
        };
        foreach (var pal in calls)
        {
            foreach (var (region, starts) in entries)
            {
                var address = table.Map(pal, region);
                Assert.True(starts.Contains(address), $"MKW_GUEST_FUNC/NEXT({pal:X8}) is not a {region} function entry (0x{address:X8})");
            }
        }
    }

    private static HashSet<uint> MapEntries(string mapPath) =>
        File.ReadLines(mapPath)
            .Select(static line => line.Trim())
            .Where(static line => line.Length >= 8 && !line.StartsWith('#'))
            .Select(static line => Convert.ToUInt32(line[..8], 16))
            .ToHashSet();

    private static Dictionary<string, uint> UniqueNames(string mapPath)
    {
        var entries = File.ReadLines(mapPath)
            .Select(static line => line.Trim())
            .Where(static line => line.Length > 9 && !line.StartsWith('#'))
            .Select(static line => (Address: Convert.ToUInt32(line[..8], 16), Name: line[9..].Trim()))
            .Where(static entry => !entry.Name.StartsWith("0x", StringComparison.Ordinal) &&
                                   !entry.Name.StartsWith("FUN_", StringComparison.Ordinal) &&
                                   !entry.Name.StartsWith("fn_", StringComparison.Ordinal))
            .ToArray();
        return entries.GroupBy(static entry => entry.Name)
            .Where(static group => group.Count() == 1)
            .ToDictionary(static group => group.Key, static group => group.Single().Address);
    }

    private static void WithTable(string contents, Action<GuestAddressTable> check, bool expectFailure = false)
    {
        var path = Path.Combine(Path.GetTempPath(), $"mkw-guest-versions-{Guid.NewGuid():N}.inc");
        File.WriteAllText(path, contents);
        try
        {
            if (expectFailure)
                Assert.Throws<InvalidDataException>(() => GuestAddressTable.Load(path));
            else
                check(GuestAddressTable.Load(path));
        }
        finally
        {
            File.Delete(path);
        }
    }
}
