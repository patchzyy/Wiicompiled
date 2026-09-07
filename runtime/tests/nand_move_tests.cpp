#include "nand_move.h"

#include <fstream>
#include <iostream>
#include <stdexcept>

#ifdef __linux__
#include <csignal>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

static void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static void Write(const fs::path& path, const std::string& data) {
    std::ofstream output(path, std::ios::binary);
    output << data;
    output.close();
    Require(bool(output), "Fixture write failed");
}

static std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    Require(bool(input), "Fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

int main() {
    const auto name = "wiicomp-nand-move-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto root = fs::temp_directory_path() / name;
    fs::path other;
    try {
        fs::create_directory(root);
        const auto source = root / "banner.bin";
        const auto destination = root / "moved.bin";
        std::error_code ec;
        Write(source, "banner");
        RuntimeNandMove::Move(source, destination, ec);
        Require(!ec && !fs::exists(source) && Read(destination) == "banner", "Same-device move failed");
        Write(source, "keep source");
        RuntimeNandMove::Move(source, destination, ec);
        Require(ec == std::errc::file_exists && Read(source) == "keep source" && Read(destination) == "banner",
                "Existing destination must not be overwritten");
        RuntimeNandMove::Move(root / "missing", root / "absent", ec);
        Require(bool(ec) && !fs::exists(root / "absent"), "Missing source must fail");
        fs::create_directory(root / "directory");
        Write(root / "directory/child", "child");
        RuntimeNandMove::Move(root / "directory", root / "renamed-directory", ec);
        Require(!ec && Read(root / "renamed-directory/child") == "child", "Same-device directory move regressed");

#ifdef __linux__
        // /dev/shm is a separate tmpfs on ordinary Linux systems, including CI
        // and WSL. Fail rather than silently passing without exercising EXDEV.
        other = fs::path("/dev/shm") / name;
        fs::create_directory(other);
        struct stat left{}, right{};
        Require(::stat(root.c_str(), &left) == 0 && ::stat(other.c_str(), &right) == 0 && left.st_dev != right.st_dev,
                "Cross-device test requires /tmp and /dev/shm on separate filesystems");
        const auto target = other / "banner.bin";
        const std::string bytes = std::string(8192, '\0') + "banner payload";
        Write(source, bytes);
        fs::rename(source, target, ec);
        Require(ec == std::errc::cross_device_link, "Fixture must reproduce the original EXDEV failure");
        RuntimeNandMove::Move(source, target, ec);
        Require(!ec && !fs::exists(source) && Read(target) == bytes, "Cross-device move must preserve every byte");

        Write(source, "do not overwrite");
        RuntimeNandMove::Move(source, target, ec);
        Require(ec == std::errc::file_exists && Read(source) == "do not overwrite" && Read(target) == bytes,
                "Cross-device move must preserve an existing destination");
        fs::remove(target);
        fs::create_symlink(other / "missing", target);
        RuntimeNandMove::Move(source, target, ec);
        Require(ec == std::errc::file_exists && fs::is_symlink(target) && Read(source) == "do not overwrite",
                "Dangling destination symlink must not be replaced");
        fs::remove(target);

        RuntimeNandMove::Move(root / "renamed-directory", other / "directory", ec);
        Require(ec == std::errc::cross_device_link && Read(root / "renamed-directory/child") == "child" &&
                !fs::exists(other / "directory"), "Unsupported directory move must leave source intact");
        fs::create_symlink(source, root / "link");
        RuntimeNandMove::Move(root / "link", other / "link", ec);
        Require(ec == std::errc::cross_device_link && fs::is_symlink(root / "link") && !fs::exists(other / "link"),
                "Cross-device source symlinks must not be dereferenced");

        // Force a real write failure after a partial copy without filling disk.
        Write(source, bytes);
        struct rlimit saved{}, limited{};
        Require(getrlimit(RLIMIT_FSIZE, &saved) == 0, "Cannot read file-size limit");
        limited = saved;
        limited.rlim_cur = 1024;
        const auto oldHandler = std::signal(SIGXFSZ, SIG_IGN);
        Require(setrlimit(RLIMIT_FSIZE, &limited) == 0, "Cannot set file-size limit");
        RuntimeNandMove::Move(source, target, ec);
        const auto copyError = ec;
        const auto restored = setrlimit(RLIMIT_FSIZE, &saved);
        std::signal(SIGXFSZ, oldHandler);
        Require(restored == 0, "Cannot restore file-size limit");
        Require(bool(copyError) && Read(source) == bytes && !fs::exists(target) && fs::is_empty(other),
                "Failed copy must retain source and remove partial staging files");

        Require(geteuid() != 0, "Run permission tests as an unprivileged user");
        fs::permissions(root, fs::perms::owner_read | fs::perms::owner_exec);
        RuntimeNandMove::Move(source, target, ec);
        fs::permissions(root, fs::perms::owner_all);
        Require(bool(ec) && Read(source) == bytes && Read(target) == bytes,
                "Failed source deletion must leave both complete copies");
        Require(std::distance(fs::directory_iterator(other), fs::directory_iterator{}) == 1,
                "Move must clean up staging directory");
        fs::remove_all(other);
#endif
        fs::remove_all(root);
        std::cout << "NAND move tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        fs::permissions(root, fs::perms::owner_all, ignored);
        fs::remove_all(root, ignored);
        if (!other.empty()) fs::remove_all(other, ignored);
        return 1;
    }
}
