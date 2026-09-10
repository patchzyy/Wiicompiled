#include "network_internal.h"

namespace NetworkHle {

static int32_t NewWiiSocket(uint32_t af, uint32_t type, uint32_t protocol) {
    if (!EnsureSocketRuntime()) {
        return -SO_EINVAL;
    }
    const int nativeAf = MapWiiAf(af);
    const int nativeType = MapWiiSocketType(type);
    if (nativeAf < 0 || nativeType < 0) {
        return -SO_EINVAL;
    }
    NativeSocket s = ::socket(nativeAf, nativeType, protocol);
    const int hostError = s == kInvalidSocket ? NativeLastError() : 0;
    const int32_t wiiFd = AddWiiSocket(s, nativeAf, nativeType, static_cast<int>(protocol));
    if (wiiFd < 0) {
        NetFail("SO_SOCKET af=%u type=%u proto=%u failed host=%d wii=%d", af, type, protocol,
                hostError, wiiFd);
    }
    return wiiFd;
}

static int32_t DeleteWiiSocket(uint32_t fd) {
    WiiSocket* s = GetWiiSocket(fd);
    if (!s) {
        return -SO_EBADF;
    }
    ClearSslSessionsForSocket(fd);
    CloseNativeSocket(s->native);
    *s = {};
    s->native = kInvalidSocket;
    // Dolphin fails everything still parked on the socket with -SO_ENOTCONN the
    // moment the descriptor closes. Leaving a blocking connect to notice on its
    // own kept the guest OSThread asleep until the next scheduler pump and then
    // reported -SO_EBADF instead.
    AbortPendingDeferredConnects(fd, -SO_ENOTCONN);
    return 0;
}

// Dolphin: WiiSockMan::Clean (IOS/Network/Socket.h:278) destroys every WiiSocket,
// which closes its host descriptor and fails its pending operations. Going
// through DeleteWiiSocket keeps SSL sessions, buffered NAS writes and parked
// connects from surviving the sockets they were bound to.
void CleanupAllWiiSockets() {
    for (uint32_t fd = 0; fd < static_cast<uint32_t>(kWiiSocketMax); ++fd) {
        if (g_sockets[fd].native != kInvalidSocket) {
            DeleteWiiSocket(fd);
        }
    }
}

struct WiiSockAddrIn {
    uint8_t len = 8;
    uint8_t family = kWiiAfInet;
    uint16_t port = 0;
    uint32_t addr = 0;
};

sockaddr_in ReadWiiSockAddr(uint32_t addr) {
    sockaddr_in native{};
    native.sin_family = AF_INET;
    native.sin_port = htons(Memory::Read16(addr + 2));
    native.sin_addr.s_addr = htonl(Memory::Read32(addr + 4));
    return native;
}

static void WriteWiiSockAddr(uint32_t addr, const sockaddr_in& native, uint32_t len) {
    if (!addr) {
        return;
    }
    Memory::Write8(addr, static_cast<uint8_t>(std::min<uint32_t>(len, 8)));
    Memory::Write8(addr + 1, kWiiAfInet);
    Memory::Write16(addr + 2, ntohs(native.sin_port));
    Memory::Write32(addr + 4, ntohl(native.sin_addr.s_addr));
}

static int MapSockOptLevel(uint32_t level) {
    return level == 0xFFFF ? SOL_SOCKET : static_cast<int>(level);
}

static int MapSockOptName(uint32_t optname) {
    switch (optname) {
    case 0x4:
        return SO_REUSEADDR;
    case 0x80:
        return SO_LINGER;
    case 0x100:
        return SO_OOBINLINE;
    case 0x1001:
        return SO_SNDBUF;
    case 0x1002:
        return SO_RCVBUF;
    case 0x1008:
        return SO_TYPE;
    case 0x1009:
        return SO_ERROR;
    default:
        return static_cast<int>(optname);
    }
}

static in_addr ResolveDefaultInterfaceIp() {
    in_addr fallback{};
    fallback.s_addr = inet_addr("10.0.1.30");
    if (!EnsureSocketRuntime()) {
        return fallback;
    }

    NativeSocket s = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kInvalidSocket) {
        return fallback;
    }

    sockaddr_in remote{};
    remote.sin_family = AF_INET;
    remote.sin_addr.s_addr = inet_addr("8.8.8.8");
    remote.sin_port = htons(53);
    connect(s, reinterpret_cast<const sockaddr*>(&remote), sizeof(remote));

    sockaddr_in local{};
    socklen_t len = sizeof(local);
    if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) != 0) {
        CloseNativeSocket(s);
        return fallback;
    }
    CloseNativeSocket(s);
    return local.sin_addr;
}

static int32_t HandleInetAton(uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    if (!inBuf || !outBuf || outLen < 4) {
        return 0;
    }
    const std::string host = ReadGuestString(inBuf, inLen ? inLen : 256);
    in_addr addr{};
    if (inet_pton(AF_INET, host.c_str(), &addr) != 1) {
        // Hostnames are resolved only through the deferred boundary. Numeric
        // conversion is the sole direct path because inet_pton cannot block.
        return 0;
    }
    Memory::Write32(outBuf, ntohl(addr.s_addr));
    return 1;
}

static int32_t HandleInetPton(uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    if (!inBuf || !outBuf || outLen < 8) {
        return 0;
    }
    const std::string address = ReadGuestString(inBuf, inLen ? inLen : 256);
    in_addr parsed{};
    if (inet_pton(AF_INET, address.c_str(), &parsed) != 1) {
        return 0;
    }
    Memory::Write32(outBuf + 4u, ntohl(parsed.s_addr));
    return 1;
}

int32_t HandleIpTopIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    switch (cmd) {
    case IOCTL_SO_INITINTERFACE:
    case IOCTL_SO_SETINTERFACE:
        return 0;
    case IOCTL_SO_SOCKET:
        if (!inBuf || inLen < 12) return -SO_EINVAL;
        return NewWiiSocket(Memory::Read32(inBuf), Memory::Read32(inBuf + 4), Memory::Read32(inBuf + 8));
    case IOCTL_SO_CLOSE:
        if (!inBuf || inLen < 4) return -SO_EINVAL;
        return DeleteWiiSocket(Memory::Read32(inBuf));
    case IOCTL_SO_BIND:
    case IOCTL_SO_CONNECT: {
        if (!inBuf || inLen < 16) return -SO_EINVAL;
        const uint32_t requestFd = Memory::Read32(inBuf);
        WiiSocket* s = GetWiiSocket(requestFd);
        if (!s) {
            return -SO_EBADF;
        }
        sockaddr_in addr = ReadWiiSockAddr(inBuf + 8);
        const int ret = (cmd == IOCTL_SO_BIND)
            ? bind(s->native, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))
            : connect(s->native, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int32_t result = 0;
        if (cmd == IOCTL_SO_CONNECT && ret < 0) {
            // Nonblocking connects only (blocking ones go through
            // Network_HLE_StartIoctl*). Dolphin passes the raw error through here
            // (IOS/Network/Socket.cpp:344-346); the guest's connect loop needs to
            // see SO_EINPROGRESS/SO_EALREADY/SO_EISCONN as distinct states.
            result = SocketErrorResult(NormalizeConnectError(NativeLastError()), false);
        } else {
            result = SocketResult(ret, false);
        }
        if (cmd == IOCTL_SO_CONNECT && result == 0) {
            s->peerPort = ntohs(addr.sin_port);
            s->peerAddr = addr;
            s->hasPeerAddr = true;
        }
        return result;
    }
    case IOCTL_SO_FCNTL: {
        if (!inBuf || inLen < 12) return -SO_EINVAL;
        WiiSocket* s = GetWiiSocket(Memory::Read32(inBuf));
        if (!s) return -SO_EBADF;
        const uint32_t op = Memory::Read32(inBuf + 4);
        const uint32_t arg = Memory::Read32(inBuf + 8);
        if (op == 3) {
            return s->nonblocking ? 4 : 0;
        }
        if (op == 4) {
            s->nonblocking = (arg & 4) != 0;
            return 0;
        }
        return 0;
    }
    case IOCTL_SO_GETSOCKNAME:
    case IOCTL_SO_GETPEERNAME: {
        if (!inBuf || !outBuf || inLen < 4 || outLen < 8) return -SO_EINVAL;
        WiiSocket* s = GetWiiSocket(Memory::Read32(inBuf));
        if (!s) return -SO_EBADF;
        sockaddr_in addr{};
        socklen_t len = sizeof(addr);
        const int ret = (cmd == IOCTL_SO_GETSOCKNAME)
            ? getsockname(s->native, reinterpret_cast<sockaddr*>(&addr), &len)
            : getpeername(s->native, reinterpret_cast<sockaddr*>(&addr), &len);
        if (ret == 0) {
            WriteWiiSockAddr(outBuf, addr, static_cast<uint32_t>(len));
        }
        return SocketResult(ret, false);
    }
    case IOCTL_SO_GETSOCKOPT: {
        if (!outBuf || outLen < 0x14) return -SO_EINVAL;
        WiiSocket* s = GetWiiSocket(Memory::Read32(outBuf));
        if (!s) return -SO_EBADF;
        const int level = MapSockOptLevel(Memory::Read32(outBuf + 4));
        const int optname = MapSockOptName(Memory::Read32(outBuf + 8));
        int value = 0;
        socklen_t valueLen = sizeof(value);
        const int ret = getsockopt(s->native, level, optname, reinterpret_cast<char*>(&value), &valueLen);
        Memory::Write32(outBuf + 0x0C, static_cast<uint32_t>(valueLen));
        Memory::Write32(outBuf + 0x10, static_cast<uint32_t>(value));
        return SocketResult(ret, false);
    }
    case IOCTL_SO_SETSOCKOPT: {
        if (!inBuf || inLen < 0x10) return -SO_EINVAL;
        WiiSocket* s = GetWiiSocket(Memory::Read32(inBuf));
        if (!s) return -SO_EBADF;
        const int level = MapSockOptLevel(Memory::Read32(inBuf + 4));
        const int optname = MapSockOptName(Memory::Read32(inBuf + 8));
        const uint32_t optLen = std::min<uint32_t>(Memory::Read32(inBuf + 0x0C), inLen - 0x10);
        const char* optVal = reinterpret_cast<const char*>(Memory::GetPointer(inBuf + 0x10, optLen));
        return SocketResult(setsockopt(s->native, level, optname, optVal, static_cast<int>(optLen)), false);
    }
    case IOCTL_SO_LISTEN: {
        if (!inBuf || inLen < 8) return -SO_EINVAL;
        WiiSocket* s = GetWiiSocket(Memory::Read32(inBuf));
        if (!s) {
            return -SO_EBADF;
        }
        return SocketResult(listen(s->native, static_cast<int>(Memory::Read32(inBuf + 4))), false);
    }
    case IOCTL_SO_SHUTDOWN: {
        // Dolphin: WiiSocket::Shutdown (IOS/Network/Socket.cpp:164-212).
        if (!inBuf || inLen < 8) return -SO_EINVAL;
        const uint32_t wiiFd = Memory::Read32(inBuf);
        WiiSocket* s = GetWiiSocket(wiiFd);
        if (!s) return -SO_EBADF;
        const uint32_t how = Memory::Read32(inBuf + 4);
        if (how > 2) {
            return -SO_EINVAL;
        }

        int socketType = 0;
        socklen_t socketTypeLen = sizeof(socketType);
        if (getsockopt(s->native, SOL_SOCKET, SO_TYPE, reinterpret_cast<char*>(&socketType),
                       &socketTypeLen) != 0 ||
            (socketType != SOCK_STREAM && socketType != SOCK_DGRAM)) {
            return -SO_EBADF;
        }
        if (socketType == SOCK_DGRAM) {
            // The Wii does nothing and reports success for UDP. Passing this
            // through to the host returned ENOTCONN for an unconnected datagram
            // socket, which made the guest's teardown path bail out early and
            // leak the descriptor.
            return SO_SUCCESS;
        }

        const int32_t result = SocketResult(shutdown(s->native, static_cast<int>(how)), false);
        if (how == 1 || how == 2) {
            // Dolphin aborts a pending blocking connect with -SO_ENETUNREACH
            // when the write half is shut down.
            AbortPendingDeferredConnects(wiiFd, -SO_ENETUNREACH);
        }
        return result;
    }
    case IOCTL_SO_POLL: {
        uint32_t descriptorCount = 0;
        if (!ValidatePollRequest(inBuf, inLen, outBuf, outLen, descriptorCount)) {
            return -SO_EINVAL;
        }

        // Zero-timeout readiness probes intentionally take this direct path.
        // ProbeNow always passes timeout zero to the host API, so this cannot
        // stall the emulation thread. Finite and infinite waits are copied and
        // serviced by the scheduler queue instead.
        std::vector<NetworkPollContract::CopiedDescriptor> descriptors =
            CopyPollDescriptors(outBuf, descriptorCount);
        const int nativeResult =
            NetworkPollContract::ProbeNow(descriptors, CopiedPollSocketIsStillValid);
        const int32_t result = nativeResult < 0 ? SocketResult(nativeResult, false) : nativeResult;
        WritePollResults(outBuf, descriptors);
        return result;
    }
    case IOCTL_SO_GETHOSTID:
        return static_cast<int32_t>(ntohl(ResolveDefaultInterfaceIp().s_addr));
    case IOCTL_SO_GETHOSTBYNAME:
        // Valid hostname lookups are installed through the deferred boundary.
        // This defensive path handles only a malformed/unroutable entry call and
        // must never invoke the host resolver on the guest scheduler thread.
        NetFail("SO_GETHOSTBYNAME took the direct path (deferred boundary bypassed); "
                "name resolution failed");
        return -1;
    case IOCTL_SO_GETLASTERROR:
        return g_lastSocketError;
    case IOCTL_SO_INETATON:
        return HandleInetAton(inBuf, inLen, outBuf, outLen);
    case IOCTL_SO_INETPTON:
        return HandleInetPton(inBuf, inLen, outBuf, outLen);
    case IOCTL_SO_INETNTOP: {
        if (!inBuf || !outBuf || inLen < 12) return -SO_EINVAL;
        char text[32]{};
        std::snprintf(text, sizeof(text), "%u.%u.%u.%u", Memory::Read8(inBuf + 8), Memory::Read8(inBuf + 9),
                      Memory::Read8(inBuf + 10), Memory::Read8(inBuf + 11));
        const uint32_t size = std::min<uint32_t>(outLen ? outLen - 1 : 0, static_cast<uint32_t>(std::strlen(text)));
        CopyToGuest(outBuf, text, size);
        Memory::Write8(outBuf + size, 0);
        return 0;
    }
    default:
        // SO_ACCEPT (cmd 1) has no handler and lands here too.
        return -SO_EINVAL;
    }
}

static int32_t HandleGetInterfaceOpt(const std::vector<IoVector>& in, const std::vector<IoVector>& out) {
    if (in.empty() || out.empty() || in[0].size < 8) {
        return -SO_EINVAL;
    }
    const uint32_t param = Memory::Read32(in[0].address);
    const uint32_t opt = Memory::Read32(in[0].address + 4);
    uint32_t offset = 0;
    if (!out.empty() && out[0].size >= 8) {
        offset = Memory::Read32(out[0].address + 4);
    }

    switch (opt) {
    case 0xb003:
        Memory::Write32(out[0].address, 0x08080808);
        if (out[0].size >= 8) {
            Memory::Write32(out[0].address + 4, 0x08080404);
        }
        return 0;
    case 0x1003:
        Memory::Write32(out[0].address, 0);
        return 0;
    case 0x1004: {
        const std::array<uint8_t, 6>& mac = RuntimeMacAddress();
        CopyToGuest(out[0].address, mac.data(), std::min<uint32_t>(out[0].size, mac.size()));
        return 0;
    }
    case 0x1005:
        Memory::Write32(out[0].address, 1);
        return 0;
    case 0x3001:
        Memory::Write32(out[0].address, 0x10);
        return 0;
    case 0x4002:
        Memory::Write32(out[0].address, 1);
        return 0;
    case 0x4003: {
        if (out.size() > 1 && out[1].address) {
            Memory::Write32(out[1].address, 0x0C);
        }
        const uint32_t hostOrderIp = ntohl(ResolveDefaultInterfaceIp().s_addr);
        Memory::Write32(out[0].address, hostOrderIp);
        if (out[0].size >= 12) {
            Memory::Write32(out[0].address + 4, 0xFFFFFF00);
            Memory::Write32(out[0].address + 8, (hostOrderIp & 0xFFFFFF00) | 0xFF);
        }
        return 0;
    }
    case 0x4005:
        Memory::Write32(out[0].address, 0x20);
        return 0;
    case 0x4006:
        if (out[0].size >= offset + 24) {
            Memory::Write32(out[0].address + offset, 0);
            Memory::Write32(out[0].address + offset + 4, 0);
            Memory::Write32(out[0].address + offset + 8, 0);
            Memory::Write32(out[0].address + offset + 12, 1);
            Memory::Write64(out[0].address + offset + 16, 0);
            offset += 24;
        }
        if (out.size() > 1 && out[1].address) {
            Memory::Write32(out[1].address, offset);
        }
        return 0;
    case 0x6003:
    case 0x600a:
    case 0x600c:
        Memory::Write32(out[0].address, 0x80);
        return 0;
    case 0xb002:
        Memory::Write32(out[0].address, 2);
        return 0;
    default:
        Memory::Write32(out[0].address, 0);
        return 0;
    }
}

int32_t HandleIpTopIoctlv(uint32_t cmd, const std::vector<IoVector>& in, const std::vector<IoVector>& out) {
    switch (cmd) {
    case IOCTLV_SO_STARTUP:
        EnsureSocketRuntime();
        return 0;
    case IOCTLV_SO_CLEANUP:
        CleanupAllWiiSockets();
        return 0;
    case IOCTLV_SO_GETINTERFACEOPT:
        return HandleGetInterfaceOpt(in, out);
    case IOCTLV_SO_SETINTERFACEOPT:
        return 0;
    case IOCTLV_SO_GETADDRINFO:
        // As with SO_GETHOSTBYNAME: resolution belongs to the deferred boundary.
        NetFail("SO_GETADDRINFO took the direct path (deferred boundary bypassed); "
                "name resolution failed");
        return SO_ERROR_HOST_NOT_FOUND;
    case IOCTLV_SO_SENDTO: {
        if (in.size() < 2) return -SO_EINVAL;
        const uint32_t fd = Memory::Read32(in[1].address);
        WiiSocket* s = GetWiiSocket(fd);
        if (!s) return -SO_EBADF;
        // Dolphin: "send/sendto only handles MSG_OOB" - flags &= SO_MSG_OOB
        // (IOS/Network/Socket.cpp:606-607). SO_MSG_PEEK (0x02) is a receive-only
        // flag; forwarding it to sendto() makes Winsock reject the whole call.
        uint32_t flags = Memory::Read32(in[1].address + 4) & 0x01;
        const uint32_t hasDest = Memory::Read32(in[1].address + 8);
        sockaddr_in dest{};
        sockaddr* destPtr = nullptr;
        socklen_t destLen = 0;
        if (hasDest) {
            dest = ReadWiiSockAddr(in[1].address + 0x0C);
            destPtr = reinterpret_cast<sockaddr*>(&dest);
            destLen = sizeof(dest);
        }
        const auto* data = Memory::GetPointer(in[0].address, in[0].size);
        std::vector<uint8_t> patched;
        const NasSslWriteAction nasAction = !destPtr
            ? PreparePlainNasTcpWrite(*s, data, in[0].size, patched)
            : NasSslWriteAction::PassThrough;
        if (nasAction == NasSslWriteAction::Buffered) {
            return static_cast<int32_t>(in[0].size);
        }

        const bool patchedWrite = nasAction == NasSslWriteAction::Ready;
        const uint8_t* sendData = patchedWrite ? patched.data() : data;
        const uint32_t sendSize = patchedWrite ? static_cast<uint32_t>(patched.size()) : in[0].size;
        const int ret = sendto(s->native, reinterpret_cast<const char*>(sendData), static_cast<int>(sendSize),
                               static_cast<int>(flags), destPtr, destLen);
        const int hostError = ret < 0 ? NativeLastError() : 0;
        int32_t result = SocketResult(ret);
        if (patchedWrite && ret == static_cast<int>(sendSize)) {
            result = static_cast<int32_t>(in[0].size);
        }
        // A blocked send is routine; anything else aborts the connection, and
        // the SDK retries it, so only a change of error is reported.
        if (ret < 0 && result != -SO_EAGAIN && result != s->lastLoggedSendError) {
            s->lastLoggedSendError = result;
            NetFail("send failed fd=%u size=%u host=%d wii=%d", fd, sendSize, hostError, result);
        }
        return result;
    }
    case IOCTLV_SO_RECVFROM: {
        if (in.empty() || out.empty()) return -SO_EINVAL;
        const uint32_t fd = Memory::Read32(in[0].address);
        WiiSocket* s = GetWiiSocket(fd);
        if (!s) return -SO_EBADF;
        const uint32_t rawFlags = Memory::Read32(in[0].address + 4);
        uint32_t flags = rawFlags & 0x03;
        const bool forceNonBlock = (rawFlags & 0x04u) != 0;
        char* data = reinterpret_cast<char*>(Memory::GetPointer(out[0].address, out[0].size));
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        sockaddr* fromPtr = out.size() > 1 && out[1].address && out[1].size >= 8 ? reinterpret_cast<sockaddr*>(&from) : nullptr;
        int ret = 0;

        ret = recvfrom(s->native, data, static_cast<int>(out[0].size), static_cast<int>(flags), fromPtr,
                       fromPtr ? &fromLen : nullptr);
        int nativeErr = ret < 0 ? NativeLastError() : 0;
        // Nonblocking sockets get -SO_EAGAIN immediately (Dolphin's retry predicate
        // short-circuits on nonBlock/forceNonBlock, IOS/Network/Socket.cpp:715-718);
        // waiting here anyway stalled the whole emulation thread on every empty read.
        constexpr int kStreamRecvWaitMs = 1000;
        const int streamWaitMs = (forceNonBlock || s->nonblocking) ? 0 : kStreamRecvWaitMs;
        const bool waited = ret < 0 && !fromPtr && s->type == SOCK_STREAM &&
            IsWouldBlockError(nativeErr) && WaitForReadable(s->native, streamWaitMs);
        if (waited) {
            ret = recvfrom(s->native, data, static_cast<int>(out[0].size), static_cast<int>(flags), nullptr,
                           nullptr);
            // WaitForReadable also returns true for POLLERR/POLLHUP, so the retry
            // is where a reset connection surfaces; the first call's would-block
            // error must not be reused or the guest retries that socket forever.
            nativeErr = ret < 0 ? NativeLastError() : 0;
        }

        if (ret >= 0 && fromPtr) {
            WriteWiiSockAddr(out[1].address, from, static_cast<uint32_t>(fromLen));
        }
        const int32_t result = ret >= 0 ? SocketResult(ret) : SocketErrorResult(nativeErr);
        if (ret < 0 && result != -SO_EAGAIN && result != s->lastLoggedRecvError) {
            s->lastLoggedRecvError = result;
            NetFail("recv failed fd=%u host=%d wii=%d", fd, nativeErr, result);
        }
        return result;
    }
    default:
        return -SO_EINVAL;
    }
}

}  // namespace NetworkHle
