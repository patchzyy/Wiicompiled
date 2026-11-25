#include "isa/ppc_isa_float.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>

#if defined(__x86_64__)
#include <xmmintrin.h>
#endif

namespace {

bool SameBits(double lhs, double rhs)
{
    return PpcBitCastToU64Inline(lhs) == PpcBitCastToU64Inline(rhs);
}

bool CheckPair(double actual, float a0, float a1, float c0, float c1, float b0, float b1,
               bool subtract, const char* operation)
{
    const float expected0 = subtract ? std::fma(a0, c0, -b0) : std::fma(a0, c0, b0);
    const float expected1 = subtract ? std::fma(a1, c1, -b1) : std::fma(a1, c1, b1);
    const double expected = PpcPackPairedInline(expected0, expected1);
    if (SameBits(actual, expected)) {
        return true;
    }
    std::fprintf(stderr, "%s produced the wrong paired lanes\n", operation);
    return false;
}

bool CheckNegatedPair(double actual, float a0, float a1, float c0, float c1, float b0, float b1,
                      bool subtract, const char* operation)
{
    const float expected0 = -std::fma(a0, c0, subtract ? -b0 : b0);
    const float expected1 = -std::fma(a1, c1, subtract ? -b1 : b1);
    const double expected = PpcPackPairedInline(expected0, expected1);
    if (SameBits(actual, expected)) {
        return true;
    }
    std::fprintf(stderr, "%s produced the wrong paired lanes\n", operation);
    return false;
}

bool CheckPairedFamily(float a0, float a1, float c0, float c1, float b0, float b1)
{
    const double a = PpcPackPairedInline(a0, a1);
    const double c = PpcPackPairedInline(c0, c1);
    const double b = PpcPackPairedInline(b0, b1);

    bool ok = true;
    ok &= CheckPair(PPC_PsMaddNoNiInline(a, c, b), a0, a1, c0, c1, b0, b1, false, "ps_madd");
    ok &= CheckPair(PPC_PsMsubNoNiInline(a, c, b), a0, a1, c0, c1, b0, b1, true, "ps_msub");
    ok &= CheckNegatedPair(PPC_PsNmaddInline(a, c, b), a0, a1, c0, c1, b0, b1, false, "ps_nmadd");
    ok &= CheckNegatedPair(PPC_PsNmsubNoNiInline(a, c, b), a0, a1, c0, c1, b0, b1, true,
                           "ps_nmsub");
    return ok;
}

bool CheckFmaResult(float actual, float expected, float a, float c, float b, const char* path,
                    const char* context)
{
    if (PpcBitCastToU32Inline(actual) == PpcBitCastToU32Inline(expected)) {
        return true;
    }
    std::fprintf(stderr,
                 "%s %s: fma(%a, %a, %a) = %a (0x%08x), expected %a (0x%08x)\n", context, path,
                 static_cast<double>(a), static_cast<double>(c), static_cast<double>(b),
                 static_cast<double>(actual), PpcBitCastToU32Inline(actual),
                 static_cast<double>(expected), PpcBitCastToU32Inline(expected));
    return false;
}

// The double-based path is what a CPU without FMA3 takes; check it directly so hosts that have
// FMA3 (and dispatch to std::fma) still cover it.
bool CheckScalarFma(float a, float c, float b, const char* context)
{
    const float expected = std::fma(a, c, b);
    bool ok = CheckFmaResult(PpcFusedMultiplyAddFloatViaDoubleInline(a, c, b), expected, a, c, b,
                             "via double", context);
    ok &= CheckFmaResult(PpcFusedMultiplyAddFloatInline(a, c, b), expected, a, c, b, "dispatched",
                         context);
    return ok;
}

// a*c = 2^6 - 2^-40 sits just below the float halfway point between b and the next float, so the
// fused result is b. Rounded to double first, the sum lands exactly on that halfway point and
// ties-to-even then picks the next float, which is what a plain double evaluation returns.
bool CheckHalfwayDoubleRounding()
{
    const float a = 1.0f + std::ldexp(1.0f, -23);
    const float c = 64.0f * (1.0f - std::ldexp(1.0f, -23));
    const float b = std::ldexp(1.0f, 30) + std::ldexp(1.0f, 7);

    const float fused = std::fma(a, c, b);
    const float viaDouble =
        static_cast<float>(static_cast<double>(a) * static_cast<double>(c) + static_cast<double>(b));
    if (fused != b || viaDouble == fused) {
        std::fprintf(stderr, "halfway case no longer exercises double rounding\n");
        return false;
    }

    bool ok = CheckScalarFma(a, c, b, "halfway double rounding");
    ok &= CheckScalarFma(-a, c, -b, "halfway double rounding (negated)");
    ok &= CheckPairedFamily(a, -a, c, c, b, -b);
    return ok;
}

// FLT_MIN - 3*2^-150 + 3*2^-190 lies just above the halfway point between the subnormals
// 0x007FFFFE and 0x007FFFFF. Double rounding drops the 2^-190 term, lands on that halfway point
// and ties to the even 0x007FFFFE. The normal-float halfway check cannot see this: the subnormal
// halfway bit sits one position higher in the double.
bool CheckSubnormalHalfwayDoubleRounding()
{
    const float a = -std::ldexp(3.0f + std::ldexp(3.0f, -20), -75);
    const float c = std::ldexp(1.0f - std::ldexp(1.0f, -20), -75);
    const float b = std::numeric_limits<float>::min();

    const float fused = std::fma(a, c, b);
    const float viaDouble =
        static_cast<float>(static_cast<double>(a) * static_cast<double>(c) + static_cast<double>(b));
    if (PpcBitCastToU32Inline(fused) != 0x007FFFFFu || viaDouble == fused) {
        std::fprintf(stderr, "subnormal halfway case no longer exercises double rounding\n");
        return false;
    }

    bool ok = CheckScalarFma(a, c, b, "subnormal halfway double rounding");
    ok &= CheckScalarFma(-a, c, -b, "subnormal halfway double rounding (negated)");
    return ok;
}

bool CheckEdgeValues(const char* mode)
{
    constexpr float kInf = std::numeric_limits<float>::infinity();
    const std::array<float, 16> values{
        0.0f,
        -0.0f,
        1.0f,
        -1.0f,
        0.5f,
        1.0f + std::numeric_limits<float>::epsilon(),
        std::numeric_limits<float>::min(),
        -std::numeric_limits<float>::min(),
        std::numeric_limits<float>::denorm_min(),
        -std::numeric_limits<float>::min() * 0.75f,
        std::numeric_limits<float>::max(),
        -std::numeric_limits<float>::max(),
        std::ldexp(1.0f, -63),
        std::ldexp(1.0f, 64),
        kInf,
        -kInf,
    };

    bool ok = true;
    for (const float a : values) {
        for (const float c : values) {
            for (const float b : values) {
                ok &= CheckScalarFma(a, c, b, mode);
            }
        }
    }
    return ok;
}

struct SplitMix64 {
    uint64_t state;

    uint64_t Next()
    {
        uint64_t z = (state += 0x9E3779B97F4A7C15ULL);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
        return z ^ (z >> 31);
    }
};

float RandomFloat(SplitMix64& rng)
{
    const uint64_t r = rng.Next();
    if ((r & 3u) == 0) {
        // Any bit pattern: NaNs, infinities, subnormals and extreme exponents.
        return PpcBitCastToFloatInline(static_cast<uint32_t>(r >> 32));
    }
    // Ordinary magnitudes, where the fast path does its work.
    const uint32_t sign = static_cast<uint32_t>(r >> 63) << 31;
    const uint32_t exponent = 127u - 40u + static_cast<uint32_t>((r >> 40) % 81u);
    const uint32_t mantissa = static_cast<uint32_t>(r >> 8) & 0x007FFFFFu;
    return PpcBitCastToFloatInline(sign | (exponent << 23) | mantissa);
}

bool CheckRandomized(const char* mode)
{
    SplitMix64 rng{0x5EED0F7A11ULL};
    int failures = 0;
    for (int i = 0; i < 2'000'000 && failures < 8; ++i) {
        const float a = RandomFloat(rng);
        const float c = RandomFloat(rng);
        float b = RandomFloat(rng);
        if ((rng.Next() & 7u) == 0) {
            // Near-total cancellation against the exact product.
            b = -static_cast<float>(static_cast<double>(a) * static_cast<double>(c));
        }
        if (!CheckScalarFma(a, c, b, mode)) {
            ++failures;
        }
    }
    return failures == 0;
}

// The runtime models Gekko NI mode as MXCSR FTZ+DAZ (MkwApplyHostNiMode).
struct FlushToZeroScope {
#if defined(__x86_64__)
    explicit FlushToZeroScope(bool enable) : saved(_mm_getcsr())
    {
        constexpr unsigned kFtzDaz = 0x8040u;
        _mm_setcsr(enable ? (saved | kFtzDaz) : (saved & ~kFtzDaz));
    }
    ~FlushToZeroScope() { _mm_setcsr(saved); }
    unsigned saved;
#else
    explicit FlushToZeroScope(bool) {}
#endif
};

bool CheckAgainstReference(const char* reference)
{
    bool ok = CheckPairedFamily(1.000000119f, -123.25f, 33554431.0f, 0.0625f, -33554430.0f, 7.75f);
    ok &= CheckHalfwayDoubleRounding();
    ok &= CheckSubnormalHalfwayDoubleRounding();

    for (const bool flushToZero : {false, true}) {
        FlushToZeroScope scope(flushToZero);
        char mode[64];
        std::snprintf(mode, sizeof(mode), "%s, %s", reference, flushToZero ? "FTZ/DAZ" : "IEEE");
        ok &= CheckEdgeValues(mode);
        ok &= CheckRandomized(mode);
    }
    return ok;
}

}  // namespace

#if defined(_WIN32) && defined(__x86_64__)
extern "C" __declspec(dllimport) int _set_FMA3_enable(int flag);
#endif

int main()
{
    bool ok = CheckAgainstReference("CRT default");
#if defined(_WIN32) && defined(__x86_64__)
    // A CPU without FMA3 gets the CRT's software fmaf as std::fma; compare against that too.
    _set_FMA3_enable(0);
    ok &= CheckAgainstReference("CRT software fmaf");
    _set_FMA3_enable(1);
#endif
    return ok ? 0 : 1;
}
