#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>

namespace RuntimeNandSettings {

using Settings = std::map<std::string, std::string>;

// Wii setting.txt is a 256-byte buffer encrypted with a rotating XOR key.
inline std::optional<Settings> Read(const std::filesystem::path& nandRoot) {
    std::ifstream input(nandRoot / "title/00000001/00000002/data/setting.txt",
                        std::ios::binary);
    std::array<uint8_t, 256> bytes{};
    if (!input.read(reinterpret_cast<char*>(bytes.data()), bytes.size())) {
        return std::nullopt;
    }
    uint32_t key = 0x73B5DBFAu;
    std::string decoded;
    for (const uint8_t byte : bytes) {
        const char value = static_cast<char>(byte ^ static_cast<uint8_t>(key));
        key = (key << 1) | (key >> 31);
        if (value == '\0') {
            break;
        }
        if (value != '\r') {
            decoded += value;
        }
    }
    Settings settings;
    for (size_t start = 0; start < decoded.size();) {
        const size_t end = decoded.find('\n', start);
        const std::string line = decoded.substr(start, end - start);
        const size_t equals = line.find('=');
        if (equals != std::string::npos && equals != 0) {
            settings.emplace(line.substr(0, equals), line.substr(equals + 1));
        }
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return settings;
}

inline bool HasIdentity(const Settings& settings) {
    const auto serial = settings.find("SERNO");
    if (serial == settings.end() || serial->second.empty() || serial->second.size() > 9 ||
        serial->second.find_first_not_of("0123456789") != std::string::npos ||
        serial->second.find_first_not_of('0') == std::string::npos) {
        return false;
    }
    for (const auto& field : {std::pair{"CODE", 5u}, {"AREA", 3u}, {"GAME", 2u}}) {
        const auto value = settings.find(field.first);
        if (value == settings.end() || value->second.empty() ||
            value->second.size() > field.second) {
            return false;
        }
    }
    return true;
}

} // namespace RuntimeNandSettings
