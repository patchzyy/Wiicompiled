#include "runtime_config.h"

#include <iostream>
#include <iterator>
#include <stdexcept>
#include <thread>

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
void Write(const std::filesystem::path& path, const std::string& text) {
    std::ofstream output(path, std::ios::binary);
    output << text;
    output.close();
    Require(static_cast<bool>(output), "test fixture write failed");
}
struct FailedWrite : RuntimeHostFile::FileOperations {
    static size_t Write(FILE* file, const char* data, size_t size) {
        if (std::ftell(file) != 0) { errno = ENOSPC; return 0; }
        return FileOperations::Write(file, data, std::min(size, size_t{3}));
    }
};
void Reject(const std::filesystem::path& path, const std::string& text) {
    Write(path, text);
    Require(!RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "true"), "invalid config was updated");
    Require(Read(path) == text, "invalid config bytes were changed");
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("wiicomp-config-io-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    bool claimed = false;
    try {
        claimed = std::filesystem::create_directory(root);
        Require(claimed, "cannot claim test directory");
        const auto path = root / "Config.toml";
        const std::string original = "# keep\n[paths]\ndvd_root = 'a/b'\n[video]\nshow_fps = false\n";
        Write(path, original);
        Require(!RuntimeConfigFile::WriteSettingFile<FailedWrite>(path, "video", "show_fps", "true"), "write failure was ignored");
        Require(Read(path) == original, "failed config update lost previous settings");
        Require(std::distance(std::filesystem::directory_iterator(root), std::filesystem::directory_iterator{}) == 1,
                "failed config update left a temporary file");
        Require(RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "true"), "config update failed");
        auto config = toml::parse_str(Read(path));
        Require(config.at("video").at("show_fps").as_boolean() &&
                config.at("paths").at("dvd_root").as_string() == "a/b" && Read(path).find("# keep") != std::string::npos,
                "config update lost unrelated fields or comments");

        Reject(path, "[video]\nshow_fps = false\nshow_fps = true\n");
        Reject(path, "[video]\nshow_fps = false\n[video]\n");
        Reject(path, "video = 1\n");
        Reject(path, "[[video]]\nshow_fps = false\n");
        Reject(path, "[video\nshow_fps = false\n");
        Write(path, original);
        Require(!RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "["), "invalid value was accepted");
        Require(Read(path) == original, "invalid value changed the document");
        Write(path, "[video]\nshow_fps = false\n");
        Require(RuntimeConfigFile::WriteSettingFile(path, "controller.wii_accel_offsets", "x", "0.5"), "missing parent tables were not created");
        config = toml::parse_str(Read(path));
        Require(config.at("controller").at("wii_accel_offsets").at("x").as_floating() == 0.5,
                "new nested setting was lost");

        std::vector<std::thread> writers;
        std::atomic<unsigned> successes{0};
        for (unsigned i = 0; i < 12; ++i) {
            writers.emplace_back([&, i] {
                if (RuntimeConfigFile::WriteSettingFile(path, "parallel", "value_" + std::to_string(i), std::to_string(i))) ++successes;
            });
        }
        for (auto& writer : writers) writer.join();
        config = toml::parse_str(Read(path));
        Require(successes == 12 && config.at("parallel").as_table().size() == 12, "concurrent updates lost settings");
        for (unsigned i = 0; i < 12; ++i) {
            Require(config.at("parallel").at("value_" + std::to_string(i)).as_integer() == i, "concurrent value was corrupted");
        }

        const auto absent = root / "new" / "Config.toml";
        Require(RuntimeConfigFile::WriteSettingFile(absent, "video", "show_fps", "true"), "missing config was not created");
        const auto directory = root / "directory.toml";
        std::filesystem::create_directory(directory);
        Require(!RuntimeConfigFile::WriteSettingFile(directory, "video", "show_fps", "true") &&
                std::filesystem::is_directory(directory), "unreadable directory config was replaced");
#ifndef _WIN32
        Write(path, original);
        std::filesystem::permissions(path, std::filesystem::perms::none);
        if (::geteuid() != 0) {
            Require(!RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "true"), "unreadable config was rewritten");
        }
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        Require(Read(path) == original, "unreadable config bytes changed");
#else
        Write(path, original);
        HANDLE locked = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "cannot lock config fixture");
        const bool updatedLocked = RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "true");
        CloseHandle(locked);
        Require(!updatedLocked && Read(path) == original, "locked config was replaced from an empty read");
        locked = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        Require(locked != INVALID_HANDLE_VALUE, "cannot lock config replacement");
        const bool replacedLocked = RuntimeConfigFile::WriteSettingFile(path, "video", "show_fps", "true");
        CloseHandle(locked);
        Require(!replacedLocked && Read(path) == original, "failed config replacement changed previous bytes");
#endif
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        if (claimed) std::filesystem::remove_all(root, ignored);
        return 1;
    }
    return 0;
}
