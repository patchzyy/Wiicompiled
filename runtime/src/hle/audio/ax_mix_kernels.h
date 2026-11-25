#pragma once

#include "isa/big_endian.h"

// AX/DSP fixed-point mix kernels: the inner loops of the AXWii command-list mix in ax_mix.cpp.
// Each has a scalar reference and architecture-specific SIMD forms.

#include <cstddef>
#include <cstdint>
#include <cstring>

#if defined(__AVX2__)
#include <immintrin.h>
#define MKW_AX_MIX_AVX2 1
#else
#define MKW_AX_MIX_AVX2 0
#endif

#if !MKW_AX_MIX_AVX2 && defined(__aarch64__) && defined(__ARM_NEON) && !defined(__AARCH64EB__)
#include <arm_neon.h>
#define MKW_AX_MIX_NEON 1
#else
#define MKW_AX_MIX_NEON 0
#endif

namespace AxMixKernels {

// Mix buses retain the low 32 bits, matching the SIMD adds.
inline int32_t AddWrap32(int32_t value, uint32_t addend) {
    const uint32_t bits = static_cast<uint32_t>(value) + addend;
    int32_t result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

inline int16_t ClampToS16(int32_t value) {
    if (value < -0x8000) {
        return static_cast<int16_t>(-0x8000);
    }
    if (value > 0x7fff) {
        return static_cast<int16_t>(0x7fff);
    }
    return static_cast<int16_t>(value);
}

// MixAdd: accumulates a voice into a 32-bit mix bus through a 16-bit volume ramp, remembering the
// last emitted sample as the de-pop value. Returns the ramp after `count` steps; `dpop` is left
// untouched when count == 0, matching the scalar loop.

inline uint16_t MixAddRampScalar(int32_t* out, const int16_t* input, uint32_t count,
                                 uint16_t volume, uint16_t delta, int16_t& dpop) {
    int16_t last = dpop;
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(input[i]) * static_cast<int32_t>(volume)) >> 15;
        const int16_t sample = ClampToS16(scaled);
        out[i] = AddWrap32(out[i], static_cast<uint32_t>(sample));
        volume = static_cast<uint16_t>(volume + delta);
        last = sample;
    }
    dpop = last;
    return volume;
}

#if MKW_AX_MIX_AVX2
inline uint16_t MixAddRampAvx2(int32_t* out, const int16_t* input, uint32_t count,
                               uint16_t volume, uint16_t delta, int16_t& dpop) {
    uint32_t i = 0;
    int16_t last = dpop;
    if (count >= 8) {
        const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i deltaVec = _mm256_set1_epi32(static_cast<int32_t>(delta));
        const __m256i wrapMask = _mm256_set1_epi32(0xFFFF);
        const __m256i clampLow = _mm256_set1_epi32(-0x8000);
        const __m256i clampHigh = _mm256_set1_epi32(0x7FFF);
        const __m256i blockStep = _mm256_set1_epi32(static_cast<int32_t>(delta) * 8);
        // volume + k*delta stays well inside int32 for every count the AX mix
        // uses (<= 96), so the wrap is applied only by the 16-bit mask below -
        // exactly where the scalar loop applies it.
        __m256i ramp = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int32_t>(volume)),
                                        _mm256_mullo_epi32(lanes, deltaVec));
        __m256i lastBlock = _mm256_setzero_si256();
        for (; i + 8 <= count; i += 8) {
            const __m256i samples = _mm256_cvtepi16_epi32(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(input + i)));
            const __m256i vol = _mm256_and_si256(ramp, wrapMask);
            // int16 x uint16 always fits in int32, so the low half of the
            // 32x32 multiply is the exact product.
            __m256i scaled = _mm256_srai_epi32(_mm256_mullo_epi32(samples, vol), 15);
            scaled = _mm256_min_epi32(_mm256_max_epi32(scaled, clampLow), clampHigh);
            const __m256i acc =
                _mm256_loadu_si256(reinterpret_cast<const __m256i*>(out + i));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(out + i),
                                _mm256_add_epi32(acc, scaled));
            lastBlock = scaled;
            ramp = _mm256_add_epi32(ramp, blockStep);
        }
        if (i != 0) {
            last = static_cast<int16_t>(_mm256_extract_epi32(lastBlock, 7));
            volume = static_cast<uint16_t>(
                static_cast<uint32_t>(_mm256_extract_epi32(ramp, 0)));
        }
    }
    for (; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(input[i]) * static_cast<int32_t>(volume)) >> 15;
        const int16_t sample = ClampToS16(scaled);
        out[i] = AddWrap32(out[i], static_cast<uint32_t>(sample));
        volume = static_cast<uint16_t>(volume + delta);
        last = sample;
    }
    dpop = last;
    return volume;
}
#endif

#if MKW_AX_MIX_NEON
inline uint16x4_t Ramp4(uint16_t volume, uint16_t delta) {
    static constexpr uint16_t lanes[] = {0, 1, 2, 3};
    return vmla_n_u16(vdup_n_u16(volume), vld1_u16(lanes), delta);
}

inline int16x4_t ScaleBlock4(int16x4_t samples, uint16x4_t ramp) {
    const int32x4_t gain = vreinterpretq_s32_u32(vmovl_u16(ramp));
    const int32x4_t product = vmulq_s32(vmovl_s16(samples), gain);
    return vqmovn_s32(vshrq_n_s32(product, 15));
}

inline uint16_t MixAddRampNeon(int32_t* out, const int16_t* input, uint32_t count,
                               uint16_t volume, uint16_t delta, int16_t& dpop) {
    uint32_t i = 0;
    int16_t last = dpop;
    if (count >= 4) {
        uint16x4_t ramp = Ramp4(volume, delta);
        const uint16x4_t step = vdup_n_u16(static_cast<uint16_t>(delta * 4u));
        int16x4_t block = vdup_n_s16(0);
        for (; count - i >= 4; i += 4) {
            block = ScaleBlock4(vld1_s16(input + i), ramp);
            vst1q_s32(out + i, vaddw_s16(vld1q_s32(out + i), block));
            ramp = vadd_u16(ramp, step);
        }
        last = vget_lane_s16(block, 3);
        volume = vget_lane_u16(ramp, 0);
    }
    volume = MixAddRampScalar(out + i, input + i, count - i, volume, delta, last);
    dpop = last;
    return volume;
}
#endif

inline uint16_t MixAddRamp(int32_t* out, const int16_t* input, uint32_t count,
                           uint16_t volume, uint16_t delta, int16_t& dpop) {
#if MKW_AX_MIX_AVX2
    return MixAddRampAvx2(out, input, count, volume, delta, dpop);
#elif MKW_AX_MIX_NEON
    return MixAddRampNeon(out, input, count, volume, delta, dpop);
#else
    return MixAddRampScalar(out, input, count, volume, delta, dpop);
#endif
}

// ---------------------------------------------------------------------------
// Voice volume envelope: scale a 16-bit sample block in place through the same
// wrapping 16-bit ramp. Returns the ramp value after `count` steps.
// ---------------------------------------------------------------------------

inline uint16_t ScaleRampScalar(int16_t* samples, uint32_t count, uint16_t volume,
                                uint16_t delta) {
    for (uint32_t i = 0; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(samples[i]) * static_cast<int32_t>(volume)) >> 15;
        samples[i] = ClampToS16(scaled);
        volume = static_cast<uint16_t>(volume + delta);
    }
    return volume;
}

#if MKW_AX_MIX_AVX2
inline uint16_t ScaleRampAvx2(int16_t* samples, uint32_t count, uint16_t volume,
                              uint16_t delta) {
    uint32_t i = 0;
    if (count >= 8) {
        const __m256i lanes = _mm256_setr_epi32(0, 1, 2, 3, 4, 5, 6, 7);
        const __m256i deltaVec = _mm256_set1_epi32(static_cast<int32_t>(delta));
        const __m256i wrapMask = _mm256_set1_epi32(0xFFFF);
        const __m256i blockStep = _mm256_set1_epi32(static_cast<int32_t>(delta) * 8);
        __m256i ramp = _mm256_add_epi32(_mm256_set1_epi32(static_cast<int32_t>(volume)),
                                        _mm256_mullo_epi32(lanes, deltaVec));
        for (; i + 8 <= count; i += 8) {
            const __m256i block = _mm256_cvtepi16_epi32(
                _mm_loadu_si128(reinterpret_cast<const __m128i*>(samples + i)));
            const __m256i vol = _mm256_and_si256(ramp, wrapMask);
            const __m256i scaled =
                _mm256_srai_epi32(_mm256_mullo_epi32(block, vol), 15);
            // Signed 32 -> 16 saturation is exactly clamp(-0x8000, 0x7fff).
            const __m128i packed = _mm_packs_epi32(_mm256_castsi256_si128(scaled),
                                                   _mm256_extracti128_si256(scaled, 1));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(samples + i), packed);
            ramp = _mm256_add_epi32(ramp, blockStep);
        }
        if (i != 0) {
            volume = static_cast<uint16_t>(
                static_cast<uint32_t>(_mm256_extract_epi32(ramp, 0)));
        }
    }
    for (; i < count; ++i) {
        const int32_t scaled =
            (static_cast<int32_t>(samples[i]) * static_cast<int32_t>(volume)) >> 15;
        samples[i] = ClampToS16(scaled);
        volume = static_cast<uint16_t>(volume + delta);
    }
    return volume;
}
#endif

#if MKW_AX_MIX_NEON
inline uint16_t ScaleRampNeon(int16_t* samples, uint32_t count, uint16_t volume,
                              uint16_t delta) {
    uint32_t i = 0;
    if (count >= 4) {
        uint16x4_t ramp = Ramp4(volume, delta);
        const uint16x4_t step = vdup_n_u16(static_cast<uint16_t>(delta * 4u));
        for (; count - i >= 4; i += 4) {
            vst1_s16(samples + i, ScaleBlock4(vld1_s16(samples + i), ramp));
            ramp = vadd_u16(ramp, step);
        }
        volume = vget_lane_u16(ramp, 0);
    }
    return ScaleRampScalar(samples + i, count - i, volume, delta);
}
#endif

inline uint16_t ScaleRamp(int16_t* samples, uint32_t count, uint16_t volume,
                          uint16_t delta) {
#if MKW_AX_MIX_AVX2
    return ScaleRampAvx2(samples, count, volume, delta);
#elif MKW_AX_MIX_NEON
    return ScaleRampNeon(samples, count, volume, delta);
#else
    return ScaleRampScalar(samples, count, volume, delta);
#endif
}

// ---------------------------------------------------------------------------
// AUX return: accumulate a 32-bit aux bus into a 32-bit main bus through a
// per-sample 16-bit ramp table.
// ---------------------------------------------------------------------------

inline void MixAccumRamp32Scalar(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                                 uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
        const int64_t product = static_cast<int64_t>(src[i]) * ramp[i];
        const uint32_t scaled = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 15);
        dst[i] = AddWrap32(dst[i], scaled);
    }
}

#if MKW_AX_MIX_AVX2
inline void MixAccumRamp32Avx2(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                               uint32_t count) {
    uint32_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i source =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        const __m256i gain = _mm256_cvtepu16_epi32(
            _mm_loadu_si128(reinterpret_cast<const __m128i*>(ramp + i)));
        // Even dwords (indices 0,2,4,6) and odd dwords (1,3,5,7) as two sets of
        // four signed 32x32 -> 64 products. |product| < 2^47, so shifting the
        // 64-bit lane logically by 15 leaves bits 15..46 of the product in the
        // low dword - the same bits the scalar (int32)(p >> 15) keeps.
        const __m256i even = _mm256_srli_epi64(_mm256_mul_epi32(source, gain), 15);
        const __m256i odd = _mm256_srli_epi64(
            _mm256_mul_epi32(_mm256_srli_epi64(source, 32), _mm256_srli_epi64(gain, 32)),
            15);
        const __m256i result =
            _mm256_blend_epi32(even, _mm256_slli_epi64(odd, 32), 0xAA);
        const __m256i acc = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(dst + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                            _mm256_add_epi32(acc, result));
    }
    for (; i < count; ++i) {
        const int64_t product = static_cast<int64_t>(src[i]) * ramp[i];
        const uint32_t scaled = static_cast<uint32_t>(static_cast<uint64_t>(product) >> 15);
        dst[i] = AddWrap32(dst[i], scaled);
    }
}
#endif

#if MKW_AX_MIX_NEON
inline void MixAccumRamp32Neon(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                               uint32_t count) {
    uint32_t i = 0;
    for (; count - i >= 4; i += 4) {
        const int32x4_t source = vld1q_s32(src + i);
        const int32x4_t gain = vreinterpretq_s32_u32(vmovl_u16(vld1_u16(ramp + i)));
        const int64x2_t lowProduct = vmull_s32(vget_low_s32(source), vget_low_s32(gain));
        const int64x2_t highProduct = vmull_s32(vget_high_s32(source), vget_high_s32(gain));
        // Keep bits 15..46, including products whose shifted value exceeds int32.
        const int32x4_t scaled = vcombine_s32(vshrn_n_s64(lowProduct, 15),
                                             vshrn_n_s64(highProduct, 15));
        vst1q_s32(dst + i, vaddq_s32(vld1q_s32(dst + i), scaled));
    }
    MixAccumRamp32Scalar(dst + i, src + i, ramp + i, count - i);
}
#endif

inline void MixAccumRamp32(int32_t* dst, const int32_t* src, const uint16_t* ramp,
                           uint32_t count) {
#if MKW_AX_MIX_AVX2
    MixAccumRamp32Avx2(dst, src, ramp, count);
#elif MKW_AX_MIX_NEON
    MixAccumRamp32Neon(dst, src, ramp, count);
#else
    MixAccumRamp32Scalar(dst, src, ramp, count);
#endif
}

// ---------------------------------------------------------------------------
// Guest mix-buffer marshalling: host int32 <-> big-endian guest words.
// ---------------------------------------------------------------------------

inline void StoreBigEndian32Scalar(uint8_t* dst, const int32_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        BigEndian::Write32(dst + i * sizeof(uint32_t), static_cast<uint32_t>(src[i]));
    }
}

inline void LoadBigEndian32Scalar(int32_t* dst, const uint8_t* src, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        dst[i] = static_cast<int32_t>(BigEndian::Read32(src + i * sizeof(uint32_t)));
    }
}

#if MKW_AX_MIX_AVX2
inline __m256i ByteSwapMask32() {
    return _mm256_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12,
                            3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
}

inline void StoreBigEndian32Avx2(uint8_t* dst, const int32_t* src, size_t count) {
    const __m256i mask = ByteSwapMask32();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i value =
            _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + i));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i * sizeof(uint32_t)),
                            _mm256_shuffle_epi8(value, mask));
    }
    StoreBigEndian32Scalar(dst + i * sizeof(uint32_t), src + i, count - i);
}

inline void LoadBigEndian32Avx2(int32_t* dst, const uint8_t* src, size_t count) {
    const __m256i mask = ByteSwapMask32();
    size_t i = 0;
    for (; i + 8 <= count; i += 8) {
        const __m256i value = _mm256_loadu_si256(
            reinterpret_cast<const __m256i*>(src + i * sizeof(uint32_t)));
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(dst + i),
                            _mm256_shuffle_epi8(value, mask));
    }
    LoadBigEndian32Scalar(dst + i, src + i * sizeof(uint32_t), count - i);
}
#endif

#if MKW_AX_MIX_NEON
inline void StoreBigEndian32Neon(uint8_t* dst, const int32_t* src, size_t count) {
    size_t i = 0;
    for (; count - i >= 4; i += 4) {
        const uint8x16_t bytes = vreinterpretq_u8_s32(vld1q_s32(src + i));
        vst1q_u8(dst + i * sizeof(uint32_t), vrev32q_u8(bytes));
    }
    StoreBigEndian32Scalar(dst + i * sizeof(uint32_t), src + i, count - i);
}

inline void LoadBigEndian32Neon(int32_t* dst, const uint8_t* src, size_t count) {
    size_t i = 0;
    for (; count - i >= 4; i += 4) {
        const uint8x16_t bytes = vld1q_u8(src + i * sizeof(uint32_t));
        vst1q_s32(dst + i, vreinterpretq_s32_u8(vrev32q_u8(bytes)));
    }
    LoadBigEndian32Scalar(dst + i, src + i * sizeof(uint32_t), count - i);
}
#endif

inline void StoreBigEndian32(uint8_t* dst, const int32_t* src, size_t count) {
#if MKW_AX_MIX_AVX2
    StoreBigEndian32Avx2(dst, src, count);
#elif MKW_AX_MIX_NEON
    StoreBigEndian32Neon(dst, src, count);
#else
    StoreBigEndian32Scalar(dst, src, count);
#endif
}

inline void LoadBigEndian32(int32_t* dst, const uint8_t* src, size_t count) {
#if MKW_AX_MIX_AVX2
    LoadBigEndian32Avx2(dst, src, count);
#elif MKW_AX_MIX_NEON
    LoadBigEndian32Neon(dst, src, count);
#else
    LoadBigEndian32Scalar(dst, src, count);
#endif
}

} // namespace AxMixKernels
