#pragma once

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string_view>
#include <system_error>

namespace RuntimeScSerial {

// SCGetProductSN's output is a u32, not a character buffer. DWC loads
// that word and formats it with the product code to construct csnum.
template <typename RangeValidator, typename WordWriter>
uint32_t Write(std::string_view serial, uint32_t address,
               RangeValidator&& contains, WordWriter&& write32) {
    if (serial.empty() || serial.size() > 9 ||
        serial.find_first_not_of("0123456789") != std::string_view::npos) return 0;
    uint32_t number = 0;
    const auto parsed = std::from_chars(serial.data(), serial.data() + serial.size(), number);
    if (parsed.ec != std::errc{} || parsed.ptr != serial.data() + serial.size() ||
        !address || !contains(address, sizeof(uint32_t))) return 0;
    write32(address, number);
    return 1;
}

} // namespace RuntimeScSerial
