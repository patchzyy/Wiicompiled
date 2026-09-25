#include "platform/host_platform.h"

#include <cstdlib>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shlobj.h>
#else
#include <climits>
#include <fcntl.h>
#include <spawn.h>
#include <unistd.h>
#endif

#if !defined(_WIN32) && !defined(__APPLE__)
#include <dirent.h>
#endif

#if defined(__APPLE__)
#include <crt_externs.h>
#include <mach-o/dyld.h>
#include <pwd.h>
#endif

namespace RuntimePlatform {

std::optional<std::filesystem::path> ExecutableDirectory() noexcept {
#if defined(_WIN32)
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return std::nullopt;
        }
        if (length < buffer.size() - 1) {
            buffer.resize(length);
            return std::filesystem::path(buffer).parent_path();
        }
        buffer.resize(buffer.size() * 2);
    }
#elif defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
        return std::nullopt;
    }
    std::string path(size, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return std::nullopt;
    }
    path.resize(std::char_traits<char>::length(path.c_str()));
    std::error_code ec;
    const auto resolved = std::filesystem::weakly_canonical(path, ec);
    return (ec ? std::filesystem::path(path) : resolved).parent_path();
#else
    return std::nullopt;
#endif
}

std::filesystem::path ApplicationDataDirectory(std::string_view applicationName) {
#if defined(_WIN32)
    PWSTR rawPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_CREATE, nullptr, &rawPath)) && rawPath) {
        const std::filesystem::path directory = std::filesystem::path(rawPath) / applicationName;
        CoTaskMemFree(rawPath);
        return directory;
    }
#elif defined(__APPLE__)
    if (const char* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / "Library" / "Application Support" / applicationName;
    }
    if (const passwd* user = getpwuid(getuid()); user && user->pw_dir && *user->pw_dir) {
        return std::filesystem::path(user->pw_dir) / "Library" / "Application Support" / applicationName;
    }
#endif
    return std::filesystem::current_path() / applicationName;
}

std::filesystem::path LogDirectory(std::string_view applicationName) {
    return ApplicationDataDirectory(applicationName) / "Logs";
}

uint64_t CurrentProcessId() noexcept {
#if defined(_WIN32)
    return static_cast<uint64_t>(::GetCurrentProcessId());
#else
    return static_cast<uint64_t>(::getpid());
#endif
}

bool RelaunchSelf() noexcept {
#if defined(_WIN32)
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) {
        return false;
    }
    path.resize(length);
    std::wstring commandLine = GetCommandLineW();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                        &startup, &process)) {
        return false;
    }
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    return true;
#else
    std::string path;
    // Mark inherited descriptors close-on-exec, or the new instance keeps this one's sockets and devices open.
#if defined(__APPLE__)
    uint32_t size = 0;
    if (_NSGetExecutablePath(nullptr, &size) != -1 || size == 0) {
        return false;
    }
    path.resize(size);
    if (_NSGetExecutablePath(path.data(), &size) != 0) {
        return false;
    }
    char** const env = *_NSGetEnviron();
    for (int fd = 3, max = getdtablesize(); fd < max; ++fd) {
        fcntl(fd, F_SETFD, FD_CLOEXEC);
    }
#else
    path.resize(PATH_MAX);
    const ssize_t length = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (length <= 0) {
        return false;
    }
    path.resize(static_cast<size_t>(length));
    char** const env = environ;
    if (DIR* dir = opendir("/proc/self/fd")) {
        while (const dirent* entry = readdir(dir)) {
            if (const int fd = std::atoi(entry->d_name); fd > 2 && fd != dirfd(dir)) {
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
        closedir(dir);
    }
#endif
    char* const argv[] = {path.data(), nullptr};
    pid_t pid = 0;
    return posix_spawn(&pid, path.c_str(), nullptr, nullptr, argv, env) == 0;
#endif
}

} // namespace RuntimePlatform
