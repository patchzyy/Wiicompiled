namespace Translator.Core.CodeGen;

/// <summary>
/// Selects the runtime helper family for guest memory accesses. Checked helpers resolve through the 1 MiB page
/// table each access; flat helpers use the 4 GiB guest reservation, so an access is just a byte swap with
/// MMIO/EFB/executable-write interception moved to host page protections. Both families behave identically.
/// </summary>
public sealed partial class CxxLinearCodeGenerator
{
    private enum FlatMemoryHelperFamily
    {
        Read,
        Write,
        WriteRam
    }

    // Index order is 8, 16, 32, 64 bits. Keep the literal spellings in one
    // table: the runtime deliberately exposes separate guarded and proven-RAM
    // write families, while Bits(sizeBytes) supplies the same normalization at
    // every call site.
    private static readonly string[] FlatHelpers =
    {
        "MemoryInline::FlatRead8", "MemoryInline::FlatRead16",
        "MemoryInline::FlatRead32", "MemoryInline::FlatRead64",
        "MemoryInline::FlatReadFloat8", "MemoryInline::FlatReadFloat16",
        "MemoryInline::FlatReadFloat32", "MemoryInline::FlatReadFloat64",
        "MemoryInline::FlatWrite8", "MemoryInline::FlatWrite16",
        "MemoryInline::FlatWrite32", "MemoryInline::FlatWrite64",
        "MemoryInline::FlatWriteFloat8", "MemoryInline::FlatWriteFloat16",
        "MemoryInline::FlatWriteFloat32", "MemoryInline::FlatWriteFloat64",
        "MemoryInline::FlatWriteRam8", "MemoryInline::FlatWriteRam16",
        "MemoryInline::FlatWriteRam32", "MemoryInline::FlatWriteRam64",
        "MemoryInline::FlatWriteRamFloat8", "MemoryInline::FlatWriteRamFloat16",
        "MemoryInline::FlatWriteRamFloat32", "MemoryInline::FlatWriteRamFloat64"
    };

    private static int FlatHelperIndex(int bits) => bits switch
    {
        8 => 0,
        16 => 1,
        64 => 3,
        _ => 2
    };

    private static string FlatHelper(FlatMemoryHelperFamily family, int bits, bool isFloat)
    {
        var familyIndex = (int)family;
        if ((uint)familyIndex >= 3u)
            throw new ArgumentOutOfRangeException(nameof(family), family, null);
        return FlatHelpers[((familyIndex * 2) + (isFloat ? 1 : 0)) * 4 + FlatHelperIndex(bits)];
    }

    /// <summary>
    /// Helper spellings are a closed set of widths and families. Selection is
    /// table-driven but remains allocation-free at each emitted access.
    /// </summary>
    private static string FlatReadHelper(int bits, bool isFloat) =>
        FlatHelper(FlatMemoryHelperFamily.Read, bits, isFloat);

    private static string FlatWriteHelper(int bits, bool isFloat) =>
        FlatHelper(FlatMemoryHelperFamily.Write, bits, isFloat);

    /// <summary>
    /// The exact predicate <c>MemoryInline::FlatWriteNeedsPolicy</c> uses:
    /// 0xCC000000..0xCDFFFFFF. Kept in sync by construction - the two must never
    /// disagree, because that is what makes the check-free family sound.
    /// </summary>
    private static bool FlatWriteNeedsPolicy(uint address) =>
        (address & 0xFE000000u) == 0xCC000000u;

    /// <summary>
    /// A translate-time-constant effective address whose complete span sits
    /// outside the MMIO policy window, and which therefore provably takes the
    /// plain-store arm of every <c>FlatWrite*</c> helper.
    /// </summary>
    private static bool IsProvenRamStore(uint address, int sizeBytes)
    {
        if (sizeBytes <= 0 || sizeBytes > 8) return false;
        var last = unchecked(address + (uint)(sizeBytes - 1));
        if (last < address) return false; // wraps past the end of the address space
        return !FlatWriteNeedsPolicy(address) && !FlatWriteNeedsPolicy(last);
    }

    private static string GuestLoadHelper(int sizeBytes, bool isFloat) =>
        FlatReadHelper(Bits(sizeBytes), isFloat);

    private static string CheckedMmioLoadHelper(int sizeBytes) => Bits(sizeBytes) switch
    {
        8 => "Memory::Read8",
        16 => "Memory::Read16",
        64 => "Memory::Read64",
        _ => "Memory::Read32"
    };

    private static string GuestStoreHelper(int sizeBytes, bool isFloat) =>
        FlatWriteHelper(Bits(sizeBytes), isFloat);

    /// <summary>
    /// Stack slots are ordinary guest memory; the checked variants only skipped
    /// the executable-write guard because a stack frame can never hold code. The
    /// flat path has no per-access tables at all, so both collapse to one helper.
    /// </summary>
    private static string StackLoadHelper(int sizeBytes, bool isFloat) =>
        FlatReadHelper(Bits(sizeBytes), isFloat);

    /// <summary>
    /// Check-free flat store family. Selected only where the caller has already
    /// proven the effective address is ordinary guest RAM; see the contract on
    /// <c>MemoryInline::FlatWriteRam*</c> in runtime/include/memory_access.h.
    /// </summary>
    private static string FlatWriteRamHelper(int bits, bool isFloat) =>
        FlatHelper(FlatMemoryHelperFamily.WriteRam, bits, isFloat);

    private static string StackStoreHelper(int sizeBytes, bool isFloat)
    {
        // A stack slot is `r1 + constant` with the whole chain back to r1
        // visible in this body (StackAddressFacts). Guest stack frames
        // live in MEM1/MEM2 and can never be MMIO, so the per-store MMIO
        // policy test is provably dead here - and 45% of all emitted flat
        // stores have this shape.
        return FlatWriteRamHelper(Bits(sizeBytes), isFloat);
    }
}
