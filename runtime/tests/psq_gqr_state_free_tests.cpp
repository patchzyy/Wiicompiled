#include "isa/ppc_isa_quantized.h"

#include <cstdio>
#include <initializer_list>
#include <stdexcept>

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <uint32_t W>
static double ReferenceLoad(uint32_t type, uint32_t scale, uint32_t address) {
    if constexpr (W == 0) {
        switch (type) {
        case 0: return PpcLoadPairPsqFloatFastInline(address);
        case 4: return PpcLoadPairPsqIntegerFastInline<uint8_t>(address, scale);
        case 5: return PpcLoadPairPsqIntegerFastInline<uint16_t>(address, scale);
        case 6: return PpcLoadPairPsqIntegerFastInline<int8_t>(address, scale);
        case 7: return PpcLoadPairPsqIntegerFastInline<int16_t>(address, scale);
        }
    } else {
        switch (type) {
        case 0: return PpcLoadSinglePsqFloatFastInline(address);
        case 4: return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(address, scale);
        case 5: return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(address, scale);
        case 6: return PpcLoadSinglePsqQuantizedFastInline<int8_t>(address, scale);
        case 7: return PpcLoadSinglePsqQuantizedFastInline<int16_t>(address, scale);
        }
    }
    std::abort();
}

template <uint32_t W>
static void ReferenceStore(uint32_t type, uint32_t scale, uint32_t address, double value) {
    if constexpr (W == 0) {
        switch (type) {
        case 0: PpcStorePairPsqFloatFastInline(address, value); return;
        case 4: PpcStorePairPsqQuantizedFastInline<uint8_t>(address, value, scale); return;
        case 5: PpcStorePairPsqQuantizedFastInline<uint16_t>(address, value, scale); return;
        case 6: PpcStorePairPsqQuantizedFastInline<int8_t>(address, value, scale); return;
        case 7: PpcStorePairPsqQuantizedFastInline<int16_t>(address, value, scale); return;
        }
    } else {
        switch (type) {
        case 0: PpcStoreSinglePsqFloatFastInline(address, value); return;
        case 4: PpcStoreSinglePsqQuantizedFastInline<uint8_t>(address, value, scale); return;
        case 5: PpcStoreSinglePsqQuantizedFastInline<uint16_t>(address, value, scale); return;
        case 6: PpcStoreSinglePsqQuantizedFastInline<int8_t>(address, value, scale); return;
        case 7: PpcStoreSinglePsqQuantizedFastInline<int16_t>(address, value, scale); return;
        }
    }
    std::abort();
}

template <uint32_t W, uint32_t I>
static void Check() {
    constexpr uint64_t values[] = {
        0x3F800000C0200000ull, 0x0000000080000000ull,
        0x477FFF00C7000000ull, 0x7FC01234FFC01234ull
    };
    for (uint32_t type : {0u, 4u, 5u, 6u, 7u})
    for (uint32_t scale = 0; scale < 64; ++scale)
    for (uint64_t bits : values) {
        const uint32_t half = (scale << 8) | type;
        const uint32_t loadGqr = (half << 16) | 0x2105u;
        const uint32_t storeGqr = half | 0x21050000u;
        constexpr uint32_t address = 32;
        PsqTestMemory::bytes.fill(0xCD);
        BigEndian::Write64(PsqTestMemory::Pointer(address), bits);
        const auto expected = PpcBitCastToU64Inline(ReferenceLoad<W>(type, scale, address));
        const auto loaded = PpcBitCastToU64Inline(PPC_PsqLStateInline<W, I, false>(loadGqr, address));
        Require(loaded == expected, "state-free GQR load bits");

        const double value = PpcBitCastToDoubleInline(bits);
        PsqTestMemory::bytes.fill(0xCD);
        ReferenceStore<W>(type, scale, address, value);
        const auto expectedBytes = PsqTestMemory::bytes;
        PsqTestMemory::bytes.fill(0xCD);
        PPC_PsqStStateInline<W, I, false>(storeGqr, address, value);
        Require(PsqTestMemory::bytes == expectedBytes, "state-free GQR store bytes and untouched lanes");
    }
}

template <uint32_t I> static void CheckIndex() { Check<0, I>(); Check<1, I>(); }

int main() {
    try {
        Require(TryGetCpuContext() == nullptr, "state-free PSQ needs no current context");
        CheckIndex<0>(); CheckIndex<1>(); CheckIndex<2>(); CheckIndex<3>();
        CheckIndex<4>(); CheckIndex<5>(); CheckIndex<6>(); CheckIndex<7>();
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
