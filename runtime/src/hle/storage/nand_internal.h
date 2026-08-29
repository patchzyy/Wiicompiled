// Shared NAND/ISFS HLE internals, split across nand_fs.cpp (config, logging, fds, paths, FaceLib),
// nand_isfs.cpp (IOS_*/ISFS layer), nand_api.cpp (sync NAND* library), and nand_async.cpp (async
// wrappers, NANDSafeOpen/Close). Anything more than one of those needs is declared here, defined once.

#pragma once

#include "hle_stubs.h"
#include "nand_check_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"
#include "nand_path.h"
#include "hle/net/network.h"
#include "recomp_mod_loader.h"
#include "runtime_config.h"
#include "runtime_product.h"
#include "wii_es_crypto.h"

#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fstream>
#include <deque>
#include <map>
#include <mutex>
#include <vector>
#include <filesystem>
#include <string>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// Mario Kart Wii Title ID
namespace {
constexpr uint32_t kNandTitleIdHi = 0x00010004;
constexpr uint32_t kNandTitleIdLo = 0x524D4350; // "RMCP" fallback
} // anonymous namespace

// ============================================================================
// Logging
// ============================================================================

// Always-on product logging: NAND failures are how save corruption and a
// misconfigured NAND root reach the player. Severity is chosen by the call site
// - never by inspecting the format string - and both levels print the same way;
// the split records intent and is the single place to add filtering later.
void LogNandError(const char* func, const char* fmt, ...);
void LogNandWarning(const char* func, const char* fmt, ...);

// ============================================================================
// File Descriptor Management
// ============================================================================

struct FileHandle {
    FILE* file = nullptr;
    std::filesystem::path path;
    int32_t mode = 0; // 1=read, 2=write, 3=read/write
    uint32_t position = 0;
    // Non-empty only for write-mode NANDSafeOpen handles. `path` then points at the
    // sibling scratch file the guest is writing into, and this is the original file it
    // atomically replaces on NANDSafeClose.
    std::filesystem::path safeCommitPath;
};

extern std::map<int32_t, FileHandle> g_fileHandles;
extern std::mutex g_fdMutex;

int32_t AllocateFd(const std::filesystem::path& path, FILE* file, int32_t mode);
FileHandle* GetHandle(int32_t fd);
void CloseFd(int32_t fd);

// ============================================================================
// Path Translation
// ============================================================================

uint32_t CurrentMkwTitleIdLo();
std::string CurrentNandDataDir();
const std::filesystem::path& GetNandBasePath();
std::filesystem::path TranslateNandPath(const char* wiiPath);

bool CreateDirectoryPath(const std::filesystem::path& path);
bool PathExists(const std::filesystem::path& path);
bool IsDirectory(const std::filesystem::path& path);

// Create the directory that contains `path`. False when `path` has no directory
// component, i.e. there was nothing to create.
bool CreateParentDirectories(const std::filesystem::path& path);

// Host paths keep their native encoding end to end; these are the only places a NAND
// path is narrowed, and they narrow to UTF-8 for display.
std::string HostPathText(const std::filesystem::path& path);

// fopen takes an ANSI-codepage name on Windows, which cannot express every path.
FILE* NandFopen(const std::filesystem::path& path, const char* mode);
bool NandRemove(const std::filesystem::path& path);
bool NandRename(const std::filesystem::path& from, const std::filesystem::path& to);

bool SeedFaceLibResource(const std::filesystem::path& hostPath);
bool IsFaceLibResourcePath(const char* path);

// ============================================================================
// stdio helpers shared by the NAND* library and the IOS_* device layer
// ============================================================================

// Guest seek whence (0/1/2) -> stdio origin. Anything else keeps SEEK_SET.
int NandSeekOrigin(int32_t whence);

struct NandFileExtent {
    long position = 0; // stream position on entry; restored before returning
    long size = 0;
};

NandFileExtent NandProbeFileExtent(FILE* file);

// ============================================================================
// NAND Error Codes
// ============================================================================

enum NANDResult {
    NAND_RESULT_OK = 0,
    NAND_RESULT_ACCESS = -1,
    NAND_RESULT_ALLOC_FAILED = -2,
    NAND_RESULT_BUSY = -3,
    NAND_RESULT_CORRUPT = -4,
    NAND_RESULT_ECC_CRIT = -5,
    NAND_RESULT_EXISTS = -6,
    NAND_RESULT_INVALID = -8,
    NAND_RESULT_MAXBLOCKS = -9,
    NAND_RESULT_MAXFD = -10,
    NAND_RESULT_MAXFILES = -11,
    NAND_RESULT_NOEXISTS = -12,
    NAND_RESULT_NOTEMPTY = -13,
    NAND_RESULT_OPENFD = -14,
    NAND_RESULT_AUTHENTICATION = -15,
    NAND_RESULT_UNKNOWN = -64,
    NAND_RESULT_FATAL_ERROR = -128,
};

// NANDFileInfo::openFlag (offset 0x8a). The RVL NAND library uses distinct values for
// plain and safe handles so NANDClose/NANDSafeClose can reject the wrong pairing
// (see nandOpen/nandClose/nandSafeOpen/nandSafeClose at 0x8019C800..0x8019CF30).
enum NANDOpenFlag {
    NAND_OPEN_FLAG_NONE = 0,
    NAND_OPEN_FLAG_OPEN = 1,               // NANDOpen
    NAND_OPEN_FLAG_CLOSED = 2,             // NANDClose
    NAND_OPEN_FLAG_SAFE_OPEN = 3,          // NANDSafeOpen (sync)
    NAND_OPEN_FLAG_SAFE_CLOSED = 4,        // NANDSafeClose (sync)
    NAND_OPEN_FLAG_SAFE_OPEN_ASYNC = 5,    // NANDSafeOpenAsync
    NAND_OPEN_FLAG_SAFE_CLOSED_ASYNC = 6,  // NANDSafeCloseAsync
};

// ISFS error codes (IOS filesystem)
enum ISFSResult {
    ISFS_OK = 0,
    ISFS_EINVAL = -101,      // Invalid argument
    ISFS_EACCESS = -102,     // Permission denied
    ISFS_ECORRUPT = -103,    // Data corrupted
    ISFS_EEXIST = -105,      // File exists
    ISFS_ENOENT = -106,      // No such file/directory
    ISFS_ENOMEM = -107,      // Out of memory
    ISFS_EFULL = -108,       // Filesystem full
    ISFS_ENOTEMPTY = -109,   // Directory not empty
    ISFS_EBUSY = -110,       // Resource busy
    ISFS_ENOENT2 = -4,       // Alternative no such file
    ISFS_EIO = -114,         // I/O error
    ISFS_MAXFD = -22,        // Max file descriptors
};

// ============================================================================
// ISFS library initialization (defined in nand_isfs.cpp)
// ============================================================================

int32_t ISFS_OpenLib_Initialize(CpuContext* ctx);

// Shadow-write machinery, defined with the NANDSafeOpen/NANDSafeClose section below.
std::filesystem::path SafeTempPathFor(const std::filesystem::path& hostPath);
bool DiscardStaleSafeTemp(const std::filesystem::path& tempPath);
bool IsHostPathOpen(const std::filesystem::path& hostPath);
int32_t CommitAndCloseFd(const char* who, int32_t fd, bool missingFdIsError);

// Synchronous NAND library entry points (defined in nand_api.cpp); the async
// wrappers in nand_async.cpp forward to them.
extern "C" int32_t NANDOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode);
extern "C" int32_t NANDClose_HLE(uint32_t fileInfoPtr);
extern "C" int32_t NANDRead_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NANDWrite_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length);
extern "C" int32_t NANDSeek_HLE(uint32_t fileInfoPtr, int32_t offset, int32_t whence);
extern "C" int32_t NANDGetLength_HLE(uint32_t fileInfoPtr, uint32_t outLengthPtr);
extern "C" int32_t NANDCreate_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr);
extern "C" int32_t NANDDelete_HLE(uint32_t pathPtr);
extern "C" int32_t NANDCreateDir_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr);
extern "C" int32_t NANDGetStatus_HLE(uint32_t pathPtr, uint32_t outStatusPtr);
