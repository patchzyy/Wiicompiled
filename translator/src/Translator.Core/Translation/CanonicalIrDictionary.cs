using System.Collections;
using Translator.Core.Ir;

namespace Translator.Core.Translation;

/// <summary>
/// Fixed keys over canonical SSA decoded on demand without caching bodies.
/// Keys, Count and ContainsKey describe the address snapshot. Enumeration requires
/// the provider to retain every body in that snapshot for the duration of the read.
/// Concurrent readers require a thread-safe provider, such as CanonicalIrStore.
/// </summary>
public sealed class CanonicalIrDictionary : IReadOnlyDictionary<uint, IrFunction>
{
    private readonly ICanonicalIrProvider _provider;
    private readonly IReadOnlyList<uint> _addresses;
    private readonly HashSet<uint> _membership;

    public CanonicalIrDictionary(ICanonicalIrProvider provider, IEnumerable<uint> addresses)
    {
        ArgumentNullException.ThrowIfNull(provider);
        ArgumentNullException.ThrowIfNull(addresses);
        _provider = provider;
        var snapshot = addresses.ToArray();
        _membership = new HashSet<uint>(snapshot);
        if (_membership.Count != snapshot.Length)
            throw new ArgumentException("Canonical IR addresses must be unique.", nameof(addresses));
        _addresses = Array.AsReadOnly(snapshot);
    }

    public int Count => _addresses.Count;
    public IEnumerable<uint> Keys => _addresses;
    public IEnumerable<IrFunction> Values
    {
        get
        {
            foreach (var address in _addresses) yield return this[address];
        }
    }

    public IrFunction this[uint key] => TryGetValue(key, out var value)
        ? value
        : throw new KeyNotFoundException($"Missing canonical SSA for 0x{key:X8}.");

    public bool ContainsKey(uint key) => _membership.Contains(key);

    public bool TryGetValue(uint key, out IrFunction value)
    {
        if (_membership.Contains(key) && _provider.TryGet(key, out value)) return true;
        value = null!;
        return false;
    }

    public IEnumerator<KeyValuePair<uint, IrFunction>> GetEnumerator()
    {
        foreach (var address in _addresses)
            yield return new KeyValuePair<uint, IrFunction>(address, this[address]);
    }

    IEnumerator IEnumerable.GetEnumerator() => GetEnumerator();
}
