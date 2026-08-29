// NAND/ISFS HLE: asynchronous entry points, guest callback dispatch and the
// crash-safe NANDSafeOpen/NANDSafeClose shadow-write machinery.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

// Guest callback dispatch: host NAND work completes synchronously, so an "async" call just
// queues its guest completion callback here and an HLE pump dispatches it later.

struct PendingNandCallback {
    uint32_t callbackPtr = 0;
    int32_t result = 0;
    uint32_t commandBlock = 0;
};

static std::mutex g_pendingNandCallbackMutex;
static std::deque<PendingNandCallback> g_pendingNandCallbacks;

static void DispatchNandCallback(CpuContext* cpu, uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    // The callback signature is: void callback(int result, void* commandBlock)
    // We need to invoke it through the translated function system
    if (!TranslatedFunctionRegistry::FindByAddressPtr(callbackPtr)) {
        LogNandError("InvokeNandCallback", "callback 0x%08X not found in registry", callbackPtr);
        return;
    }

    if (cpu) {
        // IOS/NAND completions are asynchronous side calls. Run them on a
        // scratch guest CPU context so callback argument setup and any
        // transient LR/CTR/nonvolatile register changes do not corrupt the
        // interrupted thread that happened to pump the completion queue.
        CpuContext callbackCpu = *cpu;
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        CpuContextScope callbackScope(&callbackCpu);
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    } else {
        auto& callbackCpu = GetPersistentCpuContext();
        callbackCpu.gpr[3] = static_cast<uint32_t>(result);
        callbackCpu.gpr[4] = commandBlock;
        InvokeIndirectCpu(callbackPtr, &callbackCpu);
    }
}

static void InvokeNandCallback(uint32_t callbackPtr, int32_t result, uint32_t commandBlock) {
    if (callbackPtr == 0) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
    g_pendingNandCallbacks.push_back({callbackPtr, result, commandBlock});
}

void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg) {
    InvokeNandCallback(callbackPtr, result, callbackArg);
}

// Guest NAND callbacks can re-enter this drain: RFL's hidden-Mii loader chains one async
// load per Mii as its own completion callback, and a database fattened by online play
// nested hundreds of frames deep and crashed at boot. Hardware is iterative via the IPC
// interrupt, so a nested drain refuses here and the outermost loop dispatches the queue flat.
static thread_local int g_nandCallbackDrainDepth = 0;

bool NandProcessPendingCallbacks(CpuContext* cpu, int maxToProcess) {
    if (maxToProcess <= 0) {
        return false;
    }
    if (g_nandCallbackDrainDepth != 0) {
        return false;
    }
    struct DrainScope {
        DrainScope() { ++g_nandCallbackDrainDepth; }
        ~DrainScope() { --g_nandCallbackDrainDepth; }
    } drainScope;

    // Host-side NAND work finishes immediately. Drain a generous chunk of the guest
    // callback queue in one pump so multi-step chains (open -> read -> close -> user cb)
    // do not get stranded across many retrace ticks and trip the RFL/Mii error path.
    const size_t maxCallbacksThisPump = std::max<size_t>(static_cast<size_t>(maxToProcess), 64);
    size_t processed = 0;
    while (processed < maxCallbacksThisPump) {
        PendingNandCallback cb{};
        {
            std::lock_guard<std::mutex> lock(g_pendingNandCallbackMutex);
            if (g_pendingNandCallbacks.empty()) {
                break;
            }
            cb = g_pendingNandCallbacks.front();
            g_pendingNandCallbacks.pop_front();
        }
        DispatchNandCallback(cpu, cb.callbackPtr, cb.result, cb.commandBlock);
        ++processed;
    }
    return processed != 0;
}

// Async NAND entry points run the sync call, queue the guest completion callback, and return.
// `params`'s last two args must stay named callbackPtr/commandBlockPtr since the macro bodies
// reference them directly. PPC_NATIVE_OVERRIDE calls stay written out verbatim, not macro-generated,
// because the C# translator regex-scans this file as raw text to decide which addresses to leave untranslated.

// Returns the synchronous result verbatim.
#define NAND_ASYNC_FWD_BODY(Name, Sync, params, sync_args)        \
    extern "C" int32_t Name params {                              \
        int32_t result = Sync sync_args;                          \
        InvokeNandCallback(callbackPtr, result, commandBlockPtr); \
        return result;                                            \
    }

// Reports only success or failure: the transferred count travels to the guest
// through the callback, so a non-negative result collapses to NAND_RESULT_OK.
#define NAND_ASYNC_FWD_BODY_OKZERO(Name, Sync, params, sync_args) \
    extern "C" int32_t Name params {                              \
        int32_t result = Sync sync_args;                          \
        InvokeNandCallback(callbackPtr, result, commandBlockPtr); \
        return (result < 0) ? result : NAND_RESULT_OK;            \
    }

NAND_ASYNC_FWD_BODY(NANDOpenAsync_HLE, NANDOpen_HLE,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode))
PPC_NATIVE_OVERRIDE(8019C918, NANDOpenAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDCloseAsync_HLE, NANDClose_HLE,
    (uint32_t fileInfoPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr))
PPC_NATIVE_OVERRIDE(8019CAEC, NANDCloseAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY_OKZERO(NANDReadAsync_HLE, NANDRead_HLE,
    (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, bufferPtr, length))
PPC_NATIVE_OVERRIDE(8019B80C, NANDReadAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, bufferPtr, length, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY_OKZERO(NANDWriteAsync_HLE, NANDWrite_HLE,
    (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, bufferPtr, length))
PPC_NATIVE_OVERRIDE(8019B8EC, NANDWriteAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, uint32_t bufferPtr, uint32_t length, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, bufferPtr, length, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY_OKZERO(NANDSeekAsync_HLE, NANDSeek_HLE,
    (uint32_t fileInfoPtr, int32_t offset, int32_t whence, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, offset, whence))
PPC_NATIVE_OVERRIDE(8019BA04, NANDSeekAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, int32_t offset, int32_t whence, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, offset, whence, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDGetLengthAsync_HLE, NANDGetLength_HLE,
    (uint32_t fileInfoPtr, uint32_t outLengthPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, outLengthPtr))
PPC_NATIVE_OVERRIDE(8019C048, NANDGetLengthAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, uint32_t outLengthPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, outLengthPtr, callbackPtr, commandBlockPtr));

// The private/safe variants forward to the same synchronous library; only
// NANDPrivateSafeOpen has its own implementation.
extern "C" int32_t NANDSafeOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode,
                                     uint32_t tempBufferPtr, uint32_t tempBufferSize);
extern "C" int32_t NANDSafeClose_HLE(uint32_t fileInfoPtr);
extern "C" int32_t NANDGetType_HLE(uint32_t pathPtr, uint32_t outTypePtr);

NAND_ASYNC_FWD_BODY(NANDPrivateOpenAsync_HLE, NANDOpen_HLE,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode))
PPC_NATIVE_OVERRIDE(8019C990, NANDPrivateOpenAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateSafeOpenAsync_HLE, NANDSafeOpen_HLE,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t tempBufferPtr, uint32_t tempBufferSize,
     uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode, tempBufferPtr, tempBufferSize))
PPC_NATIVE_OVERRIDE(8019D104, NANDPrivateSafeOpenAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t tempBufferPtr, uint32_t tempBufferSize,
     uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, fileInfoPtr, mode, tempBufferPtr, tempBufferSize, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDSafeCloseAsync_HLE, NANDSafeClose_HLE,
    (uint32_t fileInfoPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr))
PPC_NATIVE_OVERRIDE(8019D720, NANDSafeCloseAsync_HLE, int32_t,
    (uint32_t fileInfoPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (fileInfoPtr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateCreateAsync_HLE, NANDCreate_HLE,
    (uint32_t pathPtr, uint32_t perm, uint32_t attr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, perm, attr))
PPC_NATIVE_OVERRIDE(8019B524, NANDPrivateCreateAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t perm, uint32_t attr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, perm, attr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateCreateDirAsync_HLE, NANDCreateDir_HLE,
    (uint32_t pathPtr, uint32_t perm, uint32_t attr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, perm, attr))
PPC_NATIVE_OVERRIDE(8019BCC8, NANDPrivateCreateDirAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t perm, uint32_t attr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, perm, attr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateDeleteAsync_HLE, NANDDelete_HLE,
    (uint32_t pathPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr))
PPC_NATIVE_OVERRIDE(8019B6E4, NANDPrivateDeleteAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateGetStatusAsync_HLE, NANDGetStatus_HLE,
    (uint32_t pathPtr, uint32_t statusPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, statusPtr))
PPC_NATIVE_OVERRIDE(8019C448, NANDPrivateGetStatusAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t statusPtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, statusPtr, callbackPtr, commandBlockPtr));

NAND_ASYNC_FWD_BODY(NANDPrivateGetTypeAsync_HLE, NANDGetType_HLE,
    (uint32_t pathPtr, uint32_t outTypePtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, outTypePtr))
PPC_NATIVE_OVERRIDE(8019E7B4, NANDPrivateGetTypeAsync_HLE, int32_t,
    (uint32_t pathPtr, uint32_t outTypePtr, uint32_t callbackPtr, uint32_t commandBlockPtr),
    (pathPtr, outTypePtr, callbackPtr, commandBlockPtr));

// NANDSafeOpen/NANDSafeClose crash-safe file replacement, mirroring console semantics (nandSafeOpen
// @ 0x8019CB80, nandSafeClose @ 0x8019CF30): write modes copy the original to a same-volume scratch
// file, and safeClose commits it via fflush -> _commit/fsync -> MoveFileEx(REPLACE_EXISTING | WRITE_THROUGH).

static const char kNandSafeTempSuffix[] = ".nandsafe.tmp";

std::filesystem::path SafeTempPathFor(const std::filesystem::path& hostPath) {
    std::filesystem::path tempPath = hostPath;
    tempPath += kNandSafeTempSuffix;
    return tempPath;
}

// Push the CRT buffer out and then force the OS to put it on the platter, so the data is
// durable before the rename that publishes it.
static bool FlushFileToDisk(FILE* file) {
    if (!file) {
        return false;
    }
    if (std::fflush(file) != 0) {
        return false;
    }
#ifdef _WIN32
    const int osFd = _fileno(file);
    if (osFd < 0) {
        return false;
    }
    return _commit(osFd) == 0;
#else
    const int osFd = fileno(file);
    if (osFd < 0) {
        return false;
    }
    return fsync(osFd) == 0;
#endif
}

// Replace `targetPath` with `tempPath` in one step. Either the old or the new contents
// survive a crash; there is no window where the target is truncated or partial.
static bool AtomicReplaceHostFile(const char* who, const std::filesystem::path& tempPath,
                                  const std::filesystem::path& targetPath) {
#ifdef _WIN32
    if (MoveFileExW(tempPath.c_str(), targetPath.c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    LogNandError(who, "ERROR: MoveFileEx('%s' -> '%s') failed (err=%lu)",
            HostPathText(tempPath).c_str(), HostPathText(targetPath).c_str(),
            static_cast<unsigned long>(GetLastError()));
    return false;
#else
    if (!NandRename(tempPath, targetPath)) {
        LogNandError(who, "ERROR: rename('%s' -> '%s') failed",
                HostPathText(tempPath).c_str(), HostPathText(targetPath).c_str());
        return false;
    }
    // Durably record the directory entry so the rename itself survives a crash.
    const std::string directory = targetPath.parent_path().string();
    const int dirFd = open(directory.c_str(), O_RDONLY);
    if (dirFd >= 0) {
        fsync(dirFd);
        close(dirFd);
    }
    return true;
#endif
}

// Remove a scratch file left behind by a previous run that died between safe open and
// safe close. Its contents are worthless: the original was never replaced.
bool DiscardStaleSafeTemp(const std::filesystem::path& tempPath) {
    if (!PathExists(tempPath)) {
        return true;
    }
    LogNandWarning("nand-shadow", "WARNING: discarding stale scratch file '%s' from a previous run",
            HostPathText(tempPath).c_str());
    if (NandRemove(tempPath)) {
        return true;
    }
    LogNandError("nand-shadow", "FAILED to remove stale scratch file '%s'",
            HostPathText(tempPath).c_str());
    return false;
}

// True when any live handle already refers to `hostPath`, either directly or as the
// commit target of a shadow. Used to keep shadow writes from hiding data behind a second
// handle on the same file.
bool IsHostPathOpen(const std::filesystem::path& hostPath) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    for (const auto& entry : g_fileHandles) {
        if (entry.second.path == hostPath || entry.second.safeCommitPath == hostPath) {
            return true;
        }
    }
    return false;
}

// Retire a handle: flush writable handles all the way to disk and, when the handle is a
// shadow, atomically publish it over its commit target. On any failure the shadow is
// dropped and the original is left exactly as it was, and the error is returned so the
// guest's close call fails instead of silently reporting success.
int32_t CommitAndCloseFd(const char* who, int32_t fd, bool missingFdIsError) {
    std::filesystem::path tempPath;
    std::filesystem::path commitPath;
    FILE* file = nullptr;
    int32_t mode = 0;

    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fileHandles.find(fd);
        if (it == g_fileHandles.end()) {
            if (missingFdIsError) {
                LogNandError(who, "FAILED: unknown fd=%d", fd);
                return NAND_RESULT_INVALID;
            }
            return NAND_RESULT_OK;
        }
        tempPath = it->second.path;
        commitPath = it->second.safeCommitPath;
        file = it->second.file;
        mode = it->second.mode;
        g_fileHandles.erase(it);
    }

    const bool needsCommit = !commitPath.empty();
    const bool writable = needsCommit || mode >= 2;

    // Only writable handles get the fsync: _commit/FlushFileBuffers fails on a handle that
    // was opened read-only.
    const bool flushed = writable ? FlushFileToDisk(file) : true;
    if (file) {
        std::fclose(file);
    }

    if (!needsCommit) {
        if (!flushed) {
            LogNandError(who, "ERROR: flush of '%s' failed", HostPathText(tempPath).c_str());
            return NAND_RESULT_UNKNOWN;
        }
        return NAND_RESULT_OK;
    }

    if (!flushed) {
        LogNandError(who, "ERROR: flush of '%s' failed, discarding it and leaving '%s' untouched",
                HostPathText(tempPath).c_str(), HostPathText(commitPath).c_str());
        NandRemove(tempPath);
        return NAND_RESULT_UNKNOWN;
    }

    if (!AtomicReplaceHostFile(who, tempPath, commitPath)) {
        NandRemove(tempPath);
        return NAND_RESULT_UNKNOWN;
    }

    return NAND_RESULT_OK;
}

extern "C" int32_t NANDSafeOpen_HLE(uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode,
                                     uint32_t tempBufferPtr, uint32_t tempBufferSize) {
    (void)tempBufferPtr;
    (void)tempBufferSize;

    const char* path = pathPtr ? (const char*)Memory::GetPointer(pathPtr) : nullptr;
    if (!path || !fileInfoPtr) {
        LogNandError("NANDSafeOpen", "invalid params: path=%p fileInfo=0x%08X", path, fileInfoPtr);
        return NAND_RESULT_INVALID;
    }
    if (mode < 1 || mode > 3) {
        LogNandError("NANDSafeOpen", "FAILED: invalid access type %u", mode);
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path hostPath = TranslateNandPath(path);
    if (hostPath.empty()) {
        LogNandError("NANDSafeOpen", "FAILED to translate path '%s'", path);
        return NAND_RESULT_INVALID;
    }

    // accType is recorded in the guest struct before anything else, exactly as the
    // library does, so a caller inspecting NANDFileInfo sees the same layout.
    Memory::Write8(fileInfoPtr + 0x88, static_cast<uint8_t>(mode));

    if (mode == 1) {
        // Read-only safe open reads the original in place; the library builds no scratch
        // copy for this case.
        FILE* file = NandFopen(hostPath, "rb");
        if (!file && IsFaceLibResourcePath(path) && SeedFaceLibResource(hostPath)) {
            file = NandFopen(hostPath, "rb");
        }
        if (!file) {
            LogNandError("NANDSafeOpen", "FAILED to open '%s' for reading",
                    HostPathText(hostPath).c_str());
            return NAND_RESULT_NOEXISTS;
        }

        const int32_t fd = AllocateFd(hostPath, file, static_cast<int32_t>(mode));
        Memory::Write32(fileInfoPtr, static_cast<uint32_t>(fd));
        Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_SAFE_OPEN);
        return NAND_RESULT_OK;
    }

    // Write modes. The library queries the attributes of the original first, so a safe
    // open of a file that does not exist fails instead of creating one.
    if (!PathExists(hostPath)) {
        LogNandError("NANDSafeOpen", "FAILED: '%s' does not exist, safe open never creates it",
                HostPathText(hostPath).c_str());
        return NAND_RESULT_NOEXISTS;
    }
    if (IsDirectory(hostPath)) {
        LogNandError("NANDSafeOpen", "FAILED: '%s' is a directory", HostPathText(hostPath).c_str());
        return NAND_RESULT_INVALID;
    }

    const std::filesystem::path tempPath = SafeTempPathFor(hostPath);
    if (!DiscardStaleSafeTemp(tempPath)) {
        return NAND_RESULT_ACCESS;
    }

    // The scratch file starts as a byte-for-byte copy of the original and the guest is
    // positioned at offset 0, so partial writes keep the bytes they never touch.
    std::error_code ec;
    std::filesystem::copy_file(hostPath, tempPath,
                               std::filesystem::copy_options::overwrite_existing, ec);
    if (ec) {
        LogNandError("NANDSafeOpen", "FAILED to seed scratch file '%s' from '%s': %s",
                HostPathText(tempPath).c_str(), HostPathText(hostPath).c_str(),
                ec.message().c_str());
        NandRemove(tempPath);
        return NAND_RESULT_UNKNOWN;
    }

    FILE* file = NandFopen(tempPath, "r+b");
    if (!file) {
        LogNandError("NANDSafeOpen", "FAILED to open scratch file '%s'",
                HostPathText(tempPath).c_str());
        NandRemove(tempPath);
        return NAND_RESULT_UNKNOWN;
    }

    const int32_t fd = AllocateFd(tempPath, file, static_cast<int32_t>(mode));
    {
        std::lock_guard<std::mutex> lock(g_fdMutex);
        auto it = g_fileHandles.find(fd);
        if (it != g_fileHandles.end()) {
            it->second.safeCommitPath = hostPath;
        }
    }

    Memory::Write32(fileInfoPtr, static_cast<uint32_t>(fd));
    Memory::Write8(fileInfoPtr + 0x8a, NAND_OPEN_FLAG_SAFE_OPEN);
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019CB74, NANDSafeOpen_HLE, int32_t,
    (uint32_t pathPtr, uint32_t fileInfoPtr, uint32_t mode, uint32_t tempBufferPtr, uint32_t tempBufferSize),
    (pathPtr, fileInfoPtr, mode, tempBufferPtr, tempBufferSize));

extern "C" int32_t NANDSafeClose_HLE(uint32_t fileInfoPtr) {
    if (!fileInfoPtr) {
        return NAND_RESULT_INVALID;
    }

    uint8_t openFlag = 0;
    try {
        openFlag = Memory::Read8(fileInfoPtr + 0x8a);
    } catch (const Memory::AccessViolation&) {
        LogNandError("NANDSafeClose", "memory access violation while reading open flag");
        return NAND_RESULT_INVALID;
    }

    if (openFlag == NAND_OPEN_FLAG_OPEN) {
        // Handle came from a plain NANDOpen; there is no pending commit to publish.
        LogNandWarning("NANDSafeClose", "WARNING: handle was opened with NANDOpen (flag=%u) -> plain close", openFlag);
        return NANDClose_HLE(fileInfoPtr);
    }

    if (openFlag != NAND_OPEN_FLAG_SAFE_OPEN && openFlag != NAND_OPEN_FLAG_SAFE_OPEN_ASYNC) {
        // Safe close is allowed to be called after async sequences that already closed the file.
        return NAND_RESULT_OK;
    }

    const int32_t fd = static_cast<int32_t>(Memory::Read32(fileInfoPtr));
    const int32_t result = CommitAndCloseFd("NANDSafeClose", fd, /*missingFdIsError=*/true);
    if (result != NAND_RESULT_OK) {
        // Leave openFlag alone: NandUtil_safeClose retries transient failures.
        return result;
    }

    Memory::Write8(fileInfoPtr + 0x8a,
                   (openFlag == NAND_OPEN_FLAG_SAFE_OPEN) ? NAND_OPEN_FLAG_SAFE_CLOSED
                                                          : NAND_OPEN_FLAG_SAFE_CLOSED_ASYNC);
    return NAND_RESULT_OK;
}
PPC_NATIVE_OVERRIDE(8019CF28, NANDSafeClose_HLE, int32_t, (uint32_t fileInfoPtr), (fileInfoPtr));
