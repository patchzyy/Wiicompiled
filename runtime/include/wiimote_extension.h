#pragma once

// Follows the game's conversation with a remote's extension (type, calibration,
// encryption key) so its data can be put at rest while the overlay owns input.

#include "wiimote_ext_crypto.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace Wiimote {

class ExtensionState {
public:
    void ObserveOutput(const uint8_t* report, size_t size);
    void ObserveInput(const uint8_t* report, size_t size);
    // Centres the sticks and releases the buttons of the extension data in a
    // data report, leaving its motion data alone.
    void Neutralize(std::vector<uint8_t>& report) const;

private:
    enum class Type : uint8_t { Unknown, Nunchuk, Classic };

    void Reset();
    uint8_t Calibration(size_t index, uint8_t fallback) const;

    Type m_type = Type::Unknown;
    uint8_t m_format = 0;
    bool m_encrypted = false;
    std::array<uint8_t, 16> m_key{};
    std::optional<ExtCrypto::Tables> m_tables;
    std::array<uint8_t, 16> m_calibration{};
    bool m_haveCalibration = false;
    uint32_t m_readAddress = 0;
};

} // namespace Wiimote
