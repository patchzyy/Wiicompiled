using System.Text.Json;
using Translator.Core.IO;
using Translator.Core.Parsing.Kamek;

namespace Translator.Core.Mods;

/// <summary>
/// A base translation only really depends on the Kamek patch addresses that land inside a translated
/// function. This record stores those ranges plus which patched addresses fell inside them, so reuse
/// can key on that instead of retranslating on every Code.pul digest change.
/// </summary>
public sealed record BaseTranslationModAwareness(
    string Format,
    int FormatVersion,
    /// <summary>
    /// Echo of the translation identity, when the caller supplied one. The launcher's local build
    /// does not, so this is not what ties the record to a translation: living in the same
    /// <c>generated</c> directory under the same success-only provenance stamp is.
    /// </summary>
    string? TranslationIdentityHash,
    /// <summary>
    /// How many functions the translation that wrote this record emitted. A consumer that also reads
    /// the output metadata compares the two, so a record left behind by a different translation of
    /// the same workspace cannot be mistaken for this one's.
    /// </summary>
    int TranslatedFunctionCount,
    IReadOnlyList<BaseTranslationModAwarenessProfile> Profiles,
    /// <summary>
    /// Sorted, coalesced <c>[start, endExclusive)</c> guest ranges of every translated function,
    /// flattened to <c>start0, end0, start1, end1, ...</c>. Containment in one of these is the whole
    /// test for whether a patched address was consequential.
    /// </summary>
    IReadOnlyList<uint> TranslatedFunctionRanges)
{
    public const string CurrentFormat = "mkw-base-translation-mod-awareness";
    public const int CurrentFormatVersion = 1;

    /// <summary>The file name this record is always written under, beside the output metadata.</summary>
    public const string FileName = "base_translation_mod_awareness.json";

    public static BaseTranslationModAwareness Create(
        string? translationIdentityHash,
        int translatedFunctionCount,
        IEnumerable<(string Profile, string Region, string CodePulSha256, IReadOnlySet<uint> PatchedAddresses)> profiles,
        IReadOnlyDictionary<uint, uint> translatedFunctionEnds)
    {
        ArgumentNullException.ThrowIfNull(profiles);
        ArgumentNullException.ThrowIfNull(translatedFunctionEnds);

        var ranges = CoalesceRanges(translatedFunctionEnds);
        var recorded = profiles
            .Select(profile => new BaseTranslationModAwarenessProfile(
                profile.Profile,
                profile.Region,
                profile.CodePulSha256,
                Consequential(profile.PatchedAddresses, ranges)))
            .OrderBy(static profile => profile.Profile, StringComparer.OrdinalIgnoreCase)
            .ToArray();

        return new BaseTranslationModAwareness(CurrentFormat, CurrentFormatVersion, translationIdentityHash,
            translatedFunctionCount, recorded, ranges);
    }

    /// <summary>Whether a base translation carrying this record may be reused for
    /// <paramref name="codePulPath"/> under <paramref name="profileName"/>. Everything unexpected
    /// answers false with a reason, since a wrong "yes" silently mis-translates the product.</summary>
    public bool CoversCodePul(string profileName, string codePulPath, out string reason)
    {
        reason = string.Empty;
        if (Profiles.Count != 1)
        {
            // The recorded sets are per profile, but the decisions they describe were made from the
            // union of every profile the translation saw. Testing one profile's replacement against
            // that union is only equivalent when it is the only profile there is.
            reason = $"the base translation was produced from {Profiles.Count} mod profile(s), " +
                "so a single profile's Code.pul cannot be substituted in isolation";
            return false;
        }

        var recorded = Profiles[0];
        if (!string.Equals(recorded.Profile, profileName, StringComparison.OrdinalIgnoreCase))
        {
            reason = $"the base translation knows profile '{recorded.Profile}', not '{profileName}'";
            return false;
        }

        IReadOnlyList<uint> candidate;
        try
        {
            candidate = Consequential(PatchedAddresses(codePulPath, recorded.Region), TranslatedFunctionRanges);
        }
        catch (Exception ex) when (ex is IOException or InvalidDataException or ArgumentException)
        {
            reason = $"the candidate Code.pul could not be read as a Kamek patch set ({ex.Message})";
            return false;
        }

        if (candidate.Count != recorded.ConsequentialPatchedAddresses.Count)
        {
            reason = $"it patches {candidate.Count} translated function address(es), " +
                $"against the {recorded.ConsequentialPatchedAddresses.Count} this translation was built around";
            return false;
        }
        for (var i = 0; i < candidate.Count; i++)
        {
            if (candidate[i] == recorded.ConsequentialPatchedAddresses[i]) continue;
            reason = $"it moves a patch onto translated function address 0x{candidate[i]:X8}";
            return false;
        }
        return true;
    }

    /// <summary>The absolute Kamek command addresses one Code.pul applies in one region.</summary>
    public static IReadOnlySet<uint> PatchedAddresses(string codePulPath, string region)
    {
        var chunk = KamekPulFile.Load(codePulPath).SelectRegion(region);
        var patched = new HashSet<uint>();
        foreach (var command in chunk.Commands)
        {
            if (command.AddressIsAbsolute) patched.Add(command.Address);
        }
        return patched;
    }

    private static IReadOnlyList<uint> Consequential(IReadOnlySet<uint> patched, IReadOnlyList<uint> ranges)
    {
        var consequential = new List<uint>();
        foreach (var address in patched)
        {
            if (Contains(ranges, address)) consequential.Add(address);
        }
        consequential.Sort();
        return consequential;
    }

    /// <summary>Binary search over the flattened, non-overlapping, ascending range pairs.</summary>
    private static bool Contains(IReadOnlyList<uint> ranges, uint address)
    {
        var low = 0;
        var high = (ranges.Count / 2) - 1;
        while (low <= high)
        {
            var middle = low + ((high - low) / 2);
            var start = ranges[middle * 2];
            var end = ranges[(middle * 2) + 1];
            if (address < start) high = middle - 1;
            else if (address >= end) low = middle + 1;
            else return true;
        }
        return false;
    }

    private static uint[] CoalesceRanges(IReadOnlyDictionary<uint, uint> translatedFunctionEnds)
    {
        var ordered = translatedFunctionEnds
            .Where(static entry => entry.Value > entry.Key)
            .OrderBy(static entry => entry.Key)
            .ToArray();
        var flattened = new List<uint>(ordered.Length * 2);
        foreach (var (start, end) in ordered)
        {
            if (flattened.Count != 0 && start <= flattened[^1])
            {
                if (end > flattened[^1]) flattened[^1] = end;
                continue;
            }
            flattened.Add(start);
            flattened.Add(end);
        }
        return flattened.ToArray();
    }
}

/// <summary>One mod profile's contribution to a base translation's mod-patch awareness.</summary>
public sealed record BaseTranslationModAwarenessProfile(
    string Profile,
    string Region,
    string CodePulSha256,
    /// <summary>Ascending; the patched addresses that landed inside a translated function.</summary>
    IReadOnlyList<uint> ConsequentialPatchedAddresses);

public static class BaseTranslationModAwarenessFile
{
    private static readonly JsonSerializerOptions JsonOptions = JsonOutput.CompactCamelCase;

    public static BaseTranslationModAwareness Read(string path)
    {
        var awareness = JsonSerializer.Deserialize<BaseTranslationModAwareness>(TrackedFs.ReadAllText(path), JsonOptions)
            ?? throw new InvalidDataException($"Base translation mod-awareness record is empty: {path}");
        Validate(awareness, path);
        return awareness;
    }

    public static BaseTranslationModAwareness? TryRead(string path)
    {
        if (!TrackedFs.FileExists(path)) return null;
        try { return Read(path); }
        catch (Exception ex) when (ex is IOException or JsonException or InvalidDataException) { return null; }
    }

    public static bool WriteIfChangedAtomic(string path, BaseTranslationModAwareness awareness)
    {
        Validate(awareness, path);
        return JsonOutput.WriteIfChanged(path, awareness, JsonOptions);
    }

    private static void Validate(BaseTranslationModAwareness awareness, string source)
    {
        if (!string.Equals(awareness.Format, BaseTranslationModAwareness.CurrentFormat, StringComparison.Ordinal) ||
            awareness.FormatVersion != BaseTranslationModAwareness.CurrentFormatVersion)
        {
            throw new InvalidDataException(
                $"Unsupported base translation mod-awareness record in '{source}': " +
                $"{awareness.Format} v{awareness.FormatVersion}.");
        }
        if (awareness.TranslatedFunctionCount <= 0)
        {
            throw new InvalidDataException(
                $"Base translation mod-awareness record has no translated function count in '{source}'.");
        }
        if (awareness.TranslatedFunctionRanges.Count == 0 || (awareness.TranslatedFunctionRanges.Count % 2) != 0)
        {
            throw new InvalidDataException(
                $"Base translation mod-awareness record has a malformed range table in '{source}'.");
        }
        for (var i = 0; i < awareness.TranslatedFunctionRanges.Count; i += 2)
        {
            if (awareness.TranslatedFunctionRanges[i] >= awareness.TranslatedFunctionRanges[i + 1] ||
                (i > 0 && awareness.TranslatedFunctionRanges[i] < awareness.TranslatedFunctionRanges[i - 1]))
            {
                throw new InvalidDataException(
                    $"Base translation mod-awareness ranges are not ascending and disjoint in '{source}'.");
            }
        }
        foreach (var profile in awareness.Profiles)
        {
            if (string.IsNullOrWhiteSpace(profile.Profile) || profile.CodePulSha256.Length != 64)
            {
                throw new InvalidDataException(
                    $"Base translation mod-awareness record has an invalid profile in '{source}'.");
            }
            for (var i = 1; i < profile.ConsequentialPatchedAddresses.Count; i++)
            {
                if (profile.ConsequentialPatchedAddresses[i] <= profile.ConsequentialPatchedAddresses[i - 1])
                {
                    throw new InvalidDataException(
                        $"Base translation mod-awareness addresses are not ascending in '{source}'.");
                }
            }
        }
    }
}
