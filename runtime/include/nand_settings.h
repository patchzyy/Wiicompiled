#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace RuntimeNandSettings {

using Settings = std::map<std::string, std::string>;

inline std::filesystem::path FilePath(const std::filesystem::path& root) {
    return root / "title/00000001/00000002/data/setting.txt";
}

// Wii setting.txt is a 256-byte buffer encrypted with a rotating XOR key.
inline std::optional<Settings> Read(const std::filesystem::path& nandRoot) {
    std::ifstream input(FilePath(nandRoot), std::ios::binary);
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

// Dolphin's normal (non-deterministic) first-boot algorithm. It is independent
// of the ES device ID. Matching another NAND requires that NAND's saved serial.
inline std::string GenerateSerial(std::time_t now) {
    if (now < 0) {
        return {};
    }
    const auto digits = std::to_string(now % 1000000000);
    return std::string(9 - digits.size(), '0') + digits;
}

// This recompilation targets the European disc. These are Dolphin's PAL boot
// defaults; an existing setting.txt always takes precedence, in every region.
inline std::optional<std::array<uint8_t, 256>> EncodeNew(const std::string& serial) {
    const Settings identity{{"SERNO", serial}, {"CODE", "LEH"}, {"AREA", "EUR"}, {"GAME", "EU"}};
    if (!HasIdentity(identity)) {
        return std::nullopt;
    }
    std::array<uint8_t, 256> bytes{};
    size_t position = 0;
    uint32_t key = 0x73B5DBFAu;
    const auto writeByte = [&](char value) {
        bytes[position++] = static_cast<uint8_t>(value) ^ static_cast<uint8_t>(key);
        key = (key << 1) | (key >> 31);
    };
    for (const std::string& line : {std::string("AREA=EUR\r\n"), std::string("MODEL=RVL-001(EUR)\r\n"),
             std::string("DVD=0\r\n"), std::string("MPCH=0x7FFE\r\n"), std::string("CODE=LEH\r\n"),
             "SERNO=" + serial + "\r\n", std::string("VIDEO=PAL\r\n"), std::string("GAME=EU\r\n")}) {
        for (;;) {
            if (position + line.size() > bytes.size()) {
                return std::nullopt;
            }
            const auto start = position;
            const auto savedKey = key;
            bool hasNull = false;
            for (const char value : line) {
                writeByte(value);
                hasNull |= bytes[position - 1] == 0;
            }
            if (!hasNull) {
                break;
            }
            // Nintendo stops at an encoded NUL. Dolphin inserts an extra LF
            // before this line and retries with the shifted encryption key.
            position = start;
            key = savedKey;
            writeByte('\n');
        }
    }
    return bytes; // The unused tail stays raw zero, as in Dolphin.
}

// Atomically claim our own scratch directory. A collision belongs to another
// launch (or a previous crashed launch); leave it untouched and try another name.
inline std::optional<std::filesystem::path> CreateScratchDirectory(
    const std::filesystem::path& parent, const std::string& token, std::error_code& ec) {
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        const auto candidate = parent / (".setting-init-" + token + "-" + std::to_string(attempt));
        ec.clear();
        if (std::filesystem::create_directory(candidate, ec)) return candidate;
        if (ec && ec != std::errc::file_exists) return std::nullopt;
    }
    ec = std::make_error_code(std::errc::file_exists);
    return std::nullopt;
}

// Never replace an existing file, including an unreadable or damaged one.
// Publish a complete file atomically so simultaneous launches use one identity.
inline bool Ensure(const std::filesystem::path& root, std::string& error,
                   std::time_t now = std::time(nullptr)) {
    const auto path = FilePath(root);
    std::error_code ec;
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) {
        error = "Cannot inspect NAND setting.txt: " + ec.message();
        return false;
    }
    if (std::filesystem::exists(status)) {
        const auto existing = Read(root);
        if (existing && HasIdentity(*existing)) {
            return true;
        }
        error = "Existing NAND setting.txt is unreadable or invalid; restore it from this console's backup";
        return false;
    }

    const auto bytes = EncodeNew(GenerateSerial(now));
    if (!bytes) {
        error = "Cannot initialize NAND settings: invalid system clock";
        return false;
    }
    ec.clear();
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "Cannot create NAND settings directory: " + ec.message();
        return false;
    }
    static std::atomic<unsigned> sequence{0};
#ifdef _WIN32
    const auto processId = GetCurrentProcessId();
#else
    const auto processId = getpid();
#endif
    const auto scratch = CreateScratchDirectory(path.parent_path(),
        std::to_string(processId) + "-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
            std::to_string(sequence++), ec);
    if (!scratch) {
        error = "Cannot create temporary NAND settings directory: " + ec.message();
        return false;
    }
    const auto temporary = *scratch / "setting.txt";
    bool written = false;
    {
        std::ofstream output(temporary, std::ios::binary);
        output.write(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        output.close();
        written = static_cast<bool>(output);
    }
    bool published = false;
    if (written) {
#ifdef _WIN32
        published = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
        published = ::link(temporary.c_str(), path.c_str()) == 0;
#endif
    }
    std::filesystem::remove(temporary, ec);
    std::filesystem::remove(*scratch, ec);
    // A competing launcher may have published its settings first. Always read
    // the winner from NAND rather than using our unpersisted candidate serial.
    const auto persisted = Read(root);
    if (persisted && HasIdentity(*persisted)) {
        return true;
    }
    error = published ? "Cannot read newly initialized NAND setting.txt" :
                        "Cannot persist NAND setting.txt; check NAND directory permissions";
    return false;
}

} // namespace RuntimeNandSettings
