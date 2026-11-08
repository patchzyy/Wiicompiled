#include "hle_stubs.h"

#include <cstdint>
#include <limits>
#include "memory.h"
#include "recomp_mod_loader.h"
#include "runtime_log.h"

extern "C" void GxNotifyGuestRamDmaWrite(uint32_t addr, uint32_t size);

// Native because a crafted Yaz0 run writes past the caller's buffer
// (github.com/vabold/szsHaxx)
// https://github.com/vabold/Kinoko/blob/main/source/egg/core/Decomp.cc

template <typename Access>
static uint32_t DecodeSZS(uint32_t src, uint32_t expandSize, Access& access) {
    uint32_t srcIdx = 16;
    uint32_t dstIdx = 0;
    uint32_t mask = 0;
    uint32_t flags = 0;

    while (static_cast<int32_t>(dstIdx) < static_cast<int32_t>(expandSize)) {
        if (mask == 0) {
            flags = access.ReadSource(srcIdx++);
            mask = 0x80;
        }

        if ((flags & mask) != 0) {
            access.WriteOutput(dstIdx++, access.ReadSource(srcIdx++));
        } else {
            const uint32_t high = access.ReadSource(srcIdx);
            const uint32_t low = access.ReadSource(srcIdx + 1);
            srcIdx += 2;

            const uint32_t rep = (high << 8) | low;
            // Without this check dstIdx - distance underflows and the
            // copy leaks guest memory from before the destination buffer.
            const uint32_t distance = (rep & 0xFFF) + 1;
            if (distance > dstIdx) {
                RT_LOG(RT_TAG_HLE) << "decodeSZS: malformed stream from 0x" << std::hex << src
                                   << std::dec << ", back-reference before output" << std::endl;
                ShowRuntimeFatalPopup("corrupt compressed file",
                                      "The game stopped decoding a malformed Yaz0 file.");
                std::abort();
            }
            uint32_t copyIdx = dstIdx - distance;
            uint32_t count = rep >> 12;
            count = count != 0
                        ? count + 2
                        : static_cast<uint32_t>(access.ReadSource(srcIdx++)) + 18;

            for (uint32_t i = 0; i < count; ++i) {
                if (dstIdx >= expandSize) {
                    RT_LOG(RT_TAG_HLE) << "decodeSZS: malformed stream from 0x" << std::hex << src
                                       << std::dec << ", output overran " << expandSize << " bytes"
                                       << std::endl;
                    ShowRuntimeFatalPopup("corrupt compressed file",
                                          "The game stopped decoding a malformed Yaz0 file.");
                    std::abort();
                }
                access.WriteOutput(dstIdx++, access.ReadOutput(copyIdx++));
            }
        }

        mask >>= 1;
    }

    return expandSize;
}

struct GuestSZSAccess {
    uint32_t src;
    uint32_t dst;

    uint8_t ReadSource(uint32_t offset) const { return MemoryInline::FlatRead8(src + offset); }
    uint8_t ReadOutput(uint32_t offset) const { return MemoryInline::FlatRead8(dst + offset); }
    void WriteOutput(uint32_t offset, uint8_t value) const {
        MemoryInline::FlatWrite8(dst + offset, value);
    }
};

struct HostSZSAccess {
    const uint8_t* src;
    uint8_t* dst;

    uint8_t ReadSource(uint32_t offset) const { return src[offset]; }
    uint8_t ReadOutput(uint32_t offset) const { return dst[offset]; }
    void WriteOutput(uint32_t offset, uint8_t value) const { dst[offset] = value; }
};

static bool HasDeferredReadPages(uint32_t address, size_t length) {
    const uint32_t first = address >> MemoryInline::kPageShift;
    const uint32_t last = static_cast<uint32_t>(
        (static_cast<uint64_t>(address) + length - 1) >> MemoryInline::kPageShift);
    for (uint32_t page = first; page <= last; ++page) {
        if (MemoryInline::g_deferredReadCoveredPages[page] != 0) return true;
    }
    return false;
}

extern "C" uint32_t EGG_Decomp_decodeSZS_80218c2c(uint32_t src, uint32_t dst)
{
    const uint32_t expandSize = (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 4)) << 24) |
                                (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 5)) << 16) |
                                (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 6)) << 8) |
                                static_cast<uint32_t>(MemoryInline::FlatRead8(src + 7));

    // A token produces at least one byte; all-literal data is the largest
    // valid input (one flag per eight output bytes). Three extra bytes cover
    // the final malformed run before its output-overrun check aborts.
    const size_t maxSourceBytes = 16ull + expandSize + (static_cast<uint64_t>(expandSize) + 7) / 8 + 3;
    if (expandSize != 0 && expandSize <= static_cast<uint32_t>(std::numeric_limits<int32_t>::max()) &&
        !GuestFlat::RequiresCheckedAccess() &&
        Memory::Contains(src, maxSourceBytes) && Memory::Contains(dst, expandSize) &&
        !HasDeferredReadPages(src, maxSourceBytes) && !HasDeferredReadPages(dst, expandSize) &&
        !RecompMod::ExecutableWriteGuardMayHit(dst, expandSize)) {
        HostSZSAccess access{Memory::GetPointer(src, maxSourceBytes), Memory::GetPointer(dst, expandSize)};
        const uint32_t decoded = DecodeSZS(src, expandSize, access);
        GxNotifyGuestRamDmaWrite(dst, decoded);
        return decoded;
    }

    GuestSZSAccess access{src, dst};
    return DecodeSZS(src, expandSize, access);
}

PPC_NATIVE_OVERRIDE(80218C2C, EGG_Decomp_decodeSZS_80218c2c, uint32_t,
                    (uint32_t src, uint32_t dst), (src, dst));
