#pragma once

#include "guest_abi/exec_guard.h"
#include "guest_abi/flat_memory.h"
#include "guest_abi/memory.h"

#include <cstddef>
#include <cstdint>
#include <cstring>

// Hooks for GX HLE FIFO handling
extern "C" {
    void GX_HLE_FIFO_WriteFloat(float val);
    void GX_HLE_FIFO_Write32(uint32_t val);
    void GX_HLE_FIFO_Write16(uint16_t val);
    void GX_HLE_FIFO_Write8(uint8_t val);
    void GX_HLE_FIFO_WriteBurst(const uint8_t* data, uint32_t sizeBytes);
}

namespace MemoryInline {
#if defined(_WIN32)
#define MKW_MEMORY_FORCE_INLINE __forceinline
#define MKW_MEMORY_NO_INLINE __declspec(noinline)
#else
// See runtime/include/isa/ppc_isa_config.h for why non-Windows Clang needs the GNU-attribute
// spellings instead of the MS-extension keywords.
#define MKW_MEMORY_FORCE_INLINE __attribute__((always_inline)) inline
#define MKW_MEMORY_NO_INLINE __attribute__((noinline))
#endif
#define MKW_MEMORY_COLD __attribute__((cold))
inline constexpr uint32_t kPageShift = 20;
inline constexpr uint32_t kPageSize = 1u << kPageShift;
inline constexpr uint32_t kPageMask = kPageSize - 1u;
inline constexpr uint32_t kPageCount = 1u << (32 - kPageShift);
inline constexpr uint32_t kMaxFastScalarSize = 8;
inline constexpr uint32_t kWritableSubPageShift = RecompMod::kExecutableWriteGuardPageShift;
inline constexpr uint32_t kWritableSubPageSize = 1u << kWritableSubPageShift;
inline constexpr uint32_t kWritableSubPagesPerPage = kPageSize / kWritableSubPageSize;

struct PageEntry {
    uint8_t* base = nullptr;
    uint32_t limit = 0;
};

extern PageEntry g_pageTable[kPageCount];
// Encoded (host page base - guest page base) + 1 for full pages whose next
// page is contiguous. This permits any native access up to 8 bytes without a
// per-access mask/limit check. Zero retains the general PageEntry fallback.
extern uintptr_t g_fullPageBias[kPageCount];
// Runtime-active readable biases. Deferred-read pages clear these entries once
// instead of paying a mode branch on every translated read.
extern uintptr_t g_fullReadablePageBias[kPageCount];
// Same encoding, but only for pages proven not to contain executable bytes.
// Executable-range registration invalidates entries before guest execution.
extern uintptr_t g_fullWritablePageBias[kPageCount];

// Allocated only for mapped 1 MiB pages that contain both executable and data
// 4 KiB pages. Entries use the same encoded host bias as the coarse table.
// Exact executable bits remain authoritative and are checked at lookup time,
// including both sides of a cross-4-KiB access.
struct SparseWritablePageTable {
    uintptr_t encodedBias[kWritableSubPagesPerPage]{};
};
extern const SparseWritablePageTable* g_sparseWritablePageTables[kPageCount];

// Nonzero while any registered deferred read overlaps the page. Small
// mappings (e.g. the 16 KiB locked cache) have no coarse bias entry, so a
// zero readable bias alone cannot distinguish "deferred content pending"
// from "small but plain memory"; range resolution needs the explicit flag.
extern uint8_t g_deferredReadCoveredPages[kPageCount];

MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint8_t Read8Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint16_t Read16Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint32_t Read32Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD uint64_t Read64Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD float ReadFloat32Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD double ReadFloat64Slow(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write8Slow(uint32_t addr, uint8_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write16Slow(uint32_t addr, uint16_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write32Slow(uint32_t addr, uint32_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void Write64Slow(uint32_t addr, uint64_t val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteFloat32Slow(uint32_t addr, double val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteFloat64Slow(uint32_t addr, double val);
template <typename T>
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD T ReadResolvedFallback(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD float ReadResolvedFallbackFloat32(uint32_t addr);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD double ReadResolvedFallbackFloat64(uint32_t addr);
template <typename T>
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallback(uint32_t addr, T value);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallbackFloat32(uint32_t addr, double val);
MKW_MEMORY_NO_INLINE MKW_MEMORY_COLD void WriteResolvedFallbackFloat64(uint32_t addr, double val);
bool ResolveDeferredReads(uint32_t addr, size_t length);

constexpr bool IsMmioAddress(uint32_t addr) {
    return addr >= 0xCC000000u && addr < 0xCE000000u;
}

constexpr bool IsGpuFifoAddress(uint32_t addr) {
    return addr >= 0xCC008000u && addr < 0xCC008100u;
}

// Page protections can't cover this: an MMIO write must reach GX HLE with its value or be
// reported, and a fault record can't carry the value, so this mask/compare sits in front of
// every flat store instead.
MKW_MEMORY_FORCE_INLINE bool FlatWriteNeedsPolicy(uint32_t address) {
    return (address & 0xFE000000u) == 0xCC000000u;  // 0xCC000000..0xCDFFFFFF
}

// Gekko stfs conversion is a bit-level narrowing operation.  In particular it
// does not behave like a host double-to-float cast for values which were left
// in double precision, and it has hardware-tested handling for tiny values.
MKW_MEMORY_FORCE_INLINE uint32_t ConvertPpcDoubleToSingleBits(double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    const uint32_t exponent = static_cast<uint32_t>((bits >> 52) & 0x7FFu);
    // The subnormal-single arm applies to exactly the exponents 874..896; every
    // other exponent takes the plain sign/exponent/fraction narrowing below.
    // Inside that window the exponent field is nonzero, so the magnitude cannot
    // be zero and needs no separate test - the zero case (exponent 0) reaches
    // the narrowing exactly as it did when the two arms shared that test.
    if (exponent - 874u <= 22u) [[unlikely]]
    {
        uint32_t narrowed = static_cast<uint32_t>(
            0x80000000ULL | ((bits & 0x000FFFFFFFFFFFFFULL) >> 21));
        narrowed >>= (905u - exponent);
        narrowed |= static_cast<uint32_t>((bits >> 32) & 0x80000000ULL);
        return narrowed;
    }

    // Results below the documented conversion range are architecturally
    // undefined; this is the behavior measured on Gekko/Broadway hardware.
    return static_cast<uint32_t>(
        ((bits >> 32) & 0xC0000000ULL) | ((bits >> 29) & 0x3FFFFFFFULL));
}

MKW_MEMORY_FORCE_INLINE float PpcSingleBitsToFloat(uint32_t bits) {
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MKW_MEMORY_FORCE_INLINE bool TryGetPointerFast(uint32_t address, size_t length, uint8_t*& pointer) {
    const uint32_t page = address >> kPageShift;
    if (length <= 8) {
        const uintptr_t encodedBias = g_fullPageBias[page];
        if (encodedBias != 0) {
            pointer = reinterpret_cast<uint8_t*>((encodedBias - 1u) + address);
            return true;
        }
    }
    const uint32_t offset = address & kPageMask;
    const auto& entry = g_pageTable[page];
    if (!entry.base || offset + length > entry.limit) {
        pointer = nullptr;
        return false;
    }
    pointer = entry.base + offset;
    return true;
}

inline uint8_t* GetPointerFast(uint32_t address, size_t length) {
    uint8_t* pointer = nullptr;
    return TryGetPointerFast(address, length, pointer) ? pointer : nullptr;
}

MKW_MEMORY_FORCE_INLINE bool TryGetWritablePointerFast(
    uint32_t address, size_t length, uint8_t*& pointer) {
    // The sparse-table hit below distinguishes itself from the small-mapping
    // hit through `pointer`, so the out-parameter must start null regardless
    // of what the caller passed in. (A caller handing in an uninitialized
    // pointer used to turn every sparse hit into a write through stack
    // garbage - random host memory corruption.)
    pointer = nullptr;
    if (length == 0 || length > 8 || address > UINT32_MAX - (length - 1))
        return false;
    const uint32_t coarsePage = address >> kPageShift;
    uintptr_t encodedBias = g_fullWritablePageBias[coarsePage];
    const uint32_t endAddress = address + static_cast<uint32_t>(length - 1);
    const uint32_t firstExactPage = address >> kWritableSubPageShift;
    const uint32_t lastExactPage = endAddress >> kWritableSubPageShift;
    if (encodedBias != 0 && (endAddress >> kPageShift) != coarsePage &&
        RecompMod::g_executableWriteGuardPages[lastExactPage].load(
            std::memory_order_relaxed) != 0) {
        return false;
    }
    if (encodedBias == 0) {
        const auto* subTable = g_sparseWritablePageTables[coarsePage];
        if (subTable != nullptr) {
            encodedBias = subTable->encodedBias[
                (address & kPageMask) >> kWritableSubPageShift];
            if (encodedBias == 0)
                return false;
        } else {
            // Small mappings such as Broadway's 16 KiB locked cache cannot
            // populate the full-1-MiB bias table. Keep them native by proving
            // the exact access against the ordinary page entry, then applying
            // the same executable-write policy as a sparse-table hit.
            const uint32_t offset = address & kPageMask;
            const auto& entry = g_pageTable[coarsePage];
            if (!entry.base || offset + length > entry.limit)
                return false;
            pointer = entry.base + offset;
        }

        // The exact 4 KiB guard is the final authority. Checking it on every
        // checked/sparse hit also makes later executable-range registration
        // safe when a prebuilt table still contains the old mapped bias.
        if (RecompMod::g_executableWriteGuardPages[firstExactPage].load(
                std::memory_order_relaxed) != 0 ||
            (lastExactPage != firstExactPage &&
             RecompMod::g_executableWriteGuardPages[lastExactPage].load(
                 std::memory_order_relaxed) != 0)) {
            return false;
        }

        if (pointer != nullptr)
            return true;
    }
    if (encodedBias == 0)
        return false;
    pointer = reinterpret_cast<uint8_t*>((encodedBias - 1u) + address);
    return true;
}

// Flat form: guest_flat_memory.h's page protections already answer mapped/non-deferred/non-executable, so resolving is pure address arithmetic.
// Two checks stay inline: a wrapped guest address can't survive 64-bit `host + rangeOffset`, and an MMIO write's value isn't recoverable from a
// fault record, so a write touching that window must resolve null and fall back to Memory::Write*. Checking both range endpoints is a complete
// proof since length <= kPageSize (1 MiB) can't straddle the 32 MiB MMIO window.
MKW_MEMORY_FORCE_INLINE uint8_t* ResolveRangeHost(uint32_t base, int32_t minOffset, uint32_t length,
                                                 bool needsRead, bool needsWrite) {
    (void)needsRead;
    const uint32_t guestStart = base + static_cast<uint32_t>(minOffset);
    if (length == 0 || length > kPageSize || guestStart > UINT32_MAX - (length - 1)) return nullptr;
    if (GuestFlat::RequiresCheckedAccess()) {
        // A host page can cover multiple independently-special Wii pages.
        // Returning null keeps resolved accesses on the checked Memory::*
        // path, which materializes deferred reads and applies write policy.
        (void)needsWrite;
        return nullptr;
    }
    if (needsWrite &&
        (FlatWriteNeedsPolicy(guestStart) || FlatWriteNeedsPolicy(guestStart + (length - 1))))
        [[unlikely]] return nullptr;
    return MKW_FLAT_GUEST_BASE + guestStart;
}

// Guest-address byte order. Distinct from isa/big_endian.h, which is the
// host-pointer codec; do not "unify" them.
inline uint16_t ByteSwap16(uint16_t value) {
    return __builtin_bswap16(value);
}

inline uint32_t ByteSwap32(uint32_t value) {
    return __builtin_bswap32(value);
}

inline uint64_t ByteSwap64(uint64_t value) {
    return __builtin_bswap64(value);
}

template <typename T>
inline T MaybeByteSwap(T value) {
    if constexpr (sizeof(T) == 1) {
        return value;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(ByteSwap16(static_cast<uint16_t>(value)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(ByteSwap32(static_cast<uint32_t>(value)));
    } else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(ByteSwap64(static_cast<uint64_t>(value)));
    } else {
        return value;
    }
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool ReadResolvedScalar(uint8_t* host, uint32_t rangeOffset, T& outValue) {
    if (!host) return false;
    if constexpr (sizeof(T) == 1) {
        outValue = host[rangeOffset];
    } else {
        T value = 0;
        std::memcpy(&value, host + rangeOffset, sizeof(T));
        outValue = MaybeByteSwap(value);
    }
    return true;
}

struct ResolvedLoadPair {
    uint32_t first = 0;
    uint32_t second = 0;
    bool valid = false;
};

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair16(
    uint8_t* host, uint32_t rangeOffset) {
    uint32_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed)) return {};
    return {packed >> 16, packed & 0xFFFFu, true};
}

MKW_MEMORY_FORCE_INLINE ResolvedLoadPair ReadResolvedPair32(uint8_t* host, uint32_t rangeOffset) {
    uint64_t packed = 0;
    if (!ReadResolvedScalar(host, rangeOffset, packed)) return {};
    return {static_cast<uint32_t>(packed >> 32), static_cast<uint32_t>(packed), true};
}

template <typename Packed>
MKW_MEMORY_FORCE_INLINE bool WriteResolvedPairFast(
    uint8_t* host, uint32_t rangeOffset, Packed packed) {
    if (!host) return false;
    const Packed swapped = MaybeByteSwap(packed);
    std::memcpy(host + rangeOffset, &swapped, sizeof(swapped));
    return true;
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair16(
    uint8_t* host, uint32_t rangeOffset, uint32_t packed) {
    return WriteResolvedPairFast(host, rangeOffset, packed);
}

MKW_MEMORY_FORCE_INLINE bool WriteResolvedPair32(
    uint8_t* host, uint32_t rangeOffset, uint64_t packed) {
    return WriteResolvedPairFast(host, rangeOffset, packed);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool WriteResolvedScalar(uint8_t* host, uint32_t rangeOffset, T value) {
    if (!host) return false;
    if constexpr (sizeof(T) == 1) {
        host[rangeOffset] = static_cast<uint8_t>(value);
    } else {
        const T swapped = MaybeByteSwap(value);
        std::memcpy(host + rangeOffset, &swapped, sizeof(T));
    }
    return true;
}

template <typename T>
inline bool TryReadMappedScalar(uint32_t address, T& outValue) {
    uint8_t* ptr = nullptr;
    if (TryGetPointerFast(address, sizeof(T), ptr)) {
        if constexpr (sizeof(T) == 1) {
            outValue = *ptr;
        } else {
            T value = 0;
            std::memcpy(&value, ptr, sizeof(T));
            outValue = MaybeByteSwap(value);
        }
        return true;
    }
    return false;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool TryReadGuestScalar(uint32_t address, T& outValue) {
    const uintptr_t encodedBias = g_fullReadablePageBias[address >> kPageShift];
    if (encodedBias == 0) [[unlikely]]
        return false;
    auto* ptr = reinterpret_cast<uint8_t*>((encodedBias - 1u) + address);
    if constexpr (sizeof(T) == 1) {
        outValue = *ptr;
    } else {
        T value = 0;
        std::memcpy(&value, ptr, sizeof(T));
        outValue = MaybeByteSwap(value);
    }
    return true;
}

template <typename T>
MKW_MEMORY_FORCE_INLINE bool TryWriteGuestScalar(uint32_t address, T value) {
    static_assert(sizeof(T) >= 1 && sizeof(T) <= kMaxFastScalarSize);
    // A mixed executable/data 1 MiB page zeroes the coarse writable bias even though most of
    // its 4 KiB sub-pages are plain data; MKW's THP buffers share such a page with .text, which
    // used to force ~15% of total CPU through the cold path. The sparse sub-page tier below
    // keeps those stores native while the exact 4 KiB executable guards stay authoritative.
    if (address > UINT32_MAX - static_cast<uint32_t>(sizeof(T) - 1u)) [[unlikely]]
        return false;
    const uint32_t coarsePage = address >> kPageShift;
    const uint32_t endAddress = address + static_cast<uint32_t>(sizeof(T) - 1u);
    if ((endAddress >> kPageShift) != coarsePage) [[unlikely]]
        return false;
    const uintptr_t encodedBias = g_fullWritablePageBias[coarsePage];
    uint8_t* ptr = nullptr;
    if (encodedBias != 0) {
        ptr = reinterpret_cast<uint8_t*>((encodedBias - 1u) + address);
    } else if (!TryGetWritablePointerFast(address, sizeof(T), ptr)) [[unlikely]] {
        return false;
    }
    if constexpr (sizeof(T) == 1) {
        *ptr = static_cast<uint8_t>(value);
    } else {
        const T swapped = MaybeByteSwap(value);
        std::memcpy(ptr, &swapped, sizeof(T));
    }
    return true;
}

template <typename T>
inline bool WriteStackScalarFast(uint32_t address, T value) {
    uint8_t* ptr = nullptr;
    if (TryGetPointerFast(address, sizeof(T), ptr)) {
        if constexpr (sizeof(T) == 1) {
            *ptr = static_cast<uint8_t>(value);
        } else {
            const T swapped = MaybeByteSwap(value);
            std::memcpy(ptr, &swapped, sizeof(T));
        }
        return true;
    }
    return false;
}

inline uint8_t ReadStack8(uint32_t address) {
    uint8_t value = 0;
    return TryReadMappedScalar(address, value) ? value : Memory::Read8(address);
}

inline uint16_t ReadStack16(uint32_t address) {
    uint16_t value = 0;
    return TryReadMappedScalar(address, value) ? value : Memory::Read16(address);
}

inline uint32_t ReadStack32(uint32_t address) {
    uint32_t value = 0;
    return TryReadMappedScalar(address, value) ? value : Memory::Read32(address);
}

inline uint64_t ReadStack64(uint32_t address) {
    uint64_t value = 0;
    return TryReadMappedScalar(address, value) ? value : Memory::Read64(address);
}



inline void WriteStack8(uint32_t address, uint8_t value) {
    if (!WriteStackScalarFast(address, value)) {
        Memory::Write8(address, value);
    }
}

inline void WriteStack16(uint32_t address, uint16_t value) {
    if (!WriteStackScalarFast(address, value)) {
        Memory::Write16(address, value);
    }
}

inline void WriteStack32(uint32_t address, uint32_t value) {
    if (!WriteStackScalarFast(address, value)) {
        Memory::Write32(address, value);
    }
}

inline void WriteStack64(uint32_t address, uint64_t value) {
    if (!WriteStackScalarFast(address, value)) {
        Memory::Write64(address, value);
    }
}

inline void WriteStackFloat32(uint32_t address, double value) {
    const uint32_t bits = ConvertPpcDoubleToSingleBits(value);
    if (!WriteStackScalarFast(address, bits)) {
        Memory::WriteFloat32(address, value);
    }
}

inline void WriteStackFloat64(uint32_t address, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (!WriteStackScalarFast(address, bits)) {
        Memory::WriteFloat64(address, value);
    }
}

template <typename T, typename SlowRead>
MKW_MEMORY_FORCE_INLINE T ReadResolved(uint8_t* host, uint32_t rangeOffset, uint32_t address,
                      SlowRead slow) {
    T value = 0;
    if (ReadResolvedScalar(host, rangeOffset, value)) {
        return value;
    }
    [[unlikely]] return slow(address);
}

MKW_MEMORY_FORCE_INLINE uint8_t ReadResolved8(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint8_t>(r, o, a, ReadResolvedFallback<uint8_t>); }
MKW_MEMORY_FORCE_INLINE uint16_t ReadResolved16(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint16_t>(r, o, a, ReadResolvedFallback<uint16_t>); }
MKW_MEMORY_FORCE_INLINE uint32_t ReadResolved32(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint32_t>(r, o, a, ReadResolvedFallback<uint32_t>); }
// Live via isa/ppc_isa_quantized.h (the psq resolved tier packs two lanes into
// one 64-bit access); generated code never names it directly.
MKW_MEMORY_FORCE_INLINE uint64_t ReadResolved64(uint8_t* r, uint32_t o, uint32_t a) { return ReadResolved<uint64_t>(r, o, a, ReadResolvedFallback<uint64_t>); }
MKW_MEMORY_FORCE_INLINE float ReadResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a) {
    uint32_t bits = 0;
    if (!ReadResolvedScalar(r, o, bits)) [[unlikely]] return ReadResolvedFallbackFloat32(a);
    float value; std::memcpy(&value, &bits, sizeof(value)); return value;
}
MKW_MEMORY_FORCE_INLINE double ReadResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a) {
    uint64_t bits = 0;
    if (!ReadResolvedScalar(r, o, bits)) [[unlikely]] return ReadResolvedFallbackFloat64(a);
    double value; std::memcpy(&value, &bits, sizeof(value)); return value;
}

template <typename T, typename SlowWrite>
MKW_MEMORY_FORCE_INLINE void WriteResolved(uint8_t* host, uint32_t rangeOffset, uint32_t address, T value,
                          SlowWrite slow) {
    if (WriteResolvedScalar(host, rangeOffset, value)) {
        return;
    }
    [[unlikely]] slow(address, value);
}

MKW_MEMORY_FORCE_INLINE void WriteResolved8(uint8_t* r, uint32_t o, uint32_t a, uint8_t v) { WriteResolved(r, o, a, v, WriteResolvedFallback<uint8_t>); }
MKW_MEMORY_FORCE_INLINE void WriteResolved16(uint8_t* r, uint32_t o, uint32_t a, uint16_t v) { WriteResolved(r, o, a, v, WriteResolvedFallback<uint16_t>); }
MKW_MEMORY_FORCE_INLINE void WriteResolved32(uint8_t* r, uint32_t o, uint32_t a, uint32_t v) { WriteResolved(r, o, a, v, WriteResolvedFallback<uint32_t>); }
// Live via isa/ppc_isa_quantized.h, as ReadResolved64 above.
MKW_MEMORY_FORCE_INLINE void WriteResolved64(uint8_t* r, uint32_t o, uint32_t a, uint64_t v) { WriteResolved(r, o, a, v, WriteResolvedFallback<uint64_t>); }
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat32(uint8_t* r, uint32_t o, uint32_t a, double v) {
    const uint32_t bits = ConvertPpcDoubleToSingleBits(v);
    if (WriteResolvedScalar(r, o, bits)) return;
    [[unlikely]] WriteResolvedFallbackFloat32(a, v);
}
MKW_MEMORY_FORCE_INLINE void WriteResolvedFloat64(uint8_t* r, uint32_t o, uint32_t a, double v) {
    uint64_t bits; std::memcpy(&bits, &v, sizeof(bits));
    if (WriteResolvedScalar(r, o, bits)) return;
    [[unlikely]] WriteResolvedFallbackFloat64(a, v);
}

// Flat guest memory (audit item T-MEM): the 4 GiB reservation makes a guest access a byte swap
// around `*(T*)(base + addr)`, no page-table load or limit check (interception model documented
// in guest_flat_memory.h). The one exception kept inline is the MMIO write policy, since the
// written value can't be recovered from a fault record.
//
// When a host VM page is larger than a 4 KiB Wii page, guest-view protections
// cannot distinguish adjacent special Wii pages. The general FlatRead*/
// FlatWrite* helpers then use the checked page-table path, which materializes
// deferred reads and applies executable-write/MMIO policy before touching RAM.
// FlatWriteRam* remains direct because the translator emits it only for
// addresses it has proven are ordinary RAM.

template <typename T>
MKW_MEMORY_FORCE_INLINE T FlatLoad(uint32_t address) {
    T value{};
    std::memcpy(&value, MKW_FLAT_GUEST_BASE + address, sizeof(T));
    return MaybeByteSwap(value);
}

template <typename T>
MKW_MEMORY_FORCE_INLINE void FlatStore(uint32_t address, T value) {
    const T swapped = MaybeByteSwap(value);
    std::memcpy(MKW_FLAT_GUEST_BASE + address, &swapped, sizeof(T));
}

MKW_MEMORY_FORCE_INLINE uint8_t FlatRead8(uint32_t address) {
    if (GuestFlat::RequiresCheckedAccess()) return Memory::Read8(address);
    return FlatLoad<uint8_t>(address);
}
MKW_MEMORY_FORCE_INLINE uint16_t FlatRead16(uint32_t address) {
    if (GuestFlat::RequiresCheckedAccess()) return Memory::Read16(address);
    return FlatLoad<uint16_t>(address);
}
MKW_MEMORY_FORCE_INLINE uint32_t FlatRead32(uint32_t address) {
    if (GuestFlat::RequiresCheckedAccess()) return Memory::Read32(address);
    return FlatLoad<uint32_t>(address);
}

MKW_MEMORY_FORCE_INLINE float FlatReadFloat32(uint32_t address) {
    const uint32_t bits = FlatRead32(address);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MKW_MEMORY_FORCE_INLINE double FlatReadFloat64(uint32_t address) {
    const uint64_t bits = GuestFlat::RequiresCheckedAccess()
        ? Memory::Read64(address) : FlatLoad<uint64_t>(address);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

MKW_MEMORY_FORCE_INLINE void FlatWrite8(uint32_t address, uint8_t value) {
    if (GuestFlat::RequiresCheckedAccess()) { Memory::Write8(address, value); return; }
    if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write8Slow(address, value); return; }
    FlatStore<uint8_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite16(uint32_t address, uint16_t value) {
    if (GuestFlat::RequiresCheckedAccess()) { Memory::Write16(address, value); return; }
    if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write16Slow(address, value); return; }
    FlatStore<uint16_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWrite32(uint32_t address, uint32_t value) {
    if (GuestFlat::RequiresCheckedAccess()) { Memory::Write32(address, value); return; }
    if (FlatWriteNeedsPolicy(address)) [[unlikely]] { Write32Slow(address, value); return; }
    FlatStore<uint32_t>(address, value);
}


MKW_MEMORY_FORCE_INLINE void FlatWriteFloat32(uint32_t address, double value) {
    if (GuestFlat::RequiresCheckedAccess()) { Memory::WriteFloat32(address, value); return; }
    const uint32_t bits = ConvertPpcDoubleToSingleBits(value);
    if (FlatWriteNeedsPolicy(address)) [[unlikely]] { WriteFloat32Slow(address, value); return; }
    FlatStore<uint32_t>(address, bits);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteFloat64(uint32_t address, double value) {
    if (GuestFlat::RequiresCheckedAccess()) { Memory::WriteFloat64(address, value); return; }
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    if (FlatWriteNeedsPolicy(address)) [[unlikely]] { WriteFloat64Slow(address, value); return; }
    FlatStore<uint64_t>(address, bits);
}

// Check-free stores: emitted ONLY for addresses the translator proved at translate time are ordinary guest RAM (r1-relative stack slots, ~45%
// of flat stores), skipping the MMIO mask/compare that's pure overhead there. Still safe if that proof were ever wrong: the flat view maps
// 0xCC000000..0xCDFFFFFF PAGE_NOACCESS, so a stray MMIO store faults into the same handler and diagnostic as the checked path, just reported
// instead of dispatched inline. Never use these for an address the translator hasn't proven.

MKW_MEMORY_FORCE_INLINE void FlatWriteRam8(uint32_t address, uint8_t value) {
    FlatStore<uint8_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam16(uint32_t address, uint16_t value) {
    FlatStore<uint16_t>(address, value);
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRam32(uint32_t address, uint32_t value) {
    FlatStore<uint32_t>(address, value);
}


MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat32(uint32_t address, double value) {
    FlatStore<uint32_t>(address, ConvertPpcDoubleToSingleBits(value));
}

MKW_MEMORY_FORCE_INLINE void FlatWriteRamFloat64(uint32_t address, double value) {
    uint64_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    FlatStore<uint64_t>(address, bits);
}

} // namespace MemoryInline

#undef MKW_MEMORY_FORCE_INLINE
#undef MKW_MEMORY_NO_INLINE
#undef MKW_MEMORY_COLD

inline uint8_t Memory::Read8(uint32_t addr) {
    uint8_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value)) [[unlikely]]
        return MemoryInline::Read8Slow(addr);
    return value;
}

inline uint16_t Memory::Read16(uint32_t addr) {
    uint16_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value)) [[unlikely]]
        return MemoryInline::Read16Slow(addr);
    return value;
}

inline uint32_t Memory::Read32(uint32_t addr) {
    uint32_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value)) [[unlikely]]
        return MemoryInline::Read32Slow(addr);
    return value;
}

inline uint64_t Memory::Read64(uint32_t addr) {
    uint64_t value = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, value)) [[unlikely]]
        return MemoryInline::Read64Slow(addr);
    return value;
}

inline float Memory::ReadFloat32(uint32_t addr) {
    uint32_t bits = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, bits)) [[unlikely]]
        return MemoryInline::ReadFloat32Slow(addr);
    float value = 0.0f;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline double Memory::ReadFloat64(uint32_t addr) {
    uint64_t bits = 0;
    if (!MemoryInline::TryReadGuestScalar(addr, bits)) [[unlikely]]
        return MemoryInline::ReadFloat64Slow(addr);
    double value = 0.0;
    std::memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void Memory::Write8(uint32_t addr, uint8_t val) {
    if (!MemoryInline::TryWriteGuestScalar(addr, val)) [[unlikely]]
        MemoryInline::Write8Slow(addr, val);
}

inline void Memory::Write16(uint32_t addr, uint16_t val) {
    if (!MemoryInline::TryWriteGuestScalar(addr, val)) [[unlikely]]
        MemoryInline::Write16Slow(addr, val);
}

inline void Memory::Write32(uint32_t addr, uint32_t val) {
    if (!MemoryInline::TryWriteGuestScalar(addr, val)) [[unlikely]]
        MemoryInline::Write32Slow(addr, val);
}

inline void Memory::Write64(uint32_t addr, uint64_t val) {
    if (!MemoryInline::TryWriteGuestScalar(addr, val)) [[unlikely]]
        MemoryInline::Write64Slow(addr, val);
}

inline void Memory::WriteFloat32(uint32_t addr, double val) {
    {
        const uint32_t bits = MemoryInline::ConvertPpcDoubleToSingleBits(val);
        if (MemoryInline::TryWriteGuestScalar(addr, bits))
            return;
    }
    [[unlikely]] MemoryInline::WriteFloat32Slow(addr, val);
}

inline void Memory::WriteFloat64(uint32_t addr, double val) {
    {
        uint64_t bits = 0;
        std::memcpy(&bits, &val, sizeof(bits));
        if (MemoryInline::TryWriteGuestScalar(addr, bits))
            return;
    }
    [[unlikely]] MemoryInline::WriteFloat64Slow(addr, val);
}
