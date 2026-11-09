#include "hle_stubs.h"

#include <cstdint>
#include "memory.h"
#include "runtime_log.h"

// Native because a crafted Yaz0 run writes past the caller's buffer
// (github.com/vabold/szsHaxx)
// https://github.com/vabold/Kinoko/blob/main/source/egg/core/Decomp.cc

extern "C" uint32_t EGG_Decomp_decodeSZS_80218c2c(uint32_t src, uint32_t dst)
{
    const uint32_t expandSize = (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 4)) << 24) |
                                (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 5)) << 16) |
                                (static_cast<uint32_t>(MemoryInline::FlatRead8(src + 6)) << 8) |
                                static_cast<uint32_t>(MemoryInline::FlatRead8(src + 7));

    uint32_t srcIdx = 16;
    uint32_t dstIdx = 0;
    uint32_t mask = 0;
    uint32_t flags = 0;

    while (static_cast<int32_t>(dstIdx) < static_cast<int32_t>(expandSize)) {
        if (mask == 0) {
            flags = MemoryInline::FlatRead8(src + srcIdx++);
            mask = 0x80;
        }

        if ((flags & mask) != 0) {
            MemoryInline::FlatWrite8(dst + dstIdx++, MemoryInline::FlatRead8(src + srcIdx++));
        } else {
            const uint32_t high = MemoryInline::FlatRead8(src + srcIdx);
            const uint32_t low = MemoryInline::FlatRead8(src + srcIdx + 1);
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
                        : static_cast<uint32_t>(MemoryInline::FlatRead8(src + srcIdx++)) + 18;

            for (uint32_t i = 0; i < count; ++i) {
                if (dstIdx >= expandSize) {
                    RT_LOG(RT_TAG_HLE) << "decodeSZS: malformed stream from 0x" << std::hex << src
                                       << std::dec << ", output overran " << expandSize << " bytes"
                                       << std::endl;
                    ShowRuntimeFatalPopup("corrupt compressed file",
                                          "The game stopped decoding a malformed Yaz0 file.");
                    std::abort();
                }
                MemoryInline::FlatWrite8(dst + dstIdx++, MemoryInline::FlatRead8(dst + copyIdx++));
            }
        }

        mask >>= 1;
    }

    return expandSize;
}

PPC_NATIVE_OVERRIDE(80218C2C, EGG_Decomp_decodeSZS_80218c2c, uint32_t,
                    (uint32_t src, uint32_t dst), (src, dst));
