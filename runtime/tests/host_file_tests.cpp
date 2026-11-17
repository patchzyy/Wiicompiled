#ifdef __APPLE__
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
#include <fcntl.h>
#include <unistd.h>

namespace {
bool g_injectSync = false;
int g_fullSyncError = 0;
int g_syncError = 0;
unsigned g_fullSyncCalls = 0;
unsigned g_syncCalls = 0;

int TestFcntl(int descriptor, int command) {
    if (!g_injectSync) return ::fcntl(descriptor, command);
    if (command != F_FULLFSYNC) { errno = EINVAL; return -1; }
    ++g_fullSyncCalls;
    errno = g_fullSyncError;
    return g_fullSyncError ? -1 : 0;
}
int TestFsync(int descriptor) {
    if (!g_injectSync) return ::fsync(descriptor);
    ++g_syncCalls;
    errno = g_syncError;
    return g_syncError ? -1 : 0;
}
}
#define fcntl TestFcntl
#define fsync TestFsync
#endif

#include "host_file.h"

#ifdef __APPLE__
#undef fsync
#undef fcntl
#endif

#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <sddl.h>
#endif

namespace {
void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
#ifdef __APPLE__
void CheckFullSync(const std::filesystem::path& path) {
    FILE* file = RuntimeHostFile::Open(path, "wb");
    Require(file != nullptr, "cannot create flush fixture");
    struct Case { int fullError; int fallbackError; bool succeeds; unsigned fallbackCalls; };
    bool passed = true;
    g_injectSync = true;
    for (const auto& test : {Case{0, EIO, true, 0}, Case{ENOTSUP, 0, true, 1},
                            Case{EINVAL, 0, true, 1}, Case{ENOTSUP, EIO, false, 1},
                            Case{ENOTTY, 0, true, 1}, Case{ENOTTY, EIO, false, 1},
                            Case{EIO, 0, false, 0}, Case{EINTR, 0, false, 0}}) {
        g_fullSyncError = test.fullError;
        g_syncError = test.fallbackError;
        g_fullSyncCalls = g_syncCalls = 0;
        const bool result = RuntimeHostFile::FileOperations::Flush(file);
        passed &= result == test.succeeds && g_fullSyncCalls == 1 && g_syncCalls == test.fallbackCalls;
    }
    g_injectSync = false;
    std::fclose(file);
    std::filesystem::remove(path);
    Require(passed, "full sync did not preserve success, unsupported, or I/O failure semantics");
}
#endif
struct ShortWrites : RuntimeHostFile::FileOperations {
    static size_t Write(FILE* file, const char* data, size_t size) {
        return FileOperations::Write(file, data, std::min(size, size_t{3}));
    }
};
struct FailedWrite : RuntimeHostFile::FileOperations {
    static size_t Write(FILE* file, const char* data, size_t size) {
        if (std::ftell(file) != 0) { errno = ENOSPC; return 0; }
        return FileOperations::Write(file, data, std::min(size, size_t{4}));
    }
};
struct FailedFlush : RuntimeHostFile::FileOperations {
    static bool Flush(FILE*) { return false; }
};
struct FailedClose : RuntimeHostFile::FileOperations {
    static bool Close(FILE* file) { FileOperations::Close(file); return false; }
};
struct FailedPublish : RuntimeHostFile::FileOperations {
    static bool Publish(const std::filesystem::path&, const std::filesystem::path&, bool) { return false; }
};
struct PublishedFailure : RuntimeHostFile::FileOperations {
    static bool Publish(std::filesystem::path& temporary, const std::filesystem::path& target, bool replace) {
        FileOperations::Publish(temporary, target, replace);
        return false;
    }
};
#ifdef _WIN32
void CheckPermissions(const std::filesystem::path& path) {
    HANDLE token = nullptr;
    Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token), "cannot inspect test user");
    DWORD needed = 0;
    GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
    std::vector<uint8_t> user(needed);
    const bool read = GetTokenInformation(token, TokenUser, user.data(), needed, &needed) != 0;
    CloseHandle(token);
    Require(read, "cannot read test user SID");
    LPWSTR sid = nullptr;
    Require(ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(user.data())->User.Sid, &sid), "cannot format test user SID");
    const std::wstring aclText = L"D:P(A;;FA;;;" + std::wstring(sid) + L")";
    LocalFree(sid);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    Require(ConvertStringSecurityDescriptorToSecurityDescriptorW(aclText.c_str(), SDDL_REVISION_1,
        &descriptor, nullptr), "cannot create private test ACL");
    const bool set = SetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION, descriptor) != 0;
    LocalFree(descriptor);
    Require(set, "cannot apply private test ACL");
    const auto readAcl = [&] {
        DWORD securitySize = 0;
        GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, nullptr, 0, &securitySize);
        std::vector<uint8_t> security(securitySize);
        Require(GetFileSecurityW(path.c_str(), DACL_SECURITY_INFORMATION, security.data(), securitySize, &securitySize),
                "cannot inspect test ACL");
        // Windows may normalize this bookkeeping bit during replacement.
        Require(SetSecurityDescriptorControl(security.data(), SE_DACL_AUTO_INHERITED, 0),
                "cannot normalize ACL inheritance metadata");
        LPWSTR text = nullptr;
        const bool converted = ConvertSecurityDescriptorToStringSecurityDescriptorW(security.data(), SDDL_REVISION_1,
            DACL_SECURITY_INFORMATION, &text, nullptr) != 0;
        Require(converted, "cannot format test ACL");
        const std::wstring result(text);
        LocalFree(text);
        return result;
    };
    const auto originalAcl = readAcl();
    Require(RuntimeHostFile::WriteAtomically(path, "private contents"), "private file replacement failed");
    const auto replacedAcl = readAcl();
    if (replacedAcl != originalAcl) {
        std::wcerr << L"original ACL: " << originalAcl << L"\nreplacement ACL: " << replacedAcl << L'\n';
    }
    Require(replacedAcl == originalAcl, "replacement changed the file ACL");
}
#endif
template <class Operations>
void CheckFailure(const std::filesystem::path& path) {
    Require(!RuntimeHostFile::WriteAtomically<Operations>(path, "replacement contents"), "failure was reported as success");
    Require(Read(path) == "original contents", "a failed publication changed the destination");
    for (const auto& entry : std::filesystem::directory_iterator(path.parent_path())) {
        Require(entry.path() == path, "a failed publication left a temporary file");
    }
}
} // namespace

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("wiicomp-host-file-" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count()));
    bool claimed = false;
    try {
        claimed = std::filesystem::create_directory(root);
        Require(claimed, "cannot claim test directory");
#ifdef __APPLE__
        CheckFullSync(root / "flush-fixture");
#endif
        const auto path = root / "Config.toml";
        Require(RuntimeHostFile::WriteAtomically(path, "original contents"), "initial publication failed");
        CheckFailure<FailedWrite>(path);
        CheckFailure<FailedFlush>(path);
        CheckFailure<FailedClose>(path);
        CheckFailure<FailedPublish>(path);
        Require(RuntimeHostFile::WriteAtomically<ShortWrites>(path, "short writes are retried"), "short writes were not retried");
        Require(Read(path) == "short writes are retried", "short writes produced incomplete contents");
        Require(!RuntimeHostFile::WriteAtomically(path, "do not replace", false), "exclusive publication overwrote a file");
        Require(Read(path) == "short writes are retried", "exclusive publication truncated a file");
        Require(!RuntimeHostFile::WriteAtomically<PublishedFailure>(path, "complete after publication"), "injected post-publication failure was lost");
        Require(Read(path) == "complete after publication", "post-publication failure left partial contents");

#ifndef _WIN32
        std::filesystem::permissions(path, std::filesystem::perms::owner_read | std::filesystem::perms::owner_write);
        Require(RuntimeHostFile::WriteAtomically(path, "private contents"), "private file replacement failed");
        Require((std::filesystem::status(path).permissions() & std::filesystem::perms::all) ==
            (std::filesystem::perms::owner_read | std::filesystem::perms::owner_write), "replacement changed file permissions");
        const auto blocked = root / "blocked";
        std::filesystem::create_directory(blocked);
        std::filesystem::permissions(blocked, std::filesystem::perms::owner_read | std::filesystem::perms::owner_exec);
        if (::geteuid() != 0) {
            const bool createFailed = RuntimeHostFile::CreateExclusive(blocked / "save") == EIO;
            const bool writeFailed = !RuntimeHostFile::WriteAtomically(blocked / "Config.toml", "unwritable");
            std::filesystem::permissions(blocked, std::filesystem::perms::owner_all);
            Require(createFailed && writeFailed, "unwritable parent was accepted");
        } else {
            std::filesystem::permissions(blocked, std::filesystem::perms::owner_all);
        }
#else
        CheckPermissions(path);
#endif
        const auto link = root / "linked.toml";
        std::error_code ec;
        std::filesystem::create_symlink(path.filename(), link, ec);
        if (!ec) {
            Require(RuntimeHostFile::WriteAtomically(link, "through the link"), "symlink target replacement failed");
            Require(std::filesystem::is_symlink(link) && Read(path) == "through the link", "replacement destroyed a symlink");
            const auto dangling = root / "dangling.toml";
            std::filesystem::create_symlink("absent.toml", dangling);
            Require(!RuntimeHostFile::WriteAtomically(dangling, "no target"), "dangling symlink was replaced");
            Require(std::filesystem::is_symlink(dangling), "dangling symlink disappeared");
        } else {
            std::cout << "symlink creation unavailable: " << ec.message() << '\n';
        }

        const auto save = root / std::filesystem::path(u8"save-\u00e9-\u65e5.dat");
        Require(RuntimeHostFile::CreateExclusive(save) == 0, "Unicode exclusive create failed");
        { std::ofstream output(save, std::ios::binary); output << "save sentinel"; }
        Require(RuntimeHostFile::CreateExclusive(save) == EEXIST && Read(save) == "save sentinel", "duplicate create destroyed a save");
        Require(RuntimeHostFile::CreateExclusive(root) != 0 && std::filesystem::is_directory(root), "create accepted a directory");
        Require(RuntimeHostFile::CreateExclusive(root / "absent" / "save") == EIO, "missing parent create did not fail");
        Require(RuntimeHostFile::CreateExclusive<FailedClose>(root / "failed-close") == EIO, "create ignored close failure");

        const auto raced = root / "racing.dat";
        std::atomic<unsigned> ready{0}, successes{0}, duplicates{0};
        std::atomic<bool> start{false};
        std::vector<std::thread> creators;
        for (unsigned i = 0; i < 16; ++i) {
            creators.emplace_back([&] {
                ++ready;
                while (!start.load()) std::this_thread::yield();
                const int result = RuntimeHostFile::CreateExclusive(raced);
                if (result == 0) ++successes;
                else if (result == EEXIST) ++duplicates;
            });
        }
        while (ready.load() != 16) std::this_thread::yield();
        start = true;
        for (auto& creator : creators) creator.join();
        Require(successes == 1 && duplicates == 15, "simultaneous creates did not have exactly one winner");
        const auto directory = root / "directory";
        std::filesystem::create_directory(directory);
        Require(!RuntimeHostFile::WriteAtomically(directory, "not a file"), "atomic publication replaced a directory");
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        if (claimed) std::filesystem::remove_all(root, ignored);
        return 1;
    }
    return 0;
}
