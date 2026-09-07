#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

namespace RuntimeNandMove {

// NANDMove does not replace an existing entry. Riivolution save redirects can
// put the destination on a different filesystem from the guest's /tmp files.
inline void Move(const std::filesystem::path& source, const std::filesystem::path& destination,
                 std::error_code& ec) {
    namespace fs = std::filesystem;
    const auto status = fs::symlink_status(destination, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return;
    ec.clear();
    if (fs::exists(status)) {
        ec = std::make_error_code(std::errc::file_exists);
        return;
    }
    fs::rename(source, destination, ec);
    if (ec != std::errc::cross_device_link) return;

    // Do not turn a directory move into a partially completed recursive copy,
    // or follow a symlink and delete the link after copying its target.
    const auto sourceStatus = fs::symlink_status(source, ec);
    if (ec) return;
    if (!fs::is_regular_file(sourceStatus)) {
        ec = std::make_error_code(std::errc::cross_device_link);
        return;
    }

    static std::atomic<unsigned long long> sequence{0};
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    fs::path stagingDirectory;
    bool created = false;
    for (int attempt = 0; attempt < 64; ++attempt) {
        stagingDirectory = destination.parent_path() /
            (".nand-move-" + std::to_string(stamp) + "-" + std::to_string(sequence++));
        created = fs::create_directory(stagingDirectory, ec);
        if (created) break;
        if (ec && ec != std::errc::file_exists) return;
    }
    if (!created) {
        ec = std::make_error_code(std::errc::file_exists);
        return;
    }

    const auto staged = stagingDirectory / "data";
    fs::copy_file(source, staged, fs::copy_options::none, ec);
    if (!ec) {
        // Publication is a same-filesystem rename: readers never see a partial
        // copy. Keep the source until the complete destination is in place.
        const auto current = fs::symlink_status(destination, ec);
        if (ec == std::errc::no_such_file_or_directory) ec.clear();
        if (!ec && fs::exists(current)) ec = std::make_error_code(std::errc::file_exists);
        if (!ec) fs::rename(staged, destination, ec);
        if (!ec) fs::remove(source, ec);
    }
    // On failure the source remains available; after publication a failed
    // source removal leaves both complete copies. Preserve the original error.
    std::error_code cleanupError;
    fs::remove(staged, cleanupError);
    fs::remove(stagingDirectory, cleanupError);
}

} // namespace RuntimeNandMove
