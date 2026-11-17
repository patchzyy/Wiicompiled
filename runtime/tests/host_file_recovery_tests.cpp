#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
DWORD g_failure = 0;
bool g_failRestore = false;
bool g_restoreCollision = false;
bool g_failBackupCleanup = false;
std::filesystem::path g_replacement;
std::filesystem::path g_backup;

BOOL TestReplaceFile(LPCWSTR target, LPCWSTR replacement, LPCWSTR backup,
                     DWORD flags, LPVOID exclude, LPVOID reserved) {
    g_replacement = replacement;
    g_backup = backup ? backup : L"";
    if (g_failure == 0) return ReplaceFileW(target, replacement, backup, flags, exclude, reserved);
    if (g_failure == ERROR_UNABLE_TO_MOVE_REPLACEMENT && !backup) {
        if (!DeleteFileW(target)) throw std::runtime_error("cannot simulate missing destination");
    } else if (g_failure == ERROR_UNABLE_TO_MOVE_REPLACEMENT_2) {
        if (!backup || !MoveFileExW(target, backup, MOVEFILE_WRITE_THROUGH)) {
            throw std::runtime_error("cannot simulate moved original");
        }
    }
    SetLastError(g_failure);
    return FALSE;
}

BOOL TestMoveFile(LPCWSTR source, LPCWSTR target, DWORD flags) {
    if (g_restoreCollision && std::filesystem::path(source) == g_backup) {
        std::ofstream file{std::filesystem::path(target)};
        file << "concurrent writer";
    }
    if (g_failRestore && std::filesystem::path(source) == g_backup) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return MoveFileExW(source, target, flags);
}
BOOL TestDeleteFile(LPCWSTR path) {
    if (g_failBackupCleanup && std::filesystem::path(path) == g_backup) {
        SetLastError(ERROR_ACCESS_DENIED);
        return FALSE;
    }
    return DeleteFileW(path);
}
}

#define ReplaceFileW TestReplaceFile
#define MoveFileExW TestMoveFile
#define DeleteFileW TestDeleteFile
#include "host_file.h"
#undef DeleteFileW
#undef MoveFileExW
#undef ReplaceFileW

namespace {
void Require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
std::string Read(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

void CheckPartialFailure(const std::filesystem::path& directory, DWORD error, bool failRestore,
                         bool restoreCollision = false) {
    std::filesystem::create_directory(directory);
    const auto target = directory / "Config.toml";
    { std::ofstream file(target); file << "original contents"; }
    g_failure = error;
    g_failRestore = failRestore;
    g_restoreCollision = restoreCollision;
    g_replacement.clear();
    g_backup.clear();
    Require(!RuntimeHostFile::WriteAtomically(target, "replacement contents"), "partial failure reported success");
    Require(Read(g_replacement) == "replacement contents", "partial failure discarded replacement contents");
    Require(!g_backup.empty(), "replacement did not provide a backup path");
    if (failRestore || restoreCollision) {
        if (restoreCollision) Require(Read(target) == "concurrent writer", "restoration overwrote another writer");
        else Require(!std::filesystem::exists(target), "failed restoration unexpectedly published a file");
        Require(Read(g_backup) == "original contents", "failed restoration discarded original contents");
    } else {
        Require(Read(target) == "original contents", "original contents were not retained or restored");
        Require(!std::filesystem::exists(g_backup), "restored file left a duplicate backup");
    }
    std::filesystem::remove_all(directory);
}
}

int main() {
    const auto root = std::filesystem::temp_directory_path() / ("wiicomp-recovery-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    bool claimed = false;
    try {
        claimed = std::filesystem::create_directory(root);
        Require(claimed, "cannot create test directory");
        CheckPartialFailure(root / "before-move", ERROR_UNABLE_TO_MOVE_REPLACEMENT, false);
        CheckPartialFailure(root / "after-move", ERROR_UNABLE_TO_MOVE_REPLACEMENT_2, false);
        CheckPartialFailure(root / "restore-denied", ERROR_UNABLE_TO_MOVE_REPLACEMENT_2, true);
        CheckPartialFailure(root / "restore-collision", ERROR_UNABLE_TO_MOVE_REPLACEMENT_2, false, true);
        g_restoreCollision = false;
        const auto target = root / "Config.toml";
        { std::ofstream file(target); file << "original contents"; }
        for (DWORD error : {DWORD{ERROR_UNABLE_TO_REMOVE_REPLACED}, DWORD{ERROR_ACCESS_DENIED}}) {
            g_failure = error;
            g_failRestore = false;
            Require(!RuntimeHostFile::WriteAtomically(target, "replacement contents"), "ordinary failure reported success");
            Require(Read(target) == "original contents", "ordinary failure changed original contents");
            Require(!std::filesystem::exists(g_replacement) && !std::filesystem::exists(g_backup),
                    "ordinary failure retained unnecessary recovery files");
        }
        g_failure = 0;
        Require(RuntimeHostFile::WriteAtomically(target, "published contents"), "normal replacement failed");
        Require(Read(target) == "published contents", "normal replacement did not publish");
        for (const auto& entry : std::filesystem::directory_iterator(root)) {
            Require(entry.path() == target, "successful replacement left recovery files");
        }
        g_failBackupCleanup = true;
        Require(!RuntimeHostFile::WriteAtomically(target, "new contents"), "backup cleanup failure reported success");
        Require(Read(target) == "new contents" && Read(g_backup) == "published contents",
                "backup cleanup failure lost published or original contents");
        g_failBackupCleanup = false;
        std::filesystem::remove_all(root);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        std::error_code ignored;
        if (claimed) std::filesystem::remove_all(root, ignored);
        return 1;
    }
}
