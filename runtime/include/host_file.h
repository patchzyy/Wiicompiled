#pragma once

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <io.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace RuntimeHostFile {

inline FILE* Open(const std::filesystem::path& path, const char* mode) {
#ifdef _WIN32
    const std::string modeText(mode);
    const std::wstring wideMode(modeText.begin(), modeText.end());
    return _wfopen(path.c_str(), wideMode.c_str());
#else
    return std::fopen(path.c_str(), mode);
#endif
}

struct FileOperations {
    static size_t Write(FILE* file, const char* data, size_t size) {
        return std::fwrite(data, 1, size, file);
    }

    static bool Flush(FILE* file) {
        if (std::fflush(file) != 0) return false;
#ifdef _WIN32
        return _commit(_fileno(file)) == 0;
#else
#ifdef __APPLE__
        if (::fcntl(fileno(file), F_FULLFSYNC) == 0) return true;
        if (errno != ENOTSUP && errno != EINVAL && errno != ENOTTY) return false;
#endif
        return ::fsync(fileno(file)) == 0;
#endif
    }

    static bool Close(FILE* file) { return std::fclose(file) == 0; }

    static bool Publish(std::filesystem::path& temporary,
                        const std::filesystem::path& target, bool replace) {
#ifdef _WIN32
        if (replace) {
            auto backup = temporary;
            backup += ".bak";
            // ReplaceFile preserves the destination's ACL and other metadata.
            if (ReplaceFileW(target.c_str(), temporary.c_str(), backup.c_str(), 0, nullptr, nullptr)) {
                return DeleteFileW(backup.c_str()) != 0;
            }
            const auto error = GetLastError();
            if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT || error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                if (error == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
                    MoveFileExW(backup.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH);
                }
                // Keep recovery contents even if restoring the original name fails.
                temporary.clear();
                return false;
            }
            if (error != ERROR_FILE_NOT_FOUND) return false;
        }
        return MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_WRITE_THROUGH) != 0;
#else
        if (replace) return ::rename(temporary.c_str(), target.c_str()) == 0;
        return ::link(temporary.c_str(), target.c_str()) == 0;
#endif
    }
};

template <class Operations = FileOperations>
inline int CreateExclusive(const std::filesystem::path& path) {
    FILE* file = Open(path, "wbx");
    if (!file) return errno == EEXIST ? EEXIST : EIO;
    return Operations::Close(file) ? 0 : EIO;
}

inline std::filesystem::path WriteTarget(const std::filesystem::path& path, std::error_code& ec) {
    const auto status = std::filesystem::symlink_status(path, ec);
    if (ec && ec != std::errc::no_such_file_or_directory) return {};
    ec.clear();
    if (std::filesystem::is_symlink(status)) return std::filesystem::canonical(path, ec);
    return std::filesystem::weakly_canonical(path, ec);
}

template <class Operations = FileOperations>
inline bool WriteAtomically(const std::filesystem::path& path, std::string_view contents,
                            bool replace = true) {
    std::error_code ec;
    const auto target = WriteTarget(path, ec);
    if (ec || target.empty()) return false;
    std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) return false;

#ifdef _WIN32
    DWORD securitySize = 0;
    std::vector<uint8_t> security;
    if (!GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &securitySize)) {
        const auto error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER && error != ERROR_FILE_NOT_FOUND) return false;
    }
    if (securitySize != 0) {
        security.resize(securitySize);
        if (!GetFileSecurityW(target.c_str(), DACL_SECURITY_INFORMATION, security.data(), securitySize, &securitySize)) return false;
    }
#else
    struct stat existing {};
    const bool existed = ::stat(target.c_str(), &existing) == 0;
    if (!existed && errno != ENOENT) return false;
    if (existed && !S_ISREG(existing.st_mode)) return false;
#endif

    struct TemporaryFile {
        FILE* file = nullptr;
        std::filesystem::path path;
        ~TemporaryFile() {
            if (file) std::fclose(file);
            if (!path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
        }
    } temporary;

    static std::atomic<uint64_t> sequence{0};
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    const auto token = std::to_string(process) + "-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        auto candidate = target;
        candidate += ".tmp-" + token + "-" + std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
        temporary.file = Open(candidate, "wbx");
        if (temporary.file) {
            temporary.path = std::move(candidate);
            break;
        }
        if (errno != EEXIST) return false;
    }
    if (!temporary.file) return false;

#ifdef _WIN32
    if (!security.empty()) {
        SECURITY_DESCRIPTOR_CONTROL control = 0;
        DWORD revision = 0;
        if (!GetSecurityDescriptorControl(security.data(), &control, &revision)) return false;
        const DWORD protection = (control & SE_DACL_PROTECTED) ? PROTECTED_DACL_SECURITY_INFORMATION : UNPROTECTED_DACL_SECURITY_INFORMATION;
        if (!SetFileSecurityW(temporary.path.c_str(), DACL_SECURITY_INFORMATION | protection, security.data())) return false;
    }
#else
    if (existed && ::fchmod(fileno(temporary.file), existing.st_mode & 07777) != 0) return false;
#endif
    size_t offset = 0;
    while (offset < contents.size()) {
        const size_t written = Operations::Write(temporary.file, contents.data() + offset, contents.size() - offset);
        if (written == 0 || written > contents.size() - offset || std::ferror(temporary.file)) return false;
        offset += written;
    }
#ifndef _WIN32
    if (existed && ::fchmod(fileno(temporary.file), existing.st_mode & 07777) != 0) return false;
#endif
    if (!Operations::Flush(temporary.file)) return false;
    FILE* file = temporary.file;
    temporary.file = nullptr;
    if (!Operations::Close(file) || !Operations::Publish(temporary.path, target, replace)) return false;

#ifndef _WIN32
    if (!replace && ::unlink(temporary.path.c_str()) != 0) return false;
    temporary.path.clear();
    // Sync the directory after publishing the complete file.
    const int directory = ::open(target.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (directory < 0) return false;
    const bool synced = ::fsync(directory) == 0;
    const bool closed = ::close(directory) == 0;
    if (!synced || !closed) return false;
#else
    temporary.path.clear();
#endif
    return true;
}

} // namespace RuntimeHostFile
