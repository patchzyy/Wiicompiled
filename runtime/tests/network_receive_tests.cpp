#include "../src/hle/net/network_internal.h"
#include "fiber_manager.h"
#include "gx_guest_write.h"
#include "memory_slow_test_support.h"

#include <iostream>
#include <stdexcept>
#include <tuple>

namespace {
constexpr uint32_t kBase = 0x80000000u;
constexpr uint32_t kControl = kBase + 0x1000u;
constexpr uint32_t kVectors = kBase + 0x1100u;
constexpr uint32_t kThread = kBase + 0x2000u;
constexpr uint32_t kQueue = kBase + 0x3000u;
constexpr uint32_t kData = kBase + 0x4000u;
std::vector<uint8_t> guest(4u * 1024u * 1024u);
bool invalidOutput = false;
bool terminated = false;
uint64_t threadGeneration = 1;
bool deviceOpen = true;
int wakeups = 0;
int injectedReadError = 0;
std::vector<std::tuple<uint32_t, int32_t, uint32_t>> callbacks;

void Require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

int TestRecvFrom(NetworkHle::NativeSocket fd, char* data, int length, int flags,
                  sockaddr* from, socklen_t* fromLength) {
    if (injectedReadError) {
#ifdef _WIN32
        WSASetLastError(std::exchange(injectedReadError, 0));
#else
        errno = std::exchange(injectedReadError, 0);
#endif
        return -1;
    }
    return static_cast<int>(recvfrom(fd, data, length, flags, from, fromLength));
}
}

// Exercise the production scheduler and receive code with only the surrounding
// runtime services replaced. Native socket I/O still goes over loopback.
#define recvfrom TestRecvFrom
#include "../src/hle/net/network_socket.cpp"
#undef recvfrom
#include "../src/hle/net/network_deferred.cpp"

extern "C" const uint8_t kMkwGuestRegion = 0;
Memory::AccessViolation::AccessViolation(uint32_t address, size_t length, std::string_view reason)
    : std::runtime_error(std::string(reason)), address_(address), length_(length), reason_(reason) {}
bool Memory::Contains(uint32_t address, size_t length) {
    return !(invalidOutput && address == kData) && address >= kBase &&
        static_cast<uint64_t>(address - kBase) + length <= guest.size();
}
uint8_t* Memory::GetPointer(uint32_t address, size_t length) {
    if (!Contains(address, length)) throw AccessViolation(address, length, "test mapping");
    return guest.data() + address - kBase;
}
uint8_t MemoryInline::Read8Slow(uint32_t address) { return *Memory::GetPointer(address, 1); }
uint16_t MemoryInline::Read16Slow(uint32_t address) {
    return static_cast<uint16_t>((Memory::Read8(address) << 8) | Memory::Read8(address + 1));
}
uint32_t MemoryInline::Read32Slow(uint32_t address) {
    return (static_cast<uint32_t>(Memory::Read16(address)) << 16) | Memory::Read16(address + 2);
}
uint64_t MemoryInline::Read64Slow(uint32_t address) {
    return (static_cast<uint64_t>(Memory::Read32(address)) << 32) | Memory::Read32(address + 4);
}
void MemoryInline::Write8Slow(uint32_t address, uint8_t value) { *Memory::GetPointer(address, 1) = value; }
void MemoryInline::Write16Slow(uint32_t address, uint16_t value) {
    Memory::Write8(address, static_cast<uint8_t>(value >> 8));
    Memory::Write8(address + 1, static_cast<uint8_t>(value));
}
void MemoryInline::Write32Slow(uint32_t address, uint32_t value) {
    Memory::Write16(address, static_cast<uint16_t>(value >> 16));
    Memory::Write16(address + 2, static_cast<uint16_t>(value));
}
void MemoryInline::Write64Slow(uint32_t address, uint64_t value) {
    Memory::Write32(address, static_cast<uint32_t>(value >> 32));
    Memory::Write32(address + 4, static_cast<uint32_t>(value));
}
bool Fiber::GuestFiberManager::IsTerminated(uint32_t) { return terminated; }
uint64_t Fiber::GuestFiberManager::GetThreadGeneration(uint32_t) { return threadGeneration; }
void OS_HLE_WakeupThreadNoReschedule(CpuContext*, uint32_t queue) {
    Require(queue == kQueue, "woke the wrong queue");
    ++wakeups;
    Memory::Write16(kThread + 0x2C8u, 1);
}
void NandQueueIosCallback(uint32_t callback, int32_t result, uint32_t argument) {
    callbacks.emplace_back(callback, result, argument);
}

namespace NetworkHle {
std::array<WiiSocket, kWiiSocketMax> g_sockets;
int32_t g_lastSocketError = 0;
bool EnsureSocketRuntime() { return true; }
bool SetNonBlocking(NativeSocket socket, bool enabled) {
#ifdef _WIN32
    u_long mode = enabled ? 1 : 0;
    return ioctlsocket(socket, FIONBIO, &mode) == 0;
#else
    const int flags = fcntl(socket, F_GETFL, 0);
    return flags >= 0 && fcntl(socket, F_SETFL, enabled ? flags | O_NONBLOCK : flags & ~O_NONBLOCK) == 0;
#endif
}
int NativeLastError() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}
bool IsWouldBlockError(int error) {
#ifdef _WIN32
    return error == WSAEWOULDBLOCK;
#else
    return error == EWOULDBLOCK || error == EAGAIN;
#endif
}
int32_t TranslateSocketError(int error, bool) {
    if (IsWouldBlockError(error)) return -SO_EAGAIN;
#ifdef _WIN32
    if (error == WSAECONNRESET) return -SO_ECONNRESET;
#else
    if (error == ECONNRESET) return -SO_ECONNRESET;
#endif
    return -SO_EINVAL;
}
int32_t SocketErrorResult(int error, bool readWrite) {
    return g_lastSocketError = TranslateSocketError(error, readWrite);
}
int32_t SocketResult(int result, bool readWrite) {
    g_lastSocketError = result < 0 ? TranslateSocketError(NativeLastError(), readWrite) : 0;
    return result < 0 ? g_lastSocketError : result;
}
int NormalizeConnectError(int error) { return error; }
void NetFail(const char*, ...) {}
WiiSocket* GetWiiSocket(uint32_t fd) {
    return fd < g_sockets.size() && g_sockets[fd].native != kInvalidSocket ? &g_sockets[fd] : nullptr;
}
bool SocketIdentityIsCurrent(uint32_t fd, NativeSocket native, uint64_t generation) {
    const auto* socket = GetWiiSocket(fd);
    return socket && socket->native == native && socket->generation == generation;
}
std::optional<DeviceKind> GetDeviceKind(uint32_t fd) {
    return fd == 2000 && deviceOpen ? std::optional{DeviceKind::IpTop} : std::nullopt;
}
std::vector<IoVector> ReadVectors(uint32_t address, uint32_t count) {
    std::vector<IoVector> result;
    for (uint32_t i = 0; i < count; ++i) result.push_back(RuntimeHle::ReadIoVector(address, i));
    return result;
}
void CloseNativeSocket(NativeSocket socket) {
#ifdef _WIN32
    closesocket(socket);
#else
    close(socket);
#endif
}
void ClearSslSessionsForSocket(uint32_t) {}
const std::array<uint8_t, 6>& RuntimeMacAddress() { static const std::array<uint8_t, 6> mac{}; return mac; }
NasSslWriteAction PreparePlainNasTcpWrite(WiiSocket&, const uint8_t*, uint32_t, std::vector<uint8_t>&) {
    return NasSslWriteAction::PassThrough;
}
int MapWiiAf(uint32_t) { return AF_INET; }
int MapWiiSocketType(uint32_t) { return SOCK_STREAM; }
int MapNativeAfToWii(int family) { return family == AF_INET ? kWiiAfInet : -1; }
int MapWiiAddrInfoFamily(uint32_t) { return AF_INET; }
int MapWiiAddrInfoSocketType(uint32_t) { return SOCK_STREAM; }
uint32_t MapNativeSocketTypeToWii(int) { return kWiiSockStream; }
int32_t AddWiiSocket(NativeSocket, int, int, int) { throw std::runtime_error("unexpected socket allocation"); }
std::string ReadGuestString(uint32_t, uint32_t) { return {}; }
void ZeroMemoryRange(uint32_t address, uint32_t size) { std::memset(Memory::GetPointer(address, size), 0, size); }
void CopyToGuest(uint32_t address, const void* data, uint32_t size) {
    std::memcpy(Memory::GetPointer(address, size), data, size);
}
bool ValidatePollRequest(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t&) { return false; }
std::vector<NetworkPollContract::CopiedDescriptor> CopyPollDescriptors(uint32_t, uint32_t) { return {}; }
void WritePollResults(uint32_t, const std::vector<NetworkPollContract::CopiedDescriptor>&) {}
bool CopiedPollSocketIsStillValid(const NetworkPollContract::CopiedDescriptor&) { return false; }
int ProbeConnectSettled(NativeSocket, int) { return 0; }
int32_t ClassifySettledConnect(NativeSocket) { return 0; }
}

namespace {
using namespace NetworkHle;
using NetworkDeferredContract::StartDisposition;

struct Loopback {
    NativeSocket peer = kInvalidSocket;
    Loopback() {
        NativeSocket listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(listener != kInvalidSocket, "listener failed");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        Require(bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
                listen(listener, 1) == 0, "loopback bind failed");
        socklen_t length = sizeof(address);
        Require(getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0,
                "loopback address failed");
        peer = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        Require(connect(peer, reinterpret_cast<sockaddr*>(&address), length) == 0, "loopback connect failed");
        auto& retained = g_sockets[0];
        retained = {};
        retained.native = accept(listener, nullptr, nullptr);
        retained.generation = 1;
        CloseNativeSocket(listener);
        Require(retained.native != kInvalidSocket && SetSocketNonBlocking(retained, true), "accept failed");
    }
    ~Loopback() {
        if (peer != kInvalidSocket) CloseNativeSocket(peer);
        if (g_sockets[0].native != kInvalidSocket) DeleteWiiSocket(0);
    }
    void Send(const char* bytes) {
        const int size = static_cast<int>(std::strlen(bytes));
        Require(send(peer, bytes, size, 0) == size, "loopback send failed");
        WaitReady();
    }
    void WaitReady() {
        NetworkPollContract::NativePollFd descriptor{};
        descriptor.fd = g_sockets[0].native;
        descriptor.events = POLLRDNORM;
#ifdef _WIN32
        const int result = WSAPoll(&descriptor, 1, 1000);
#else
        const int result = poll(&descriptor, 1, 1000);
#endif
        Require(result > 0, "loopback fixture did not become readable");
    }
};

void ResetRequest(uint32_t flags = 0, uint32_t size = 16) {
    auto& store = GetDeferredNetworkStore();
    Require(store.pendingReceives.empty(), "previous receive was stranded");
    store.syncResults.clear();
    callbacks.clear();
    wakeups = 0;
    terminated = invalidOutput = false;
    deviceOpen = true;
    std::fill(guest.begin(), guest.end(), 0xA5);
    Memory::Write32(kBase + 0xE4, kThread);
    Memory::Write32(kControl, 0);
    Memory::Write32(kControl + 4, flags);
    Memory::Write32(kVectors, kControl);
    Memory::Write32(kVectors + 4, 8);
    Memory::Write32(kVectors + 8, kData);
    Memory::Write32(kVectors + 12, size);
}

auto Start(bool async = false) {
    const auto result = async
        ? Network_HLE_StartIoctlvAsync(2000, IOCTLV_SO_RECVFROM, 1, 1, kVectors, 0x1234, 0x5678)
        : Network_HLE_StartIoctlvSync(2000, IOCTLV_SO_RECVFROM, 1, 1, kVectors, kQueue);
    if (!async && result.disposition == StartDisposition::Started) {
        Memory::Write16(kThread + 0x2C8, 4);
        Memory::Write32(kThread + 0x2DC, kQueue);
        Memory::Write32(kThread + 0x2E0, 0);
        Memory::Write32(kThread + 0x2E4, 0);
        Memory::Write32(kQueue, kThread);
        Memory::Write32(kQueue + 4, kThread);
    }
    return result;
}

void Pump() { CpuContext cpu{}; Network_HLE_ProcessCompletions(&cpu); }

void ExpectResult(uint64_t token, int32_t expected, bool async) {
    Require(HandleIpTopIoctl(IOCTL_SO_GETLASTERROR, 0, 0, 0, 0) == (expected < 0 ? expected : 0),
            "completion published an incorrect last socket error");
    if (async) {
        Require(callbacks.size() == 1 && callbacks[0] == std::tuple{0x1234u, expected, 0x5678u},
                "wrong callback result or count");
    } else {
        int32_t result = 0;
        Require(wakeups == 1 && Network_HLE_TakeSyncResult(token, &result) && result == expected,
                "wrong synchronous result");
        Require(!Network_HLE_TakeSyncResult(token, &result), "sync result delivered twice");
    }
    Pump();
    Require(GetDeferredNetworkStore().pendingReceives.empty(), "completed receive was retained");
    Require(async ? callbacks.size() == 1 : wakeups == 1, "completion delivered twice");
}

void TestReceive(bool async) {
    Loopback connection;
    ResetRequest();
    auto started = Start(async);
    Require(started.disposition == StartDisposition::Started, "empty blocking receive was not parked");
    const auto remaining = GetDeferredNetworkStore().pendingReceives.front().timeout.Deadline() -
                           NetworkPollContract::Timeout::Clock::now();
    Require(remaining > std::chrono::milliseconds(500) && remaining <= std::chrono::milliseconds(1000),
            "receive timeout changed from 1000 milliseconds");
    Pump();
    Require(wakeups == 0 && callbacks.empty() && Memory::Read8(kData) == 0xA5,
            "empty receive completed or touched output");
    const auto generation = GxGuestWrite::GenerationForRange(kData, 16);
    // The descriptor/control storage can be recycled after async submission.
    if (async) {
        terminated = true;
        Memory::Write32(kVectors + 8, 0xFFFFFFFFu);
        Memory::Write32(kControl, 23);
    }
    connection.Send("abc");
    Pump();
    ExpectResult(started.token, 3, async);
    Require(std::memcmp(Memory::GetPointer(kData, 3), "abc", 3) == 0 && Memory::Read8(kData + 3) == 0xA5,
            "receive wrote the wrong output range");
    Require(GxGuestWrite::GenerationForRange(kData, 16) != generation, "receive did not invalidate guest data");

    ResetRequest();
    started = Start(async);
    auto& work = GetDeferredNetworkStore().pendingReceives.front();
    work.timeout = NetworkPollContract::Timeout::FromMilliseconds(0);
    Pump();
    ExpectResult(started.token, -SO_EAGAIN, async);
    Require(Memory::Read8(kData) == 0xA5, "timeout touched output");

    ResetRequest();
    started = Start(async);
    GetDeferredNetworkStore().pendingReceives.front().timeout = NetworkPollContract::Timeout::FromMilliseconds(0);
    connection.Send("late");
    Pump();
    ExpectResult(started.token, 4, async);

    ResetRequest();
    started = Start(async);
    connection.Send("r");
#ifdef _WIN32
    injectedReadError = WSAEWOULDBLOCK;
#else
    injectedReadError = EWOULDBLOCK;
#endif
    Pump();
    Require(GetDeferredNetworkStore().pendingReceives.size() == 1 && wakeups == 0 && callbacks.empty(),
            "lost readiness shortened the receive wait");
    Pump();
    ExpectResult(started.token, 1, async);

    ResetRequest();
    started = Start(async);
    connection.Send("x");
    invalidOutput = true;
    Pump();
    invalidOutput = false;
    ExpectResult(started.token, -SO_EINVAL, async);
    Require(ReceiveFromNow(0, 0, kData, 16) == 1, "invalid output consumed socket bytes");

    ResetRequest();
    started = Start(async);
    ++g_sockets[0].generation;
    connection.Send("g");
    Pump();
    ExpectResult(started.token, -SO_ENOTCONN, async);
    Require(ReceiveFromNow(0, 0, kData, 16) == 1, "stale socket generation consumed bytes");

    ResetRequest();
    started = Start(async);
    connection.Send("d");
    deviceOpen = false;
    Pump();
    deviceOpen = true;
    ExpectResult(started.token, -SO_ENOTCONN, async);
    Require(ReceiveFromNow(0, 0, kData, 16) == 1, "closed IOS device consumed bytes");

    ResetRequest();
    started = Start(async);
    connection.Send("e");
#ifdef _WIN32
    injectedReadError = WSAECONNRESET;
#else
    injectedReadError = ECONNRESET;
#endif
    Pump();
    ExpectResult(started.token, -SO_ECONNRESET, async);
    Require(ReceiveFromNow(0, 0, kData, 16) == 1, "reset injection lost fixture data");

    ResetRequest();
    started = Start(async);
    shutdown(connection.peer, 1);
    connection.WaitReady();
    Pump();
    ExpectResult(started.token, 0, async);
}

void TestCancellationAndModes() {
    Loopback connection;
    for (int cancellation = 0; cancellation < 4; ++cancellation) {
        ResetRequest();
        const auto started = Start();
        Require(started.disposition == StartDisposition::Started, "cancel fixture did not park");
        if (cancellation == 0) terminated = true;
        if (cancellation == 1) Memory::Write64(kQueue + 8, started.token + 1);
        if (cancellation == 2) Memory::Write32(kThread + 0x2DC, kQueue + 16);
        if (cancellation == 3) ++threadGeneration;
        connection.Send("c");
        Pump();
        Require(wakeups == 0 && callbacks.empty() && GetDeferredNetworkStore().pendingReceives.empty(),
                "cancelled waiter completed");
        Require(Memory::Read8(kData) == 0xA5 && ReceiveFromNow(0, 0, kData, 16) == 1,
                "cancelled waiter wrote data or consumed bytes");
    }
    ResetRequest(4);
    Require(Start().disposition == StartDisposition::NotApplicable &&
            HandleIpTopIoctlv(IOCTLV_SO_RECVFROM, {{kControl, 8}}, {{kData, 16}}) == -SO_EAGAIN,
            "forced nonblocking receive parked");
    ResetRequest();
    g_sockets[0].nonblocking = true;
    Require(Start().disposition == StartDisposition::NotApplicable, "nonblocking socket parked");
    g_sockets[0].nonblocking = false;
    ResetRequest(0, 0);
    const int32_t expectedZero = ReceiveFromNow(0, 0, kData, 0);
    auto result = Start();
    Require(result.disposition == StartDisposition::NotApplicable &&
            HandleIpTopIoctlv(IOCTLV_SO_RECVFROM, {{kControl, 8}}, {{kData, 0}}) == expectedZero,
            "zero-size stream receive parked");
    ResetRequest(2);
    connection.Send("peek");
    result = Start();
    Require(result.disposition == StartDisposition::ImmediateResult && result.result == 4 &&
            ReceiveFromNow(0, 0, kData, 16) == 4, "MSG_PEEK consumed bytes");
    Require(SetSocketNonBlocking(g_sockets[0], false), "blocking fixture failed");
    ResetRequest();
    result = Start();
    Require(result.disposition == StartDisposition::Started && !g_sockets[0].nativeNonblocking,
            "receive did not restore native blocking mode");
    AbortPendingDeferredReceives(0, -SO_ENOTCONN);
    Pump();
    ExpectResult(result.token, -SO_ENOTCONN, false);
    ResetRequest();
    Memory::Write32(kVectors + 4, 4);
    result = Start();
    Require(result.disposition == StartDisposition::ImmediateResult && result.result == -SO_EINVAL,
            "short control vector accepted");
    result = Network_HLE_StartIoctlvSync(2000, IOCTLV_SO_RECVFROM, 0xFFFFFFFFu, 2, kVectors, kQueue);
    Require(result.disposition == StartDisposition::ImmediateResult && result.result == -SO_EINVAL,
            "overflowed vector count accepted");
    ResetRequest();
    Memory::Write32(kVectors + 16, kData + 64);
    Memory::Write32(kVectors + 20, 8);
    result = Network_HLE_StartIoctlvSync(2000, IOCTLV_SO_RECVFROM, 1, 2, kVectors, kQueue);
    Require(result.disposition == StartDisposition::NotApplicable &&
            HandleIpTopIoctlv(IOCTLV_SO_RECVFROM, {{kControl, 8}}, {{kData, 16}, {kData + 64, 8}}) == -SO_EAGAIN,
            "source-address receive parked or waited");
}

void TestCloseAndShutdown(bool async) {
    for (int action = 0; action < 3; ++action) {
        Loopback connection;
        ResetRequest();
        const auto started = Start(async);
        Require(started.disposition == StartDisposition::Started, "teardown fixture did not park");
        if (action == 0) {
            Require(DeleteWiiSocket(0) == 0, "socket close failed");
        } else if (action == 1) {
            CleanupAllWiiSockets();
        } else {
            Memory::Write32(kControl + 4, 0);
            Require(HandleIpTopIoctl(IOCTL_SO_SHUTDOWN, kControl, 8, 0, 0) == 0,
                    "read shutdown failed");
        }
        Pump();
        ExpectResult(started.token, action == 2 ? 0 : -SO_ENOTCONN, async);
        Require(Memory::Read8(kData) == 0xA5, "teardown touched receive data");
    }
}
}

int main() {
#ifdef _WIN32
    WSADATA data{};
    if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return 1;
#endif
    try {
        TestReceive(false);
        TestReceive(true);
        TestCancellationAndModes();
        TestCloseAndShutdown(false);
        TestCloseAndShutdown(true);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
