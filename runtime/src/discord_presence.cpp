#include "discord_presence.h"


#ifdef MKW_PLATFORM_IOS

// iOS has no Discord client for the IPC socket to reach, so the whole thing is
// stubbed out rather than retrying a connection that cannot succeed.
namespace DiscordPresence {
void Initialize(const std::string&, const std::string&) {}
void SetClient(const std::string&) {}
void SetActivity(Activity) {}
void Reset() {}
void Shutdown() {}
} // namespace DiscordPresence

#else

#include "runtime_log.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace DiscordPresence {
namespace {

constexpr uint32_t kHandshakeOpcode = 0;
constexpr uint32_t kFrameOpcode = 1;
constexpr size_t kMaxClientIdLength = 32;
constexpr auto kConnectionRetryCooldown = std::chrono::seconds(1);

bool IsClientId(std::string_view value) {
    return !value.empty() && value.size() <= kMaxClientIdLength &&
           std::all_of(value.begin(), value.end(), [](unsigned char ch) { return std::isdigit(ch) != 0; });
}

std::string EscapeJson(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const unsigned char ch : value) {
        switch (ch) {
        case '\\': escaped += "\\\\"; break;
        case '\"': escaped += "\\\""; break;
        case '\b': escaped += "\\b"; break;
        case '\f': escaped += "\\f"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default:
            if (ch < 0x20) {
                static constexpr char kHex[] = "0123456789abcdef";
                escaped += "\\u00";
                escaped += kHex[ch >> 4];
                escaped += kHex[ch & 0x0f];
            } else {
                escaped += static_cast<char>(ch);
            }
        }
    }
    return escaped;
}

void AppendJsonString(std::ostringstream& output, bool& hasValue, std::string_view key, std::string_view value) {
    if (value.empty()) {
        return;
    }
    if (hasValue) {
        output << ',';
    }
    output << '\"' << key << "\":\"" << EscapeJson(value) << '\"';
    hasValue = true;
}

std::string BuildActivityPayload(const Activity& activity) {
    std::ostringstream json;
    json << "{\"cmd\":\"SET_ACTIVITY\",\"nonce\":\"wiicompiled\",\"args\":{\"pid\":";
#if defined(_WIN32)
    json << static_cast<unsigned long>(::GetCurrentProcessId());
#else
    json << static_cast<long>(::getpid());
#endif
    json << ",\"activity\":{";
    bool hasActivityField = false;
    AppendJsonString(json, hasActivityField, "details", activity.details);
    AppendJsonString(json, hasActivityField, "state", activity.state);
    const auto appendSection = [&](std::string_view name, const auto& append) {
        if (hasActivityField) {
            json << ',';
        }
        json << '\"' << name << "\":{";
        append();
        json << '}';
        hasActivityField = true;
    };
    const bool hasAssets = !activity.largeImageKey.empty() || !activity.largeImageText.empty() ||
                           !activity.smallImageKey.empty() || !activity.smallImageText.empty();
    if (hasAssets) {
        appendSection("assets", [&] {
            bool hasAsset = false;
            AppendJsonString(json, hasAsset, "large_image", activity.largeImageKey);
            AppendJsonString(json, hasAsset, "large_text", activity.largeImageText);
            AppendJsonString(json, hasAsset, "small_image", activity.smallImageKey);
            AppendJsonString(json, hasAsset, "small_text", activity.smallImageText);
        });
    }
    if (activity.startTimestamp > 0 || activity.endTimestamp > 0) {
        appendSection("timestamps", [&] {
            if (activity.startTimestamp > 0) {
                json << "\"start\":" << activity.startTimestamp;
            }
            if (activity.endTimestamp > 0) {
                if (activity.startTimestamp > 0) {
                    json << ',';
                }
                json << "\"end\":" << activity.endTimestamp;
            }
        });
    }
    if (activity.partySize > 0 && activity.partyMax > 0) {
        appendSection("party", [&] {
            json << "\"size\":[" << activity.partySize << ',' << activity.partyMax << ']';
        });
    }
    if (hasActivityField) {
        json << ',';
    }
    json << "\"instance\":false}}}";
    return json.str();
}

class Client {
public:
    void Initialize(const std::string& clientId, const std::string& title) {
        std::lock_guard<std::mutex> lock(mutex_);
        basicClientId_ = IsClientId(clientId) ? clientId : std::string{};
        basicActivity_ = {};
        basicActivity_.details = title;
        basicActivity_.startTimestamp = UnixSeconds();
        customClient_ = false;
        activity_ = basicActivity_;
        ReconnectAndSendLocked();
    }

    void SetClient(const std::string& clientId) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!IsClientId(clientId)) {
            RT_LOG(RT_TAG_RUNTIME) << "Ignoring invalid Discord client ID from /dev/dolphin" << std::endl;
            return;
        }
        if (clientId_ == clientId && connected_) {
            return;
        }
        customClient_ = true;
        clientId_ = clientId;
        CloseLocked();
        ReconnectAndSendLocked();
    }

    void SetActivity(Activity activity) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!customClient_) {
            return;
        }
        activity_ = std::move(activity);
        SendActivityLocked();
    }

    void Reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        customClient_ = false;
        clientId_.clear();
        activity_ = basicActivity_;
        CloseLocked();
        ReconnectAndSendLocked();
    }

    void Shutdown() {
        std::lock_guard<std::mutex> lock(mutex_);
        CloseLocked();
    }

private:
    static int64_t UnixSeconds() {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    }

    std::string ActiveClientIdLocked() const {
        return customClient_ ? clientId_ : basicClientId_;
    }

    bool WriteFrameLocked(uint32_t opcode, std::string_view payload) {
        std::array<uint8_t, 8> header{};
        const uint32_t size = static_cast<uint32_t>(payload.size());
        for (size_t i = 0; i < 4; ++i) {
            header[i] = static_cast<uint8_t>(opcode >> (i * 8));
            header[4 + i] = static_cast<uint8_t>(size >> (i * 8));
        }
        return WriteAllLocked(header.data(), header.size()) &&
               WriteAllLocked(reinterpret_cast<const uint8_t*>(payload.data()), payload.size());
    }

    void ReconnectAndSendLocked() {
        const std::string clientId = ActiveClientIdLocked();
        if (clientId.empty() || !ConnectLocked()) {
            return;
        }
        const std::string handshake = "{\"v\":1,\"client_id\":\"" + clientId + "\"}";
        if (!WriteFrameLocked(kHandshakeOpcode, handshake)) {
            RecordFailedConnectionLocked();
            CloseLocked();
            return;
        }
        if (!ReadReadyLocked()) {
            RecordFailedConnectionLocked();
            CloseLocked();
            return;
        }
        SendActivityLocked();
    }

    void SendActivityLocked() {
        if (!connected_) {
            ReconnectAndSendLocked();
            return;
        }
        if (ActiveClientIdLocked().empty()) {
            return;
        }
        if (!WriteFrameLocked(kFrameOpcode, BuildActivityPayload(activity_))) {
            RecordFailedConnectionLocked();
            CloseLocked();
        }
    }

    bool ConnectionRetryAllowedLocked() const {
        return !lastFailedConnectionAttempt_ ||
               std::chrono::steady_clock::now() - *lastFailedConnectionAttempt_ >= kConnectionRetryCooldown;
    }

    void RecordFailedConnectionLocked() {
        lastFailedConnectionAttempt_ = std::chrono::steady_clock::now();
    }

#if defined(_WIN32)
    bool ConnectLocked() {
        if (connected_) {
            return true;
        }
        if (!ConnectionRetryAllowedLocked()) {
            return false;
        }
        for (unsigned int index = 0; index < 10; ++index) {
            const std::string name = "\\\\.\\pipe\\discord-ipc-" + std::to_string(index);
            handle_ = ::CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (handle_ != INVALID_HANDLE_VALUE) {
                DWORD mode = PIPE_READMODE_BYTE;
                ::SetNamedPipeHandleState(handle_, &mode, nullptr, nullptr);
                connected_ = true;
                lastFailedConnectionAttempt_.reset();
                return true;
            }
        }
        RecordFailedConnectionLocked();
        return false;
    }

    bool WriteAllLocked(const uint8_t* data, size_t size) {
        while (size != 0) {
            DWORD written = 0;
            if (!::WriteFile(handle_, data, static_cast<DWORD>(size), &written, nullptr) || written == 0) {
                return false;
            }
            data += written;
            size -= written;
        }
        return true;
    }

    bool ReadAllLocked(uint8_t* data, size_t size) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (size != 0) {
            if (std::chrono::steady_clock::now() >= deadline) {
                return false;
            }
            DWORD available = 0;
            if (!::PeekNamedPipe(handle_, nullptr, 0, nullptr, &available, nullptr)) {
                return false;
            }
            if (available == 0) {
                if (std::chrono::steady_clock::now() >= deadline) {
                    return false;
                }
                ::Sleep(10);
                continue;
            }
            DWORD read = 0;
            const DWORD requested = static_cast<DWORD>(std::min<size_t>(size, available));
            if (!::ReadFile(handle_, data, requested, &read, nullptr) || read == 0) {
                return false;
            }
            data += read;
            size -= read;
        }
        return true;
    }

    void CloseLocked() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            ::CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
        connected_ = false;
    }

    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    bool ConnectLocked() {
        if (connected_) {
            return true;
        }
        if (!ConnectionRetryAllowedLocked()) {
            return false;
        }
        std::array<std::string, 5> roots{};
        size_t rootCount = 0;
        if (const char* runtimeDir = std::getenv("XDG_RUNTIME_DIR"); runtimeDir && *runtimeDir) {
            roots[rootCount++] = runtimeDir;
        }
        if (const char* tempDir = std::getenv("TMPDIR"); tempDir && *tempDir) {
            roots[rootCount++] = tempDir;
        }
        if (const char* tempDir = std::getenv("TMP"); tempDir && *tempDir && rootCount < roots.size()) {
            roots[rootCount++] = tempDir;
        }
        if (const char* tempDir = std::getenv("TEMP"); tempDir && *tempDir && rootCount < roots.size()) {
            roots[rootCount++] = tempDir;
        }
        roots[rootCount++] = "/tmp";
        for (size_t rootIndex = 0; rootIndex < rootCount; ++rootIndex) {
            for (unsigned int index = 0; index < 10; ++index) {
                const std::string path = roots[rootIndex] + "/discord-ipc-" + std::to_string(index);
                if (path.size() >= sizeof(sockaddr_un::sun_path)) {
                    continue;
                }
                const int socketFd = ::socket(AF_UNIX, SOCK_STREAM, 0);
                if (socketFd < 0) {
                    continue;
                }
                sockaddr_un address{};
                address.sun_family = AF_UNIX;
                std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
                if (::connect(socketFd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
                    timeval timeout{};
                    timeout.tv_sec = 1;
                    ::setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
                    fd_ = socketFd;
                    connected_ = true;
                    lastFailedConnectionAttempt_.reset();
                    return true;
                }
                ::close(socketFd);
            }
        }
        RecordFailedConnectionLocked();
        return false;
    }

    bool WriteAllLocked(const uint8_t* data, size_t size) {
        while (size != 0) {
            const ssize_t written = ::send(fd_, data, size, MSG_NOSIGNAL);
            if (written <= 0) {
                return false;
            }
            data += written;
            size -= static_cast<size_t>(written);
        }
        return true;
    }

    bool ReadAllLocked(uint8_t* data, size_t size) {
        while (size != 0) {
            const ssize_t read = ::recv(fd_, data, size, 0);
            if (read <= 0) {
                return false;
            }
            data += read;
            size -= static_cast<size_t>(read);
        }
        return true;
    }

    void CloseLocked() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        connected_ = false;
    }

    int fd_ = -1;
#endif

    bool ReadReadyLocked() {
        std::array<uint8_t, 8> header{};
        if (!ReadAllLocked(header.data(), header.size())) {
            return false;
        }
        uint32_t opcode = 0;
        uint32_t size = 0;
        for (size_t i = 0; i < 4; ++i) {
            opcode |= static_cast<uint32_t>(header[i]) << (i * 8);
            size |= static_cast<uint32_t>(header[4 + i]) << (i * 8);
        }
        if (opcode != kFrameOpcode || size > 1024 * 1024) {
            return false;
        }
        std::vector<uint8_t> payload(size);
        if (!ReadAllLocked(payload.data(), payload.size())) {
            return false;
        }
        const std::string_view json(reinterpret_cast<const char*>(payload.data()), payload.size());
        return json.find("\"evt\":\"READY\"") != std::string_view::npos;
    }

    std::mutex mutex_;
    bool connected_ = false;
    bool customClient_ = false;
    std::optional<std::chrono::steady_clock::time_point> lastFailedConnectionAttempt_;
    std::string basicClientId_;
    std::string clientId_;
    Activity basicActivity_;
    Activity activity_;
};

Client g_client;

} // namespace

void Initialize(const std::string& basicClientId, const std::string& basicTitle) {
    g_client.Initialize(basicClientId, basicTitle);
}

void SetClient(const std::string& clientId) {
    g_client.SetClient(clientId);
}

void SetActivity(Activity activity) {
    g_client.SetActivity(std::move(activity));
}

void Reset() {
    g_client.Reset();
}

void Shutdown() {
    g_client.Shutdown();
}

} // namespace DiscordPresence

#endif
