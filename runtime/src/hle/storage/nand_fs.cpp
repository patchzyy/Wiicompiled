// NAND/ISFS HLE: redirects Wii NAND paths (e.g. /title/00010004/524d4350/data/rksys.dat) to
// <nand_root>\title\00010004\524d4350\data\rksys.dat on the host.

#include "nand_internal.h"

#include "isa/big_endian.h"
#include "hle/storage/riivolution.h"
#include "runtime_log.h"

// ============================================================================
// Configuration
// ============================================================================

// Base path for the host Wii NAND directory (resolved at runtime).
static std::string g_dolphinWiiBase;
static std::once_flag g_dolphinWiiBaseOnce;

// ============================================================================
// Logging
// ============================================================================

static void EmitNandLog(const char* func, const char* fmt, va_list args) {
    char buf[512];
    vsnprintf(buf, sizeof(buf), fmt, args);
    RT_LOGF(RT_TAG_NAND, "%s: %s\n", func, buf);
}

void LogNandError(const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EmitNandLog(func, fmt, args);
    va_end(args);
}

void LogNandWarning(const char* func, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    EmitNandLog(func, fmt, args);
    va_end(args);
}

// ============================================================================
// File Descriptor Management
// ============================================================================

std::map<int32_t, FileHandle> g_fileHandles;
static int32_t g_nextFd = 100; // Start at 100 to avoid confusion with stdio fds
std::mutex g_fdMutex;

int32_t AllocateFd(const std::string& path, FILE* file, int32_t mode) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    int32_t fd = g_nextFd++;
    g_fileHandles[fd] = {file, path, mode, 0};
    return fd;
}

FileHandle* GetHandle(int32_t fd) {
    auto it = g_fileHandles.find(fd);
    if (it == g_fileHandles.end()) {
        return nullptr;
    }
    return &it->second;
}

void CloseFd(int32_t fd) {
    std::lock_guard<std::mutex> lock(g_fdMutex);
    auto it = g_fileHandles.find(fd);
    if (it != g_fileHandles.end()) {
        if (it->second.file) {
            std::fclose(it->second.file);
        }
        g_fileHandles.erase(it);
    }
}

// ============================================================================
// Path Translation
// ============================================================================

uint32_t CurrentMkwTitleIdLo() {
    return RuntimeHle::CurrentGameCode(kNandTitleIdLo);
}

std::string CurrentNandDataDir() {
    char path[64];
    std::snprintf(path, sizeof(path), "/title/%08x/%08x/data",
                  kNandTitleIdHi, CurrentMkwTitleIdLo());
    return path;
}

const std::string& GetNandBasePath() {
    std::call_once(g_dolphinWiiBaseOnce, []() {
        g_dolphinWiiBase = RuntimeNandPath::DiscoverNandRootString();
    });

    return g_dolphinWiiBase;
}

static std::string BuildHostNandPath(std::string wiiPathStr) {
    std::string hostPath = GetNandBasePath();
    for (char& c : wiiPathStr) {
        if (c == '/') {
#ifdef _WIN32
            c = '\\';
#endif
        }
    }

    if (!wiiPathStr.empty() && (wiiPathStr[0] == '\\' || wiiPathStr[0] == '/')) {
        hostPath += wiiPathStr;
    } else {
        hostPath += "\\";
        hostPath += wiiPathStr;
    }
    return hostPath;
}

// Collapses "." and ".." components against the NAND root. Without this, guest paths like
// "/title/../../../../Users/..." escape the NAND root into the real filesystem under the
// current user's permissions. ".." at the root clamps to the root, matching IOS.
static std::string LexicallyResolveWiiPath(const std::string& path) {
    std::vector<std::string> components;
    std::string component;
    // path is always absolute here, so a leading empty component is implied.
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i < path.size() && path[i] != '/') {
            component.push_back(path[i]);
            continue;
        }
        if (component == "..") {
            if (!components.empty()) {
                components.pop_back();
            }
        } else if (!component.empty() && component != ".") {
            components.push_back(component);
        }
        component.clear();
    }

    std::string resolved;
    for (const std::string& part : components) {
        resolved.push_back('/');
        resolved += part;
    }
    return resolved.empty() ? "/" : resolved;
}

static std::string NormalizeAbsoluteWiiPath(const char* wiiPath) {
    if (!wiiPath || wiiPath[0] == '\0') {
        return {};
    }

    std::string path = wiiPath;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path[0] != '/') {
        std::string cwd = CurrentNandDataDir();
        if (!cwd.empty() && cwd.back() != '/') {
            cwd.push_back('/');
        }
        path = cwd + path;
    }
    path = LexicallyResolveWiiPath(path);
    while (path.size() > 1 && path.back() == '/') {
        path.pop_back();
    }
    return path;
}

struct RiivolutionSaveRedirect {
    bool enabled = false;
    bool clone = false;
    std::string hostDirectory;
};

static std::once_flag g_riivolutionSaveRedirectOnce;
static RiivolutionSaveRedirect g_riivolutionSaveRedirect;

static const RiivolutionSaveRedirect& GetRiivolutionSaveRedirect() {
    // The active pack's <savegame> patch, resolved against the virtual SD root
    // exactly like Dolphin resolves it for Riivolution launches. This keeps
    // the recomp's save in the same folder a Dolphin/WheelWizard launch of the
    // pack uses (e.g. .../Riivolution/WheelWizard/riivolution/save/RetroWFC/RMCP).
    std::call_once(g_riivolutionSaveRedirectOnce, []() {
        const auto& redirect = RuntimeRiivolution::GetSaveRedirect();
        if (!redirect) {
            return;
        }
        g_riivolutionSaveRedirect.enabled = true;
        g_riivolutionSaveRedirect.clone = redirect->clone;
        g_riivolutionSaveRedirect.hostDirectory = redirect->hostDirectory.string();
        // Riivolution creates the redirect folder if it does not exist.
        CreateDirectoryPath(g_riivolutionSaveRedirect.hostDirectory);
    });

    return g_riivolutionSaveRedirect;
}

static void CloneRiivolutionSaveIfNeeded(const std::string& sourceHostPath,
                                         const std::string& redirectedHostPath,
                                         const RiivolutionSaveRedirect& redirect) {
    if (!redirect.clone || PathExists(redirectedHostPath) || !PathExists(sourceHostPath)) {
        return;
    }

    CreateParentDirectories(redirectedHostPath);

    std::error_code ec;
    std::filesystem::copy_file(sourceHostPath, redirectedHostPath,
                               std::filesystem::copy_options::skip_existing, ec);
    if (ec) {
        LogNandWarning("RiivolutionSave", "WARNING: failed to clone '%s' -> '%s': %s",
                       sourceHostPath.c_str(), redirectedHostPath.c_str(), ec.message().c_str());
    }
}

static bool ResolveRiivolutionSaveHostPath(const std::string& absoluteWiiPath, std::string& outHostPath) {
    const RiivolutionSaveRedirect& redirect = GetRiivolutionSaveRedirect();
    if (!redirect.enabled) {
        return false;
    }

    const std::string dataDir = CurrentNandDataDir();
    if (absoluteWiiPath != dataDir &&
        (absoluteWiiPath.size() <= dataDir.size() ||
         absoluteWiiPath.compare(0, dataDir.size(), dataDir) != 0 ||
         absoluteWiiPath[dataDir.size()] != '/')) {
        return false;
    }

    std::string relative;
    if (absoluteWiiPath.size() > dataDir.size()) {
        relative = absoluteWiiPath.substr(dataDir.size() + 1);
    }

    std::filesystem::path redirected(redirect.hostDirectory);
    if (!relative.empty()) {
        redirected /= std::filesystem::path(relative);
    }

    outHostPath = redirected.string();
    CloneRiivolutionSaveIfNeeded(BuildHostNandPath(absoluteWiiPath), outHostPath, redirect);
    return true;
}

std::string TranslateNandPath(const char* wiiPath) {
    std::string wiiPathStr = NormalizeAbsoluteWiiPath(wiiPath);
    if (wiiPathStr.empty()) {
        return "";
    }

    std::string redirectedHostPath;
    if (ResolveRiivolutionSaveHostPath(wiiPathStr, redirectedHostPath)) {
        return redirectedHostPath;
    }

    return BuildHostNandPath(wiiPathStr);
}

// ============================================================================
// U8 archive helper (used to seed FaceLib resources from the ISO)
// ============================================================================

struct U8Node {
    uint32_t typeAndNameOffset;
    uint32_t dataOffset;
    uint32_t size;
};

static bool ExtractFromU8(const std::string& archivePath, const char* targetName, std::vector<uint8_t>& outData) {
    std::ifstream file(archivePath, std::ios::binary);
    if (!file) {
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamsize size = file.tellg();
    if (size < 0x20) {
        return false;
    }
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(static_cast<size_t>(size));
    if (!file.read(reinterpret_cast<char*>(data.data()), size)) {
        return false;
    }

    const uint32_t magic = BigEndian::Read32(data.data());
    if (magic != 0x55AA382Du) { // "U\xAA8-"
        return false;
    }

    const uint32_t rootOffset = BigEndian::Read32(data.data() + 0x04);
    if (rootOffset + 0x0C > data.size()) {
        return false;
    }

    const U8Node* root = reinterpret_cast<const U8Node*>(data.data() + rootOffset);
    const uint32_t nodeCount = BigEndian::Read32(reinterpret_cast<const uint8_t*>(&root->size));
    const uint32_t nodeTableSize = nodeCount * sizeof(U8Node);
    const uint32_t stringTableOffset = rootOffset + nodeTableSize;
    if (stringTableOffset >= data.size()) {
        return false;
    }

    struct DirFrame {
        std::string path;
        uint32_t endIndex;
    };
    std::vector<DirFrame> stack;
    stack.push_back({"", nodeCount});

    for (uint32_t index = 1; index < nodeCount; ++index) {
        while (!stack.empty() && index >= stack.back().endIndex) {
            stack.pop_back();
        }
        if (stack.empty()) {
            break;
        }

        const U8Node* node = reinterpret_cast<const U8Node*>(data.data() + rootOffset + index * sizeof(U8Node));
        const uint32_t typeAndName = BigEndian::Read32(reinterpret_cast<const uint8_t*>(&node->typeAndNameOffset));
        const uint32_t nameOffset = typeAndName & 0x00FFFFFFu;
        const bool isDir = (typeAndName >> 24) != 0;
        if (stringTableOffset + nameOffset >= data.size()) {
            return false;
        }

        const char* name = reinterpret_cast<const char*>(data.data() + stringTableOffset + nameOffset);
        const std::string fullPath = stack.back().path + name;

        if (isDir) {
            const uint32_t endIndex = BigEndian::Read32(reinterpret_cast<const uint8_t*>(&node->size));
            stack.push_back({fullPath + "/", endIndex});
            continue;
        }

        if (fullPath == targetName || std::strcmp(name, targetName) == 0) {
            const uint32_t fileOffset = BigEndian::Read32(reinterpret_cast<const uint8_t*>(&node->dataOffset));
            const uint32_t fileSize = BigEndian::Read32(reinterpret_cast<const uint8_t*>(&node->size));
            // 64-bit sum: fileOffset + fileSize as uint32_t can wrap and bypass this check on a
            // corrupt/crafted archive, letting the iterator arithmetic below run out of range.
            if (static_cast<uint64_t>(fileOffset) + fileSize > data.size()) {
                return false;
            }
            outData.assign(data.begin() + fileOffset, data.begin() + fileOffset + fileSize);
            return true;
        }
    }

    return false;
}

// The only FaceLib resource the game ever seeds; every call site used to pass it
// as an argument and the path predicate below hard-codes the same name.
static constexpr char kFaceLibResourceName[] = "RFL_Res.dat";

bool SeedFaceLibResource(const std::string& hostPath) {
    std::vector<uint8_t> payload;
    if (const auto dvdRoot = RuntimeConfigFile::ResolvedDvdRoot(); !dvdRoot.empty()) {
        const auto arcPath = dvdRoot / "files" / "contents" / "RFLRes01.arc";
        std::error_code ec;
        if (std::filesystem::exists(arcPath, ec)) {
            ExtractFromU8(arcPath.string(), kFaceLibResourceName, payload);
        }
    }

    if (payload.empty()) {
        LogNandError("FaceLibSeed", "Failed to locate %s in RFLRes01.arc", kFaceLibResourceName);
        return false;
    }

    CreateParentDirectories(hostPath);

    std::ofstream out(hostPath, std::ios::binary);
    if (!out) {
        LogNandError("FaceLibSeed", "Failed to create %s", hostPath.c_str());
        return false;
    }
    out.write(reinterpret_cast<const char*>(payload.data()), static_cast<std::streamsize>(payload.size()));
    if (!out) {
        LogNandError("FaceLibSeed", "Failed to write %s", hostPath.c_str());
        return false;
    }

    return true;
}

bool IsFaceLibResourcePath(const char* path) {
    return std::strcmp(path, "/shared2/menu/FaceLib/RFL_Res.dat") == 0;
}

// Create directories recursively
bool CreateDirectoryPath(const std::string& path) {
    if (path.empty()) {
        return true;
    }

    std::error_code ec;
    std::filesystem::create_directories(path, ec);
    return !ec || std::filesystem::is_directory(path);
}

// Check if a path exists
bool PathExists(const std::string& path) {
#ifdef _WIN32
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
#else
    return access(path.c_str(), F_OK) == 0;
#endif
}

// Check if path is a directory
bool IsDirectory(const std::string& path) {
#ifdef _WIN32
    DWORD attrs = GetFileAttributesA(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

bool CreateParentDirectories(const std::string& path) {
    size_t lastSlash = path.rfind('\\');
    if (lastSlash == std::string::npos) {
        lastSlash = path.rfind('/');
    }
    if (lastSlash == std::string::npos) {
        return false;
    }
    CreateDirectoryPath(path.substr(0, lastSlash));
    return true;
}

// ============================================================================
// stdio helpers shared by the NAND* library and the IOS_* device layer
// ============================================================================

int NandSeekOrigin(int32_t whence) {
    if (whence == 1) return SEEK_CUR;
    if (whence == 2) return SEEK_END;
    return SEEK_SET;
}

NandFileExtent NandProbeFileExtent(FILE* file) {
    NandFileExtent extent;
    extent.position = std::ftell(file);
    std::fseek(file, 0, SEEK_END);
    extent.size = std::ftell(file);
    std::fseek(file, extent.position, SEEK_SET);
    return extent;
}
