// NAND/ISFS HLE: the synchronous RVL NAND* library.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

// ============================================================================
// Local helpers
// ============================================================================

// Unchecked NUL-terminated write into guest memory. The RVL NAND library hands
// out fixed-size caller buffers and never tells us how big they are, so this
// deliberately matches the library's own unbounded copy.
static void WriteGuestNandPath(uint32_t address, const std::string& value) {
    for (size_t i = 0; i < value.size(); ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
    Memory::Write8(address + static_cast<uint32_t>(value.size()), 0);
}

// NANDGetHomeDir and NANDGetCurrentDir both answer with the title's data
// directory; the SDK keeps no separate working directory for us to track.
static int32_t WriteNandDataDir(uint32_t outPathPtr) {
    if (!outPathPtr) {
        return NAND_RESULT_INVALID;
    }
    WriteGuestNandPath(outPathPtr, CurrentNandDataDir());
    return NAND_RESULT_OK;
}

// fileInfoPtr -> fd -> live handle. Logs and returns null when the guest hands
// us a descriptor that was never opened (or was already closed).
static FileHandle* ResolveNandFileHandle(const char* who, uint32_t fileInfoPtr) {
    const int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));
    FileHandle* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError(who, "invalid fd=%d", fd);
        return nullptr;
    }
    return handle;
}

// ============================================================================
// The synchronous RVL NAND* library
// ============================================================================

extern "C" int32_t NANDInit_HLE(void) {
    // Initialize ISFS
    ISFS_OpenLib_Initialize(&GetPersistentCpuContext());

    // NANDHomeDir is at 0x80346D20 (from ESP_GetDataDir output)
    WriteGuestNandPath(0x80346D20, CurrentNandDataDir());

    // Mark NAND as initialized (0x80386848 = 2)
    Memory::Write32(0x80386848, 2);

    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019E18C, NANDInit_HLE, int32_t, (void), ());

extern "C" int32_t NANDGetCurrentDir_HLE(uint32_t outPathPtr) {
    return WriteNandDataDir(outPathPtr);
}
PPC_NATIVE_OVERRIDE(8019E390, NANDGetCurrentDir_HLE, int32_t, (uint32_t outPathPtr), (outPathPtr));

extern "C" int32_t NANDCheck_HLE(uint32_t blockSize, uint32_t blockCount, uint32_t outResults) {
    const int32_t result = NandCheckContract::WriteHealthyResult(
        outResults,
        [](uint32_t address, size_t length) { return Memory::Contains(address, length); },
        [](uint32_t address, uint32_t value) {
            try {
                Memory::Write32(address, value);
                return true;
            } catch (const Memory::AccessViolation&) {
                return false;
            }
        });
    if (result != NAND_RESULT_OK) {
        LogNandError("NANDCheck",
                     "FAILED: invalid four-byte result buffer 0x%08X (blockSize=%u blockCount=%u)",
                     outResults, blockSize, blockCount);
    }
    return result;
}
PPC_NATIVE_OVERRIDE(8019EAD0, NANDCheck_HLE, int32_t, (uint32_t blockSize, uint32_t blockCount, uint32_t outResults), (blockSize, blockCount, outResults));

extern "C" int32_t NANDOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !fileInfoPtr) {
        LogNandError("NANDOpen", "invalid params: path=%p fileInfo=0x%08X", path, fileInfoPtr);
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    // Existing-file write opens go through a shadow copy seeded from the original, so a
    // crash between NANDWrite and NANDClose cannot leave a torn file (the game patches
    // sub-ranges, e.g. ghost saves at a non-zero offset). New files still create in place.
    if ((mode == 2 || mode == 3) && PathExists(hostPath) && !IsDirectory(hostPath)) {
        if (IsHostPathOpen(hostPath)) {
            // Another live handle already refers to this file. A shadow would hide the
            // writes from that handle, so stay in place for this open.
            LogNandWarning("NANDOpen", "WARNING: '%s' already has a live handle, writing in place",
                           HostPathText(hostPath).c_str());
        } else {
            const std::filesystem::path tempPath = SafeTempPathFor(hostPath);
            if (DiscardStaleSafeTemp(tempPath)) {
                std::error_code ec;
                std::filesystem::copy_file(hostPath, tempPath,
                                           std::filesystem::copy_options::overwrite_existing, ec);
                if (ec) {
                    LogNandWarning("NANDOpen", "WARNING: could not seed shadow '%s' (%s), writing in place",
                                   HostPathText(tempPath).c_str(), ec.message().c_str());
                    NandRemove(tempPath);
                } else {
                    FILE* shadow = NandFopen(tempPath, "r+b");
                    if (!shadow) {
                        LogNandWarning("NANDOpen", "WARNING: could not open shadow '%s', writing in place",
                                       HostPathText(tempPath).c_str());
                        NandRemove(tempPath);
                    } else {
                        const int32_t shadowFd = AllocateFd(tempPath, shadow, static_cast<int32_t>(mode));
                        {
                            std::lock_guard<std::mutex> lock(g_fdMutex);
                            auto it = g_fileHandles.find(shadowFd);
                            if (it != g_fileHandles.end()) {
                                it->second.safeCommitPath = hostPath;
                            }
                        }
                        Memory::Write32(fileInfoPtr, static_cast<uint32_t>(shadowFd));
                        Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
                        return NAND_RESULT_OK;
                    }
                }
            }
        }
    }

    const char* fopenMode = "rb";
    if (mode == 1) fopenMode = "rb";
    else if (mode == 2) fopenMode = "r+b";
    else if (mode == 3) fopenMode = "r+b";
    
    FILE* file = NandFopen(hostPath, fopenMode);
    if (!file && mode >= 2) {
        // Try creating for write modes
        file = NandFopen(hostPath, "w+b");
    }
    
    // Create parent directories and retry
    if (!file && CreateParentDirectories(hostPath)) {
        file = NandFopen(hostPath, mode >= 2 ? "w+b" : "rb");
    }

    if (!file) {
        if (IsFaceLibResourcePath(path) && SeedFaceLibResource(hostPath)) {
            file = NandFopen(hostPath, fopenMode);
        }
        if (!file) {
            LogNandError("NANDOpen", "FAILED to open");
            return NAND_RESULT_NOEXISTS;
        }
    }

    int32_t fd = AllocateFd(hostPath, file, mode);
    Memory::Write32(fileInfoPtr, static_cast<uint32_t>(fd));
    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_OPEN);
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019C800, NANDOpen_HLE, int32_t, (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode), (pathPtr, fileInfoPtr, mode));

extern "C" int32_t NANDClose_HLE(uint32_t fileInfoPtr) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }
    
    uint8_t openFlag = Memory::Read8(fileInfoPtr + 0x8a);
    if (openFlag == NAND_OPEN_FLAG_SAFE_OPEN || openFlag == NAND_OPEN_FLAG_SAFE_OPEN_ASYNC) {
        // Console behaviour (nandClose @ 0x8019CA80): only openFlag==1 is accepted, a safe
        // handle falls through to -8. It is neither closed nor committed here.
        LogNandWarning("NANDClose", "WARNING: refusing safe-open handle (flag=%u), NANDSafeClose is required", openFlag);
        return NAND_RESULT_INVALID;
    }
    if (openFlag != NAND_OPEN_FLAG_OPEN) {
        LogNandWarning("NANDClose", "file not open (flag=%u)", openFlag);
        return NAND_RESULT_INVALID;
    }

    int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));

    // Flush, fsync and (for shadowed write handles) atomically publish. A failure here is
    // reported to the guest instead of being swallowed, so NandUtil_close surfaces it as a
    // save error rather than a silent success on top of a half-written file.
    const int32_t result = CommitAndCloseFd("NANDClose", fd, /*missingFdIsError=*/false);
    if (result != NAND_RESULT_OK) {
        return result;
    }

    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_CLOSED); // Mark as closed
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019CA80, NANDClose_HLE, int32_t, (uint32_t fileInfoPtr), (fileInfoPtr));

extern "C" int32_t NANDRead_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDRead", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    uint8_t* buffer = (uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        return NAND_RESULT_INVALID;
    }

    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(8019B7A4, NANDRead_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

extern "C" int32_t NANDWrite_HLE(uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDWrite", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    const uint8_t* buffer = (const uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        return NAND_RESULT_INVALID;
    }

    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(8019B884, NANDWrite_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length), (fileInfoPtr, bufferPtr, length));

extern "C" int32_t NANDSeek_HLE(uint32_t fileInfoPtr, int32_t offset, int32_t whence) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDSeek", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        return NAND_RESULT_UNKNOWN;
    }

    return static_cast<int32_t>(std::ftell(handle->file));
}
PPC_NATIVE_OVERRIDE(8019B964, NANDSeek_HLE, int32_t, (uint32_t fileInfoPtr, int32_t offset, int32_t whence), (fileInfoPtr, offset, whence));

extern "C" int32_t NANDGetLength_HLE(uint32_t fileInfoPtr, uint32_t outLengthPtr) {
    if (!fileInfoPtr || !outLengthPtr) {
        return NAND_RESULT_INVALID;
    }

    FileHandle* handle = ResolveNandFileHandle("NANDGetLength", fileInfoPtr);
    if (!handle) {
        return NAND_RESULT_INVALID;
    }

    const NandFileExtent extent = NandProbeFileExtent(handle->file);
    Memory::Write32(outLengthPtr, static_cast<uint32_t>(extent.size));
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019BF4C, NANDGetLength_HLE, int32_t, (uint32_t fileInfoPtr, uint32_t outLengthPtr), (fileInfoPtr, outLengthPtr));

extern "C" int32_t NANDCreate_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    CreateParentDirectories(hostPath);

    // Check if file already exists
    if (PathExists(hostPath)) {
        return NAND_RESULT_EXISTS;
    }
    
    // Create empty file
    FILE* f = NandFopen(hostPath, "wb");
    if (!f) {
        return NAND_RESULT_UNKNOWN;
    }
    std::fclose(f);
    
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019B43C, NANDCreate_HLE, int32_t, (uint32_t pathPtr, uint32_t perm, uint32_t attr), (pathPtr, perm, attr));

extern "C" int32_t NANDDelete_HLE(uint32_t pathPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    if (NandRemove(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
PPC_NATIVE_OVERRIDE(8019B59C, NANDDelete_HLE, int32_t, (uint32_t pathPtr), (pathPtr));

extern "C" int32_t NANDCreateDir_HLE(uint32_t pathPtr, uint32_t perm, uint32_t attr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (PathExists(hostPath)) {
        if (IsDirectory(hostPath)) {
            return NAND_RESULT_OK; // Already exists
        }
        return NAND_RESULT_EXISTS;
    }
    
    if (CreateDirectoryPath(hostPath)) {
        return NAND_RESULT_OK;
    }
    
    return NAND_RESULT_UNKNOWN;
}
PPC_NATIVE_OVERRIDE(8019BBE0, NANDCreateDir_HLE, int32_t, (uint32_t pathPtr, uint32_t perm, uint32_t attr), (pathPtr, perm, attr));

extern "C" int32_t NANDMove_HLE(uint32_t srcPathPtr, uint32_t dstPathPtr) {
    const char* srcPath = srcPathPtr ? (const char*)Memory::GetPointer(srcPathPtr) : nullptr;
    const char* dstPath = dstPathPtr ? (const char*)Memory::GetPointer(dstPathPtr) : nullptr;
    
    if (!srcPath || !dstPath) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path srcHost = TranslateNandPath(srcPath);
    const std::filesystem::path dstDirectoryHost = TranslateNandPath(dstPath);
    const std::filesystem::path srcName = srcHost.filename();
    if (srcName.empty()) {
        return NAND_RESULT_INVALID;
    }

    // RVL SDK nandMove always appends the source entry's relative name to the
    // second argument. The latter is a directory, not a complete destination
    // filename (for example /tmp/banner.bin -> <title home>/banner.bin).
    const std::filesystem::path dstHost = dstDirectoryHost / srcName;

    if (!PathExists(srcHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    if (!IsDirectory(dstDirectoryHost)) {
        return NAND_RESULT_NOEXISTS;
    }
    if (PathExists(dstHost)) {
        return NAND_RESULT_EXISTS;
    }

    std::error_code ec;
    std::filesystem::rename(srcHost, dstHost, ec);
    if (!ec) {
        return NAND_RESULT_OK;
    }

    LogNandError("NANDMove", "FAILED error=%d message='%s'", ec.value(), ec.message().c_str());
    return NAND_RESULT_UNKNOWN;
}
PPC_NATIVE_OVERRIDE(8019BEE8, NANDMove_HLE, int32_t, (uint32_t srcPathPtr, uint32_t dstPathPtr), (srcPathPtr, dstPathPtr));

extern "C" int32_t NANDGetStatus_HLE(uint32_t pathPtr, uint32_t outStatusPtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !outStatusPtr) {
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);

    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // NANDStatus structure - fill with fake values
    // This is typically used to check permissions and file type
    Memory::Write32(outStatusPtr, 0);      // Magic/type
    Memory::Write32(outStatusPtr + 4, 0);  // Permissions  
    
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019C380, NANDGetStatus_HLE, int32_t, (uint32_t pathPtr, uint32_t outStatusPtr), (pathPtr, outStatusPtr));

extern "C" int32_t NANDGetType_HLE(uint32_t pathPtr, uint32_t outTypePtr) {
    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !outTypePtr) {
        return NAND_RESULT_INVALID;
    }
    
    const std::filesystem::path hostPath = TranslateNandPath(path);
    
    if (!PathExists(hostPath)) {
        return NAND_RESULT_NOEXISTS;
    }
    
    // 1 = file, 2 = directory
    uint8_t type = IsDirectory(hostPath) ? 2 : 1;
    Memory::Write8(outTypePtr, type);
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019E770, NANDGetType_HLE, int32_t, (uint32_t pathPtr, uint32_t outTypePtr), (pathPtr, outTypePtr));

// ============================================================================
// contentFastOpenNAND/contentReadNAND/contentCloseNAND: NAND-installed content (WADs) never
// exists here, everything comes from the DVD image, so refusing is the correct SDK answer.
// ============================================================================

extern "C" int32_t contentFastOpenNAND_HLE(uint32_t contentId, uint32_t outHandlePtr) {
    return ISFS_ENOENT;
}
PPC_NATIVE_OVERRIDE(8015BC80, contentFastOpenNAND_HLE, int32_t, (uint32_t contentId, uint32_t outHandlePtr), (contentId, outHandlePtr));

extern "C" int32_t contentReadNAND_HLE(uint32_t handlePtr, uint32_t buffer, uint32_t length, uint32_t outReadPtr) {
    return ISFS_EINVAL;
}
PPC_NATIVE_OVERRIDE(8015BCF8, contentReadNAND_HLE, int32_t, (uint32_t handlePtr, uint32_t buffer, uint32_t length, uint32_t outReadPtr), (handlePtr, buffer, length, outReadPtr));
