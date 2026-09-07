#include "nand_file_ops.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <string>
#include <system_error>

#ifdef _WIN32
#include <windows.h>
#elif defined(__linux__)
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <stdio.h>
#endif

namespace {

// Unlike std::filesystem::rename on POSIX, this cannot overwrite a destination
// created by another writer between checking it and publishing the move.
void RenameNoReplace(const std::filesystem::path& source,
                     const std::filesystem::path& destination, std::error_code& ec) {
#ifdef _WIN32
    if (MoveFileExW(source.c_str(), destination.c_str(), 0)) ec.clear();
    else ec = std::error_code(GetLastError(), std::system_category());
#else
#ifdef __linux__
    const auto result = syscall(SYS_renameat2, AT_FDCWD, source.c_str(),
                                AT_FDCWD, destination.c_str(), RENAME_NOREPLACE);
#else
    const auto result = renamex_np(source.c_str(), destination.c_str(), RENAME_EXCL);
#endif
    if (result == 0) {
        ec.clear();
        return;
    }
    ec = std::error_code(errno, std::generic_category());
    if (ec != std::errc::function_not_supported && ec != std::errc::invalid_argument &&
        ec != std::errc::operation_not_supported) return;

    // Older filesystems may lack exclusive rename. Linking also publishes
    // without replacement; never fall back to an overwriting rename.
    std::filesystem::create_hard_link(source, destination, ec);
    if (!ec) std::filesystem::remove(source, ec);
#endif
}

struct StagedMove {
    std::filesystem::path directory;
    std::filesystem::path file;
    ~StagedMove() {
        std::error_code ignored;
        std::filesystem::remove(file, ignored);
        std::filesystem::remove(directory, ignored);
    }
};

} // namespace

// NANDMove does not replace an existing entry. Riivolution save redirects can
// put the destination on a different filesystem from the guest's /tmp files.
void NandMove(const std::filesystem::path& source, const std::filesystem::path& destination,
              std::error_code& ec) {
    namespace fs = std::filesystem;
    const auto status = fs::symlink_status(destination, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return;
    ec.clear();
    if (fs::exists(status)) {
        ec = std::make_error_code(std::errc::file_exists);
        return;
    }
    RenameNoReplace(source, destination, ec);
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

    const StagedMove staging{stagingDirectory, stagingDirectory / "data"};
    const auto& staged = staging.file;
    fs::copy_file(source, staged, fs::copy_options::none, ec);
    if (!ec) {
        // Publication is a same-filesystem rename: readers never see a partial
        // copy. Keep the source until the complete destination is in place.
        RenameNoReplace(staged, destination, ec);
        if (!ec) fs::remove(source, ec);
    }
    // A failed source removal leaves both complete copies and reports failure.
    // Staging cleanup preserves ec, including on exceptions.
}
