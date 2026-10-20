#pragma once

// HCI and L2CAP fields are little-endian.

#include <cstdint>
#include <vector>

namespace Bluetooth {

inline uint16_t Le16(const uint8_t* p) {
    return static_cast<uint16_t>(p[0] | (p[1] << 8));
}

inline void PutLe16(std::vector<uint8_t>& out, uint16_t v) {
    out.push_back(static_cast<uint8_t>(v));
    out.push_back(static_cast<uint8_t>(v >> 8));
}

} // namespace Bluetooth
