#pragma once


#include "ppc_isa_config.h"
#include "ppc_isa_context.h"
#include "ppc_isa_float.h"
#include "big_endian.h"

#include "ppc_isa_memory.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>


inline uint32_t PpcLoadPsqFloatBitsInline(uint32_t value)
{
    const uint32_t magnitude = value & 0x7FFFFFFFu;
    if (magnitude > 0x7F800000u)
        return value | 0x00400000u;
    return value;
}

inline uint32_t PpcStorePsqFloatBitsInline(uint32_t value)
{
    const uint32_t magnitude = value & 0x7FFFFFFFu;
    if (magnitude < 0x00800000u)
        return value & 0x80000000u;
    if (magnitude > 0x7F800000u)
        return value | 0x00400000u;
    return value;
}


inline uint64_t PpcLoadPairPsqFloatBitsPackedInline(uint64_t value)
{
#if defined(__x86_64__)
    const __m128i lanes = _mm_cvtsi64_si128(static_cast<long long>(value));
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i result = _mm_or_si128(
        lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    return static_cast<uint64_t>(_mm_cvtsi128_si64(result));
#elif defined(__aarch64__)
    // Equivalent to applying PpcLoadPsqFloatBitsInline to each 32-bit lane: the
    // x86 body above only ever acts on these same two lanes (the upper 64 bits
    // _mm_cvtsi64_si128 zero-fills never survive the final truncating extract).
    const uint32_t lo = PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(value));
    const uint32_t hi = PpcLoadPsqFloatBitsInline(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

inline uint64_t PpcStorePairPsqFloatBitsPackedInline(uint64_t value)
{
#if defined(__x86_64__)
    const __m128i lanes = _mm_cvtsi64_si128(static_cast<long long>(value));
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i subnormalMask = _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i quieted = _mm_or_si128(
        lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    const __m128i signedZero = _mm_and_si128(
        lanes, _mm_set1_epi32(std::numeric_limits<int32_t>::min()));
    const __m128i result = _mm_or_si128(
        _mm_and_si128(subnormalMask, signedZero),
        _mm_andnot_si128(subnormalMask, quieted));
    return static_cast<uint64_t>(_mm_cvtsi128_si64(result));
#elif defined(__aarch64__)
    // Equivalent to applying PpcStorePsqFloatBitsInline to each 32-bit lane;
    // same reasoning as the load-side port above.
    const uint32_t lo = PpcStorePsqFloatBitsInline(static_cast<uint32_t>(value));
    const uint32_t hi = PpcStorePsqFloatBitsInline(static_cast<uint32_t>(value >> 32));
    return (static_cast<uint64_t>(hi) << 32) | lo;
#endif
}

#if defined(__x86_64__)
inline __m128i PpcPsqSwapPairBytesInline(__m128i lanes)
{
    const __m128i order = _mm_setr_epi8(
        static_cast<char>(7), static_cast<char>(6), static_cast<char>(5), static_cast<char>(4),
        static_cast<char>(3), static_cast<char>(2), static_cast<char>(1), static_cast<char>(0),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80),
        static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80), static_cast<char>(0x80));
    return _mm_shuffle_epi8(lanes, order);
}

// Lane-local sNaN quieting; identical rule to PpcLoadPairPsqFloatBitsPackedInline.
inline __m128i PpcLoadPairPsqFloatBitsLanesInline(__m128i lanes)
{
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    return _mm_or_si128(lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
}

// Lane-local denormal flush + sNaN quieting; identical rule to
// PpcStorePairPsqFloatBitsPackedInline.
inline __m128i PpcStorePairPsqFloatBitsLanesInline(__m128i lanes)
{
    const __m128i magnitude = _mm_and_si128(lanes, _mm_set1_epi32(0x7FFFFFFF));
    const __m128i subnormalMask = _mm_cmplt_epi32(magnitude, _mm_set1_epi32(0x00800000));
    const __m128i nanMask = _mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F800000));
    const __m128i quieted = _mm_or_si128(
        lanes, _mm_and_si128(nanMask, _mm_set1_epi32(0x00400000)));
    const __m128i signedZero = _mm_and_si128(
        lanes, _mm_set1_epi32(std::numeric_limits<int32_t>::min()));
    return _mm_or_si128(
        _mm_and_si128(subnormalMask, signedZero),
        _mm_andnot_si128(subnormalMask, quieted));
}
#endif // defined(__x86_64__)

// host -> packed FPR double, guard already proven by the caller.
inline double PpcLoadPairPsqFloatFromHostInline(const uint8_t* host)
{
#if defined(__x86_64__)
    const __m128i raw = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(host));
    return PpcM128ToPsInline(_mm_castsi128_ps(
        PpcLoadPairPsqFloatBitsLanesInline(PpcPsqSwapPairBytesInline(raw))));
#elif defined(__aarch64__)
    // The x86 path's full 8-byte pshufb reversal plus a same-endian load is,
    // taken together, exactly a 64-bit byteswap of a plain little-endian load:
    // it turns the on-disk [ps0 big-endian][ps1 big-endian] byte layout into a
    // native uint64 with low 32 bits = ps1, high 32 bits = ps0 (this file's
    // documented packed-double lane convention).
    uint64_t raw = 0;
    std::memcpy(&raw, host, sizeof(raw));
    const uint64_t swapped = __builtin_bswap64(raw);
    return PpcBitCastToDoubleInline(PpcLoadPairPsqFloatBitsPackedInline(swapped));
#endif
}

// packed FPR double -> host, guard already proven by the caller.
inline void PpcStorePairPsqFloatToHostInline(uint8_t* host, double value)
{
#if defined(__x86_64__)
    const __m128i lanes = PpcStorePairPsqFloatBitsLanesInline(
        _mm_castps_si128(PpcPsToM128Inline(value)));
    _mm_storel_epi64(reinterpret_cast<__m128i*>(host), PpcPsqSwapPairBytesInline(lanes));
#elif defined(__aarch64__)
    // Inverse of the load path above: bswap64 is its own inverse, so applying
    // it to the quieted packed value reproduces the on-disk big-endian bytes.
    const uint64_t quieted = PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value));
    const uint64_t swapped = __builtin_bswap64(quieted);
    std::memcpy(host, &swapped, sizeof(swapped));
#endif
}

template <typename SignedType>
inline SignedType PpcScaleAndClampPsqInline(float value, uint32_t scale)
{
    if (scale == 0u)
    {
        constexpr float kMin = static_cast<float>(std::numeric_limits<SignedType>::min());
        constexpr float kMax = static_cast<float>(std::numeric_limits<SignedType>::max());
        return static_cast<SignedType>(std::clamp(value, kMin, kMax));
    }

    const float factor = scale < 32u
        ? std::ldexp(1.0f, static_cast<int>(scale))
        : std::ldexp(1.0f, -(64 - static_cast<int>(scale)));
    const float scaled = value * factor;
    constexpr float kMin = static_cast<float>(std::numeric_limits<SignedType>::min());
    constexpr float kMax = static_cast<float>(std::numeric_limits<SignedType>::max());
    return static_cast<SignedType>(std::clamp(scaled, kMin, kMax));
}

template <typename SignedType>
inline float PpcDequantizePsqInline(SignedType value, uint32_t scale)
{
    if (scale == 0u)
    {
        return static_cast<float>(value);
    }

    const float factor = scale < 32u
        ? std::ldexp(1.0f, -static_cast<int>(scale))
        : std::ldexp(1.0f, static_cast<int>(64u - scale));
    return static_cast<float>(value) * factor;
}

template <typename T>
inline T PpcReadUnpairedPsqInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        return static_cast<T>(Memory::Read8(addr));
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(Memory::Read16(addr));
    }
    else
    {
        return static_cast<T>(Memory::Read32(addr));
    }
}

template <typename T>
inline std::pair<T, T> PpcReadPairPsqInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = Memory::Read16(addr);
        return { static_cast<T>(packed >> 8), static_cast<T>(packed) };
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = Memory::Read32(addr);
        return { static_cast<T>(packed >> 16), static_cast<T>(packed) };
    }
    else
    {
        const uint64_t packed = Memory::Read64(addr);
        return { static_cast<T>(packed >> 32), static_cast<T>(packed) };
    }
}

template <typename T>
inline void PpcWriteUnpairedPsqInline(uint32_t addr, T value)
{
    if constexpr (sizeof(T) == 1)
    {
        Memory::Write8(addr, static_cast<uint8_t>(value));
    }
    else if constexpr (sizeof(T) == 2)
    {
        Memory::Write16(addr, static_cast<uint16_t>(value));
    }
    else
    {
        Memory::Write32(addr, static_cast<uint32_t>(value));
    }
}

template <typename T>
inline void PpcWritePairPsqInline(uint32_t addr, T first, T second)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = (static_cast<uint16_t>(first) << 8) | static_cast<uint16_t>(second);
        Memory::Write16(addr, packed);
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
        Memory::Write32(addr, packed);
    }
    else
    {
        const uint64_t packed = (static_cast<uint64_t>(first) << 32) | static_cast<uint64_t>(second);
        Memory::Write64(addr, packed);
    }
}

// The flat reservation makes every 32-bit guest address a host address; a pending-deferred
// (EFB) page is PAGE_NOACCESS so the load faults through the vectored handler instead of
// reading stale bytes, and unmapped pages commit on demand, same as MemoryInline::Flat* loads.
MKW_PPC_FORCE_INLINE const uint8_t* PpcTryGetPsqReadableHostInline(uint32_t addr)
{
    if (GuestFlat::RequiresCheckedAccess()) [[unlikely]]
        return nullptr;
    return MKW_FLAT_GUEST_BASE + addr;
}

// Same reduction for stores. Still refuses a 32-bit address wrap (one host access can't
// reproduce it) and keeps MMIO write policy inline, since a fault record can't carry the
// stored value and a null result must preserve the cold GX FIFO dispatch path byte for byte.
// The old 1 MiB page-cross refusal is gone since the whole 4 GiB is now contiguous; deferred,
// executable, and unmapped pages still trap.
MKW_PPC_FORCE_INLINE uint8_t* PpcTryGetPsqWritableHostInline(uint32_t addr)
{
    if (GuestFlat::RequiresCheckedAccess()) [[unlikely]]
        return nullptr;
    if (addr > UINT32_MAX - 7u) [[unlikely]]
        return nullptr;
    if (MemoryInline::FlatWriteNeedsPolicy(addr) ||
        MemoryInline::FlatWriteNeedsPolicy(addr + 7u)) [[unlikely]]
        return nullptr;
    return MKW_FLAT_GUEST_BASE + addr;
}

inline double PpcLoadPairPsqFloatFastInline(uint32_t addr)
{
    const uint8_t* host = PpcTryGetPsqReadableHostInline(addr);
    if (host != nullptr) [[likely]]
    {
        return PpcLoadPairPsqFloatFromHostInline(host);
    }
    return PpcBitCastToDoubleInline(
        PpcLoadPairPsqFloatBitsPackedInline(Memory::Read64(addr)));
}

inline double PpcLoadSinglePsqFloatFastInline(uint32_t addr)
{
    const uint32_t raw = PpcReadUnpairedPsqInline<uint32_t>(addr);
    return PpcMakePairedResultInline(
        PpcBitCastToFloatInline(raw),
        1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerFastInline(uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1)
    {
        const uint16_t packed = Memory::Read16(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else if constexpr (sizeof(ValueType) == 2)
    {
        const uint32_t packed = Memory::Read32(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else
    {
        const uint64_t packed = Memory::Read64(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedFastInline(uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    const UnsignedType value = PpcReadUnpairedPsqInline<UnsignedType>(addr);
    return PpcMakePairedResultInline(
        PpcDequantizePsqInline(static_cast<SignedType>(value), scale),
        1.0f).d;
}

inline void PpcStorePairPsqFloatFastInline(uint32_t addr, double value)
{
    uint8_t* host = PpcTryGetPsqWritableHostInline(addr);
    if (host != nullptr) [[likely]]
    {
        PpcStorePairPsqFloatToHostInline(host, value);
        return;
    }
    Memory::Write64(
        addr, PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value)));
}

inline uint8_t PpcQuantizePsqU8Scale61Inline(float value)
{
    const float scaled = value * 0.125f;
    if (!(scaled > 0.0f))
    {
        return 0;
    }
    if (scaled >= 255.0f)
    {
        return 255;
    }
    return static_cast<uint8_t>(scaled);
}

// GQR U8 scale 61 is common in paired stores. Quantize both payload lanes as
// one native vector so the normal finite path does not branch once per lane.
// MAXPS with zero as the second operand also maps either NaN lane to zero,
// matching PpcQuantizePsqU8Scale61Inline's !(scaled > 0) rule.
inline uint16_t PpcQuantizePairPsqU8Scale61PackedInline(double value)
{
#if defined(__x86_64__)
    const __m128 scaled = _mm_mul_ps(PpcPsToM128Inline(value), _mm_set1_ps(0.125f));
    const __m128 nonNegative = _mm_max_ps(scaled, _mm_setzero_ps());
    const __m128 clamped = _mm_min_ps(nonNegative, _mm_set1_ps(255.0f));
    const __m128i lanes32 = _mm_cvttps_epi32(clamped);
    const __m128i lanes16 = _mm_packs_epi32(lanes32, lanes32);
    const __m128i lanes8 = _mm_packus_epi16(lanes16, lanes16);
    // Native lane 0 is ps1 and lane 1 is ps0. Packing to the low uint16_t
    // therefore produces the guest-order numeric value (ps0 << 8) | ps1.
    return static_cast<uint16_t>(_mm_cvtsi128_si32(lanes8));
#elif defined(__aarch64__)
    // Equivalent to two calls of the already-portable scalar quantizer above
    // (its own !(scaled > 0) rule maps NaN to 0, matching what MAXPS-with-zero
    // does on the x86 path per the comment there), packed the same way the
    // fallback path just below already does for the non-SIMD case.
    PPC_FPR fpr{};
    fpr.d = value;
    const uint8_t q0 = PpcQuantizePsqU8Scale61Inline(fpr.paired.ps0);
    const uint8_t q1 = PpcQuantizePsqU8Scale61Inline(fpr.paired.ps1);
    return static_cast<uint16_t>((static_cast<uint16_t>(q0) << 8) | static_cast<uint16_t>(q1));
#endif
}

// Preserve the complete memory/MMIO/executable-write behavior off the leaf
// path. Keeping this out of line prevents those uncommon checks from being
// replicated at every compile-time-known PSQ store.
MKW_PPC_NO_INLINE inline void PpcStorePairPsqU8Scale61Fallback(
    uint32_t addr, double value)
{
    PPC_FPR fpr{};
    fpr.d = value;
    const uint16_t packed = static_cast<uint16_t>(
        (static_cast<uint16_t>(PpcQuantizePsqU8Scale61Inline(fpr.paired.ps0)) << 8) |
        static_cast<uint16_t>(PpcQuantizePsqU8Scale61Inline(fpr.paired.ps1)));
    Memory::Write16(addr, packed);
}

// The full-writable bias is already the runtime proof that this two-byte store
// is mapped, in range, debug-compatible, and cannot touch executable guest
// code. Check that proof before doing any conversion work, then perform the
// endian-aware native store directly. A zero entry takes the exact cold path.
MKW_PPC_FORCE_INLINE void PpcStorePairPsqU8Scale61FastInline(
    uint32_t addr, double value)
{
    uint8_t* host = nullptr;
    if (!MemoryInline::TryGetWritablePointerFast(addr, sizeof(uint16_t), host))
    {
        PpcStorePairPsqU8Scale61Fallback(addr, value);
        return;
    }

    const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
    BigEndian::Write16(host, packed);
}

MKW_PPC_FORCE_INLINE void PpcStorePairPsqU8Scale61ResolvedInline(
    uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost)
    {
        PpcStorePairPsqU8Scale61Fallback(addr, value);
        return;
    }

    const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
    BigEndian::Write16(resolvedHost + offset, packed);
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedFastInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;
    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            PpcStorePairPsqU8Scale61FastInline(addr, value);
            return;
        }
    }

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        const auto second = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale)));
        PpcWritePairPsqInline(addr, first, second);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        const auto second = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale));
        PpcWritePairPsqInline(addr, first, second);
    }
}

inline void PpcStoreSinglePsqFloatFastInline(uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(
        PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    PpcWriteUnpairedPsqInline(addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedFastInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;
    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        PpcWriteUnpairedPsqInline(addr, first);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        PpcWriteUnpairedPsqInline(addr, first);
    }
}

inline double PpcLoadPairPsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    // A resolved range is already the proof; MemoryInline::ReadResolved64 only
    // falls back when the host pointer is null.
    if (resolvedHost != nullptr) [[likely]]
        return PpcLoadPairPsqFloatFromHostInline(resolvedHost + offset);
    return PpcBitCastToDoubleInline(PpcLoadPairPsqFloatBitsPackedInline(
        MemoryInline::ReadResolvedFallback<uint64_t>(addr)));
}

inline double PpcLoadSinglePsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    const uint32_t raw = MemoryInline::ReadResolved32(resolvedHost, offset, addr);
    return PpcMakePairedResultInline(PpcBitCastToFloatInline(raw), 1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerResolvedInline(uint8_t* resolvedHost, uint32_t offset,
                                                   uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1) {
        const uint16_t packed = MemoryInline::ReadResolved16(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    } else if constexpr (sizeof(ValueType) == 2) {
        const uint32_t packed = MemoryInline::ReadResolved32(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    } else {
        const uint64_t packed = MemoryInline::ReadResolved64(resolvedHost, offset, addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    UnsignedType value;
    if constexpr (sizeof(UnsignedType) == 1) value = MemoryInline::ReadResolved8(resolvedHost, offset, addr);
    else if constexpr (sizeof(UnsignedType) == 2) value = MemoryInline::ReadResolved16(resolvedHost, offset, addr);
    else value = MemoryInline::ReadResolved32(resolvedHost, offset, addr);
    return PpcMakePairedResultInline(PpcDequantizePsqInline(static_cast<SignedType>(value), scale), 1.0f).d;
}

inline void PpcStorePairPsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    // Mirrors MemoryInline::WriteResolved64: the resolved range is the proof,
    // and only a null host takes the original-address fallback.
    if (resolvedHost != nullptr) [[likely]]
    {
        PpcStorePairPsqFloatToHostInline(resolvedHost + offset, value);
        return;
    }
    MemoryInline::WriteResolvedFallback<uint64_t>(
        addr, PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value)));
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{}; fpr.d = value;
    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            PpcStorePairPsqU8Scale61ResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    }
    using Unsigned = std::make_unsigned_t<QuantizedType>;
    const auto quantize = [scale](float lane) -> Unsigned {
        return static_cast<Unsigned>(static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(lane, scale)));
    };
    const Unsigned first = quantize(fpr.paired.ps0);
    const Unsigned second = quantize(fpr.paired.ps1);
    if constexpr (sizeof(QuantizedType) == 1)
        MemoryInline::WriteResolved16(resolvedHost, offset, addr, (static_cast<uint16_t>(first) << 8) | second);
    else if constexpr (sizeof(QuantizedType) == 2)
        MemoryInline::WriteResolved32(resolvedHost, offset, addr, (static_cast<uint32_t>(first) << 16) | second);
    else
        MemoryInline::WriteResolved64(resolvedHost, offset, addr, (static_cast<uint64_t>(first) << 32) | second);
}

inline void PpcStoreSinglePsqFloatResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    MemoryInline::WriteResolved32(resolvedHost, offset, addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedResolvedInline(uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{}; fpr.d = value;
    using Unsigned = std::make_unsigned_t<QuantizedType>;
    const Unsigned first = static_cast<Unsigned>(static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
    if constexpr (sizeof(QuantizedType) == 1) MemoryInline::WriteResolved8(resolvedHost, offset, addr, first);
    else if constexpr (sizeof(QuantizedType) == 2) MemoryInline::WriteResolved16(resolvedHost, offset, addr, first);
    else MemoryInline::WriteResolved32(resolvedHost, offset, addr, first);
}

template <typename T>
inline T PpcReadUnpairedPsqStackInline(uint32_t addr)
{
    if constexpr (sizeof(T) == 1)
    {
        return static_cast<T>(MemoryInline::ReadStack8(addr));
    }
    else if constexpr (sizeof(T) == 2)
    {
        return static_cast<T>(MemoryInline::ReadStack16(addr));
    }
    else
    {
        return static_cast<T>(MemoryInline::ReadStack32(addr));
    }
}

template <typename T>
inline void PpcWriteUnpairedPsqStackInline(uint32_t addr, T value)
{
    if constexpr (sizeof(T) == 1)
    {
        MemoryInline::WriteStack8(addr, static_cast<uint8_t>(value));
    }
    else if constexpr (sizeof(T) == 2)
    {
        MemoryInline::WriteStack16(addr, static_cast<uint16_t>(value));
    }
    else
    {
        MemoryInline::WriteStack32(addr, static_cast<uint32_t>(value));
    }
}

template <typename T>
inline void PpcWritePairPsqStackInline(uint32_t addr, T first, T second)
{
    if constexpr (sizeof(T) == 1)
    {
        const uint16_t packed = (static_cast<uint16_t>(first) << 8) | static_cast<uint16_t>(second);
        MemoryInline::WriteStack16(addr, packed);
    }
    else if constexpr (sizeof(T) == 2)
    {
        const uint32_t packed = (static_cast<uint32_t>(first) << 16) | static_cast<uint32_t>(second);
        MemoryInline::WriteStack32(addr, packed);
    }
    else
    {
        const uint64_t packed = (static_cast<uint64_t>(first) << 32) | static_cast<uint64_t>(second);
        MemoryInline::WriteStack64(addr, packed);
    }
}

inline double PpcLoadPairPsqFloatStackInline(uint32_t addr)
{
    // MemoryInline::ReadStack64's exact proof (TryGetPointerFast), then the
    // same Memory::Read64 fallback it uses on a miss.
    uint8_t* host = nullptr;
    if (MemoryInline::TryGetPointerFast(addr, sizeof(uint64_t), host)) [[likely]]
        return PpcLoadPairPsqFloatFromHostInline(host);
    return PpcBitCastToDoubleInline(PpcLoadPairPsqFloatBitsPackedInline(Memory::Read64(addr)));
}

inline double PpcLoadSinglePsqFloatStackInline(uint32_t addr)
{
    const uint32_t raw = PpcReadUnpairedPsqStackInline<uint32_t>(addr);
    return PpcMakePairedResultInline(PpcBitCastToFloatInline(raw), 1.0f).d;
}

template <typename ValueType>
inline double PpcLoadPairPsqIntegerStackInline(uint32_t addr, uint32_t scale = 0u)
{
    if constexpr (sizeof(ValueType) == 1)
    {
        const uint16_t packed = MemoryInline::ReadStack16(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 8), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else if constexpr (sizeof(ValueType) == 2)
    {
        const uint32_t packed = MemoryInline::ReadStack32(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 16), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
    else
    {
        const uint64_t packed = MemoryInline::ReadStack64(addr);
        return PpcMakePairedResultInline(
            PpcDequantizePsqInline(static_cast<ValueType>(packed >> 32), scale),
            PpcDequantizePsqInline(static_cast<ValueType>(packed), scale)).d;
    }
}

template <typename SignedType>
inline double PpcLoadSinglePsqQuantizedStackInline(uint32_t addr, uint32_t scale)
{
    using UnsignedType = std::make_unsigned_t<SignedType>;
    const UnsignedType value = PpcReadUnpairedPsqStackInline<UnsignedType>(addr);
    return PpcMakePairedResultInline(
        PpcDequantizePsqInline(static_cast<SignedType>(value), scale),
        1.0f).d;
}

inline void PpcStorePairPsqFloatStackInline(uint32_t addr, double value)
{
    // MemoryInline::WriteStack64's exact proof (WriteStackScalarFast ->
    // TryGetPointerFast), then the same Memory::Write64 fallback on a miss.
    uint8_t* host = nullptr;
    if (MemoryInline::TryGetPointerFast(addr, sizeof(uint64_t), host)) [[likely]]
    {
        PpcStorePairPsqFloatToHostInline(host, value);
        return;
    }
    Memory::Write64(addr, PpcStorePairPsqFloatBitsPackedInline(PpcBitCastToU64Inline(value)));
}

template <typename QuantizedType>
inline void PpcStorePairPsqQuantizedStackInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;
    if constexpr (std::is_same_v<QuantizedType, uint8_t>)
    {
        if (scale == 61u)
        {
            const uint16_t packed = PpcQuantizePairPsqU8Scale61PackedInline(value);
            MemoryInline::WriteStack16(addr, packed);
            return;
        }
    }

    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        const auto second = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale)));
        PpcWritePairPsqStackInline(addr, first, second);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        const auto second = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps1, scale));
        PpcWritePairPsqStackInline(addr, first, second);
    }
}

inline void PpcStoreSinglePsqFloatStackInline(uint32_t addr, double value)
{
    const uint32_t first = PpcConvertToSingleFTZInline(
        PpcBitCastToU64Inline(static_cast<double>(PpcGetPs0Inline(value))));
    PpcWriteUnpairedPsqStackInline(addr, first);
}

template <typename QuantizedType>
inline void PpcStoreSinglePsqQuantizedStackInline(uint32_t addr, double value, uint32_t scale)
{
    PPC_FPR fpr{};
    fpr.d = value;
    if constexpr (std::is_signed_v<QuantizedType>)
    {
        const auto first = static_cast<std::make_unsigned_t<QuantizedType>>(
            static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale)));
        PpcWriteUnpairedPsqStackInline(addr, first);
    }
    else
    {
        const auto first = static_cast<QuantizedType>(PpcScaleAndClampPsqInline<QuantizedType>(fpr.paired.ps0, scale));
        PpcWriteUnpairedPsqStackInline(addr, first);
    }
}

// Paired-Single helpers (psq_l / psq_st). w=0 loads/stores both ps0/ps1 (paired), w=1 only
// ps0 (scalar); i is the GQR index (0-7) for quantization type and scale.
extern "C" double PPC_PsqL(uint32_t addr, uint32_t w, uint32_t i);
extern "C" void PPC_PsqSt(uint32_t addr, double value, uint32_t w, uint32_t i);

// Cold generic body for psq_l. The complete quantization switch lives in one
// out-of-line copy so PPC_PsqLInline's float fast path can be force-inlined at
// guest call sites without spilling live registers around a call.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLGeneric(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        switch (gqr & 0xFFFF0000u)
        {
        case 0x00000000u:
return PpcLoadPairPsqFloatFastInline(addr);
        case 0x00040000u:
return PpcLoadPairPsqIntegerFastInline<uint8_t>(addr);
        case 0x00050000u:
return PpcLoadPairPsqIntegerFastInline<uint16_t>(addr);
        case 0x00060000u:
return PpcLoadPairPsqIntegerFastInline<int8_t>(addr);
        case 0x00070000u:
return PpcLoadPairPsqIntegerFastInline<int16_t>(addr);
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = (gqr >> 16) & 0x7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;
        switch (type)
        {
        case 0u:
return PpcLoadSinglePsqFloatFastInline(addr);
        case 4u:
return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
        case 5u:
return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
        case 6u:
return PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
        case 7u:
return PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
        default:
            break;
        }
    }

    return PPC_PsqL(addr, W, I);
}

// Hot wrapper: the float encoding (quantization type 0) is what the SDK leaves
// in the GQRs for the overwhelming majority of psq_l sites, so it is tested with
// one compare and handled inline. Every other encoding, including the reserved
// ones that must reach PPC_PsqL, tail-calls the cold generic body above.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLInline(CpuContext* cpu, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatFastInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqr >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatFastInline(addr);
        }
    }

    return PPC_PsqLGeneric<W, I>(gqr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLInline(uint32_t addr)
{
    return PPC_PsqLInline<W, I>(TryGetCpuContext(), addr);
}

// GQR-hoisting entry points (audit T-GQR). Identical bodies to PPC_PsqLInline/PPC_PsqStInline
// above, but the GQR value arrives as an argument instead of being reloaded from ctx->gqr[I]
// at every access, since pointer indirection otherwise forces a reload after every intervening
// store. I is only consulted for reserved encodings that tail-call PPC_PsqL/PPC_PsqSt.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE double PPC_PsqLGqrInline(uint32_t gqrValue, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqrValue & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatFastInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqrValue >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatFastInline(addr);
        }
    }

    return PPC_PsqLGeneric<W, I>(gqrValue, addr);
}

// Context-carrying form, for emission sites that still hand the CpuContext to
// every helper. The context is not read; the null check is retained so a broken
// call site fails the same way the ctx->gqr[I] form does.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE double PPC_PsqLGqrInline(CpuContext* cpu, uint32_t gqrValue, uint32_t addr)
{
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }
    return PPC_PsqLGqrInline<W, I>(gqrValue, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownInline(CpuContext*, uint32_t addr)
{
    static_assert(W <= 1u, "psq load W must be 0 or 1");
    static_assert(I < 8u, "psq load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatFastInline(addr);
        else return PpcLoadSinglePsqFloatFastInline(addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<int8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerFastInline<int16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
    }
    else
    {
        return PPC_PsqL(addr, W, I);
    }
}

// Cold generic body for the stack-form psq_l, mirroring PPC_PsqLGeneric: the
// complete quantization switch lives in one out-of-line copy so the hot
// wrapper's float path can be force-inlined at guest call sites without
// spilling live registers around a call.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLStackGeneric(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        switch (gqr & 0xFFFF0000u)
        {
        case 0x00000000u:
return PpcLoadPairPsqFloatStackInline(addr);
        case 0x00040000u:
return PpcLoadPairPsqIntegerStackInline<uint8_t>(addr);
        case 0x00050000u:
return PpcLoadPairPsqIntegerStackInline<uint16_t>(addr);
        case 0x00060000u:
return PpcLoadPairPsqIntegerStackInline<int8_t>(addr);
        case 0x00070000u:
return PpcLoadPairPsqIntegerStackInline<int16_t>(addr);
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = (gqr >> 16) & 0x7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;
        switch (type)
        {
        case 0u:
return PpcLoadSinglePsqFloatStackInline(addr);
        case 4u:
return PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale);
        case 5u:
return PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale);
        case 6u:
return PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale);
        case 7u:
return PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale);
        default:
            break;
        }
    }

    return PPC_PsqL(addr, W, I);
}

// Hot wrapper: the float encoding (quantization type 0) is what the SDK leaves
// in the GQRs for the overwhelming majority of psq_l sites, so it is tested with
// one compare and handled inline. Every other encoding, including the reserved
// ones that must reach PPC_PsqL, tail-calls the cold generic body above.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLStackInline(CpuContext* cpu, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u)
    {
        // Paired float requires type 0 *and* load scale 0 (the pair path in the
        // generic body dispatches on the whole load halfword for that reason).
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
        {
            return PpcLoadPairPsqFloatStackInline(addr);
        }
    }
    else
    {
        // Scalar float ignores the load scale, exactly like the generic switch.
        if (((gqr >> 16) & 0x7u) == 0u) [[likely]]
        {
            return PpcLoadSinglePsqFloatStackInline(addr);
        }
    }

    return PPC_PsqLStackGeneric<W, I>(gqr, addr);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLStackInline(uint32_t addr)
{
    return PPC_PsqLStackInline<W, I>(TryGetCpuContext(), addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownStackInline(CpuContext*, uint32_t addr)
{
    static_assert(W <= 1u, "psq stack load W must be 0 or 1");
    static_assert(I < 8u, "psq stack load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatStackInline(addr);
        else return PpcLoadSinglePsqFloatStackInline(addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<int8_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerStackInline<int16_t>(addr, scale);
        else return PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale);
    }
    else
    {
        return PPC_PsqL(addr, W, I);
    }
}

// Cold generic body for psq_st. See PPC_PsqLGeneric: the full switch stays out
// of line so the float fast path in PPC_PsqStInline can be force-inlined.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStGeneric(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        switch (gqr & 0x0000FFFFu)
        {
        case 0x0000u:
PpcStorePairPsqFloatFastInline(addr, value);
            return;
        case 0x0004u:
PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 0u);
            return;
        case 0x0005u:
PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, 0u);
            return;
        case 0x0006u:
PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, 0u);
            return;
        case 0x0007u:
PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, 0u);
            return;
        case 0x3D04u:
PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 61u);
            return;
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = gqr & 0x7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;
        switch (type)
        {
        case 0u:
PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        case 4u:
PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
            return;
        case 5u:
PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale);
            return;
        case 6u:
PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale);
            return;
        case 7u:
PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale);
            return;
        default:
            break;
        }
    }

    PPC_PsqSt(addr, value, W, I);
}

// Hot wrapper: store type 0 (float) is the common GQR setup, tested with a
// single compare and handled inline. Quantized encodings (including the U8
// scale-61 pair case) and reserved encodings go to the cold generic body.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(CpuContext* cpu, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqr & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatFastInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqr & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        }
    }

    PPC_PsqStGeneric<W, I>(gqr, addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStInline(uint32_t addr, double value)
{
    PPC_PsqStInline<W, I>(TryGetCpuContext(), addr, value);
}

// GQR-hoisting store entry points. See PPC_PsqLGqrInline above for why the GQR
// value is passed in and what the template parameters still mean.
template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(uint32_t gqrValue, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqrValue & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatFastInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqrValue & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatFastInline(addr, value);
            return;
        }
    }

    PPC_PsqStGeneric<W, I>(gqrValue, addr, value);
}

template <uint32_t W, uint32_t I = 0u>
MKW_PPC_FORCE_INLINE void PPC_PsqStGqrInline(CpuContext* cpu, uint32_t gqrValue, uint32_t addr, double value)
{
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }
    PPC_PsqStGqrInline<W, I>(gqrValue, addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownInline(CpuContext*, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq store W must be 0 or 1");
    static_assert(I < 8u, "psq store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatFastInline(addr, value);
        else PpcStoreSinglePsqFloatFastInline(addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale);
    }
    else
    {
        PPC_PsqSt(addr, value, W, I);
    }
}

// Cold generic body for the stack-form psq_st. See PPC_PsqStGeneric: the full
// switch stays out of line so the float fast path in the hot wrapper below can
// be force-inlined.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStStackGeneric(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");
    if constexpr (W == 0u)
    {
        switch (gqr & 0x0000FFFFu)
        {
        case 0x0000u:
PpcStorePairPsqFloatStackInline(addr, value);
            return;
        case 0x0004u:
PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 0u);
            return;
        case 0x0005u:
PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, 0u);
            return;
        case 0x0006u:
PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, 0u);
            return;
        case 0x0007u:
PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, 0u);
            return;
        case 0x3D04u:
PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 61u);
            return;
        default:
            break;
        }
    }
    else
    {
        const uint32_t type = gqr & 0x7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;
        switch (type)
        {
        case 0u:
PpcStoreSinglePsqFloatStackInline(addr, value);
            return;
        case 4u:
PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
            return;
        case 5u:
PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale);
            return;
        case 6u:
PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale);
            return;
        case 7u:
PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale);
            return;
        default:
            break;
        }
    }

    PPC_PsqSt(addr, value, W, I);
}

// Hot wrapper: store type 0 (float) is the common GQR setup, tested with a
// single compare and handled inline. Quantized encodings (including the U8
// scale-61 pair case) and reserved encodings go to the cold generic body.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(CpuContext* cpu, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");
    if (!cpu) [[unlikely]]
    {
        std::abort();
    }

    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u)
    {
        // Paired float requires store type 0 *and* store scale 0, matching the
        // generic body's dispatch on the whole store halfword.
        if ((gqr & 0x0000FFFFu) == 0x0000u) [[likely]]
        {
            PpcStorePairPsqFloatStackInline(addr, value);
            return;
        }
    }
    else
    {
        // Scalar float ignores the store scale, exactly like the generic switch.
        if ((gqr & 0x7u) == 0u) [[likely]]
        {
            PpcStoreSinglePsqFloatStackInline(addr, value);
            return;
        }
    }

    PPC_PsqStStackGeneric<W, I>(gqr, addr, value);
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStStackInline(uint32_t addr, double value)
{
    PPC_PsqStStackInline<W, I>(TryGetCpuContext(), addr, value);
}

// Context-free PSQ entries for translated regions which own GQR state as an
// ordinary native value. All architecturally valid quantization encodings are
// handled directly; reserved encodings retain the generic helper's abort.
template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLStateFallback(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);
    const uint32_t type = (gqr >> 16) & 0x7u;
    const uint32_t scale = (gqr >> 24) & 0x3Fu;
    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: return Stack ? PpcLoadPairPsqFloatStackInline(addr) : PpcLoadPairPsqFloatFastInline(addr);
        case 4u: return Stack ? PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, scale);
        case 5u: return Stack ? PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, scale);
        case 6u: return Stack ? PpcLoadPairPsqIntegerStackInline<int8_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<int8_t>(addr, scale);
        case 7u: return Stack ? PpcLoadPairPsqIntegerStackInline<int16_t>(addr, scale) : PpcLoadPairPsqIntegerFastInline<int16_t>(addr, scale);
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: return Stack ? PpcLoadSinglePsqFloatStackInline(addr) : PpcLoadSinglePsqFloatFastInline(addr);
        case 4u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
        case 5u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
        case 6u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
        case 7u: return Stack ? PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, scale) : PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
        default: std::abort();
        }
    }
}

// Keep the normal explicit-state path small and directly optimizable. Exact
// unscaled encodings cover the SDK's common GQR setup; scaled and reserved
// encodings retain the complete implementation in one cold outlined body.
template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE double PPC_PsqLStateInline(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);
    switch (gqr & 0xFFFF0000u)
    {
    case 0x00000000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqFloatStackInline(addr) : PpcLoadPairPsqFloatFastInline(addr);
        else return Stack ? PpcLoadSinglePsqFloatStackInline(addr) : PpcLoadSinglePsqFloatFastInline(addr);
    case 0x00040000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<uint8_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint8_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, 0u);
    case 0x00050000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<uint16_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<uint16_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, 0u);
    case 0x00060000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<int8_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<int8_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<int8_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, 0u);
    case 0x00070000u:
        if constexpr (W == 0u) return Stack ? PpcLoadPairPsqIntegerStackInline<int16_t>(addr, 0u) : PpcLoadPairPsqIntegerFastInline<int16_t>(addr, 0u);
        else return Stack ? PpcLoadSinglePsqQuantizedStackInline<int16_t>(addr, 0u) : PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, 0u);
    default:
        return PPC_PsqLStateFallback<W, I, Stack>(gqr, addr);
    }
}

template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStStateFallback(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);
    const uint32_t type = gqr & 0x7u;
    const uint32_t scale = (gqr >> 8) & 0x3Fu;
    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: Stack ? PpcStorePairPsqFloatStackInline(addr, value) : PpcStorePairPsqFloatFastInline(addr, value); return;
        case 4u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7u: if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, scale); else PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: if constexpr (Stack) PpcStoreSinglePsqFloatStackInline(addr, value); else PpcStoreSinglePsqFloatFastInline(addr, value); return;
        case 4u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7u: if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale); else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I, bool Stack>
MKW_PPC_FORCE_INLINE void PPC_PsqStStateInline(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);
    switch (gqr & 0xFFFFu)
    {
    case 0x0000u:
        if constexpr (W == 0u) Stack ? PpcStorePairPsqFloatStackInline(addr, value) : PpcStorePairPsqFloatFastInline(addr, value);
        else Stack ? PpcStoreSinglePsqFloatStackInline(addr, value) : PpcStoreSinglePsqFloatFastInline(addr, value);
        return;
    case 0x0004u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, 0u); }
        return;
    case 0x0005u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, 0u); }
        return;
    case 0x0006u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, 0u); }
        return;
    case 0x0007u:
        if constexpr (W == 0u) { if constexpr (Stack) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, 0u); else PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, 0u); }
        else { if constexpr (Stack) PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, 0u); else PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, 0u); }
        return;
    default:
        PPC_PsqStStateFallback<W, I, Stack>(gqr, addr, value);
        return;
    }
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLResolvedStateInline(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    static_assert(W <= 1u && I < 8u);
    if (!resolvedHost) [[unlikely]] return PPC_PsqLStateInline<W, I, false>(gqr, addr);
    const uint32_t type = (gqr >> 16) & 0x7u;
    const uint32_t scale = (gqr >> 24) & 0x3Fu;
    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedStateInline(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    static_assert(W <= 1u && I < 8u);
    if (!resolvedHost) [[unlikely]]
    {
        PPC_PsqStStateInline<W, I, false>(gqr, addr, value);
        return;
    }
    const uint32_t type = gqr & 0x7u;
    const uint32_t scale = (gqr >> 8) & 0x3Fu;
    if constexpr (W == 0u)
    {
        switch (type)
        {
        case 0u: PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: std::abort();
        }
    }
    else
    {
        switch (type)
        {
        case 0u: PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: std::abort();
        }
    }
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownStackInline(CpuContext*, uint32_t addr, double value)
{
    static_assert(W <= 1u, "psq stack store W must be 0 or 1");
    static_assert(I < 8u, "psq stack store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatStackInline(addr, value);
        else PpcStoreSinglePsqFloatStackInline(addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint8_t>(addr, value, scale);
            else PpcStoreSinglePsqQuantizedStackInline<uint8_t>(addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<uint16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<uint16_t>(addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<int8_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<int8_t>(addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedStackInline<int16_t>(addr, value, scale);
        else PpcStoreSinglePsqQuantizedStackInline<int16_t>(addr, value, scale);
    }
    else
    {
        PPC_PsqSt(addr, value, W, I);
    }
}

// Cold generic body for the resolved-host psq_l. Only reached with a non-null
// resolvedHost; the null case keeps its original PPC_PsqL behaviour in the
// force-inlined wrapper below.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLResolvedGeneric(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    if constexpr (W == 0u) {
        switch (gqr & 0xFFFF0000u) {
        case 0x00000000u: return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        case 0x00040000u: return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr);
        case 0x00050000u: return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr);
        case 0x00060000u: return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr);
        case 0x00070000u: return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr);
        default: return PPC_PsqL(addr, W, I);
        }
    } else {
        const uint32_t type = (gqr >> 16) & 7u;
        const uint32_t scale = (gqr >> 24) & 0x3Fu;
        switch (type) {
        case 0u: return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
        case 4u: return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        case 5u: return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        case 6u: return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        case 7u: return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        default: return PPC_PsqL(addr, W, I);
        }
    }
}

// Hot wrapper: resolved host pointer plus the float GQR encoding is the case
// that dominates translated code, so it is a compare and a direct byteswapped
// access. Null host and every non-float encoding stay out of line.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE double PPC_PsqLResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                   uint32_t offset, uint32_t addr)
{
    if (!resolvedHost) [[unlikely]] return PPC_PsqL(addr, W, I);
    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u) {
        // Paired float: load type 0 with load scale 0.
        if ((gqr & 0xFFFF0000u) == 0x00000000u) [[likely]]
            return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
    } else {
        // Scalar float ignores the load scale.
        if (((gqr >> 16) & 7u) == 0u) [[likely]]
            return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
    }
    return PPC_PsqLResolvedGeneric<W, I>(gqr, resolvedHost, offset, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline double PPC_PsqLKnownResolvedFallback(
    uint32_t addr)
{
    return PPC_PsqLKnownInline<W, I, GQR>(nullptr, addr);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE double PPC_PsqLKnownResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                        uint32_t offset, uint32_t addr)
{
    if (!resolvedHost) [[unlikely]] return PPC_PsqLKnownResolvedFallback<W, I, GQR>(addr);
    static_assert(W <= 1u, "psq resolved load W must be 0 or 1");
    static_assert(I < 8u, "psq resolved load GQR index must be 0..7");
    constexpr uint32_t type = (GQR >> 16) & 0x7u;
    constexpr uint32_t scale = (GQR >> 24) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqFloatResolvedInline(resolvedHost, offset, addr);
        else return PpcLoadSinglePsqFloatResolvedInline(resolvedHost, offset, addr);
    }
    else if constexpr (type == 4u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) return PpcLoadPairPsqIntegerResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
        else return PpcLoadSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, scale);
    }
    else
    {
        return PPC_PsqL(addr, W, I);
    }
}

// Cold generic body for the resolved-host psq_st. Only reached with a non-null
// resolvedHost; the null case keeps its original PPC_PsqSt behaviour in the
// force-inlined wrapper below.
template <uint32_t W, uint32_t I>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStResolvedGeneric(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    if constexpr (W == 0u) {
        switch (gqr & 0xFFFFu) {
        case 0x0000u: PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 0x0004u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0005u: PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0006u: PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x0007u: PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, 0u); return;
        case 0x3D04u: PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, 61u); return;
        default: PPC_PsqSt(addr, value, W, I); return;
        }
    } else {
        const uint32_t type = gqr & 7u;
        const uint32_t scale = (gqr >> 8) & 0x3Fu;
        switch (type) {
        case 0u: PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value); return;
        case 4u: PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale); return;
        case 5u: PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale); return;
        case 6u: PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale); return;
        case 7u: PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale); return;
        default: PPC_PsqSt(addr, value, W, I); return;
        }
    }
}

// Hot wrapper: resolved host pointer plus the float GQR encoding becomes a
// compare and a direct byteswapped store. Null host and every non-float
// encoding (including U8 scale 61) stay out of line.
template <uint32_t W, uint32_t I>
MKW_PPC_FORCE_INLINE void PPC_PsqStResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                  uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost) [[unlikely]] { PPC_PsqSt(addr, value, W, I); return; }
    const uint32_t gqr = cpu->gqr[I];
    if constexpr (W == 0u) {
        // Paired float: store type 0 with store scale 0.
        if ((gqr & 0xFFFFu) == 0x0000u) [[likely]] {
            PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    } else {
        // Scalar float ignores the store scale.
        if ((gqr & 7u) == 0u) [[likely]] {
            PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value);
            return;
        }
    }
    PPC_PsqStResolvedGeneric<W, I>(gqr, resolvedHost, offset, addr, value);
}

template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_NO_INLINE MKW_PPC_COLD inline void PPC_PsqStKnownResolvedFallback(
    uint32_t addr, double value)
{
    PPC_PsqStKnownInline<W, I, GQR>(nullptr, addr, value);
}


template <uint32_t W, uint32_t I, uint32_t GQR>
MKW_PPC_FORCE_INLINE void PPC_PsqStKnownResolvedInline(CpuContext* cpu, uint8_t* resolvedHost,
                                                       uint32_t offset, uint32_t addr, double value)
{
    if (!resolvedHost) [[unlikely]] { PPC_PsqStKnownResolvedFallback<W, I, GQR>(addr, value); return; }
    static_assert(W <= 1u, "psq resolved store W must be 0 or 1");
    static_assert(I < 8u, "psq resolved store GQR index must be 0..7");
    constexpr uint32_t type = GQR & 0x7u;
    constexpr uint32_t scale = (GQR >> 8) & 0x3Fu;
if constexpr (type == 0u)
    {
if constexpr (W == 0u) PpcStorePairPsqFloatResolvedInline(resolvedHost, offset, addr, value);
        else PpcStoreSinglePsqFloatResolvedInline(resolvedHost, offset, addr, value);
    }
    else if constexpr (type == 4u)
    {
        if constexpr (W == 0u && scale == 61u)
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
            else PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
        }
        else
        {
            if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
            else PpcStoreSinglePsqQuantizedResolvedInline<uint8_t>(resolvedHost, offset, addr, value, scale);
        }
    }
    else if constexpr (type == 5u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<uint16_t>(resolvedHost, offset, addr, value, scale);
    }
    else if constexpr (type == 6u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<int8_t>(resolvedHost, offset, addr, value, scale);
    }
    else if constexpr (type == 7u)
    {
if constexpr (W == 0u) PpcStorePairPsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale);
        else PpcStoreSinglePsqQuantizedResolvedInline<int16_t>(resolvedHost, offset, addr, value, scale);
    }
    else
    {
        PPC_PsqSt(addr, value, W, I);
    }
}
