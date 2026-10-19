#include "isa/ppc_isa_quantized.h"

#if defined(__FAST_MATH__) || __FINITE_MATH_ONLY__
#error "PSQ helpers require strict PPC floating-point options"
#endif

template <uint32_t W, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD double PPC_PsqLStateFallback(uint32_t gqr, uint32_t addr)
{
    static_assert(W <= 1u);
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

template <uint32_t W, bool Stack>
MKW_PPC_NO_INLINE MKW_PPC_COLD void PPC_PsqStStateFallback(uint32_t gqr, uint32_t addr, double value)
{
    static_assert(W <= 1u);
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

template <uint32_t W>
MKW_PPC_NO_INLINE MKW_PPC_COLD double PPC_PsqLResolvedStateFallback(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr)
{
    static_assert(W <= 1u);
    if (!resolvedHost) [[unlikely]] return PPC_PsqLStateInline<W, 0u, false>(gqr, addr);
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

template <uint32_t W>
MKW_PPC_NO_INLINE MKW_PPC_COLD void PPC_PsqStResolvedStateFallback(
    uint32_t gqr, uint8_t* resolvedHost, uint32_t offset, uint32_t addr, double value)
{
    static_assert(W <= 1u);
    if (!resolvedHost) [[unlikely]]
    {
        PPC_PsqStStateInline<W, 0u, false>(gqr, addr, value);
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

template double PPC_PsqLStateFallback<0u, false>(uint32_t, uint32_t);
template void PPC_PsqStStateFallback<0u, false>(uint32_t, uint32_t, double);
template double PPC_PsqLStateFallback<0u, true>(uint32_t, uint32_t);
template void PPC_PsqStStateFallback<0u, true>(uint32_t, uint32_t, double);
template double PPC_PsqLResolvedStateFallback<0u>(uint32_t, uint8_t*, uint32_t, uint32_t);
template void PPC_PsqStResolvedStateFallback<0u>(uint32_t, uint8_t*, uint32_t, uint32_t, double);
template double PPC_PsqLStateFallback<1u, false>(uint32_t, uint32_t);
template void PPC_PsqStStateFallback<1u, false>(uint32_t, uint32_t, double);
template double PPC_PsqLStateFallback<1u, true>(uint32_t, uint32_t);
template void PPC_PsqStStateFallback<1u, true>(uint32_t, uint32_t, double);
template double PPC_PsqLResolvedStateFallback<1u>(uint32_t, uint8_t*, uint32_t, uint32_t);
template void PPC_PsqStResolvedStateFallback<1u>(uint32_t, uint8_t*, uint32_t, uint32_t, double);
