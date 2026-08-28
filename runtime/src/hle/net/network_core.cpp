#include "network_internal.h"

#include "console_identity.h"
#include "hle/net/network.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "runtime_product.h"

namespace NetworkHle {

bool RetroRewindProfileActive();

static std::mutex g_mutex;
static std::map<int32_t, DeviceKind> g_devices;
static int32_t g_nextDeviceFd = 2000;

const std::array<uint8_t, 6>& RuntimeMacAddress() {
    return RuntimeConsoleIdentity::Current().mac;
}

uint64_t RuntimeGeneratedUserId() {
    return 0x000000014D4B5752ull;
}

void ZeroMemoryRange(uint32_t addr, uint32_t size) {
    if (!addr || !size) {
        return;
    }
    for (uint32_t i = 0; i < size; ++i) {
        Memory::Write8(addr + i, 0);
    }
}

void CopyToGuest(uint32_t addr, const void* data, uint32_t size) {
    if (!addr || !data || !size) {
        return;
    }
    uint8_t* out = Memory::GetPointer(addr, size);
    if (out) {
        std::memcpy(out, data, size);
    }
}

std::string ReadGuestString(uint32_t addr, uint32_t maxLen) {
    std::string result;
    if (!addr) {
        return result;
    }
    for (uint32_t i = 0; i < maxLen; ++i) {
        const char ch = static_cast<char>(Memory::Read8(addr + i));
        if (ch == '\0') {
            break;
        }
        result.push_back(ch);
    }
    return result;
}

std::string Lower(std::string_view text) {
    return RuntimeHle::Lower(text);
}

bool RetroRewindProfileActive() {
    return RuntimeProduct::IsRetroRewind();
}

bool StartsWith(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

// No host rewriting here: the Retro-WFC payload already redirects Nintendo
// hostnames at its own resolver hooks before the name reaches this HLE.

std::vector<IoVector> ReadVectors(uint32_t vectorPtr, uint32_t count) {
    std::vector<IoVector> vectors;
    vectors.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        vectors.push_back(RuntimeHle::ReadIoVector(vectorPtr, i));
    }
    return vectors;
}

bool WriteReturn(uint32_t outBuf, uint32_t outLen, int32_t value) {
    if (!outBuf || outLen < 4) {
        return false;
    }
    Memory::Write32(outBuf, static_cast<uint32_t>(value));
    return true;
}

bool WriteVectorReturn(const std::vector<IoVector>& out, size_t index, int32_t value) {
    if (index >= out.size() || out[index].size < 4 || !out[index].address) {
        return false;
    }
    Memory::Write32(out[index].address, static_cast<uint32_t>(value));
    return true;
}

static int32_t AllocateDevice(DeviceKind kind) {
    std::lock_guard<std::mutex> lock(g_mutex);
    const int32_t fd = g_nextDeviceFd++;
    g_devices.emplace(fd, kind);
    return fd;
}

std::optional<DeviceKind> GetDeviceKind(uint32_t fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(static_cast<int32_t>(fd));
    if (it == g_devices.end()) {
        return std::nullopt;
    }
    return it->second;
}

static bool RemoveDevice(uint32_t fd) {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_devices.erase(static_cast<int32_t>(fd)) != 0;
}

void NetFail(const char* format, ...) {
    char line[512];
    std::va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(line, sizeof(line), format, args);
    va_end(args);
    if (written < 0) {
        return;
    }
    RT_LOGF(RT_TAG_NET, "%s\n", line);
    std::fflush(stderr);
}

std::array<WiiSocket, kWiiSocketMax> g_sockets;
int32_t g_lastSocketError = 0;
static uint64_t g_nextSocketGeneration = 1;

static uint64_t NextSocketGeneration() {
    const uint64_t generation = g_nextSocketGeneration++;
    if (g_nextSocketGeneration == 0) {
        g_nextSocketGeneration = 1;
    }
    return generation;
}

bool EnsureSocketRuntime() {
#ifdef _WIN32
    static bool initialized = [] {
        WSADATA data{};
        const int result = WSAStartup(MAKEWORD(2, 2), &data);
        if (result != 0) {
            RT_LOGF(RT_TAG_NET, "WSAStartup failed: %d\n", result);
            return false;
        }
        return true;
    }();
    return initialized;
#else
    return true;
#endif
}

int NativeLastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// Dolphin: WiiSockMan::TranslateErrorCode (IOS/Network/Socket.cpp:42-86).
// isReadWrite mirrors Dolphin's is_rw: a nonblocking connect/bind must still
// report SO_EINPROGRESS on would-block while send/recv/accept report SO_EAGAIN.
// Unmapped codes collapse to -1 rather than -SO_EINVAL, which would make
// unrelated failures look like malformed requests to the SDK.
int32_t TranslateSocketError(int err, bool isReadWrite) {
#ifdef _WIN32
    switch (err) {
    case WSAEMSGSIZE:
        return -1;
    case WSAENOTSOCK:
        return -SO_EBADF;
    case WSAEADDRINUSE:
        return -SO_EADDRINUSE;
    case WSAECONNRESET:
        return -SO_ECONNRESET;
    case WSAEISCONN:
        return -SO_EISCONN;
    case WSAENOTCONN:
        return -SO_ENOTCONN;
    case WSAEINPROGRESS:
        return -SO_EINPROGRESS;
    case WSAEALREADY:
        return -SO_EALREADY;
    case WSAEACCES:
        return -SO_EACCES;
    case WSAECONNREFUSED:
        return -SO_ECONNREFUSED;
    case WSAENETUNREACH:
        return -SO_ENETUNREACH;
    case WSAEHOSTUNREACH:
        return -SO_EHOSTUNREACH;
    case WSAENOBUFS:
        return -SO_ENOMEM;
    case WSAENETRESET:
        return -SO_ENETRESET;
    case WSAEWOULDBLOCK:
        return isReadWrite ? -SO_EAGAIN : -SO_EINPROGRESS;
    default:
        return -1;
    }
#else
    switch (err) {
    case EMSGSIZE:
        return -1;
    case EBADF:
        return -SO_EBADF;
    case EADDRINUSE:
        return -SO_EADDRINUSE;
    case ECONNRESET:
        return -SO_ECONNRESET;
    case EISCONN:
        return -SO_EISCONN;
    case ENOTCONN:
        return -SO_ENOTCONN;
    case EINPROGRESS:
        return -SO_EINPROGRESS;
    case EALREADY:
        return -SO_EALREADY;
    case EACCES:
        return -SO_EACCES;
    case ECONNREFUSED:
        return -SO_ECONNREFUSED;
    case ENETUNREACH:
        return -SO_ENETUNREACH;
    case EHOSTUNREACH:
        return -SO_EHOSTUNREACH;
    case ENOMEM:
    case ENOBUFS:
        return -SO_ENOMEM;
    case ENETRESET:
        return -SO_ENETRESET;
#if defined(EWOULDBLOCK) && EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
    case EAGAIN:
        return isReadWrite ? -SO_EAGAIN : -SO_EINPROGRESS;
    default:
        return -1;
    }
#endif
}

#ifdef _WIN32
// Dolphin: WiiSockMan::GetNetErrorCode (IOS/Network/Socket.cpp:100-109). Winsock
// reports WSAEINVAL rather than WSAEALREADY when a connect is already pending on
// the socket (a Winsock 1.1 compatibility behaviour, also seen when a layered
// provider hijacks ws2_32). Only the connect paths may apply this.
int NormalizeConnectError(int err) {
    return err == WSAEINVAL ? WSAEALREADY : err;
}
#else
int NormalizeConnectError(int err) {
    return err;
}
#endif

int32_t SocketResult(int ret, bool isReadWrite) {
    if (ret >= 0) {
        g_lastSocketError = 0;
        return ret;
    }
    g_lastSocketError = TranslateSocketError(NativeLastError(), isReadWrite);
    return g_lastSocketError;
}

int32_t SocketErrorResult(int nativeErr, bool isReadWrite) {
    g_lastSocketError = TranslateSocketError(nativeErr, isReadWrite);
    return g_lastSocketError;
}

bool IsWouldBlockError(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS || err == WSAEALREADY;
#else
    return err == EWOULDBLOCK || err == EAGAIN || err == EINPROGRESS || err == EALREADY;
#endif
}

// Mirrors Dolphin's WiiSocket::GetConnectingState (IOS/Network/Socket.cpp:751-826):
// select() on write+except, never WSAPoll, since WSAPoll does not report a failed
// connect and a refused/unreachable peer would stay "pending" forever. Returns >0
// once settled, 0 while in flight, <0 if the probe itself failed.
int ProbeConnectSettled(NativeSocket socket, int timeoutMs) {
#ifdef _WIN32
    fd_set writeFds;
    fd_set exceptFds;
    FD_ZERO(&writeFds);
    FD_ZERO(&exceptFds);
    FD_SET(socket, &writeFds);
    FD_SET(socket, &exceptFds);
    timeval limit{};
    limit.tv_sec = timeoutMs / 1000;
    limit.tv_usec = (timeoutMs % 1000) * 1000;
    const int ret = select(0, nullptr, &writeFds, &exceptFds, timeoutMs < 0 ? nullptr : &limit);
    if (ret <= 0) {
        return ret;
    }
    return (FD_ISSET(socket, &writeFds) || FD_ISSET(socket, &exceptFds)) ? 1 : 0;
#else
    pollfd descriptor{};
    descriptor.fd = socket;
    descriptor.events = POLLOUT;
    const int ret = poll(&descriptor, 1, timeoutMs);
    if (ret <= 0) {
        return ret;
    }
    return (descriptor.revents & (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) ? 1 : 0;
#endif
}

// Dolphin confirms a settled connect with getsockopt(SO_ERROR) followed by
// getpeername(); the second call is what rejects a socket that reported
// writability without ever reaching ESTABLISHED.
int32_t ClassifySettledConnect(NativeSocket socket) {
    int socketError = 0;
    socklen_t socketErrorLen = sizeof(socketError);
    if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socketError),
                   &socketErrorLen) != 0) {
        return TranslateSocketError(NativeLastError(), false);
    }
    if (socketError != 0) {
        return TranslateSocketError(socketError, false);
    }

    sockaddr_in peer{};
    socklen_t peerLen = sizeof(peer);
    if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peerLen) != 0) {
        return TranslateSocketError(NativeLastError(), false);
    }
    return 0;
}

static int32_t FinishNonBlockingConnect(NativeSocket socket, int timeoutMs) {
    const int pollRet = ProbeConnectSettled(socket, timeoutMs);
    if (pollRet <= 0) {
        // Dolphin reports a connect that never settled as -SO_ENETUNREACH
        // (IOS/Network/Socket.cpp:352-356), not -SO_ETIMEDOUT: ETIMEDOUT is not
        // even reachable through its error table, so the SDK never handles it.
        const int hostError = pollRet < 0 ? NativeLastError() : 0;
        return pollRet == 0 ? -SO_ENETUNREACH : TranslateSocketError(hostError, false);
    }
    return ClassifySettledConnect(socket);
}

bool WaitForReadable(NativeSocket socket, int timeoutMs) {
    if (timeoutMs <= 0) {
        return false;
    }

    pollfd pfd{};
    pfd.fd = socket;
    pfd.events = POLLRDNORM;
#ifdef _WIN32
    const int ret = WSAPoll(&pfd, 1, timeoutMs);
#else
    const int ret = poll(&pfd, 1, timeoutMs);
#endif
    return ret > 0 && (pfd.revents & (POLLRDNORM | POLLIN | POLLERR | POLLHUP)) != 0;
}

void CloseNativeSocket(NativeSocket socket) {
    if (socket == kInvalidSocket) {
        return;
    }
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}

void SetNonBlocking(NativeSocket socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    ioctlsocket(socket, FIONBIO, &mode);
#else
    int flags = fcntl(socket, F_GETFL, 0);
    if (flags >= 0) {
        if (enabled) {
            flags |= O_NONBLOCK;
        } else {
            flags &= ~O_NONBLOCK;
        }
        fcntl(socket, F_SETFL, flags);
    }
#endif
}



static void ConfigureUdpSocket(NativeSocket socket, int type) {
#ifdef _WIN32
    if (type != SOCK_DGRAM) {
        return;
    }

    BOOL disabled = FALSE;
    DWORD bytesReturned = 0;
    WSAIoctl(socket, SIO_UDP_CONNRESET, &disabled, sizeof(disabled),
             nullptr, 0, &bytesReturned, nullptr, nullptr);
#else
    (void)socket;
    (void)type;
#endif
}

int32_t ReconnectWiiSocket(WiiSocket& socket, uint16_t port) {
    if (!socket.hasPeerAddr || socket.type != SOCK_STREAM) {
        return -SO_EINVAL;
    }

    NativeSocket native = ::socket(socket.af, socket.type, socket.protocol);
    if (native == kInvalidSocket) {
        return SocketResult(-1, false);
    }
    ConfigureUdpSocket(native, socket.type);

    SetNonBlocking(native, true);
    sockaddr_in addr = socket.peerAddr;
    addr.sin_port = htons(port);
    const int ret = connect(native, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
    const int nativeErr = ret < 0 ? NormalizeConnectError(NativeLastError()) : 0;
    int32_t result = 0;
    if (ret < 0 && IsWouldBlockError(nativeErr)) {
        result = FinishNonBlockingConnect(native, 10000);
    } else if (ret < 0) {
        result = SocketErrorResult(nativeErr, false);
    } else {
        result = SocketResult(ret, false);
    }
    g_lastSocketError = result;
    if (result != 0) {
        CloseNativeSocket(native);
        return result;
    }

    CloseNativeSocket(socket.native);
    socket.native = native;
    socket.generation = NextSocketGeneration();
    socket.peerAddr = addr;
    socket.peerPort = port;
    socket.nonblocking = false;
    SetNonBlocking(socket.native, false);
    return 0;
}

int MapWiiAf(uint32_t af) {
    if (af == kWiiAfInet) {
        return AF_INET;
    }
#ifdef AF_INET6
    if (af == kWiiAfInet6) {
        return AF_INET6;
    }
#endif
    return -1;
}

int MapNativeAfToWii(int af) {
    if (af == AF_INET) {
        return kWiiAfInet;
    }
#ifdef AF_INET6
    if (af == AF_INET6) {
        return kWiiAfInet6;
    }
#endif
    return -1;
}

int MapWiiSocketType(uint32_t type) {
    if (type == 0) {
        return 0;
    }
    if (type == kWiiSockStream) {
        return SOCK_STREAM;
    }
    if (type == kWiiSockDgram) {
        return SOCK_DGRAM;
    }
#ifdef SOCK_RAW
    if (type == kWiiSockRaw) {
        return SOCK_RAW;
    }
#endif
#ifdef SOCK_RDM
    if (type == kWiiSockRdm) {
        return SOCK_RDM;
    }
#endif
#ifdef SOCK_SEQPACKET
    if (type == kWiiSockSeqPacket) {
        return SOCK_SEQPACKET;
    }
#endif
    return -1;
}

int MapWiiAddrInfoFamily(uint32_t family) {
    const int mapped = MapWiiAf(family);
    // Dolphin passes unfamiliar addrinfo hint values through to the host
    // resolver, which then returns the platform's EAI_FAMILY equivalent. Do
    // not turn an invalid family into AF_INET and accidentally resolve it.
    return mapped >= 0 ? mapped : static_cast<int>(family);
}

int MapWiiAddrInfoSocketType(uint32_t type) {
    const int mapped = MapWiiSocketType(type);
    // Wii and host socket type values match for the common types, but retain
    // Dolphin's pass-through behavior for an unfamiliar hint so getaddrinfo
    // can reject it instead of silently broadening the query.
    return mapped >= 0 ? mapped : static_cast<int>(type);
}

uint32_t MapNativeSocketTypeToWii(int type) {
    if (type == 0) {
        return 0;
    }
    if (type == SOCK_STREAM) {
        return kWiiSockStream;
    }
    if (type == SOCK_DGRAM) {
        return kWiiSockDgram;
    }
#ifdef SOCK_RAW
    if (type == SOCK_RAW) {
        return kWiiSockRaw;
    }
#endif
#ifdef SOCK_RDM
    if (type == SOCK_RDM) {
        return kWiiSockRdm;
    }
#endif
#ifdef SOCK_SEQPACKET
    if (type == SOCK_SEQPACKET) {
        return kWiiSockSeqPacket;
    }
#endif
    // Dolphin writes ai_socktype without coercion. Preserve an unfamiliar host
    // value rather than silently changing it to SOCK_STREAM.
    return static_cast<uint32_t>(type);
}

int32_t AddWiiSocket(NativeSocket native, int af, int type, int protocol) {
    if (native == kInvalidSocket) {
        return SocketResult(-1, false);
    }
    for (int i = 0; i < kWiiSocketMax; ++i) {
        if (g_sockets[i].native == kInvalidSocket) {
            ConfigureUdpSocket(native, type);
            if (type == SOCK_DGRAM) {
                // Dolphin: WiiSockMan::AddSocket - "Wii UDP sockets can use
                // broadcast address by default" (IOS/Network/Socket.cpp:905-916).
                // Without this a sendto() to a broadcast address fails with
                // EACCES/WSAEACCES on the host but succeeds on hardware.
                const int enableBroadcast = 1;
                setsockopt(native, SOL_SOCKET, SO_BROADCAST,
                           reinterpret_cast<const char*>(&enableBroadcast),
                           sizeof(enableBroadcast));
            }
            SetNonBlocking(native, true);
            // IOS keeps the host descriptor nonblocking while the guest flag starts cleared.
            g_sockets[i] = {native, af, type, protocol, false};
            g_sockets[i].generation = NextSocketGeneration();
            return i;
        }
    }
    CloseNativeSocket(native);
    return -SO_EMFILE;
}

WiiSocket* GetWiiSocket(uint32_t fd) {
    if (fd >= kWiiSocketMax || g_sockets[fd].native == kInvalidSocket) {
        g_lastSocketError = -SO_EBADF;
        return nullptr;
    }
    return &g_sockets[fd];
}

bool SocketIdentityIsCurrent(uint32_t wiiFd, NativeSocket nativeFd, uint64_t generation) {
    if (wiiFd >= g_sockets.size()) {
        return false;
    }
    const WiiSocket& socket = g_sockets[wiiFd];
    return socket.native != kInvalidSocket && socket.native == nativeFd &&
           socket.generation == generation;
}

bool CopiedPollSocketIsStillValid(const NetworkPollContract::CopiedDescriptor& descriptor) {
    return SocketIdentityIsCurrent(descriptor.wiiFd, descriptor.nativeFd,
                                   descriptor.socketGeneration);
}

bool ValidatePollRequest(uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen,
                         uint32_t& descriptorCount) {
    descriptorCount = outLen / 12u;
    return inBuf && outBuf && inLen >= 8u && descriptorCount != 0 &&
           descriptorCount <= NetworkPollContract::kMaxDescriptors &&
           Memory::Contains(inBuf, 8u) &&
           Memory::Contains(outBuf, static_cast<size_t>(descriptorCount) * 12u);
}

std::vector<NetworkPollContract::CopiedDescriptor> CopyPollDescriptors(uint32_t outBuf,
                                                                      uint32_t descriptorCount) {
    std::vector<NetworkPollContract::CopiedDescriptor> descriptors;
    descriptors.reserve(descriptorCount);
    for (uint32_t i = 0; i < descriptorCount; ++i) {
        const uint32_t entry = outBuf + i * 12u;
        NetworkPollContract::CopiedDescriptor descriptor{};
        descriptor.wiiFd = Memory::Read32(entry);
        descriptor.events = NetworkPollContract::WiiEventsToNative(Memory::Read32(entry + 4u));
        if (descriptor.wiiFd < g_sockets.size()) {
            const WiiSocket& socket = g_sockets[descriptor.wiiFd];
            if (socket.native != kInvalidSocket) {
                descriptor.nativeFd = socket.native;
                descriptor.socketGeneration = socket.generation;
            }
        }
        descriptors.push_back(descriptor);
    }
    return descriptors;
}

void WritePollResults(uint32_t outAddress,
                      const std::vector<NetworkPollContract::CopiedDescriptor>& descriptors) {
    for (size_t i = 0; i < descriptors.size(); ++i) {
        Memory::Write32(outAddress + static_cast<uint32_t>(i * 12u + 8u),
                        NetworkPollContract::NativeEventsToWii(descriptors[i].revents));
    }
}

}  // namespace NetworkHle

using namespace NetworkHle;

extern "C" int32_t Network_HLE_OpenDevice(const char* path, uint32_t mode) {
    if (!path) {
        return -101;
    }
    if (!RuntimeConfigFile::NetworkEnabled(true)) {
        // The guest opens several /dev/net nodes at boot and retries; report the
        // reason online will not work exactly once.
        static bool reported = false;
        if (!reported) {
            reported = true;
            NetFail("networking is disabled in Config.toml; online play is unavailable");
        }
        return 0;
    }
    DeviceKind kind;
    if (std::strcmp(path, "/dev/net/kd/request") == 0) {
        kind = DeviceKind::KdRequest;
    } else if (std::strcmp(path, "/dev/net/kd/time") == 0) {
        kind = DeviceKind::KdTime;
    } else if (std::strcmp(path, "/dev/net/ncd/manage") == 0) {
        kind = DeviceKind::NcdManage;
    } else if (std::strcmp(path, "/dev/net/ip/top") == 0) {
        kind = DeviceKind::IpTop;
        EnsureSocketRuntime();
    } else if (std::strcmp(path, "/dev/net/ssl") == 0) {
        kind = DeviceKind::Ssl;
        EnsureSocketRuntime();
    } else {
        return 0;
    }

    return AllocateDevice(kind);
}

extern "C" bool Network_HLE_IsFd(uint32_t fd) {
    return GetDeviceKind(fd).has_value();
}

extern "C" int32_t Network_HLE_Close(uint32_t fd) {
    if (!GetDeviceKind(fd)) {
        return -101;
    }
    RemoveDevice(fd);
    return 0;
}

extern "C" int32_t Network_HLE_Ioctl(uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
                                      uint32_t outBuf, uint32_t outLen) {
    const auto device = GetDeviceKind(fd);
    if (!device) {
        return -101;
    }
    // The handlers below read/write inBuf/outBuf via Memory::Read*/Write*/GetPointer with no
    // Memory::Contains pre-check; an out-of-range guest address throws Memory::AccessViolation,
    // which would otherwise unwind uncaught through translated PPC call frames and take down the
    // whole runtime (same bug class already found and fixed in NAND_IOS_Ioctl_HLE).
    try {
        switch (*device) {
        case DeviceKind::KdRequest:
            return HandleKdIoctl(cmd, inBuf, inLen, outBuf, outLen);
        case DeviceKind::KdTime:
            return HandleKdTimeIoctl(cmd, inBuf, inLen, outBuf, outLen);
        case DeviceKind::IpTop:
            return HandleIpTopIoctl(cmd, inBuf, inLen, outBuf, outLen);
        case DeviceKind::NcdManage:
            return 0;
        case DeviceKind::Ssl:
            return 0;
        }
    } catch (const Memory::AccessViolation&) {
        return -SO_EINVAL;
    }
    return -101;
}

extern "C" int32_t Network_HLE_Ioctlv(uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
                                       uint32_t vectorPtr) {
    const auto device = GetDeviceKind(fd);
    if (!device) {
        return -101;
    }
    // numIn/numOut are guest-controlled; sum in 64-bit so a wrap can't make
    // `vectors` shorter than numIn and run the iterator arithmetic below off
    // the end. Matches PrepareDeferredGetAddrInfo in network_deferred.cpp.
    const uint64_t vectorCount = static_cast<uint64_t>(numIn) + numOut;
    if (vectorCount > kMaxIoVectors || (vectorCount != 0 &&
        (!vectorPtr || !Memory::Contains(vectorPtr, static_cast<size_t>(vectorCount) * 8u)))) {
        return -SO_EINVAL;
    }

    // Same unguarded-guest-pointer bug class as Network_HLE_Ioctl above: the vector addresses
    // themselves are range-checked, but the handlers below dereference guest memory at offsets
    // derived from vector contents (and ReadVectors itself reads guest memory) with no
    // Memory::Contains pre-check, so an out-of-range address throws Memory::AccessViolation.
    try {
        const std::vector<IoVector> vectors = ReadVectors(vectorPtr, static_cast<uint32_t>(vectorCount));
        const std::vector<IoVector> in(vectors.begin(), vectors.begin() + numIn);
        const std::vector<IoVector> out(vectors.begin() + numIn, vectors.end());

        switch (*device) {
        case DeviceKind::NcdManage:
            return HandleNcdIoctlv(cmd, in, out);
        case DeviceKind::IpTop:
            return HandleIpTopIoctlv(cmd, in, out);
        case DeviceKind::KdRequest:
            return HandleKdIoctlv(cmd, in, out);
        case DeviceKind::KdTime: {
            const uint32_t inBuf = in.empty() ? 0 : in[0].address;
            const uint32_t inLen = in.empty() ? 0 : in[0].size;
            const uint32_t outBuf = out.empty() ? 0 : out[0].address;
            const uint32_t outLen = out.empty() ? 0 : out[0].size;
            return HandleKdTimeIoctl(cmd, inBuf, inLen, outBuf, outLen);
        }
        case DeviceKind::Ssl:
            return HandleSslIoctlv(cmd, in, out);
        }
    } catch (const Memory::AccessViolation&) {
        return -SO_EINVAL;
    }
    return -101;
}
