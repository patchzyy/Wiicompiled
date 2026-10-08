#include "prs.h"

namespace sonic {

bool prsDecompress(const u8* src, size_t len, std::vector<u8>& out) {
    out.clear();
    size_t pos = 0;
    int bitsLeft = 0;
    u8 ctrl = 0;
    auto getBit = [&]() -> int {
        if (bitsLeft == 0) {
            if (pos >= len) return -1;
            ctrl = src[pos++];
            bitsLeft = 8;
        }
        int b = ctrl & 1;
        ctrl >>= 1;
        bitsLeft--;
        return b;
    };
    for (;;) {
        int b = getBit();
        if (b < 0) return false;
        if (b) {
            if (pos >= len) return false;
            out.push_back(src[pos++]);
            continue;
        }
        int offset, size;
        b = getBit();
        if (b < 0) return false;
        if (b) {
            if (pos + 1 >= len) return false;
            u16 v = u16(src[pos] | (src[pos + 1] << 8));
            pos += 2;
            if (v == 0) break;  // end of stream
            size = v & 7;
            offset = (v >> 3) | -0x2000;
            if (size == 0) {
                if (pos >= len) return false;
                size = src[pos++] + 1;
            } else {
                size += 2;
            }
        } else {
            int b1 = getBit(), b2 = getBit();
            if (b1 < 0 || b2 < 0) return false;
            size = ((b1 << 1) | b2) + 2;
            if (pos >= len) return false;
            offset = int(src[pos++]) | -0x100;
        }
        size_t start = out.size();
        if (size_t(-offset) > start) return false;
        for (int i = 0; i < size; i++) out.push_back(out[start + offset + i]);
    }
    return true;
}

}  // namespace sonic
