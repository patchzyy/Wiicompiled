#pragma once

#include "runtime_config.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace RuntimeConsoleIdentity {

struct Identity {
    std::string serial;
    std::array<uint8_t, 6> mac;
};

inline bool IsValidSerial(const std::string& serial) {
    return serial.size() == 9 &&
           serial != "000000000" &&
           std::all_of(serial.begin(), serial.end(),
                       [](unsigned char value) { return std::isdigit(value) != 0; });
}

inline Identity FromSerial(std::string serial) {
    // Keep Nintendo's Wii OUI. The suffix is derived from the persisted serial
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

inline std::optional<std::string> ReadSerial(const std::filesystem::path& path) {
    std::ifstream input(path);
    std::string line;
    if (!input || !std::getline(input, line)) {
        return std::nullopt;
    }
    constexpr std::string_view prefix = "serial=";
    if (line.rfind(prefix, 0) != 0) {
        return std::nullopt;
    }
    std::string serial = line.substr(prefix.size());
    if (!IsValidSerial(serial)) {
        return std::nullopt;
    }
    return serial;
}

inline bool WriteSerial(const std::filesystem::path& path, const std::string& serial) {
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        return false;
    }

    std::filesystem::path temporary = path;
    temporary += ".tmp";
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) {
            return false;
        }
        output << "serial=" << serial << '\n';
        output.close();
        if (!output) {
            return false;
        }
    }

    std::filesystem::rename(temporary, path, ec);
    if (!ec) {
        return true;
    }
    std::filesystem::remove(temporary, ec);
    return false;
}

inline std::string GenerateSerial() {
    std::random_device entropy;
    std::seed_seq seed{
        entropy(),
        entropy(),
        entropy(),
        entropy(),
    };
    std::mt19937 generator(seed);
    std::uniform_int_distribution<uint32_t> distribution(100000000u, 999999999u);
    return std::to_string(distribution(generator));
}

inline Identity LoadOrCreate(const std::filesystem::path& path) {
    if (const auto serial = ReadSerial(path)) {
        return FromSerial(*serial);
    }

    const std::string generated = GenerateSerial();
    if (WriteSerial(path, generated)) {
        return FromSerial(generated);
    }

    // Remain operational in a read-only environment. This fallback matches
    // Dolphin's deterministic serial while keeping the same valid identity shape.
    return FromSerial("123456789");
}

inline const Identity& Current() {
    static const Identity identity =
        LoadOrCreate(RuntimeConfigFile::ApplicationDataDirectory() / "ConsoleIdentity.txt");
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
