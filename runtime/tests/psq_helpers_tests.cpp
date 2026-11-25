#include "isa/ppc_isa_quantized.h"

#include <csignal>
#include <iostream>
#include <stdexcept>
#include <string>

static uint64_t checksum = 0;
static unsigned checks = 0;
static void Require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
static void Hash(uint64_t value) { checksum = (checksum ^ value) * 1099511628211ull; }

template <uint32_t W> static double ReferenceLoad(uint32_t type, uint32_t scale, uint32_t addr) {
    if constexpr (W == 0u) {
        switch (type) {
        case 0: return PpcLoadPairPsqFloatFastInline(addr);
        case 4: return PpcLoadPairPsqIntegerFastInline<uint8_t>(addr, scale);
        case 5: return PpcLoadPairPsqIntegerFastInline<uint16_t>(addr, scale);
        case 6: return PpcLoadPairPsqIntegerFastInline<int8_t>(addr, scale);
        case 7: return PpcLoadPairPsqIntegerFastInline<int16_t>(addr, scale);
        }
    } else {
        switch (type) {
        case 0: return PpcLoadSinglePsqFloatFastInline(addr);
        case 4: return PpcLoadSinglePsqQuantizedFastInline<uint8_t>(addr, scale);
        case 5: return PpcLoadSinglePsqQuantizedFastInline<uint16_t>(addr, scale);
        case 6: return PpcLoadSinglePsqQuantizedFastInline<int8_t>(addr, scale);
        case 7: return PpcLoadSinglePsqQuantizedFastInline<int16_t>(addr, scale);
        }
    }
    std::abort();
}
template <uint32_t W> static void ReferenceStore(uint32_t type, uint32_t scale, uint32_t addr, double value) {
    if constexpr (W == 0u) {
        switch (type) {
        case 0: PpcStorePairPsqFloatFastInline(addr, value); return;
        case 4: PpcStorePairPsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5: PpcStorePairPsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6: PpcStorePairPsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7: PpcStorePairPsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        }
    } else {
        switch (type) {
        case 0: PpcStoreSinglePsqFloatFastInline(addr, value); return;
        case 4: PpcStoreSinglePsqQuantizedFastInline<uint8_t>(addr, value, scale); return;
        case 5: PpcStoreSinglePsqQuantizedFastInline<uint16_t>(addr, value, scale); return;
        case 6: PpcStoreSinglePsqQuantizedFastInline<int8_t>(addr, value, scale); return;
        case 7: PpcStoreSinglePsqQuantizedFastInline<int16_t>(addr, value, scale); return;
        }
    }
    std::abort();
}

template <uint32_t W, uint32_t I> static void Check() {
    constexpr uint32_t edges[] = {
        0, 0x80000000u, 1, 0x80000001u, 0x007FFFFFu, 0x00800000u,
        0x3F000000u, 0xBF000000u, 0x3F800000u, 0xBF800000u,
        0x437F0000u, 0x477FFF00u, 0xC7000000u, 0x7F7FFFFFu,
        0x7F800000u, 0xFF800000u, 0x7F800001u, 0x7FC01234u, 0xFFC01234u
    };
    for (uint32_t type : {0u, 4u, 5u, 6u, 7u}) for (uint32_t scale = 0; scale < 64; ++scale)
    for (unsigned n = 0; n < std::size(edges); ++n) {
        // Include ignored GQR bits and a different type/scale in the unused half.
        const uint32_t half = (scale << 8) | type | 0xC0F8u;
        const uint32_t loadGqr = (half << 16) | 0x2105u;
        const uint32_t storeGqr = half | 0x21050000u;
        const uint64_t raw = (uint64_t(edges[n]) << 32) | edges[(n + 7) % std::size(edges)];
        const double value = PpcBitCastToDoubleInline(raw);
        for (unsigned mode = 0; mode < 5; ++mode) {
            const uint32_t addr = mode == 0 ? 32u : mode == 1 ? 0xCC008000u : mode == 2 ? 0u : 0xFFFFFF00u;
            constexpr uint32_t offset = 17;
            PsqTestMemory::directStack = mode == 4;
            PsqTestMemory::bytes.fill(0xCD);
            BigEndian::Write64(PsqTestMemory::Pointer(addr), raw);
            const auto expected = PpcBitCastToU64Inline(ReferenceLoad<W>(type, scale, addr));
            PsqTestMemory::accesses = 0;
            double loaded;
            if (mode == 0) loaded = PPC_PsqLResolvedStateInline<W, I>(loadGqr, PsqTestMemory::Pointer(addr) - offset, offset, addr);
            else if (mode == 1) loaded = PPC_PsqLResolvedStateInline<W, I>(loadGqr, nullptr, offset, addr);
            else if (mode == 2) loaded = PPC_PsqLStateInline<W, I, false>(loadGqr, addr);
            else loaded = PPC_PsqLStateInline<W, I, true>(loadGqr, addr);
            Require(PpcBitCastToU64Inline(loaded) == expected, "load bits / lane order");
            Hash(expected);
            const size_t width = (type == 0 ? 4u : (type == 4 || type == 6) ? 1u : 2u) * (2u - W);
            Require(PsqTestMemory::accesses == ((mode == 0 || mode == 4) ? 0u : 1u), "load route");
            if (PsqTestMemory::accesses) Require(PsqTestMemory::address == addr && PsqTestMemory::width == width, "load slow address / width");

            PsqTestMemory::bytes.fill(0xCD);
            ReferenceStore<W>(type, scale, addr, value);
            const auto expectedBytes = PsqTestMemory::bytes;
            PsqTestMemory::bytes.fill(0xCD);
            PsqTestMemory::accesses = 0;
            if (mode == 0) PPC_PsqStResolvedStateInline<W, I>(storeGqr, PsqTestMemory::Pointer(addr) - offset, offset, addr, value);
            else if (mode == 1) PPC_PsqStResolvedStateInline<W, I>(storeGqr, nullptr, offset, addr, value);
            else if (mode == 2) PPC_PsqStStateInline<W, I, false>(storeGqr, addr, value);
            else PPC_PsqStStateInline<W, I, true>(storeGqr, addr, value);
            Require(PsqTestMemory::bytes == expectedBytes, "store bits / untouched lanes");
            Hash(PsqTestMemory::ReadHost<uint64_t>(PsqTestMemory::Pointer(addr)));
            Require(PsqTestMemory::accesses == ((mode == 0 || mode == 4) ? 0u : 1u), "store route");
            if (PsqTestMemory::accesses) Require(PsqTestMemory::address == addr && PsqTestMemory::width == width, "store slow address / width");
        }
    }
}

int main(int argc, char** argv) {
    try {
        if (argc > 1) {
            std::signal(SIGABRT, [](int) { std::_Exit(86); });
            const uint32_t type = static_cast<uint32_t>(std::stoul(argv[2]));
            const bool single = std::string(argv[3]) == "1";
            uint8_t* host = std::string(argv[4]) == "resolved" ? PsqTestMemory::bytes.data() : nullptr;
            if (std::string(argv[1]) == "load") {
                if (single) PPC_PsqLResolvedStateInline<1, 7>(type << 16, host, 0, 0);
                else PPC_PsqLResolvedStateInline<0, 7>(type << 16, host, 0, 0);
            } else {
                if (single) PPC_PsqStResolvedStateInline<1, 7>(type, host, 0, 0, 0);
                else PPC_PsqStResolvedStateInline<0, 7>(type, host, 0, 0, 0);
            }
            return 0;
        }
        const auto saved = MkwGetHostFpControl();
        CpuContext ctx{};
        for (uint32_t ni : {0u, 4u}) for (uint32_t rounding = 0; rounding < 4; ++rounding) {
            ctx.fpscr = ni;
            CpuContextScope scope(&ctx);
#if defined(__x86_64__)
            MkwSetHostFpControl((MkwGetHostFpControl() & ~(3u << 13)) | (rounding << 13));
#endif
            Check<0, 0>(); Check<1, 0>(); Check<0, 7>(); Check<1, 7>();
        }
        MkwRestoreHostMxcsr(saved);
        std::cout << "passed " << checks << " checks, checksum " << std::hex << checksum << '\n';
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
