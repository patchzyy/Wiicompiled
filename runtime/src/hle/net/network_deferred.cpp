#include "network_internal.h"

#include "fiber_manager.h"
#include "hle/net/network.h"
#include "ppc_runtime.h"
#include "runtime_log.h"

void OS_HLE_WakeupThreadNoReschedule(CpuContext* ctx, uint32_t waitQueue);
void NandQueueIosCallback(uint32_t callbackPtr, int32_t result, uint32_t callbackArg);

namespace NetworkHle {

enum class DeferredDnsKind {
    GetHostByName,
    GetAddrInfo,
    InetAton,
};

static const char* DeferredDnsKindName(DeferredDnsKind kind) {
    switch (kind) {
    case DeferredDnsKind::GetHostByName:
        return "gethostbyname";
    case DeferredDnsKind::GetAddrInfo:
        return "getaddrinfo";
    case DeferredDnsKind::InetAton:
        return "inet_aton";
    }
    return "?";
}

enum class DeferredNetworkCompletionKind {
    SyncWaitQueue,
    AsyncCallback,
};

struct DeferredNetworkRoute {
    DeferredNetworkCompletionKind kind = DeferredNetworkCompletionKind::SyncWaitQueue;
    uint64_t token = 0;
    uint32_t waitQueue = 0;
    uint32_t expectedThread = 0;
    uint32_t callback = 0;
    uint32_t callbackArg = 0;
};

// This is deliberately a host-only value object. Guest strings, vectors and
// hints are copied by the emulation thread before the worker is launched. The
// worker may carry guest output addresses as opaque integers, but it has no
// CpuContext or guest-memory pointer with which to dereference them.
struct DeferredDnsWork {
    DeferredDnsKind kind = DeferredDnsKind::GetHostByName;
    DeferredNetworkRoute route{};
    std::string node;
    std::string service;
    bool hasHints = false;
    int hintFlags = 0;
    int hintFamily = AF_UNSPEC;
    int hintSockType = 0;
    int hintProtocol = 0;
    uint32_t outAddress = 0;
    uint32_t outSize = 0;
};

struct DeferredDnsAddress {
    int flags = 0;
    uint32_t family = 0;
    int sockType = 0;
    int protocol = 0;
    uint32_t addressLength = 0;
    uint32_t ipv4Address = 0;
    std::array<uint8_t, NetworkDeferredContract::kWiiSockAddrPayloadBytes> socketData{};
};

static std::optional<DeferredDnsAddress> NormalizeDeferredDnsAddress(const addrinfo& native) {
    // This IOS result layout currently owns one complete IPv4 sockaddr. Do not
    // advertise an IPv6 ai_addrlen while retaining only sockaddr::sa_data.
    if (!native.ai_addr ||
        !NetworkDeferredContract::CanCopyIpv4SockAddr(
            native.ai_family, native.ai_addrlen, AF_INET)) {
        return std::nullopt;
    }
    const int wiiFamily = MapNativeAfToWii(native.ai_family);
    if (wiiFamily != kWiiAfInet) {
        return std::nullopt;
    }

    DeferredDnsAddress normalized{};
    normalized.flags = native.ai_flags;
    normalized.family = static_cast<uint32_t>(wiiFamily);
    normalized.sockType = native.ai_socktype;
    normalized.protocol = native.ai_protocol;
    normalized.addressLength =
        static_cast<uint32_t>(NetworkDeferredContract::kWiiIpv4SockAddrBytes);
    const auto* socketAddress = reinterpret_cast<const sockaddr*>(native.ai_addr);
    std::memcpy(normalized.socketData.data(), socketAddress->sa_data,
                normalized.socketData.size());
    const auto* ipv4 = reinterpret_cast<const sockaddr_in*>(native.ai_addr);
    normalized.ipv4Address = ntohl(ipv4->sin_addr.s_addr);
    return normalized;
}

struct DeferredDnsCompletion {
    DeferredDnsWork work{};
    int32_t result = 0;
    std::vector<DeferredDnsAddress> addresses;
};

struct DeferredPollWork {
    DeferredNetworkRoute route{};
    NetworkPollContract::Timeout timeout{};
    uint32_t outAddress = 0;
    uint32_t outSize = 0;
    std::vector<NetworkPollContract::CopiedDescriptor> descriptors;
};

struct DeferredConnectWork {
    DeferredNetworkRoute route{};
    NetworkPollContract::Timeout timeout{};
    uint32_t wiiFd = 0;
    NativeSocket nativeFd = kInvalidSocket;
    uint64_t socketGeneration = 0;
    sockaddr_in peerAddress{};
    std::optional<int32_t> initialResult;
};

using DeferredDnsPreparation =
    NetworkDeferredContract::Preparation<DeferredDnsWork>;
using DeferredPollPreparation =
    NetworkDeferredContract::Preparation<DeferredPollWork>;
using DeferredConnectPreparation =
    NetworkDeferredContract::Preparation<DeferredConnectWork>;

struct DeferredNetworkStore {
    std::mutex completedMutex;
    std::deque<DeferredDnsCompletion> completed;
    // Pending polls are submitted and probed only by the emulation scheduler
    // thread, so unlike DNS worker completions they need no cross-thread lock.
    std::vector<DeferredPollWork> pendingPolls;
    std::vector<DeferredConnectWork> pendingConnects;
    std::mutex resultMutex;
    std::unordered_map<uint64_t, int32_t> syncResults;
    std::mutex schedulerThreadMutex;
    std::optional<std::thread::id> schedulerThread;
    std::atomic<uint64_t> nextToken{1};
};

static DeferredNetworkStore& GetDeferredNetworkStore() {
    // DNS workers are detached so the resolver can never block runtime
    // shutdown. Keep their destination alive for the lifetime of the process;
    // there is intentionally no static destructor for this state.
    static auto* store = new DeferredNetworkStore();
    return *store;
}

// pendingConnects is only ever touched from the emulation/scheduler thread, so
// this needs no lock. Marking the operation resolved makes the next completion
// pump wake the parked OSThread with the requested result; the connect itself is
// never re-probed afterwards.
void AbortPendingDeferredConnects(uint32_t wiiFd, int32_t result) {
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    for (DeferredConnectWork& work : store.pendingConnects) {
        if (work.wiiFd == wiiFd && !work.initialResult) {
            work.initialResult = result;
        }
    }
}

static void NoteDeferredNetworkSchedulerThread(const char* operation) {
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    const std::thread::id current = std::this_thread::get_id();
    std::lock_guard<std::mutex> lock(store.schedulerThreadMutex);
    if (!store.schedulerThread) {
        store.schedulerThread = current;
        return;
    }
    if (*store.schedulerThread != current) {
        RT_LOGF(RT_TAG_NET,
                     "deferred network %s ran off the emulation thread\n",
                     operation);
#ifndef NDEBUG
        assert(*store.schedulerThread == current);
#endif
    }
}

static int32_t DeferredDnsFailureResult(DeferredDnsKind kind) {
    switch (kind) {
    case DeferredDnsKind::GetHostByName:
        return -1;
    case DeferredDnsKind::GetAddrInfo:
        return SO_ERROR_HOST_NOT_FOUND;
    case DeferredDnsKind::InetAton:
        return 0;
    }
    return -1;
}

static void QueueDeferredDnsCompletion(DeferredDnsCompletion completion) {
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    std::lock_guard<std::mutex> lock(store.completedMutex);
    store.completed.push_back(std::move(completion));
}

static DeferredDnsCompletion ResolveDeferredDns(DeferredDnsWork work) {
    DeferredDnsCompletion completion{};
    completion.work = std::move(work);

    addrinfo hints{};
    addrinfo* hintPtr = nullptr;
    if (completion.work.kind != DeferredDnsKind::GetAddrInfo) {
        hints.ai_family = AF_INET;
        hintPtr = &hints;
    } else {
        if (completion.work.hasHints) {
#ifdef AF_INET6
            if (completion.work.hintFamily == AF_INET6) {
                // The guest result encoder deliberately owns IPv4 sockaddr
                // payloads only. An IPv6-only query therefore has no valid
                // result rather than a truncated sockaddr_in6.
                completion.result = DeferredDnsFailureResult(completion.work.kind);
                return completion;
            }
#endif
            hints.ai_flags = completion.work.hintFlags;
            hints.ai_family = completion.work.hintFamily == AF_UNSPEC
                ? AF_INET
                : completion.work.hintFamily;
            hints.ai_socktype = completion.work.hintSockType;
            hints.ai_protocol = completion.work.hintProtocol;
        } else {
            hints.ai_family = AF_INET;
        }
        hintPtr = &hints;
    }

    addrinfo* nativeResults = nullptr;
    const char* node = completion.work.node.empty() ? nullptr : completion.work.node.c_str();
    const char* service = completion.work.service.empty() ? nullptr : completion.work.service.c_str();
    const int gai = getaddrinfo(node, service, hintPtr, &nativeResults);
    if (gai != 0 || !nativeResults) {
        completion.result = DeferredDnsFailureResult(completion.work.kind);
        if (nativeResults) {
            freeaddrinfo(nativeResults);
        }
        // The name the guest asked for is what proves whether the Retro-WFC
        // payload's own resolver hooks rewrote the Nintendo hostname or not.
        NetFail("dns %s '%s'%s%s FAILED gai=%d result=%d",
                DeferredDnsKindName(completion.work.kind), completion.work.node.c_str(),
                completion.work.service.empty() ? "" : " service=",
                completion.work.service.c_str(), gai, completion.result);
        return completion;
    }

    // The host resolver's linked list and sockaddr pointers are owned by its
    // allocator. Normalize everything to POD before leaving the worker.
    const size_t maxAddresses = completion.work.kind == DeferredDnsKind::GetHostByName
        ? 71u
        : (completion.work.kind == DeferredDnsKind::InetAton ? 1u : 256u);
    completion.addresses.reserve(std::min<size_t>(maxAddresses, 16u));
    for (addrinfo* it = nativeResults; it && completion.addresses.size() < maxAddresses; it = it->ai_next) {
        if (auto normalized = NormalizeDeferredDnsAddress(*it)) {
            completion.addresses.push_back(std::move(*normalized));
        }
    }
    freeaddrinfo(nativeResults);
    completion.result = completion.addresses.empty()
        ? DeferredDnsFailureResult(completion.work.kind)
        : 0;
    return completion;
}

static bool LaunchDeferredDns(DeferredDnsWork work) {
    std::unique_ptr<std::thread> worker;
    try {
        worker = std::make_unique<std::thread>([work = std::move(work)]() mutable {
            NetworkDeferredContract::RunWorker(
                std::move(work),
                [](const DeferredDnsWork& copiedWork) {
                    return ResolveDeferredDns(copiedWork);
                },
                [](DeferredDnsCompletion completion) {
                    QueueDeferredDnsCompletion(std::move(completion));
                },
                [](DeferredDnsWork failedWork, std::exception_ptr error) {
                    (void)error;
                    DeferredDnsCompletion failure{};
                    failure.work = std::move(failedWork);
                    failure.result = DeferredDnsFailureResult(failure.work.kind);
                    try {
                        QueueDeferredDnsCompletion(std::move(failure));
                    } catch (const std::exception& queueError) {
                        RT_LOGF(RT_TAG_NET,
                                     "failed to queue deferred DNS failure: %s\n",
                                     queueError.what());
                    } catch (...) {
                        RT_LOGF(RT_TAG_NET,
                                     "failed to queue deferred DNS failure\n");
                    }
                });
        });
    } catch (const std::exception&) {
        return false;
    }

    (void)NetworkDeferredContract::DetachOrRelease(
        std::move(worker), [](std::exception_ptr error) noexcept {
            try {
                if (error) {
                    std::rethrow_exception(error);
                }
            } catch (const std::exception& detachError) {
                RT_LOGF(RT_TAG_NET,
                             "failed to detach deferred DNS worker; "
                             "retaining running worker: %s\n",
                             detachError.what());
                return;
            } catch (...) {
            }
            RT_LOGF(RT_TAG_NET,
                         "failed to detach deferred DNS worker; "
                         "retaining running worker\n");
        });
    return true;
}

// Every deferred operation is an /dev/net/ip/top command; anything else must
// fall straight through to the direct path.
static bool IsIpTopDevice(uint32_t fd) {
    const std::optional<DeviceKind> kind = GetDeviceKind(fd);
    return kind && *kind == DeviceKind::IpTop;
}

static bool IsIpTopCommand(uint32_t fd, uint32_t cmd, uint32_t expectedCmd) {
    return cmd == expectedCmd && IsIpTopDevice(fd);
}

template <typename Work>
static bool IsApplicable(const NetworkDeferredContract::Preparation<Work>& prepared) {
    return prepared.State() != NetworkDeferredContract::PreparationState::NotApplicable;
}

static DeferredDnsPreparation PrepareDeferredGetHostByName(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    if (!IsIpTopCommand(fd, cmd, IOCTL_SO_GETHOSTBYNAME)) {
        return DeferredDnsPreparation::NotApplicable();
    }
    if (!inBuf || !outBuf || outLen < 0x460 || !Memory::Contains(outBuf, outLen)) {
        return DeferredDnsPreparation::Error(-1);
    }

    const uint32_t maxInput = inLen ? inLen : 256u;
    if (!Memory::Contains(inBuf, maxInput)) {
        return DeferredDnsPreparation::Error(-1);
    }

    DeferredDnsWork work{};
    work.kind = DeferredDnsKind::GetHostByName;
    work.node = ReadGuestString(inBuf, maxInput);
    if (work.node.empty()) {
        return DeferredDnsPreparation::Error(-1);
    }
    work.outAddress = outBuf;
    work.outSize = outLen;
    return DeferredDnsPreparation::Ready(std::move(work), -1);
}

static DeferredDnsPreparation PrepareDeferredInetAton(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    if (!IsIpTopCommand(fd, cmd, IOCTL_SO_INETATON)) {
        return DeferredDnsPreparation::NotApplicable();
    }

    const uint32_t maxInput = inLen ? inLen : 256u;
    if (!inBuf || !outBuf || outLen < 4u ||
        !Memory::Contains(inBuf, maxInput) || !Memory::Contains(outBuf, 4u)) {
        return DeferredDnsPreparation::Error(0);
    }

    DeferredDnsWork work{};
    work.kind = DeferredDnsKind::InetAton;
    work.node = ReadGuestString(inBuf, maxInput);
    if (work.node.empty()) {
        return DeferredDnsPreparation::Error(0);
    }
    in_addr numericAddress{};
    if (inet_pton(AF_INET, work.node.c_str(), &numericAddress) == 1) {
        // Numeric conversion cannot block; retain the existing immediate path.
        return DeferredDnsPreparation::NotApplicable();
    }
    work.outAddress = outBuf;
    work.outSize = 4u;
    return DeferredDnsPreparation::Ready(std::move(work), 0);
}

static DeferredDnsPreparation PrepareDeferredScalarDns(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    auto getHostByName =
        PrepareDeferredGetHostByName(fd, cmd, inBuf, inLen, outBuf, outLen);
    if (IsApplicable(getHostByName)) {
        return getHostByName;
    }
    return PrepareDeferredInetAton(fd, cmd, inBuf, inLen, outBuf, outLen);
}

static DeferredPollPreparation PrepareDeferredPoll(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen) {
    if (!IsIpTopCommand(fd, cmd, IOCTL_SO_POLL)) {
        return DeferredPollPreparation::NotApplicable();
    }

    uint32_t descriptorCount = 0;
    if (!ValidatePollRequest(inBuf, inLen, outBuf, outLen, descriptorCount)) {
        return DeferredPollPreparation::Error(-SO_EINVAL);
    }

    const int64_t timeoutMilliseconds = static_cast<int64_t>(Memory::Read64(inBuf));
    if (!NetworkPollContract::RequiresSchedulerWait(timeoutMilliseconds)) {
        // A timeout-zero poll is guaranteed not to block and is cheaper to
        // execute directly than to copy, sleep, wake, and copy back.
        return DeferredPollPreparation::NotApplicable();
    }

    DeferredPollWork work{};
    work.outAddress = outBuf;
    work.outSize = outLen;
    work.timeout = NetworkPollContract::Timeout::FromMilliseconds(timeoutMilliseconds);
    work.descriptors = CopyPollDescriptors(outBuf, descriptorCount);
    return DeferredPollPreparation::Ready(std::move(work), -SO_EINVAL);
}

static DeferredConnectPreparation PrepareDeferredConnect(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen) {
    if (!IsIpTopCommand(fd, cmd, IOCTL_SO_CONNECT)) {
        return DeferredConnectPreparation::NotApplicable();
    }
    if (!inBuf || inLen < 16u || !Memory::Contains(inBuf, 16u)) {
        return DeferredConnectPreparation::Error(-SO_EINVAL);
    }

    const uint32_t wiiFd = Memory::Read32(inBuf);
    WiiSocket* socket = GetWiiSocket(wiiFd);
    if (!socket) {
        return DeferredConnectPreparation::Error(-SO_EBADF);
    }
    if (socket->nonblocking) {
        // The direct path is already a zero-wait host connect and returns
        // SO_EINPROGRESS to the guest as required.
        return DeferredConnectPreparation::NotApplicable();
    }

    DeferredConnectWork work{};
    work.wiiFd = wiiFd;
    work.nativeFd = socket->native;
    work.socketGeneration = socket->generation;
    work.peerAddress = ReadWiiSockAddr(inBuf + 8u);
    work.timeout = NetworkPollContract::Timeout::FromMilliseconds(
        NetworkConnectContract::kGuestBlockingTimeoutMilliseconds);
    return DeferredConnectPreparation::Ready(std::move(work), -SO_EINVAL);
}

static DeferredDnsPreparation PrepareDeferredGetAddrInfo(
    uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut, uint32_t vectorPtr) {
    if (!IsIpTopCommand(fd, cmd, IOCTLV_SO_GETADDRINFO)) {
        return DeferredDnsPreparation::NotApplicable();
    }
    const uint64_t vectorCount = static_cast<uint64_t>(numIn) + numOut;
    if (vectorCount == 0 || vectorCount > UINT32_MAX || !vectorPtr ||
        !Memory::Contains(vectorPtr, static_cast<size_t>(vectorCount) * 8u)) {
        return DeferredDnsPreparation::Error(-SO_EINVAL);
    }

    const std::vector<IoVector> vectors = ReadVectors(vectorPtr, static_cast<uint32_t>(vectorCount));
    const std::vector<IoVector> in(vectors.begin(), vectors.begin() + numIn);
    const std::vector<IoVector> out(vectors.begin() + numIn, vectors.end());
    if (out.empty() || !out[0].address || !out[0].size ||
        !Memory::Contains(out[0].address, out[0].size)) {
        return DeferredDnsPreparation::Error(-SO_EINVAL);
    }
    for (const IoVector& vector : vectors) {
        if ((vector.address == 0) != (vector.size == 0) ||
            (vector.address && !Memory::Contains(vector.address, vector.size))) {
            return DeferredDnsPreparation::Error(-SO_EINVAL);
        }
    }

    DeferredDnsWork work{};
    work.kind = DeferredDnsKind::GetAddrInfo;
    work.outAddress = out[0].address;
    work.outSize = out[0].size;
    if (!in.empty() && in[0].address && in[0].size) {
        if (!Memory::Contains(in[0].address, in[0].size)) {
            return DeferredDnsPreparation::Error(-SO_EINVAL);
        }
        work.node = ReadGuestString(in[0].address, in[0].size);
    }
    if (in.size() > 1 && in[1].address && in[1].size) {
        if (!Memory::Contains(in[1].address, in[1].size)) {
            return DeferredDnsPreparation::Error(-SO_EINVAL);
        }
        work.service = ReadGuestString(in[1].address, in[1].size);
    }
    if (in.size() > 2 && in[2].address) {
        if (in[2].size < 0x10 || !Memory::Contains(in[2].address, 0x10)) {
            return DeferredDnsPreparation::Error(-SO_EINVAL);
        }
        work.hasHints = true;
        work.hintFlags = static_cast<int>(Memory::Read32(in[2].address));
        work.hintFamily = MapWiiAddrInfoFamily(Memory::Read32(in[2].address + 4));
        work.hintSockType = MapWiiAddrInfoSocketType(Memory::Read32(in[2].address + 8));
        work.hintProtocol = static_cast<int>(Memory::Read32(in[2].address + 0x0C));
    }
    if (work.node.empty() && work.service.empty()) {
        return DeferredDnsPreparation::Error(SO_ERROR_HOST_NOT_FOUND);
    }
    return DeferredDnsPreparation::Ready(
        std::move(work), SO_ERROR_HOST_NOT_FOUND);
}

static int32_t ApplyDeferredDnsCompletion(const DeferredDnsCompletion& completion) {
    if (completion.result != 0) {
        return completion.result;
    }

    const uint32_t outAddress = completion.work.outAddress;
    const uint32_t outSize = completion.work.outSize;
    if (!outAddress || !outSize || !Memory::Contains(outAddress, outSize)) {
        return -SO_EINVAL;
    }

    if (completion.work.kind == DeferredDnsKind::InetAton) {
        if (completion.addresses.empty()) {
            return 0;
        }
        Memory::Write32(outAddress, completion.addresses.front().ipv4Address);
        return 1;
    }

    ZeroMemoryRange(outAddress, outSize);
    if (completion.work.kind == DeferredDnsKind::GetHostByName) {
        constexpr uint32_t kStructSize = 0x10;
        constexpr uint32_t kIpListOffset = 0x110;
        constexpr uint32_t kIpPtrListOffset = 0x340;
        const std::string canonical = completion.work.node.size() < 240
            ? completion.work.node
            : completion.work.node.substr(0, 239);
        for (uint32_t i = 0; i <= canonical.size(); ++i) {
            Memory::Write8(outAddress + kStructSize + i,
                           i < canonical.size() ? static_cast<uint8_t>(canonical[i]) : 0);
        }
        Memory::Write32(outAddress, outAddress + kStructSize);
        Memory::Write16(outAddress + 8, kWiiAfInet);
        Memory::Write16(outAddress + 10, 4);
        Memory::Write32(outAddress + 12, outAddress + kIpPtrListOffset);

        const uint32_t count = static_cast<uint32_t>(std::min<size_t>(completion.addresses.size(), 71u));
        for (uint32_t i = 0; i < count; ++i) {
            Memory::Write32(outAddress + kIpListOffset + i * 4,
                            completion.addresses[i].ipv4Address);
            Memory::Write32(outAddress + kIpPtrListOffset + i * 4,
                            outAddress + kIpListOffset + i * 4);
        }
        Memory::Write32(outAddress + kIpPtrListOffset + count * 4, 0);
        // Hardware returns an empty aliases list. Point h_aliases at the same
        // terminator that ends h_addr_list, matching IOS/Dolphin exactly.
        Memory::Write32(outAddress + 4, outAddress + kIpPtrListOffset + count * 4);
        return 0;
    }

    constexpr uint32_t kInfoSize = 0x20;
    constexpr uint32_t kSockAddrOffset = 0x460;
    constexpr uint32_t kSockAddrStride = 0x1C;
    const uint64_t end = static_cast<uint64_t>(outAddress) + outSize;
    const uint64_t infoEnd = std::min<uint64_t>(end, static_cast<uint64_t>(outAddress) + kSockAddrOffset);
    uint32_t infoAddress = outAddress;
    uint32_t socketAddress = outAddress + kSockAddrOffset;
    size_t writeCount = 0;
    while (writeCount < completion.addresses.size() &&
           static_cast<uint64_t>(infoAddress) + kInfoSize <= infoEnd &&
           static_cast<uint64_t>(socketAddress) + kSockAddrStride <= end) {
        ++writeCount;
        infoAddress += kInfoSize;
        socketAddress += kSockAddrStride;
    }

    infoAddress = outAddress;
    socketAddress = outAddress + kSockAddrOffset;
    for (size_t i = 0; i < writeCount; ++i) {
        const DeferredDnsAddress& address = completion.addresses[i];
        if (address.family != kWiiAfInet ||
            address.addressLength != NetworkDeferredContract::kWiiIpv4SockAddrBytes ||
            !NetworkDeferredContract::AdvertisedSockAddrFits(address.addressLength)) {
            return -SO_EINVAL;
        }
        Memory::Write32(infoAddress, static_cast<uint32_t>(address.flags));
        Memory::Write32(infoAddress + 0x04, address.family);
        Memory::Write32(infoAddress + 0x08, MapNativeSocketTypeToWii(address.sockType));
        Memory::Write32(infoAddress + 0x0C, static_cast<uint32_t>(address.protocol));
        Memory::Write32(infoAddress + 0x10, address.addressLength);
        Memory::Write32(infoAddress + 0x14, 0);
        Memory::Write32(infoAddress + 0x18, socketAddress);
        Memory::Write8(socketAddress, static_cast<uint8_t>(address.addressLength & 0xFFu));
        Memory::Write8(socketAddress + 1, static_cast<uint8_t>(address.family & 0xFFu));
        CopyToGuest(socketAddress + 2, address.socketData.data(),
                    static_cast<uint32_t>(address.socketData.size()));
        Memory::Write32(infoAddress + 0x1C,
                        i + 1 < writeCount ? infoAddress + kInfoSize : 0);
        infoAddress += kInfoSize;
        socketAddress += kSockAddrStride;
    }
    return 0;
}

}  // namespace NetworkHle

using namespace NetworkHle;

static bool InitializeDeferredSyncRoute(DeferredNetworkRoute& route, uint32_t waitQueue) {
    if (!waitQueue) {
        return false;
    }
    NoteDeferredNetworkSchedulerThread("submission");
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    uint64_t nextToken = store.nextToken.fetch_add(1, std::memory_order_relaxed);
    if (nextToken == 0) {
        nextToken = store.nextToken.fetch_add(1, std::memory_order_relaxed);
    }
    // OSSleepThread uses the running OSThread, or the SDK default thread during
    // early scheduler setup. Capture its identity before yielding so a late
    // resolver result cannot wake a stale/reused stack queue after cancellation.
    constexpr uint32_t kOSRunningContextAddr = 0x800000E4u;
    constexpr uint32_t kDefaultThreadContextAddr = MKW_GADDR(80347498);
    uint32_t expectedThread = Memory::Read32(kOSRunningContextAddr);
    if (expectedThread == 0) {
        expectedThread = kDefaultThreadContextAddr;
    }
    if (!Memory::Contains(expectedThread + 0x2C8u, 0x20u) ||
        !Memory::Contains(waitQueue, 16u)) {
        return false;
    }
    route.kind = DeferredNetworkCompletionKind::SyncWaitQueue;
    route.token = nextToken;
    route.waitQueue = waitQueue;
    route.expectedThread = expectedThread;
    // ABA cookie: thread/stack addresses are reused after cancellation.
    Memory::Write64(waitQueue + 8u, nextToken);
    return true;
}

static void InitializeDeferredAsyncRoute(DeferredNetworkRoute& route, uint32_t callback,
                                         uint32_t callbackArg) {
    NoteDeferredNetworkSchedulerThread("submission");
    route.kind = DeferredNetworkCompletionKind::AsyncCallback;
    route.callback = callback;
    route.callbackArg = callbackArg;
}

using DeferredStartOutcome = NetworkDeferredContract::StartOutcome;

// Install a prepared operation on either completion route. The sync/async delta
// is entirely in `initializeRoute`: the sync form allocates the token that
// Network_HLE_TakeSyncResult later redeems, the async form leaves route.token at
// zero, which StartPrepared documents as valid for a callback route.
template <typename Work, typename InitializeRoute, typename Install>
static DeferredStartOutcome StartDeferred(NetworkDeferredContract::Preparation<Work> prepared,
                                          InitializeRoute&& initializeRoute, Install&& install) {
    return NetworkDeferredContract::StartPrepared(
        std::move(prepared),
        [&initializeRoute, &install](Work work) -> std::optional<uint64_t> {
            if (!EnsureSocketRuntime() || !initializeRoute(work.route)) {
                return std::nullopt;
            }
            const uint64_t token = work.route.token;
            if (!install(std::move(work))) {
                return std::nullopt;
            }
            return token;
        });
}

static bool DeferredConnectSocketIsStillValid(const DeferredConnectWork& work) {
    return SocketIdentityIsCurrent(work.wiiFd, work.nativeFd, work.socketGeneration);
}

static bool InstallDeferredDns(DeferredDnsWork work) {
    return LaunchDeferredDns(std::move(work));
}

static bool InstallDeferredPoll(DeferredPollWork work) {
    GetDeferredNetworkStore().pendingPolls.push_back(std::move(work));
    return true;
}

// A blocking connect issues the host connect() at submission time, because the
// SDK's own retry loop depends on seeing the immediate outcome; only a genuinely
// in-flight connect is parked for the completion pump to probe.
static bool InstallDeferredConnect(DeferredConnectWork work) {
    if (!DeferredConnectSocketIsStillValid(work)) {
        work.initialResult = -SO_EBADF;
    } else {
        const int ret = connect(work.nativeFd, reinterpret_cast<sockaddr*>(&work.peerAddress),
                                sizeof(work.peerAddress));
        if (ret == 0) {
            work.initialResult = 0;
        } else {
            const int nativeError = NormalizeConnectError(NativeLastError());
            if (!IsWouldBlockError(nativeError)) {
                const int32_t translated = TranslateSocketError(nativeError, false);
                // Dolphin rewrites -SO_EISCONN to SO_SUCCESS here (IOS/Network/
                // Socket.cpp:356-359): the SDK detects a completed blocking
                // connect by re-issuing connect() on it, so reporting EISCONN
                // as an error aborted session setup.
                work.initialResult = translated == -SO_EISCONN ? 0 : translated;
            }
        }
    }
    GetDeferredNetworkStore().pendingConnects.push_back(std::move(work));
    return true;
}

static auto SyncRoute(uint32_t waitQueue) {
    return [waitQueue](DeferredNetworkRoute& route) {
        return InitializeDeferredSyncRoute(route, waitQueue);
    };
}

static auto AsyncRoute(uint32_t callback, uint32_t callbackArg) {
    return [callback, callbackArg](DeferredNetworkRoute& route) {
        InitializeDeferredAsyncRoute(route, callback, callbackArg);
        return true;
    };
}

static std::optional<int32_t> ScalarDeferredFailureResult(uint32_t fd, uint32_t cmd) {
    if (!IsIpTopDevice(fd)) {
        return std::nullopt;
    }
    switch (cmd) {
    case IOCTL_SO_CONNECT:
        return -SO_EINVAL;
    case IOCTL_SO_POLL:
        return -SO_EINVAL;
    case IOCTL_SO_GETHOSTBYNAME:
        return -1;
    case IOCTL_SO_INETATON:
        return 0;
    default:
        return std::nullopt;
    }
}

// Guest-input marshalling faults must never escape into the IOS bridge. Note
// that the two failure results are NOT the same for the ioctlv entry points: a
// faulted descriptor array is a malformed request (-SO_EINVAL) while any other
// failure is reported to the resolver's caller as a lookup miss.
template <typename Body>
static DeferredStartOutcome RunDeferredEntry(int32_t accessViolationResult,
                                             int32_t exceptionResult, Body&& body) {
    try {
        return body();
    } catch (const Memory::AccessViolation&) {
        return DeferredStartOutcome::Immediate(accessViolationResult);
    } catch (const std::bad_alloc&) {
        return DeferredStartOutcome::Immediate(-SO_ENOMEM);
    } catch (const std::exception&) {
        return DeferredStartOutcome::Immediate(exceptionResult);
    }
}

// SO_CONNECT, SO_POLL and the scalar DNS commands all arrive through ioctl and
// are distinguished by which Prepare* claims them.
template <typename InitializeRoute>
static DeferredStartOutcome StartScalarDeferredIoctl(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf, uint32_t outLen,
    InitializeRoute initializeRoute) {
    auto connect = PrepareDeferredConnect(fd, cmd, inBuf, inLen);
    if (IsApplicable(connect)) {
        return StartDeferred(std::move(connect), initializeRoute, InstallDeferredConnect);
    }
    auto poll = PrepareDeferredPoll(fd, cmd, inBuf, inLen, outBuf, outLen);
    if (IsApplicable(poll)) {
        return StartDeferred(std::move(poll), initializeRoute, InstallDeferredPoll);
    }
    return StartDeferred(PrepareDeferredScalarDns(fd, cmd, inBuf, inLen, outBuf, outLen),
                         initializeRoute, InstallDeferredDns);
}

NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlSync(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
    uint32_t outBuf, uint32_t outLen, uint32_t waitQueue) {
    const std::optional<int32_t> failureResult = ScalarDeferredFailureResult(fd, cmd);
    if (!failureResult) {
        return DeferredStartOutcome::NotApplicable();
    }
    return RunDeferredEntry(*failureResult, *failureResult, [&] {
        return StartScalarDeferredIoctl(fd, cmd, inBuf, inLen, outBuf, outLen,
                                        SyncRoute(waitQueue));
    });
}

NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlvSync(
    uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
    uint32_t vectorPtr, uint32_t waitQueue) {
    if (!IsIpTopCommand(fd, cmd, IOCTLV_SO_GETADDRINFO)) {
        return DeferredStartOutcome::NotApplicable();
    }
    return RunDeferredEntry(-SO_EINVAL, SO_ERROR_HOST_NOT_FOUND, [&] {
        return StartDeferred(PrepareDeferredGetAddrInfo(fd, cmd, numIn, numOut, vectorPtr),
                             SyncRoute(waitQueue), InstallDeferredDns);
    });
}

NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlAsync(
    uint32_t fd, uint32_t cmd, uint32_t inBuf, uint32_t inLen,
    uint32_t outBuf, uint32_t outLen, uint32_t callback, uint32_t callbackArg) {
    const std::optional<int32_t> failureResult = ScalarDeferredFailureResult(fd, cmd);
    if (!failureResult) {
        return DeferredStartOutcome::NotApplicable();
    }
    return RunDeferredEntry(*failureResult, *failureResult, [&] {
        return StartScalarDeferredIoctl(fd, cmd, inBuf, inLen, outBuf, outLen,
                                        AsyncRoute(callback, callbackArg));
    });
}

NetworkDeferredContract::StartOutcome Network_HLE_StartIoctlvAsync(
    uint32_t fd, uint32_t cmd, uint32_t numIn, uint32_t numOut,
    uint32_t vectorPtr, uint32_t callback, uint32_t callbackArg) {
    if (!IsIpTopCommand(fd, cmd, IOCTLV_SO_GETADDRINFO)) {
        return DeferredStartOutcome::NotApplicable();
    }
    return RunDeferredEntry(-SO_EINVAL, SO_ERROR_HOST_NOT_FOUND, [&] {
        return StartDeferred(PrepareDeferredGetAddrInfo(fd, cmd, numIn, numOut, vectorPtr),
                             AsyncRoute(callback, callbackArg), InstallDeferredDns);
    });
}

bool Network_HLE_TakeSyncResult(uint64_t token, int32_t* result) {
    if (!token || !result) {
        return false;
    }
    NoteDeferredNetworkSchedulerThread("result retrieval");
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    std::lock_guard<std::mutex> lock(store.resultMutex);
    const auto it = store.syncResults.find(token);
    if (it == store.syncResults.end()) {
        return false;
    }
    *result = it->second;
    store.syncResults.erase(it);
    return true;
}

static bool DeferredNetworkWaiterIsValid(const DeferredNetworkRoute& route) {
    if (!route.waitQueue || !route.expectedThread ||
        !Memory::Contains(route.waitQueue, 16u) ||
        !Memory::Contains(route.expectedThread + 0x2C8u, 0x20u) ||
        Fiber::GuestFiberManager::IsTerminated(route.expectedThread)) {
        return false;
    }

    constexpr uint16_t kThreadStateWaiting = 4;
    constexpr uint32_t kThreadStateOffset = 0x2C8u;
    constexpr uint32_t kThreadQueueOffset = 0x2DCu;
    constexpr uint32_t kThreadNextOffset = 0x2E0u;
    constexpr uint32_t kThreadPrevOffset = 0x2E4u;
    try {
        return Memory::Read16(route.expectedThread + kThreadStateOffset) == kThreadStateWaiting &&
               Memory::Read32(route.expectedThread + kThreadQueueOffset) == route.waitQueue &&
               Memory::Read32(route.waitQueue) == route.expectedThread &&
               Memory::Read32(route.waitQueue + 4u) == route.expectedThread &&
               Memory::Read64(route.waitQueue + 8u) == route.token &&
               Memory::Read32(route.expectedThread + kThreadNextOffset) == 0 &&
               Memory::Read32(route.expectedThread + kThreadPrevOffset) == 0;
    } catch (const Memory::AccessViolation&) {
        return false;
    }
}

static void DeliverDeferredNetworkResult(CpuContext* cpu, DeferredNetworkStore& store,
                                         const DeferredNetworkRoute& route, int32_t result) {
    if (route.kind == DeferredNetworkCompletionKind::SyncWaitQueue) {
        {
            std::lock_guard<std::mutex> lock(store.resultMutex);
            const auto [it, inserted] = store.syncResults.emplace(route.token, result);
            if (!inserted) {
                RT_LOGF(RT_TAG_NET,
                             "duplicate deferred network token %" PRIu64 "\n",
                             route.token);
                it->second = result;
            }
        }
        // Output and result are committed before the waiter becomes READY.
        OS_HLE_WakeupThreadNoReschedule(cpu, route.waitQueue);
    } else {
        // NandProcessPendingCallbacks runs immediately after this pump, on
        // this same emulation/scheduler host thread.
        NandQueueIosCallback(route.callback, result, route.callbackArg);
    }
}

static std::optional<int32_t> ProbeDeferredPoll(
    DeferredPollWork& work, NetworkPollContract::Timeout::TimePoint now) {
    const int nativeResult = NetworkPollContract::ProbeNow(
        work.descriptors, NetworkHle::CopiedPollSocketIsStillValid);
    const int32_t result = nativeResult < 0 ? SocketResult(nativeResult, false) : nativeResult;
    if (work.timeout.ShouldRemainPending(nativeResult, now)) {
        return std::nullopt;
    }
    return result;
}

static int32_t ApplyDeferredPollCompletion(const DeferredPollWork& work, int32_t result) {
    const size_t outputBytes = work.descriptors.size() * 12u;
    if (!work.outAddress || outputBytes == 0 || outputBytes > work.outSize ||
        !Memory::Contains(work.outAddress, outputBytes)) {
        return -SO_EINVAL;
    }
    WritePollResults(work.outAddress, work.descriptors);
    return result;
}

static std::optional<int32_t> ProbeDeferredConnect(
    DeferredConnectWork& work, NetworkPollContract::Timeout::TimePoint now) {
    if (work.initialResult) {
        return *work.initialResult;
    }

    const bool identityIsCurrent = DeferredConnectSocketIsStillValid(work);
    int pollResult = 0;
    if (identityIsCurrent) {
        pollResult = ProbeConnectSettled(work.nativeFd, 0);
    }

    using NetworkConnectContract::ProbeDisposition;
    switch (NetworkConnectContract::ClassifyProbe(
        identityIsCurrent, pollResult, work.timeout.IsExpired(now))) {
    case ProbeDisposition::StaleSocket:
        return -SO_EBADF;
    case ProbeDisposition::PollError:
        return TranslateSocketError(NativeLastError(), false);
    case ProbeDisposition::TimedOut:
        // Dolphin's blocking-connect timeout reports -SO_ENETUNREACH
        // (IOS/Network/Socket.cpp:352-356). -SO_ETIMEDOUT is not reachable
        // through its error table at all, so the SDK has no handler for it.
        return -SO_ENETUNREACH;
    case ProbeDisposition::Pending:
        return std::nullopt;
    case ProbeDisposition::SocketReady:
        break;
    }

    return ClassifySettledConnect(work.nativeFd);
}

bool Network_HLE_ProcessCompletions(CpuContext* cpu) {
    if (!cpu) {
        return false;
    }
    NoteDeferredNetworkSchedulerThread("completion pump");

    std::deque<DeferredDnsCompletion> ready;
    DeferredNetworkStore& store = GetDeferredNetworkStore();
    {
        std::lock_guard<std::mutex> lock(store.completedMutex);
        ready.swap(store.completed);
    }
    bool handledAny = false;

    while (!ready.empty()) {
        DeferredDnsCompletion completion = std::move(ready.front());
        ready.pop_front();

        const DeferredNetworkRoute& route = completion.work.route;
        if (route.kind == DeferredNetworkCompletionKind::SyncWaitQueue &&
            !DeferredNetworkWaiterIsValid(route)) {
            continue;
        }

        int32_t result = completion.result;
        if (result == 0) {
            try {
                result = ApplyDeferredDnsCompletion(completion);
            } catch (const Memory::AccessViolation& error) {
                const std::string_view reason = error.reason();
                NetFail("dns %s '%s' result write faulted: %.*s -> wii=%d",
                        DeferredDnsKindName(completion.work.kind), completion.work.node.c_str(),
                        static_cast<int>(reason.size()), reason.data(), -SO_EINVAL);
                result = -SO_EINVAL;
            }
        }
        DeliverDeferredNetworkResult(cpu, store, route, result);
        handledAny = true;
    }

    const auto now = NetworkPollContract::Timeout::Clock::now();
    for (auto it = store.pendingPolls.begin(); it != store.pendingPolls.end();) {
        DeferredPollWork& work = *it;
        const DeferredNetworkRoute route = work.route;
        if (route.kind == DeferredNetworkCompletionKind::SyncWaitQueue &&
            !DeferredNetworkWaiterIsValid(route)) {
            it = store.pendingPolls.erase(it);
            continue;
        }

        const std::optional<int32_t> probedResult = ProbeDeferredPoll(work, now);
        if (!probedResult) {
            ++it;
            continue;
        }

        int32_t result = *probedResult;
        const size_t descriptorCount = work.descriptors.size();
        try {
            result = ApplyDeferredPollCompletion(work, result);
        } catch (const Memory::AccessViolation& error) {
            const std::string_view reason = error.reason();
            NetFail("SO_POLL(wait) result write faulted for %llu descriptor(s): %.*s -> wii=%d",
                    static_cast<unsigned long long>(descriptorCount),
                    static_cast<int>(reason.size()), reason.data(), -SO_EINVAL);
            result = -SO_EINVAL;
        }
        it = store.pendingPolls.erase(it);
        DeliverDeferredNetworkResult(cpu, store, route, result);
        handledAny = true;
    }

    for (auto it = store.pendingConnects.begin();
         it != store.pendingConnects.end();) {
        DeferredConnectWork& work = *it;
        const DeferredNetworkRoute route = work.route;
        if (route.kind == DeferredNetworkCompletionKind::SyncWaitQueue &&
            !DeferredNetworkWaiterIsValid(route)) {
            it = store.pendingConnects.erase(it);
            continue;
        }

        const std::optional<int32_t> probedResult =
            ProbeDeferredConnect(work, now);
        if (!probedResult) {
            ++it;
            continue;
        }

        int32_t result = *probedResult;
        if (result == 0) {
            if (DeferredConnectSocketIsStillValid(work)) {
                WiiSocket& socket = g_sockets[work.wiiFd];
                socket.peerPort = ntohs(work.peerAddress.sin_port);
                socket.peerAddr = work.peerAddress;
                socket.hasPeerAddr = true;
            } else {
                result = -SO_EBADF;
            }
        }
        g_lastSocketError = result;
        // Every blocking connect settles here exactly once, whether it was
        // resolved at submission, probed, timed out or aborted by a close - so
        // this is the single place a failed WFC connect becomes visible.
        if (result != 0) {
            char peer[INET_ADDRSTRLEN]{};
            inet_ntop(AF_INET, &work.peerAddress.sin_addr, peer, sizeof(peer));
            NetFail("SO_CONNECT fd=%u -> %s:%u FAILED wii=%d", work.wiiFd,
                    peer[0] ? peer : "0.0.0.0",
                    static_cast<unsigned>(ntohs(work.peerAddress.sin_port)), result);
        }
        it = store.pendingConnects.erase(it);
        DeliverDeferredNetworkResult(cpu, store, route, result);
        handledAny = true;
    }
    return handledAny;
}
