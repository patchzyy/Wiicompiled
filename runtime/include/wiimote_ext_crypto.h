#pragma once

// The Wii's extension controller encryption: the game writes a random key to
// the extension, which then XORs and offsets every byte it reports.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace Wiimote::ExtCrypto {

struct Tables {
    std::array<uint8_t, 8> ft{};
    std::array<uint8_t, 8> sb{};
    void Encrypt(uint8_t* data, uint32_t address, size_t length) const;
    void Decrypt(uint8_t* data, uint32_t address, size_t length) const;
};

// Tables for the key the game wrote to registers 0x40..0x4F, or nullopt when it
// does not come from the first-party key schedule.
std::optional<Tables> FromKey(const std::array<uint8_t, 16>& keyData);

} // namespace Wiimote::ExtCrypto
