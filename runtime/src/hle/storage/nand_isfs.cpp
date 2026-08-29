// NAND/ISFS HLE: the IOS_* device layer.
//
// Shared state and helpers live in nand_internal.h.

#include "nand_internal.h"

#include "runtime_log.h"

extern "C" void OSSleepThread_HLE_801aa9b8(CpuContext* ctx);

// ============================================================================
// SHA device handles
// ============================================================================

struct ShaHandle {
    CryptoPP::SHA1 hash;
    uint64_t byteCount = 0;

    void Restart() {
        hash.Restart();
        byteCount = 0;
    }
};

static std::map<int32_t, ShaHandle> g_shaHandles;
static int32_t g_nextShaFd = 0x10001;
static std::mutex g_shaMutex;

static int32_t AllocateShaFd() {
    std::lock_guard<std::mutex> lock(g_shaMutex);
    const int32_t fd = g_nextShaFd++;
    g_shaHandles.try_emplace(fd);
    return fd;
}

static ShaHandle* GetShaHandle(int32_t fd) {
    auto it = g_shaHandles.find(fd);
    if (it == g_shaHandles.end()) {
        return nullptr;
    }
    return &it->second;
}

static void CloseShaFd(int32_t fd) {
    std::lock_guard<std::mutex> lock(g_shaMutex);
    g_shaHandles.erase(fd);
}

struct ISFSFileStats {
    uint32_t length;    // File size in bytes
    uint32_t position;  // Current file position
};

// ============================================================================
// Device identifiers and ioctl commands
// ============================================================================

// Special FD for /dev/fs (the ISFS device)
static constexpr int32_t ISFS_DEV_FD = 1;
static constexpr int32_t ES_DEV_FD = 3;
static constexpr int32_t DOLPHIN_DEV_FD = 4;
static constexpr uint32_t ES_IOCTL_GETDEVICEID = 0x07;
static constexpr uint32_t ES_IOCTL_GETDEVICECERT = 0x1E;
static constexpr uint32_t ES_IOCTL_GETTITLEID = 0x20;
static constexpr uint32_t ES_IOCTL_SIGN = 0x30;
static constexpr uint32_t DOLPHIN_IOCTL_GET_ELAPSED_TIME = 0x01;
static constexpr uint32_t DOLPHIN_IOCTL_GET_VERSION = 0x02;
static constexpr uint32_t DOLPHIN_IOCTL_GET_SPEED_LIMIT = 0x03;
static constexpr uint32_t DOLPHIN_IOCTL_SET_SPEED_LIMIT = 0x04;
static constexpr uint32_t DOLPHIN_IOCTL_GET_CPU_SPEED = 0x05;
static constexpr uint32_t DOLPHIN_IOCTL_GET_REAL_PRODUCT_CODE = 0x06;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_SET_CLIENT = 0x07;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_SET_PRESENCE = 0x08;
static constexpr uint32_t DOLPHIN_IOCTL_DISCORD_RESET = 0x09;
static constexpr uint32_t DOLPHIN_IOCTL_GET_SYSTEM_TIME = 0x0A;
static constexpr uint32_t SHA_IOCTL_INIT = 0;
static constexpr uint32_t SHA_IOCTL_UPDATE = 1;
static constexpr uint32_t SHA_IOCTL_FINAL = 2;

static std::string ReadGuestCString(uint32_t address, size_t maxLength = 1024) {
    std::string text;
    if (address == 0) {
        return text;
    }

    for (size_t i = 0; i < maxLength; ++i) {
        const uint32_t current = address + static_cast<uint32_t>(i);
        if (!Memory::Contains(current, 1)) {
            break;
        }
        const char ch = static_cast<char>(Memory::Read8(current));
        if (ch == '\0') {
            break;
        }
        text.push_back(ch);
    }
    return text;
}
static constexpr uint32_t SHA_CONTEXT_SIZE = 0x1c;
static constexpr uint32_t SHA_DIGEST_SIZE = 0x14;

// Same guest layout as the /dev/net ioctlv descriptors; see runtime_parse_helpers.h.
using IosVector = RuntimeHle::IoVector;
using RuntimeHle::ReadIoVector;

static IosVector ReadIosVector(uint32_t vectorPtr, uint32_t index) {
    return ReadIoVector(vectorPtr, index);
}

static uint64_t CurrentMkwTitleId() {
    uint32_t low = CurrentMkwTitleIdLo();
    return (static_cast<uint64_t>(kNandTitleIdHi) << 32) | low;
}

static bool WriteGuestBytes(uint32_t address, uint32_t size, const uint8_t* data, size_t dataSize) {
    if (address == 0 || size < dataSize || !Memory::Contains(address, dataSize)) {
        return false;
    }
    uint8_t* out = Memory::GetPointer(address, dataSize);
    std::memcpy(out, data, dataSize);
    return true;
}

static bool IsValidGuestRange(uint32_t address, uint32_t size) {
    return size == 0 || (address != 0 && Memory::Contains(address, size));
}

using DolphinClock = std::chrono::steady_clock;
// Dolphin starts this clock when the emulation device is constructed, not on its first ioctl.
static const DolphinClock::time_point g_dolphinElapsedStart = DolphinClock::now();

static uint32_t DolphinElapsedMilliseconds() {
    return static_cast<uint32_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(DolphinClock::now() - g_dolphinElapsedStart).count());
}

static int32_t HandleDolphinIoctlv(uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (vectorPtr != 0 && !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
        return ISFS_EINVAL;
    }

    // Every /dev/dolphin command answers through exactly one output vector.
    // `minimumSize` 0 means the case validates the buffer itself (WriteGuestBytes).
    const auto singleOut = [&](uint32_t minimumSize, IosVector& out) -> bool {
        if (numOut != 1 || vectorPtr == 0) {
            return false;
        }
        out = ReadIosVector(vectorPtr, numIn);
        return minimumSize == 0 ||
               (out.size >= minimumSize && Memory::Contains(out.address, minimumSize));
    };

    IosVector out;
    switch (cmd) {
        case DOLPHIN_IOCTL_GET_ELAPSED_TIME: {
            if (!singleOut(4u, out)) {
                return ISFS_EINVAL;
            }
            Memory::Write32(out.address, DolphinElapsedMilliseconds());
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_VERSION: {
            if (!singleOut(0u, out)) {
                return ISFS_EINVAL;
            }
            static constexpr char kVersion[] = "WiiCompiled-DolphinDevice";
            if (!WriteGuestBytes(out.address, out.size,
                                 reinterpret_cast<const uint8_t*>(kVersion), sizeof(kVersion))) {
                return ISFS_EINVAL;
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_SPEED_LIMIT:
        case DOLPHIN_IOCTL_GET_CPU_SPEED: {
            if (!singleOut(4u, out)) {
                return ISFS_EINVAL;
            }
            Memory::Write32(out.address, cmd == DOLPHIN_IOCTL_GET_SPEED_LIMIT ? 100u : 729000000u);
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_GET_REAL_PRODUCT_CODE: {
            if (!singleOut(0u, out)) {
                return ISFS_EINVAL;
            }
            char productCode[8] = {};
            uint32_t discId = Memory::Contains(0x80000000u, 4u) ? Memory::Read32(0x80000000u) : kNandTitleIdLo;
            productCode[0] = static_cast<char>((discId >> 24) & 0xffu);
            productCode[1] = static_cast<char>((discId >> 16) & 0xffu);
            productCode[2] = static_cast<char>((discId >> 8) & 0xffu);
            productCode[3] = static_cast<char>(discId & 0xffu);
            productCode[4] = '0';
            productCode[5] = '1';
            if (!WriteGuestBytes(out.address, out.size,
                                 reinterpret_cast<const uint8_t*>(productCode), sizeof(productCode))) {
                return ISFS_EINVAL;
            }
            return ISFS_OK;
        }

        case DOLPHIN_IOCTL_SET_SPEED_LIMIT:
        case DOLPHIN_IOCTL_DISCORD_SET_CLIENT:
        case DOLPHIN_IOCTL_DISCORD_SET_PRESENCE:
        case DOLPHIN_IOCTL_DISCORD_RESET:
            return ISFS_OK;

        case DOLPHIN_IOCTL_GET_SYSTEM_TIME: {
            if (!singleOut(8u, out)) {
                return ISFS_EINVAL;
            }
            const auto nowMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            const uint64_t value = static_cast<uint64_t>(nowMs);
            Memory::Write32(out.address, static_cast<uint32_t>(value >> 32));
            Memory::Write32(out.address + 4u, static_cast<uint32_t>(value));
            return ISFS_OK;
        }

        default:
            LogNandWarning("IOS_Ioctlv", "/dev/dolphin unsupported cmd=%u", cmd);
            return ISFS_EINVAL;
    }
}

static bool WriteShaOutputs(const ShaHandle& handle, const IosVector& context, const IosVector& hash) {
    CryptoPP::SHA1 snapshot = handle.hash;
    std::array<uint8_t, CryptoPP::SHA1::DIGESTSIZE> digest{};
    snapshot.Final(digest.data());
    if (!WriteGuestBytes(hash.address, hash.size, digest.data(), digest.size())) {
        return false;
    }

    if (context.address != 0 && context.size >= SHA_CONTEXT_SIZE && Memory::Contains(context.address, SHA_CONTEXT_SIZE)) {
        Memory::Write32(context.address + 0x00, 0x67452301u);
        Memory::Write32(context.address + 0x04, 0xEFCDAB89u);
        Memory::Write32(context.address + 0x08, 0x98BADCFEu);
        Memory::Write32(context.address + 0x0c, 0x10325476u);
        Memory::Write32(context.address + 0x10, 0xC3D2E1F0u);
        const uint64_t bitCount = handle.byteCount * 8u;
        Memory::Write32(context.address + 0x14, static_cast<uint32_t>(bitCount >> 32));
        Memory::Write32(context.address + 0x18, static_cast<uint32_t>(bitCount));
    }

    return true;
}

static int32_t HandleShaIoctlv(int32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
        return ISFS_EINVAL;
    }
    if (numIn != 1 || numOut != 2) {
        LogNandWarning("IOS_Ioctlv", "/dev/sha unsupported vector shape cmd=%u numIn=%u numOut=%u",
                cmd, numIn, numOut);
        return ISFS_EINVAL;
    }

    ShaHandle* handle = GetShaHandle(fd);
    if (!handle) {
        return ISFS_EINVAL;
    }

    const IosVector input = ReadIosVector(vectorPtr, 0);
    const IosVector context = ReadIosVector(vectorPtr, 1);
    const IosVector hash = ReadIosVector(vectorPtr, 2);
    if (!IsValidGuestRange(input.address, input.size) ||
        context.size < SHA_CONTEXT_SIZE || hash.size < SHA_DIGEST_SIZE ||
        !IsValidGuestRange(context.address, SHA_CONTEXT_SIZE) ||
        !IsValidGuestRange(hash.address, SHA_DIGEST_SIZE)) {
        LogNandWarning("IOS_Ioctlv",
                "/dev/sha invalid buffers cmd=%u in=0x%08X/%u ctx=0x%08X/%u hash=0x%08X/%u",
                cmd, input.address, input.size, context.address, context.size, hash.address, hash.size);
        return ISFS_EINVAL;
    }

    if (cmd == SHA_IOCTL_INIT) {
        handle->Restart();
    } else if (cmd != SHA_IOCTL_UPDATE && cmd != SHA_IOCTL_FINAL) {
        LogNandWarning("IOS_Ioctlv", "/dev/sha unsupported cmd=%u", cmd);
        return ISFS_EINVAL;
    }

    if (input.size != 0) {
        const uint8_t* bytes = Memory::GetPointer(input.address, input.size);
        handle->hash.Update(bytes, input.size);
        handle->byteCount += input.size;
    }

    if (!WriteShaOutputs(*handle, context, hash)) {
        return ISFS_EINVAL;
    }

    if (cmd == SHA_IOCTL_FINAL) {
        handle->Restart();
    }
    return ISFS_OK;
}

extern "C" int32_t NAND_IOS_Open_HLE(uint32_t pathPtr, uint32_t mode) {
    const std::string pathStorage = ReadGuestCString(pathPtr);
    const char* path = pathPtr == 0 ? nullptr : pathStorage.c_str();
    
    if (!path) {
        LogNandError("IOS_Open", "null path");
        return ISFS_EINVAL;
    }
    
    // Handle special device paths
    if (std::strncmp(path, "/dev/", 5) == 0) {
        if (std::strcmp(path, "/dev/fs") == 0) {
            return ISFS_DEV_FD;
        }
        if (std::strcmp(path, "/dev/es") == 0) {
            return ES_DEV_FD;
        }
        if (std::strcmp(path, "/dev/sha") == 0) {
            const int32_t fd = AllocateShaFd();
            return fd;
        }
        if (const int32_t netFd = Network_HLE_OpenDevice(path, mode)) {
            return netFd;
        }
        if (std::strcmp(path, "/dev/dolphin") == 0) {
            return DOLPHIN_DEV_FD;
        }
        LogNandWarning("IOS_Open", "unknown device '%s' mode=%u", path, mode);
        return ISFS_ENOENT;
    }
    
    // It's a NAND file path
    const std::filesystem::path hostPath = TranslateNandPath(path);
    
    // Seed FaceLib resources before the existence check so every open mode can
    // still find them on a fresh managed NAND.
    if (!PathExists(hostPath) && IsFaceLibResourcePath(path)) {
        SeedFaceLibResource(hostPath);
    }

    // Determine file mode. IOS never creates files on open - creation happens
    // exclusively through ISFS CreateFile (which we implement). The previous
    // create-on-open fallback ("w+b") silently materialized 0-byte files (for
    // example /shared2/sys/net/02/config.dat) that later reads treated as
    // valid, poisoning persistent state across sessions.
    const char* fopenMode = "rb";
    if (mode == 2 || mode == 3) {
        if (!PathExists(hostPath)) {
            LogNandWarning("IOS_Open", "'%s' does not exist; open mode %u never creates it",
                    HostPathText(hostPath).c_str(), mode);
            return ISFS_ENOENT;
        }
        fopenMode = "r+b";      // Write-only opens still need read for seeks
    }

    FILE* file = NandFopen(hostPath, fopenMode);

    if (!file) {
        LogNandError("IOS_Open", "FAILED to open '%s'", HostPathText(hostPath).c_str());
        return ISFS_ENOENT;
    }
    
    int32_t fd = AllocateFd(hostPath, file, mode);
    return fd;
}
PPC_NATIVE_OVERRIDE(801938F8, NAND_IOS_Open_HLE, int32_t, (uint32_t pathPtr, uint32_t mode), (pathPtr, mode));

extern "C" void NAND_IOS_OpenBody_HLE_801938FC(CpuContext* ctx) {
    const int32_t result = NAND_IOS_Open_HLE(ctx->gpr[3], ctx->gpr[4]);
    ctx->gpr[3] = static_cast<uint32_t>(result);
    ctx->gpr[1] = ctx->gpr[1] + 32u;
}
REGISTER_NATIVE_FUNCTION_AS(0x801938FC, NAND_IOS_OpenBody_HLE_801938FC, "NAND_IOS_OpenBody_HLE_801938FC");

extern "C" int32_t NAND_IOS_Close_HLE(uint32_t fd) {
    if (fd == ISFS_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == ES_DEV_FD) {
        return ISFS_OK;
    }
    if (fd == DOLPHIN_DEV_FD) {
        return ISFS_OK;
    }
    if (GetShaHandle(static_cast<int32_t>(fd))) {
        CloseShaFd(static_cast<int32_t>(fd));
        return ISFS_OK;
    }
    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Close(fd);
    }
    
    auto* handle = GetHandle(fd);
    if (!handle) {
        LogNandError("IOS_Close", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    CloseFd(fd);
    return ISFS_OK;
}
PPC_NATIVE_OVERRIDE(80193AD8, NAND_IOS_Close_HLE, int32_t, (uint32_t fd), (fd));

extern "C" int32_t NAND_IOS_Read_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Read", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    uint8_t* buffer = (uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        LogNandError("IOS_Read", "invalid buffer ptr 0x%08X", bufferPtr);
        return ISFS_EINVAL;
    }
    
    size_t bytesRead = std::fread(buffer, 1, length, handle->file);
    handle->position += static_cast<uint32_t>(bytesRead);
    
    return static_cast<int32_t>(bytesRead);
}
PPC_NATIVE_OVERRIDE(80193C80, NAND_IOS_Read_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Write_HLE(uint32_t fd, uint32_t bufferPtr, uint32_t length) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Write", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (!bufferPtr || length == 0) {
        return 0;
    }
    
    const uint8_t* buffer = (const uint8_t*)Memory::GetPointer(bufferPtr);
    if (!buffer) {
        LogNandError("IOS_Write", "invalid buffer ptr 0x%08X", bufferPtr);
        return ISFS_EINVAL;
    }
    
    size_t bytesWritten = std::fwrite(buffer, 1, length, handle->file);
    std::fflush(handle->file);
    handle->position += static_cast<uint32_t>(bytesWritten);
    
    return static_cast<int32_t>(bytesWritten);
}
PPC_NATIVE_OVERRIDE(80193E88, NAND_IOS_Write_HLE, int32_t, (uint32_t fd, uint32_t bufferPtr, uint32_t length), (fd, bufferPtr, length));

extern "C" int32_t NAND_IOS_Seek_HLE(uint32_t fd, int32_t offset, int32_t whence) {
    auto* handle = GetHandle(fd);
    if (!handle || !handle->file) {
        LogNandError("IOS_Seek", "invalid fd=%d", fd);
        return ISFS_EINVAL;
    }
    
    if (std::fseek(handle->file, offset, NandSeekOrigin(whence)) != 0) {
        LogNandError("IOS_Seek", "fd=%d offset=%d whence=%d FAILED", fd, offset, whence);
        return ISFS_EIO;
    }
    
    handle->position = static_cast<uint32_t>(std::ftell(handle->file));
    return static_cast<int32_t>(handle->position);
}
PPC_NATIVE_OVERRIDE(80194070, NAND_IOS_Seek_HLE, int32_t, (uint32_t fd, int32_t offset, int32_t whence), (fd, offset, whence));

// ============================================================================
// IOS_Ioctl HLE - Handles filesystem commands
// ============================================================================

// ISFS Ioctl commands
enum ISFSCommand {
    ISFS_IOCTL_FORMAT = 1,
    ISFS_IOCTL_GETSTATS = 2,
    ISFS_IOCTL_CREATEDIR = 3,
    ISFS_IOCTL_READDIR = 4,
    ISFS_IOCTL_SETATTR = 5,
    ISFS_IOCTL_GETATTR = 6,
    ISFS_IOCTL_DELETE = 7,
    ISFS_IOCTL_RENAME = 8,
    ISFS_IOCTL_CREATEFILE = 9,
    ISFS_IOCTL_SETFILEVERCTRL = 10,
    ISFS_IOCTL_GETFILESTATS = 11,
    ISFS_IOCTL_GETUSAGE = 12,
    ISFS_IOCTL_SHUTDOWN = 13,
};

extern "C" int32_t NAND_IOS_Ioctl_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t inBufPtr, uint32_t inLen,
    uint32_t outBufPtr, uint32_t outLen)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctl(fd, cmd, inBufPtr, inLen, outBufPtr, outLen);
    }

    if (GetShaHandle(static_cast<int32_t>(fd))) {
        LogNandWarning("IOS_Ioctl", "/dev/sha does not support scalar ioctl cmd=%u", cmd);
        return ISFS_EINVAL;
    }
    if (fd == DOLPHIN_DEV_FD) {
        LogNandWarning("IOS_Ioctl", "/dev/dolphin does not support scalar ioctl cmd=%u", cmd);
        return ISFS_EINVAL;
    }
    
    // Handle /dev/fs ISFS commands
    if (fd == ISFS_DEV_FD) {
        switch (cmd) {
            case ISFS_IOCTL_CREATEDIR: {
                // Input buffer: path + attributes
                if (!inBufPtr || inLen < 0x4c) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                if (CreateDirectoryPath(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_DELETE: {
                if (!inBufPtr || inLen < 0x40) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                // fs::remove refuses a non-empty directory, matching rmdir.
                if (NandRemove(hostPath)) {
                    return ISFS_OK;
                }
                return ISFS_ENOENT;
            }
            
            case ISFS_IOCTL_GETATTR: {
                if (!inBufPtr || !outBufPtr) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                
                if (!PathExists(hostPath)) {
                    return ISFS_ENOENT;
                }
                
                // Return fake attributes (owner UID, group ID, permissions)
                // Format: u32 ownerID, u16 groupID, u8 ownerPerm, u8 groupPerm, u8 otherPerm, u8 attrs
                uint8_t* outBuf = (uint8_t*)Memory::GetPointer(outBufPtr);
                if (outBuf && outLen >= 0x4c) {
                    std::memset(outBuf, 0, outLen);
                    // Owner UID = 0
                    Memory::Write32(outBufPtr, 0);
                    // Group ID = 0
                    Memory::Write16(outBufPtr + 4, 0);
                    // Permissions: 3 = read/write for all
                    Memory::Write8(outBufPtr + 0x49, 3); // owner perm
                    Memory::Write8(outBufPtr + 0x46, 3); // group perm
                    Memory::Write8(outBufPtr + 0x47, 3); // other perm
                    Memory::Write8(outBufPtr + 0x48, IsDirectory(hostPath) ? 2 : 1); // attrs (2=dir, 1=file)
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_CREATEFILE: {
                if (!inBufPtr || inLen < 0x4c) {
                    return ISFS_EINVAL;
                }
                const char* path = (const char*)Memory::GetPointer(inBufPtr + 6);
                const std::filesystem::path hostPath = TranslateNandPath(path);
                CreateParentDirectories(hostPath);

                // Create empty file
                FILE* f = NandFopen(hostPath, "wb");
                if (f) {
                    std::fclose(f);
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETFILESTATS: {
                // GETFILESTATS is addressed to a file fd, never to /dev/fs.
                LogNandWarning("IOS_Ioctl", "GETFILESTATS on ISFS device - unexpected");
                return ISFS_EINVAL;
            }
            
            case ISFS_IOCTL_RENAME: {
                if (!inBufPtr || inLen < 0x80) {
                    return ISFS_EINVAL;
                }
                const char* srcPath = (const char*)Memory::GetPointer(inBufPtr);
                const char* dstPath = (const char*)Memory::GetPointer(inBufPtr + 0x40);
                const std::filesystem::path srcHost = TranslateNandPath(srcPath);
                const std::filesystem::path dstHost = TranslateNandPath(dstPath);
                
                if (NandRename(srcHost, dstHost)) {
                    return ISFS_OK;
                }
                return ISFS_EIO;
            }
            
            case ISFS_IOCTL_GETSTATS: {
                // Return filesystem stats (fake values)
                if (outBufPtr && outLen >= 0x1c) {
                    Memory::Write32(outBufPtr + 0x00, 0x200000);  // Total blocks
                    Memory::Write32(outBufPtr + 0x04, 0x100000);  // Free blocks
                    Memory::Write32(outBufPtr + 0x08, 0);         // Used blocks
                    Memory::Write32(outBufPtr + 0x0C, 0);         // Bad blocks
                    Memory::Write32(outBufPtr + 0x10, 0);         // Reserved blocks
                    Memory::Write32(outBufPtr + 0x14, 0x20);      // Block size
                    Memory::Write32(outBufPtr + 0x18, 0);         // Free inodes
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_SETATTR: {
                // Ignore attribute changes - we don't implement file permissions
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_GETUSAGE: {
                // Return usage info (fake values)
                if (outBufPtr && outLen >= 8) {
                    Memory::Write32(outBufPtr + 0, 100);   // Files
                    Memory::Write32(outBufPtr + 4, 10000); // Blocks used
                }
                return ISFS_OK;
            }
            
            case ISFS_IOCTL_READDIR: {
                // Read directory listing
                // This is complex - return empty for now
                if (outBufPtr && outLen >= 4) {
                    Memory::Write32(outBufPtr, 0); // 0 entries
                }
                return ISFS_OK;
            }
            
            default:
                LogNandWarning("IOS_Ioctl", "unknown ISFS cmd=%u", cmd);
                return ISFS_OK;
        }
    }
    
    // Handle file-specific commands
    auto* handle = GetHandle(fd);
    if (handle && handle->file) {
        if (cmd == ISFS_IOCTL_GETFILESTATS) {
            // Get file stats
            if (!outBufPtr || outLen < 8) {
                return ISFS_EINVAL;
            }
            
            const NandFileExtent extent = NandProbeFileExtent(handle->file);
            Memory::Write32(outBufPtr, static_cast<uint32_t>(extent.size));
            Memory::Write32(outBufPtr + 4, static_cast<uint32_t>(extent.position));
            
            return ISFS_OK;
        }
    }
    
    // Unknown command - return success to not block game
    return ISFS_OK;
}

// The stack frame a guest thread parks on while a deferred network ioctl runs.
// `newStack` is always oldStack - kFrameSize, even when the frame could not be
// built, because the sleep path installs it unconditionally.
struct IosWaitFrame {
    bool valid = false;
    uint32_t oldStack = 0;
    uint32_t newStack = 0;
    uint32_t waitQueue = 0;
};

static IosWaitFrame InitializeIosWaitQueueFrame(CpuContext* ctx) {
    constexpr uint32_t kFrameSize = 0x40u;
    constexpr uint32_t kWaitQueueOffset = 0x30u;

    IosWaitFrame frame;
    frame.oldStack = ctx->gpr[1];
    frame.newStack = frame.oldStack - kFrameSize;
    if (frame.oldStack < kFrameSize || !Memory::Contains(frame.newStack, kFrameSize)) {
        return frame;
    }
    // Preserve the PPC linkage area and the required r3-r10 outgoing-argument
    // save area. The queue lives in local storage beyond sp+0x28 so a guest
    // switch callback cannot legally spill over it while this thread sleeps.
    Memory::Write32(frame.newStack, frame.oldStack);
    Memory::Write32(frame.newStack + 4u, 0);
    Memory::Write32(frame.newStack + kWaitQueueOffset, 0);
    Memory::Write32(frame.newStack + kWaitQueueOffset + 4u, 0);
    frame.waitQueue = frame.newStack + kWaitQueueOffset;
    frame.valid = true;
    return frame;
}

static void FinishDeferredIosWait(CpuContext* ctx, uint32_t oldStack, uint64_t token) {
    int32_t result = -101;
    if (!Network_HLE_TakeSyncResult(token, &result)) {
        RT_LOGF(RT_TAG_NAND,
                     "deferred network waiter resumed without result token=%llu\n",
                     static_cast<unsigned long long>(token));
    }
    ctx->gpr[1] = oldStack;
    ctx->gpr[3] = static_cast<uint32_t>(result);
}

// IOS_Ioctl and IOS_Ioctlv park a network request the same way: build the wait
// frame, hand its queue to the network layer, and either sleep on it or take the
// immediate answer. True when the request was handled here.
template <typename StartSync>
static bool TryDeferredNetworkIosSync(CpuContext* ctx, StartSync&& startSync) {
    const IosWaitFrame frame = InitializeIosWaitQueueFrame(ctx);
    const auto deferred = startSync(frame.valid ? frame.waitQueue : 0u);
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::Started) {
        ctx->gpr[1] = frame.newStack;
        ctx->gpr[3] = frame.waitQueue;
        OSSleepThread_HLE_801aa9b8(ctx);
        FinishDeferredIosWait(ctx, frame.oldStack, deferred.token);
        return true;
    }
    if (deferred.disposition == NetworkDeferredContract::StartDisposition::ImmediateResult) {
        ctx->gpr[3] = static_cast<uint32_t>(deferred.result);
        return true;
    }
    return false;
}

extern "C" void NAND_IOS_Ioctl_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t inBufPtr = ctx->gpr[5];
    const uint32_t inLen = ctx->gpr[6];
    const uint32_t outBufPtr = ctx->gpr[7];
    const uint32_t outLen = ctx->gpr[8];

    if (Network_HLE_IsFd(fd)) {
        const bool handled = TryDeferredNetworkIosSync(ctx, [&](uint32_t waitQueue) {
            return Network_HLE_StartIoctlSync(fd, cmd, inBufPtr, inLen, outBufPtr, outLen, waitQueue);
        });
        if (handled) {
            return;
        }
    }

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctl_HLE(fd, cmd, inBufPtr, inLen, outBufPtr, outLen));
}
PPC_NATIVE_OVERRIDE_VOID(80194290, NAND_IOS_Ioctl_Entry_HLE, (CpuContext* ctx), (ctx));

// ============================================================================
// ISFS_OpenLib - Initialize ISFS
// ============================================================================

// Global state for ISFS initialization
static bool g_isfsInitialized = false;

// The ISFS/IPC globals ISFS_OpenLib touches, as negative r13 (SDA1) offsets.
// These are address-exact: they name the SDK's own variables, so the numbers are
// load-bearing and must not be "tidied". Names come from the RVL IPC/ISFS
// sources; only the naming changed here, never a value.
namespace {
constexpr uint32_t kIsfsFdSda1Offset = 29408u;              // __ISFS_fd
constexpr uint32_t kIsfsPathSda1Offset = 29400u;            // __ISFS_path ("/dev/fs")
constexpr uint32_t kIpcBufferLoSda1Offset = 25620u;         // IPC buffer window, low
constexpr uint32_t kIpcBufferHiSda1Offset = 25616u;         // IPC buffer window, high
constexpr uint32_t kIpcArenaLoSda1Offset = 25732u;          // __IPCArenaLo
constexpr uint32_t kIpcArenaHiSda1Offset = 25728u;          // __IPCArenaHi
constexpr uint32_t kIsfsHeapHandleSda1Offset = 25724u;      // ISFS heap handle
constexpr uint32_t kIsfsHeapBaseSda1Offset = 25740u;        // ISFS heap base address
constexpr uint32_t kIsfsHeapInitializedSda1Offset = 25744u; // ISFS heap created flag
} // namespace

static void WriteGuestString(uint32_t address, const char* value) {
    if (!value) {
        return;
    }
    const size_t length = std::strlen(value) + 1;
    if (!Memory::Contains(address, length)) {
        return;
    }
    for (size_t i = 0; i < length; ++i) {
        Memory::Write8(address + static_cast<uint32_t>(i), static_cast<uint8_t>(value[i]));
    }
}

int32_t ISFS_OpenLib_Initialize(CpuContext* ctx) {
    g_isfsInitialized = true;
    
    // Create the title data directory if it doesn't exist
    char titleId[32];
    std::snprintf(titleId, sizeof(titleId), "%08x", kNandTitleIdHi);
    char gameId[32];
    std::snprintf(gameId, sizeof(gameId), "%08x", CurrentMkwTitleIdLo());
    CreateDirectoryPath(GetNandBasePath() / "title" / titleId / gameId / "data");

    if (!ctx) {
        return ISFS_OK;
    }

    const uint32_t r13 = ctx->gpr[13];
    if (r13 == 0) {
        return ISFS_OK;
    }

    const uint32_t isfsFdGlobal = r13 - kIsfsFdSda1Offset;
    const uint32_t isfsPathGlobal = r13 - kIsfsPathSda1Offset;
    const uint32_t ipcBufferLoGlobal = r13 - kIpcBufferLoSda1Offset;
    const uint32_t ipcBufferHiGlobal = r13 - kIpcBufferHiSda1Offset;
    const uint32_t ipcArenaLoGlobal = r13 - kIpcArenaLoSda1Offset;
    const uint32_t ipcArenaHiGlobal = r13 - kIpcArenaHiSda1Offset;
    const uint32_t isfsHeapGlobal = r13 - kIsfsHeapHandleSda1Offset;
    const uint32_t isfsHeapBaseGlobal = r13 - kIsfsHeapBaseSda1Offset;
    const uint32_t isfsHeapInitializedGlobal = r13 - kIsfsHeapInitializedSda1Offset;

    WriteGuestString(isfsPathGlobal, "/dev/fs");

    if (Memory::Contains(isfsFdGlobal, 4)) {
        Memory::Write32(isfsFdGlobal, static_cast<uint32_t>(ISFS_DEV_FD));
    }

    // The heap bring-up below reads and writes all seven IPC globals, so it only
    // runs when every one of them is inside guest memory.
    for (const uint32_t global : {ipcBufferLoGlobal, ipcBufferHiGlobal, ipcArenaLoGlobal,
                                  ipcArenaHiGlobal, isfsHeapGlobal, isfsHeapBaseGlobal,
                                  isfsHeapInitializedGlobal}) {
        if (!Memory::Contains(global, 4)) {
            return ISFS_OK;
        }
    }

    uint32_t ipcLo = Memory::Read32(ipcBufferLoGlobal);
    uint32_t ipcHi = Memory::Read32(ipcBufferHiGlobal);
    if (ipcLo == 0 || ipcHi == 0 || ipcLo >= ipcHi) {
        return ISFS_OK;
    }

    if (Memory::Read32(isfsHeapInitializedGlobal) == 0) {
        Memory::Write32(ipcArenaLoGlobal, ipcLo);
        Memory::Write32(ipcArenaHiGlobal, ipcHi);

        const uint32_t heapBase = (ipcLo + 31u) & ~31u;
        const uint32_t heapSize = 5440u;
        if (heapBase + heapSize <= ipcHi) {
            Memory::Write32(isfsHeapBaseGlobal, heapBase);

            const uint32_t savedR3 = ctx->gpr[3];
            const uint32_t savedR4 = ctx->gpr[4];
            const uint32_t savedR5 = ctx->gpr[5];
            const uint32_t savedLr = ctx->lr;

            ctx->gpr[3] = heapBase;
            ctx->gpr[4] = heapSize;
            ctx->lr = 0x80169BCCu;
            InvokeDirectCpu<0x801949B8u>(ctx);
            const uint32_t heapHandle = ctx->gpr[3];

            ctx->gpr[3] = heapBase + heapSize;
            ctx->lr = 0x80169BCCu;
            InvokeDirectCpu<0x80193040u>(ctx);

            ctx->gpr[3] = savedR3;
            ctx->gpr[4] = savedR4;
            ctx->gpr[5] = savedR5;
            ctx->lr = savedLr;

            Memory::Write32(isfsHeapGlobal, heapHandle);
            Memory::Write32(isfsHeapInitializedGlobal, 1u);
        }
    }

    return ISFS_OK;
}

extern "C" void ISFS_OpenLib_HLE_80169BCC(CpuContext* ctx) {
    ctx->gpr[3] = static_cast<uint32_t>(ISFS_OpenLib_Initialize(ctx));
}

REGISTER_NATIVE_FUNCTION_AS(0x80169BCC, ISFS_OpenLib_HLE_80169BCC, "ISFS_OpenLib_HLE_80169BCC");

// ============================================================================
// IOS_Ioctlv HLE - Vector Ioctl for complex ISFS operations
// ============================================================================

static int32_t HandleIsfsReadDir(uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    const bool countOnly = (numIn == 1 && numOut == 1);
    if (!countOnly && !(numIn == 2 && numOut == 2)) {
        LogNandWarning("IOS_Ioctlv", "READDIR unsupported vector shape numIn=%u numOut=%u",
                numIn, numOut);
        return ISFS_EINVAL;
    }

    const IosVector pathVec = ReadIosVector(vectorPtr, 0);
    const std::string wiiPath = ReadGuestCString(pathVec.address, 64);
    if (wiiPath.empty()) {
        return ISFS_EINVAL;
    }
    const std::filesystem::path hostPath = TranslateNandPath(wiiPath.c_str());
    if (!IsDirectory(hostPath)) {
        return ISFS_ENOENT;
    }

    // NAND names are at most 12 characters; longer host names cannot exist on
    // a real NAND (this also hides *.nandsafe.tmp write shadows).
    constexpr size_t kMaxNandNameLength = 12;
    std::vector<std::string> names;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(hostPath, ec)) {
        std::string name = HostPathText(entry.path().filename());
        if (name.empty() || name.size() > kMaxNandNameLength) {
            continue;
        }
        names.push_back(std::move(name));
    }
    std::sort(names.begin(), names.end());

    if (countOnly) {
        const IosVector countOut = ReadIosVector(vectorPtr, 1);
        if (countOut.size < 4 || !Memory::Contains(countOut.address, 4)) {
            return ISFS_EINVAL;
        }
        Memory::Write32(countOut.address, static_cast<uint32_t>(names.size()));
        return ISFS_OK;
    }

    const IosVector maxVec = ReadIosVector(vectorPtr, 1);
    const IosVector namesOut = ReadIosVector(vectorPtr, 2);
    const IosVector countOut = ReadIosVector(vectorPtr, 3);
    if (maxVec.size < 4 || !Memory::Contains(maxVec.address, 4) ||
        countOut.size < 4 || !Memory::Contains(countOut.address, 4) ||
        !IsValidGuestRange(namesOut.address, namesOut.size)) {
        return ISFS_EINVAL;
    }
    const uint32_t maxCount = Memory::Read32(maxVec.address);

    constexpr uint32_t kEntryWindow = 13; // 12 chars + terminator
    uint32_t cursor = 0;
    uint32_t written = 0;
    for (const std::string& name : names) {
        if (written >= maxCount || cursor + kEntryWindow > namesOut.size) {
            break;
        }
        uint8_t* out = Memory::GetPointer(namesOut.address + cursor, kEntryWindow);
        std::memset(out, 0, kEntryWindow);
        std::memcpy(out, name.data(), name.size());
        cursor += static_cast<uint32_t>(name.size()) + 1;
        ++written;
    }
    Memory::Write32(countOut.address, written);
    return ISFS_OK;
}

extern "C" int32_t NAND_IOS_Ioctlv_HLE(
    uint32_t fd,
    uint32_t cmd,
    uint32_t numIn,
    uint32_t numOut,
    uint32_t vectorPtr)
{

    if (Network_HLE_IsFd(fd)) {
        return Network_HLE_Ioctlv(fd, cmd, numIn, numOut, vectorPtr);
    }

    if (GetShaHandle(static_cast<int32_t>(fd))) {
        return HandleShaIoctlv(static_cast<int32_t>(fd), cmd, numIn, numOut, vectorPtr);
    }

    if (fd == DOLPHIN_DEV_FD) {
        return HandleDolphinIoctlv(cmd, numIn, numOut, vectorPtr);
    }

    if (fd == ISFS_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }
        if (cmd == ISFS_IOCTL_READDIR) {
            return HandleIsfsReadDir(numIn, numOut, vectorPtr);
        }
        return ISFS_OK;
    }

    if (fd == ES_DEV_FD) {
        if (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(numIn + numOut) * 8u)) {
            return ISFS_EINVAL;
        }

        switch (cmd) {
            case ES_IOCTL_GETDEVICEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 4 || out.address == 0 || !Memory::Contains(out.address, 4)) {
                    return ISFS_EINVAL;
                }
                const WiiEsCrypto::Identity& identity = WiiEsCrypto::CurrentIdentity();
                Memory::Write32(out.address, identity.deviceId);
                return ISFS_OK;
            }

            case ES_IOCTL_GETDEVICECERT: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                const auto cert = WiiEsCrypto::GetDeviceCertificate();
                if (!WriteGuestBytes(out.address, out.size, cert.data(), cert.size())) {
                    return ISFS_EINVAL;
                }
                return ISFS_OK;
            }

            case ES_IOCTL_GETTITLEID: {
                if (numIn != 0 || numOut != 1) {
                    return ISFS_EINVAL;
                }
                const IosVector out = ReadIosVector(vectorPtr, 0);
                if (out.size < 8 || out.address == 0 || !Memory::Contains(out.address, 8)) {
                    return ISFS_EINVAL;
                }
                const uint64_t titleId = CurrentMkwTitleId();
                Memory::Write32(out.address, static_cast<uint32_t>(titleId >> 32));
                Memory::Write32(out.address + 4u, static_cast<uint32_t>(titleId));
                return ISFS_OK;
            }

            case ES_IOCTL_SIGN: {
                if (numIn != 1 || numOut != 2) {
                    return ISFS_EINVAL;
                }
                const IosVector in = ReadIosVector(vectorPtr, 0);
                const IosVector sigOut = ReadIosVector(vectorPtr, 1);
                const IosVector certOut = ReadIosVector(vectorPtr, 2);
                if (in.address == 0 || !Memory::Contains(in.address, in.size)) {
                    return ISFS_EINVAL;
                }
                const uint8_t* input = Memory::GetPointer(in.address, in.size);
                WiiEsCrypto::EcSignature signature{};
                WiiEsCrypto::EccCert cert{};
                WiiEsCrypto::Sign(CurrentMkwTitleId(), input, in.size, signature, cert);
                if (!WriteGuestBytes(sigOut.address, sigOut.size, signature.data(), signature.size()) ||
                    !WriteGuestBytes(certOut.address, certOut.size, cert.data(), cert.size())) {
                    return ISFS_EINVAL;
                }
                return ISFS_OK;
            }

            default:
                LogNandWarning("IOS_Ioctlv", "unsupported /dev/es cmd=%u", cmd);
                return ISFS_EINVAL;
        }
    }
    
    // Non-device ioctlv has no ISFS command we need to service.
    return ISFS_OK;
}

extern "C" void NAND_IOS_Ioctlv_Entry_HLE(CpuContext* ctx) {
    const uint32_t fd = ctx->gpr[3];
    const uint32_t cmd = ctx->gpr[4];
    const uint32_t numIn = ctx->gpr[5];
    const uint32_t numOut = ctx->gpr[6];
    const uint32_t vectorPtr = ctx->gpr[7];

    if (Network_HLE_IsFd(fd)) {
        const bool handled = TryDeferredNetworkIosSync(ctx, [&](uint32_t waitQueue) {
            return Network_HLE_StartIoctlvSync(fd, cmd, numIn, numOut, vectorPtr, waitQueue);
        });
        if (handled) {
            return;
        }
    }

    ctx->gpr[3] = static_cast<uint32_t>(
        NAND_IOS_Ioctlv_HLE(fd, cmd, numIn, numOut, vectorPtr));
}
PPC_NATIVE_OVERRIDE_VOID(801945E0, NAND_IOS_Ioctlv_Entry_HLE, (CpuContext* ctx), (ctx));
