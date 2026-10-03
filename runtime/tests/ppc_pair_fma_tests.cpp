#include "isa/ppc_isa_float.h"

#include <cmath>
#include <cstdint>
#include <cstdio>

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

}  // namespace

int main()
{
    const double a = PpcPackPairedInline(1.000000119f, -123.25f);
    const double c = PpcPackPairedInline(33554431.0f, 0.0625f);
    const double b = PpcPackPairedInline(-33554430.0f, 7.75f);

    bool ok = true;
    ok &= CheckPair(PPC_PsMaddNoNiInline(a, c, b), 1.000000119f, -123.25f,
                    33554431.0f, 0.0625f, -33554430.0f, 7.75f, false, "ps_madd");
    ok &= CheckPair(PPC_PsMsubNoNiInline(a, c, b), 1.000000119f, -123.25f,
                    33554431.0f, 0.0625f, -33554430.0f, 7.75f, true, "ps_msub");
    ok &= CheckNegatedPair(PPC_PsNmaddInline(a, c, b), 1.000000119f, -123.25f,
                           33554431.0f, 0.0625f, -33554430.0f, 7.75f, false, "ps_nmadd");
    ok &= CheckNegatedPair(PPC_PsNmsubNoNiInline(a, c, b), 1.000000119f, -123.25f,
                           33554431.0f, 0.0625f, -33554430.0f, 7.75f, true, "ps_nmsub");
    return ok ? 0 : 1;
}
