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
    public void MapsEachColumnAndRejectsUnknownAndDuplicateRows()
    {
        WithTable("""
            // comment
            MKW_GUEST_ADDRESS(801A9E84, 801A9DE4, 801A9DA4)  // OSCreateThread
            MKW_GUEST_ADDRESS(80386664, 80382304, 80385FE4)
            """, table =>
            {
                Assert.Equal(2, table.Count);
                Assert.Equal(0x801A9E84u, table.Map(0x801A9E84, GuestRegion.Pal));
                Assert.Equal(0x801A9DE4u, table.Map(0x801A9E84, GuestRegion.NtscU));
                Assert.Equal(0x80385FE4u, table.Map(0x80386664, GuestRegion.NtscJ));
                Assert.Throws<InvalidDataException>(() => table.Map(0x80000000, GuestRegion.Pal));
            });
        WithTable("""
            MKW_GUEST_ADDRESS(801A9E84, 801A9DE4, 801A9DA4)
            MKW_GUEST_ADDRESS(801a9e84, 801A9DE4, 801A9DA4)
            """, _ => { }, expectFailure: true);
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
            MKW_GUEST_ADDRESS(801A9E84, 801A9DE4, 801A9DA4)
            MKW_GUEST_ADDRESS(80386664, 80382304, 80385FE4)
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

    // Every address the runtime registers must have a row, or a regional build cannot place it.
    [Fact]
    public void RepositoryTableCoversEveryRuntimeRegistration()
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var sources = Path.Combine(root, "runtime", "src");
        var table = GuestAddressTable.Load(GuestAddressTable.DefaultPath(sources));
        var index = RuntimeNativeIndexBuilder.Build(sources);
        Assert.NotEmpty(index.Registrations);
        foreach (var region in Enum.GetValues<GuestRegion>())
            Assert.Equal(index.Registrations.Length, index.ForRegion(table, region).Registrations.Length);
    }

    // Distinct PAL identities must stay distinct and in the same order in every region.
    [Fact]
    public void RepositoryTableIsInjectiveAndOrderedPerRegion()
    {
        var root = ProjectPathsForTests.FindRepositoryRoot();
        var path = Path.Combine(root, "runtime", "include", "guest_abi", "guest_address_table.inc");
        var rows = File.ReadLines(path)
            .Where(static line => line.StartsWith("MKW_GUEST_ADDRESS(", StringComparison.Ordinal))
            .Select(static line => line["MKW_GUEST_ADDRESS(".Length..line.IndexOf(')')]
                .Split(',').Select(static value => Convert.ToUInt32(value.Trim(), 16)).ToArray())
            .ToArray();
        Assert.NotEmpty(rows);
        for (var column = 0; column < 3; column++)
        {
            for (var i = 1; i < rows.Length; i++)
                Assert.True(rows[i - 1][column] < rows[i][column],
                    $"column {column} row {i}: 0x{rows[i - 1][column]:X8} >= 0x{rows[i][column]:X8}");
        }
    }

    private static void WithTable(string contents, Action<GuestAddressTable> check, bool expectFailure = false)
    {
        var path = Path.Combine(Path.GetTempPath(), $"mkw-guest-table-{Guid.NewGuid():N}.inc");
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
