using System.Buffers.Binary;
using System.Collections;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using Translator.Core.Analysis;
using Translator.Core.Ir;
using Translator.Core.Loading;
using Translator.Core.Translation;
using Xunit;

namespace Translator.Tests;

public sealed class CanonicalIrDictionaryTests
{
    [Fact]
    public void KeysAndMembershipUseAnIndependentSnapshotWithoutDecoding()
    {
        var store = new CanonicalIrStore();
        store.Put(20, Function("second", new IrReturn(null)));
        store.Put(10, Function("first", new IrReturn(null)));
        store.Put(30, Function("outside", new IrReturn(null)));
        var provider = new CountingProvider(store);
        var addresses = new uint[] { 20, 10 };
        var view = new CanonicalIrDictionary(provider, addresses);
        addresses[0] = 0;

        Assert.Equal(2, view.Count);
        Assert.Equal(new uint[] { 20, 10 }, view.Keys);
        Assert.True(view.ContainsKey(10));
        Assert.False(view.ContainsKey(0));
        Assert.False(view.ContainsKey(30));
        Assert.False(view.TryGetValue(30, out var missing));
        Assert.Null(missing);
        Assert.Throws<KeyNotFoundException>(() => view[30]);
        Assert.Equal(0, provider.Reads);

        var first = view[10];
        Assert.NotSame(first, view[10]);
        Assert.Equal(2, provider.Reads);
        Assert.Equal("second", Assert.Single(view.Values.Take(1)).Name);
        Assert.Equal(3, provider.Reads);
        using var entries = view.GetEnumerator();
        Assert.Equal(3, provider.Reads);
        Assert.True(entries.MoveNext());
        Assert.Equal(20u, entries.Current.Key);
        Assert.Equal(4, provider.Reads);
        Assert.Equal(view.Keys, view.Select(static pair => pair.Key));
        Assert.Equal(new[] { "second", "first" }, view.Values.Select(static body => body.Name));
        Assert.Equal(view.Keys, ((IEnumerable)view).Cast<KeyValuePair<uint, IrFunction>>().Select(static pair => pair.Key));
    }

    [Fact]
    public void ReplacedBodiesAreReadAndMissingSnapshotBodiesViolateTheInvariant()
    {
        var store = new CanonicalIrStore();
        store.Put(10, Function("original", new IrReturn(null)));
        var view = new CanonicalIrDictionary(store, new uint[] { 10 });
        Assert.Equal("original", view[10].Name);
        store.Put(10, Function("replacement", new IrReturn(null)));
        Assert.Equal("replacement", view[10].Name);
        store.Remove(10);

        Assert.True(view.ContainsKey(10));
        Assert.Equal(new uint[] { 10 }, view.Keys);
        Assert.Equal(view.Keys.Count(), view.Count);
        var error = Assert.Throws<InvalidOperationException>(() => view.TryGetValue(10, out _));
        Assert.Contains("0x0000000A", error.Message, StringComparison.Ordinal);
        Assert.Throws<InvalidOperationException>(() => view[10]);
        Assert.Throws<InvalidOperationException>(() => view.Values.ToArray());
        Assert.Throws<InvalidOperationException>(() => view.ToArray());
    }

    [Fact]
    public void ConstructorRejectsNullInputsAndDuplicateKeys()
    {
        var store = new CanonicalIrStore();
        Assert.Throws<ArgumentNullException>(() => new CanonicalIrDictionary(null!, Array.Empty<uint>()));
        Assert.Throws<ArgumentNullException>(() => new CanonicalIrDictionary(store, null!));
        Assert.Throws<ArgumentException>(() => new CanonicalIrDictionary(store, new uint[] { 10, 20, 10 }));
        var empty = new CanonicalIrDictionary(store, Array.Empty<uint>());
        Assert.Empty(empty);
        Assert.Empty(empty.Keys);
        Assert.Empty(empty.Values);
    }

    [Fact]
    public void ConcurrentReadersEnumerateInSnapshotOrderAndDecodeIndependentBodies()
    {
        var store = new CanonicalIrStore();
        var addresses = Enumerable.Range(1, 16).Reverse().Select(static index => (uint)index).ToArray();
        foreach (var address in addresses) store.Put(address, Function($"f{address}", new IrReturn(null)));
        var provider = new CountingProvider(store);
        var view = new CanonicalIrDictionary(provider, addresses);
        var bodies = new IrFunction[32];

        Parallel.For(0, bodies.Length, new ParallelOptions { MaxDegreeOfParallelism = 4 }, index =>
        {
            Assert.Equal(addresses, view.Keys);
            Assert.All(addresses, address => Assert.True(view.ContainsKey(address)));
            Assert.False(view.TryGetValue(0, out _));
            Assert.Equal(addresses, view.Select(static pair => pair.Key));
            Assert.Equal(addresses.Select(static address => $"f{address}"), view.Values.Select(static body => body.Name));
            bodies[index] = view[1];
        });

        Assert.Equal(bodies.Length * (2 * addresses.Length + 1), provider.Reads);
        for (var index = 1; index < bodies.Length; ++index) Assert.NotSame(bodies[0], bodies[index]);
    }

    [Fact]
    public void LazyCanonicalBodyPreservesSerializedIrAndLoweredSource()
    {
        var memory = new byte[8];
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(0, 4), 0x38630001u); // addi r3,r3,1
        BinaryPrimitives.WriteUInt32BigEndian(memory.AsSpan(4, 4), 0x4E800020u); // blr
        var image = new ProgramImage(memory,
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length),
            AddressRange.FromStartAndSize(MemoryLayout.RamBase, (uint)memory.Length), default, "bounded-ir-test");
        var translator = new FunctionTranslator(image);
        var options = TranslationOptions.Default with { PreferredName = "bounded_ir_test" };
        var canonical = translator.Discover(MemoryLayout.RamBase, options).Ssa.Function;
        var store = new CanonicalIrStore();
        store.Put(MemoryLayout.RamBase, canonical);
        var view = new CanonicalIrDictionary(store, new[] { MemoryLayout.RamBase });
        var buffered = translator.LowerCanonical(MemoryLayout.RamBase, canonical, options);
        var lazy = translator.LowerCanonical(MemoryLayout.RamBase, view[MemoryLayout.RamBase], options);

        Assert.Equal(buffered.CxxCode, lazy.CxxCode);
        Assert.Equal(JsonSerializer.Serialize(canonical), JsonSerializer.Serialize(view[MemoryLayout.RamBase]));
    }

    [Theory]
    [InlineData(1)]
    [InlineData(2)]
    [InlineData(-1)]
    public void LazyAnalysisMatchesBufferedContractsAndComponents(int parallelism)
    {
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80003000] = Function("leaf",
                new IrAssign("r7", IrValue.Register("r6")),
                new IrCall("lr", "0x80004000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80001000] = Function("first",
                new IrAssign("r5", IrValue.Register("r3")),
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80002000] = Function("second",
                new IrAssign("r6", IrValue.Register("r4")),
                new IrCall("lr", "0x80001000", Array.Empty<IrValue>()),
                new IrCall("lr", "0x80003000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80005000] = Function("self",
                new IrCall("lr", "0x80005000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80006000] = Function("unknown",
                new IrCall("lr", "0x80007000", Array.Empty<IrValue>()),
                new IrIndirectCall("lr", IrValue.Register("ctr"), Array.Empty<IrValue>()), new IrReturn(null))
        };
        var external = new Dictionary<uint, GuestAbiContract>
        {
            [0x80004000] = GuestAbiContractAnalyzer.Analyze(Function("external",
                new IrAssign("r8", IrValue.Register("r9")), new IrReturn(null)))
        };
        var store = new CanonicalIrStore();
        foreach (var pair in functions) store.Put(pair.Key, pair.Value);
        var view = new CanonicalIrDictionary(store, functions.Keys.Reverse());
        var buffered = GuestAbiInterproceduralAnalyzer.Analyze(functions, external);
        var lazy = GuestAbiInterproceduralAnalyzer.Analyze(view, external,
            retainBodies: false, maxDegreeOfParallelism: parallelism);

        Assert.Equal(JsonSerializer.Serialize(buffered), JsonSerializer.Serialize(lazy));
        Assert.Equal(functions.Keys.OrderBy(static address => address), lazy.Contracts.Keys);
        Assert.Contains(lazy.StronglyConnectedComponents,
            component => component.SequenceEqual(new uint[] { 0x80001000, 0x80002000 }));
        Assert.Contains(lazy.StronglyConnectedComponents,
            component => component.SequenceEqual(new uint[] { 0x80005000 }));
        Assert.True(lazy.Contracts[0x80006000].HasFullSynchronizationFence);
        Assert.NotEqual(0u, lazy.Contracts[0x80003000].GprReadBeforeWriteMask & (1u << 9));
    }

    [Fact]
    public void LocalContractsSaveOneLazyBodyReadWithoutChangingTheResult()
    {
        var store = new CanonicalIrStore();
        var body = Function("leaf", new IrReturn(null));
        store.Put(10, body);
        var provider = new CountingProvider(store);
        var view = new CanonicalIrDictionary(provider, new uint[] { 10 });

        var buffered = GuestAbiInterproceduralAnalyzer.Analyze(view);
        Assert.Equal(1, provider.Reads);
        var lazy = GuestAbiInterproceduralAnalyzer.Analyze(view, retainBodies: false, maxDegreeOfParallelism: 1);
        Assert.Equal(4, provider.Reads);
        Assert.Equal(JsonSerializer.Serialize(buffered), JsonSerializer.Serialize(lazy));

        var localContracts = new Dictionary<uint, GuestAbiContract> { [10] = GuestAbiContractAnalyzer.Analyze(body) };
        var seeded = GuestAbiInterproceduralAnalyzer.Analyze(view, retainBodies: false,
            maxDegreeOfParallelism: 1, localContracts: localContracts);
        Assert.Equal(6, provider.Reads);
        Assert.Equal(JsonSerializer.Serialize(lazy), JsonSerializer.Serialize(seeded));
    }

    [Theory]
    [InlineData(false)]
    [InlineData(true)]
    public void LocalContractsMustMatchFunctionAddressesBeforeDecoding(bool includeUnrelatedAddress)
    {
        var provider = new CountingProvider(new CanonicalIrStore());
        var view = new CanonicalIrDictionary(provider, new uint[] { 10 });
        var localContracts = new Dictionary<uint, GuestAbiContract>();
        if (includeUnrelatedAddress)
            localContracts[20] = GuestAbiContractAnalyzer.Analyze(Function("other", new IrReturn(null)));

        var error = Assert.Throws<ArgumentException>(() => GuestAbiInterproceduralAnalyzer.Analyze(
            view, retainBodies: false, maxDegreeOfParallelism: 1, localContracts: localContracts));
        Assert.Equal("localContracts", error.ParamName);
        Assert.Equal(0, provider.Reads);
    }

    [Fact]
    public void LazyAnalysisRevisitsCallersOfInlineThunksAfterTheirCalleesChange()
    {
        var range = GuestSaveRestoreThunks.Current.SaveGpr;
        Assert.NotNull(range);
        var thunk = range!.BaseAddress;
        var functions = new Dictionary<uint, IrFunction>
        {
            [0x80001000] = Function("caller",
                new IrCall("lr", $"0x{thunk:X8}", Array.Empty<IrValue>()), new IrReturn(null)),
            [thunk] = Function("thunk",
                new IrCall("lr", "0x80002000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80002000] = Function("middle",
                new IrCall("lr", "0x80003000", Array.Empty<IrValue>()), new IrReturn(null)),
            [0x80003000] = Function("leaf",
                new IrAssign("r8", IrValue.Register("r9")), new IrReturn(null))
        };
        var store = new CanonicalIrStore();
        foreach (var pair in functions) store.Put(pair.Key, pair.Value);
        var buffered = GuestAbiInterproceduralAnalyzer.Analyze(functions);
        var lazy = GuestAbiInterproceduralAnalyzer.Analyze(new CanonicalIrDictionary(store, functions.Keys),
            retainBodies: false, maxDegreeOfParallelism: 2,
            localContracts: functions.ToDictionary(static pair => pair.Key,
                static pair => GuestAbiContractAnalyzer.Analyze(pair.Value)));

        Assert.Equal(JsonSerializer.Serialize(buffered), JsonSerializer.Serialize(lazy));
        Assert.DoesNotContain(thunk, lazy.Contracts[0x80001000].DirectCallTargets);
        Assert.Equal(1u << 9, lazy.Contracts[0x80001000].GprReadBeforeWriteMask);
        Assert.NotEqual(0u, lazy.Contracts[0x80001000].GprPossibleWriteMask & (1u << 8));
    }

    [Theory]
    [InlineData(true, 0)]
    [InlineData(false, 0)]
    [InlineData(true, -2)]
    [InlineData(false, -2)]
    public void InvalidParallelismFailsBeforeDecoding(bool retainBodies, int parallelism)
    {
        var provider = new CountingProvider(new CanonicalIrStore());
        var view = new CanonicalIrDictionary(provider, new uint[] { 10 });
        Assert.Throws<ArgumentOutOfRangeException>(() => GuestAbiInterproceduralAnalyzer.Analyze(
            view, retainBodies: retainBodies, maxDegreeOfParallelism: parallelism));
        Assert.Equal(0, provider.Reads);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public void EmptyAnalysisPreservesEmptyResults(bool retainBodies)
    {
        var view = new CanonicalIrDictionary(new CanonicalIrStore(), Array.Empty<uint>());
        var result = GuestAbiInterproceduralAnalyzer.Analyze(view, retainBodies: retainBodies, maxDegreeOfParallelism: 1);
        Assert.Empty(result.Contracts);
        Assert.Empty(result.StronglyConnectedComponents);
    }

    [Fact]
    public void SingleWorkerLimitAppliesToLazyProviderReadsInEveryAnalysisPass()
    {
        var store = new CanonicalIrStore();
        var addresses = Enumerable.Range(1, 32).Select(static index => (uint)index).ToArray();
        foreach (var address in addresses) store.Put(address, Function($"f{address}", new IrReturn(null)));
        var provider = new CountingProvider(store, requireSerial: true);
        var result = GuestAbiInterproceduralAnalyzer.Analyze(new CanonicalIrDictionary(provider, addresses),
            retainBodies: false, maxDegreeOfParallelism: 1);
        Assert.Equal(addresses.Length, result.Contracts.Count);
        Assert.Equal(3 * addresses.Length, provider.Reads);
    }

    private static IrFunction Function(string name, params IrInstruction[] instructions) =>
        new(name, "entry", new[] { new IrBasicBlock("entry", instructions) });

    private sealed class CountingProvider(ICanonicalIrProvider inner, bool requireSerial = false) : ICanonicalIrProvider
    {
        private int _reads;
        private int _active;
        public int Reads => Volatile.Read(ref _reads);

        public bool TryGet(uint entryPoint, out IrFunction function)
        {
            var active = Interlocked.Increment(ref _active);
            try
            {
                if (requireSerial) Assert.Equal(1, active);
                Interlocked.Increment(ref _reads);
                return inner.TryGet(entryPoint, out function);
            }
            finally
            {
                Interlocked.Decrement(ref _active);
            }
        }
    }
}
