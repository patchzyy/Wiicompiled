#pragma once

#include "nand_path.h"
#include "nand_settings.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>

namespace RuntimeConsoleIdentity {

struct Identity {
    std::string serial;
    std::string productCode;
    std::string area;
    std::string gameRegion;
    std::array<uint8_t, 6> mac;
};

inline Identity FromSerial(std::string serial) {
    // Keep Nintendo's Wii OUI. The suffix is derived from the NAND serial
    // so every API exposes one coherent, stable virtual-console identity.
    uint32_t hash = 2166136261u;
    for (const unsigned char value : serial) {
        hash ^= value;
        hash *= 16777619u;
    }
    uint32_t suffix = hash & 0x00FFFFFFu;
    if (suffix == 0 || suffix == 0x00FFFFFFu) {
        suffix ^= 0x005A17C3u;
    }

    return {
        std::move(serial),
        {}, {}, {},
        {
            0x00,
            0x09,
            0xBF,
            static_cast<uint8_t>(suffix >> 16),
            static_cast<uint8_t>(suffix >> 8),
            static_cast<uint8_t>(suffix),
        },
    };
}

inline Identity LoadFromNand() {
    const auto root = RuntimeNandPath::DiscoverNandRootPath();
    const auto settings = RuntimeNandSettings::Read(root);
    if (!settings || !RuntimeNandSettings::HasIdentity(*settings)) {
        RuntimeNandPath::FailNandRoot(
            "NAND setting.txt is missing or has invalid console identity fields (SERNO, CODE, AREA, GAME)",
            root / "title/00000001/00000002/data/setting.txt");
    }
    Identity identity = FromSerial(settings->at("SERNO"));
    identity.productCode = settings->at("CODE");
    identity.area = settings->at("AREA");
    identity.gameRegion = settings->at("GAME");
    return identity;
}

inline const Identity& Current() {
    static const Identity identity = LoadFromNand();
    return identity;
}

inline std::string FormatMac(const std::array<uint8_t, 6>& mac) {
    std::ostringstream output;
    output << std::uppercase << std::hex << std::setfill('0');
    for (size_t index = 0; index < mac.size(); ++index) {
        if (index != 0) {
            output << ':';
        }
        output << std::setw(2) << static_cast<unsigned>(mac[index]);
    }
    return output.str();
}

} // namespace RuntimeConsoleIdentity
