#pragma once

// Shared internals of the /dev/net HLE. Only entities used by more than one
// network_*.cpp live here; everything else stays file-local.

#include "hle/network_poll_contract.h"
#include "hle/runtime_parse_helpers.h"
#include "memory.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cassert>
#include <cctype>
#include <ctime>
#include <chrono>
#include <cinttypes>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mstcpip.h>
#include <iphlpapi.h>
#define SECURITY_WIN32
#include <security.h>
#include <schannel.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace NetworkHle {

enum class DeviceKind {
    KdRequest,
    KdTime,
    NcdManage,
    IpTop,
    Ssl,
};

using IoVector = RuntimeHle::IoVector;

// Upper bound on the descriptor count an Ioctlv may declare. Real IOS commands
// use a handful of vectors; this only exists so a guest-supplied count cannot
// drive an unbounded allocation or overflow the in/out split.
inline constexpr uint64_t kMaxIoVectors = 32;

// Ordinal positions of Dolphin's SO_E* table (IOS/Network/Socket.h). Load-bearing:
// the guest SDK compares the negated value against its own copy, so an off-by-one
// here silently turns "connection refused" into the wrong error on the guest.
enum WiiSocketError {
    SO_SUCCESS = 0,
    SO_EACCES = 2,
    SO_EADDRINUSE = 3,
    SO_EAGAIN = 6,
    SO_EALREADY = 7,
    SO_EBADF = 8,
    SO_ECONNABORTED = 13,
    SO_ECONNREFUSED = 14,
    SO_ECONNRESET = 15,
    SO_EHOSTUNREACH = 23,
    SO_EINPROGRESS = 26,
    SO_EINVAL = 28,
    SO_EISCONN = 30,
    SO_EMFILE = 33,
    SO_ENETRESET = 39,
    SO_ENETUNREACH = 40,
    SO_ENOBUFS = 42,
    SO_ENOMEM = 49,
    SO_ENOPROTOOPT = 51,
    SO_ENOTCONN = 56,
    SO_ENOTSOCK = 59,
    SO_EPROTONOSUPPORT = 68,
    SO_ETIMEDOUT = 76,
    SO_ERROR_HOST_NOT_FOUND = -305,
};

constexpr int kWiiAfInet = 2;
constexpr int kWiiAfInet6 = 23;
constexpr int kWiiSockStream = 1;
constexpr int kWiiSockDgram = 2;
constexpr int kWiiSockRaw = 3;
constexpr int kWiiSockRdm = 4;
constexpr int kWiiSockSeqPacket = 5;
constexpr int kWiiSocketMax = 24;

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
#endif

struct WiiSocket {
    NativeSocket native = kInvalidSocket;
    int af = AF_INET;
    int type = SOCK_STREAM;
    int protocol = 0;
    bool nonblocking = false;
    uint16_t peerPort = 0;
    bool hasPeerAddr = false;
    sockaddr_in peerAddr{};
    std::vector<uint8_t> nasWriteBuffer;
    uint64_t generation = 0;
    // Failure-report one-shots: a failing send/recv repeats every retry, so only
    // a change of error is reported. Reset with the rest of the struct when the
    // fd is recycled, so a new socket reports its own failures again.
    int32_t lastLoggedSendError = 0;
    int32_t lastLoggedRecvError = 0;
};

extern std::array<WiiSocket, kWiiSocketMax> g_sockets;
extern int32_t g_lastSocketError;

// Dolphin's WiiSocket::CloseFd and WiiSocket::Shutdown resolve every operation
// still queued on the socket at the moment it goes away instead of letting them
// linger (IOS/Network/Socket.cpp:214-235 and 164-212). Defined below, next to
// the deferred store it walks.
void AbortPendingDeferredConnects(uint32_t wiiFd, int32_t result);

enum class NasSslWriteAction {
    PassThrough,
    Buffered,
    Ready,
};

enum NetIoctl {
    IOCTL_SO_ACCEPT = 1,
    IOCTL_SO_BIND = 2,
    IOCTL_SO_CLOSE = 3,
    IOCTL_SO_CONNECT = 4,
    IOCTL_SO_FCNTL = 5,
    IOCTL_SO_GETPEERNAME = 6,
    IOCTL_SO_GETSOCKNAME = 7,
    IOCTL_SO_GETSOCKOPT = 8,
    IOCTL_SO_SETSOCKOPT = 9,
    IOCTL_SO_LISTEN = 10,
    IOCTL_SO_POLL = 11,
    IOCTLV_SO_RECVFROM = 12,
    IOCTLV_SO_SENDTO = 13,
    IOCTL_SO_SHUTDOWN = 14,
    IOCTL_SO_SOCKET = 15,
    IOCTL_SO_GETHOSTID = 16,
    IOCTL_SO_GETHOSTBYNAME = 17,
    IOCTL_SO_GETLASTERROR = 20,
    IOCTL_SO_INETATON = 21,
    IOCTL_SO_INETPTON = 22,
    IOCTL_SO_INETNTOP = 23,
    IOCTLV_SO_GETADDRINFO = 24,
    IOCTLV_SO_STARTUP = 26,
    IOCTLV_SO_CLEANUP = 27,
    IOCTLV_SO_GETINTERFACEOPT = 28,
    IOCTLV_SO_SETINTERFACEOPT = 29,
    IOCTL_SO_SETINTERFACE = 30,
    IOCTL_SO_INITINTERFACE = 31,
};

// network_core.cpp
bool RetroRewindProfileActive();
const std::array<uint8_t, 6>& RuntimeMacAddress();
uint64_t RuntimeGeneratedUserId();
void ZeroMemoryRange(uint32_t addr, uint32_t size);
void CopyToGuest(uint32_t addr, const void* data, uint32_t size);
std::string ReadGuestString(uint32_t addr, uint32_t maxLen = 1024);
std::string Lower(std::string_view text);
bool StartsWith(std::string_view text, std::string_view prefix);
std::vector<IoVector> ReadVectors(uint32_t vectorPtr, uint32_t count);
bool WriteReturn(uint32_t outBuf, uint32_t outLen, int32_t value);
bool WriteVectorReturn(const std::vector<IoVector>& out, size_t index, int32_t value);
std::optional<DeviceKind> GetDeviceKind(uint32_t fd);
// Always-on product logging, reserved for failures a player can act on: a
// connect/auth/DNS/SSL failure or a socket error that aborts a connection.
// Routine per-packet traffic must not be reported here. Host error codes are
// captured before the call, because std::fprintf may clobber errno.
void NetFail(const char* format, ...);
bool EnsureSocketRuntime();
int NativeLastError();
int32_t TranslateSocketError(int err, bool isReadWrite = true);
int NormalizeConnectError(int err);
int32_t SocketResult(int ret, bool isReadWrite = true);
int32_t SocketErrorResult(int nativeErr, bool isReadWrite = true);
bool IsWouldBlockError(int err);
int ProbeConnectSettled(NativeSocket socket, int timeoutMs);
int32_t ClassifySettledConnect(NativeSocket socket);
bool WaitForReadable(NativeSocket socket, int timeoutMs);
void CloseNativeSocket(NativeSocket socket);
void SetNonBlocking(NativeSocket socket, bool enabled);
int32_t ReconnectWiiSocket(WiiSocket& socket, uint16_t port);
int MapWiiAf(uint32_t af);
int MapNativeAfToWii(int af);
int MapWiiSocketType(uint32_t type);
int MapWiiAddrInfoFamily(uint32_t family);
int MapWiiAddrInfoSocketType(uint32_t type);
uint32_t MapNativeSocketTypeToWii(int type);
int32_t AddWiiSocket(NativeSocket native, int af, int type, int protocol);
WiiSocket* GetWiiSocket(uint32_t fd);

// A Wii fd alone does not identify a socket: the slot is recycled by SOSocket
// after SOClose, so a copied descriptor must also match the host descriptor and
// the generation counter it was captured with.
bool SocketIdentityIsCurrent(uint32_t wiiFd, NativeSocket nativeFd, uint64_t generation);
bool CopiedPollSocketIsStillValid(const NetworkPollContract::CopiedDescriptor& descriptor);

// SO_POLL request marshalling, shared by the direct (timeout-zero) path in
// network_socket.cpp and the scheduler-queued path in network_deferred.cpp.
bool ValidatePollRequest(uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen,
                         uint32_t& descriptorCount);
std::vector<NetworkPollContract::CopiedDescriptor> CopyPollDescriptors(uint32_t outBuf,
                                                                      uint32_t descriptorCount);
void WritePollResults(uint32_t outAddress,
                      const std::vector<NetworkPollContract::CopiedDescriptor>& descriptors);

// network_socket.cpp
void CleanupAllWiiSockets();
sockaddr_in ReadWiiSockAddr(uint32_t addr);
int32_t HandleIpTopIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                         uint32_t outLen);
int32_t HandleIpTopIoctlv(uint32_t cmd, const std::vector<IoVector>& in,
                          const std::vector<IoVector>& out);

// network_ssl.cpp
void ClearSslSessionsForSocket(uint32_t fd);
NasSslWriteAction PreparePlainNasTcpWrite(WiiSocket& socket, const uint8_t* data,
                                         uint32_t size, std::vector<uint8_t>& patched);
int32_t HandleSslIoctlv(uint32_t cmd, const std::vector<IoVector>& in,
                        const std::vector<IoVector>& out);

// network_config.cpp
int32_t HandleKdTimeIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                          uint32_t outLen);
int32_t HandleKdIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                      uint32_t outLen);
int32_t HandleKdIoctlv(uint32_t cmd, const std::vector<IoVector>& in,
                       const std::vector<IoVector>& out);
int32_t HandleNcdIoctlv(uint32_t cmd, const std::vector<IoVector>& in,
                        const std::vector<IoVector>& out);

}  // namespace NetworkHle
