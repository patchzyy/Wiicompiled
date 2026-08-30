#include "network_internal.h"

#include "abi_bridge.h"
#include "ppc_runtime.h"

namespace NetworkHle {

extern "C" void MKW_GUEST_FUNC(801D8D30)(CpuContext* ctx);
extern "C" void MKW_GUEST_FUNC(801D9E94)(CpuContext* ctx);

static uint32_t GetNHttpSystemInfo(CpuContext* ctx, uint32_t& savedR3, uint32_t& savedR4, uint32_t& savedR5) {
    savedR3 = ctx->gpr[3];
    savedR4 = ctx->gpr[4];
    savedR5 = ctx->gpr[5];
    MKW_GUEST_FUNC(801D9E94)(ctx);
    const uint32_t systemInfo = ctx->gpr[3];
    ctx->gpr[3] = savedR3;
    ctx->gpr[4] = savedR4;
    ctx->gpr[5] = savedR5;
    return systemInfo;
}

extern "C" void NHTTPStartup_Reentrant_HLE_801d8d30(CpuContext* ctx) {
    if (!ctx) {
        return;
    }

    uint32_t savedR3 = 0;
    uint32_t savedR4 = 0;
    uint32_t savedR5 = 0;
    const uint32_t systemInfo = GetNHttpSystemInfo(ctx, savedR3, savedR4, savedR5);
    if (Memory::Contains(systemInfo + 1996u, 4) && Memory::Read32(systemInfo + 1996u) != 0) {
        // ponytail: NHTTP stays active across DWC auth/GHTTP handoffs, but the
        // allocator callbacks are caller-owned and can outlive the auth object.
        if (Memory::Contains(systemInfo + 1988u, 8)) {
            const uint32_t oldAlloc = Memory::Read32(systemInfo + 1988u);
            const uint32_t oldFree = Memory::Read32(systemInfo + 1992u);
            if (oldAlloc != savedR3 || oldFree != savedR4) {
                Memory::Write32(systemInfo + 1988u, savedR3);
                Memory::Write32(systemInfo + 1992u, savedR4);
            }
        }
        ctx->gpr[3] = 0;
        return;
    }

    ctx->gpr[3] = savedR3;
    ctx->gpr[4] = savedR4;
    ctx->gpr[5] = savedR5;
    MKW_GUEST_FUNC(801D8D30)(ctx);

    const int32_t result = static_cast<int32_t>(ctx->gpr[3]);
    if (result < 0) {
        uint32_t retryR3 = 0;
        uint32_t retryR4 = 0;
        uint32_t retryR5 = 0;
        const uint32_t retrySystemInfo = GetNHttpSystemInfo(ctx, retryR3, retryR4, retryR5);
        const bool becameActive =
            Memory::Contains(retrySystemInfo + 1996u, 4) && Memory::Read32(retrySystemInfo + 1996u) != 0;
        if (becameActive) {
            ctx->gpr[3] = 0;
        }
    }
}

REGISTER_NATIVE_FUNCTION_AS(MKW_GADDR(801D8D30), NHTTPStartup_Reentrant_HLE_801d8d30, "NHTTPStartup_Reentrant_HLE_801d8d30");

static void WriteNcdConfig(uint32_t addr, uint32_t len) {
    if (!addr || len == 0) {
        return;
    }
    ZeroMemoryRange(addr, len);
    if (len >= 0x04) {
        Memory::Write32(addr, 1);
    }
    if (len >= 0x08) {
        Memory::Write8(addr + 4, 2);  // wired
        Memory::Write8(addr + 5, 30);
        Memory::Write8(addr + 6, 7);  // NWC24 permission: all
    }
    if (len >= 0x18) {
        const uint32_t connection = addr + 8;
        Memory::Write8(connection, 0xA7);  // selected, tested, DHCP IP/DNS, wired
        Memory::Write8(connection + 1, 3); // wired link
        const char* name = "MKW Recompiled";
        for (uint32_t i = 0; i < 14 && i + 2 < len - 8 && name[i]; ++i) {
            Memory::Write8(connection + 2 + i, static_cast<uint8_t>(name[i]));
        }
    }
}

enum class KdTrySuspendPhase {
    Boot,
    PostResumeProbe,
    Ready,
};

static KdTrySuspendPhase g_kdTrySuspendPhase = KdTrySuspendPhase::Boot;
static bool g_kdBootProbeSeen = false;

// /dev/net/kd/time state, mirroring Dolphin's NetKDTimeDevice. Any negative
// result here turns into NWC24's generic -42 ("Failed to synchronize time"),
// and DWC's auth stack also reads universal time through this device.
static int64_t g_kdTimeUtcDiff = 0;
static uint64_t g_kdTimeRtc = 0;

static uint64_t KdAdjustedUtcSeconds() {
    const int64_t now = static_cast<int64_t>(std::time(nullptr));
    return static_cast<uint64_t>(now + g_kdTimeUtcDiff);
}

int32_t HandleKdTimeIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                                 uint32_t outLen) {
    // Dolphin: NetKDTimeDevice::IOCtl. Every reply writes the WC24 result word
    // at out+0; the u64 payloads live at out+4.
    enum : uint32_t {
        IOCTL_NW24_GET_UNIVERSAL_TIME = 0x14,
        IOCTL_NW24_SET_UNIVERSAL_TIME = 0x15,
        IOCTL_NW24_UNIMPLEMENTED = 0x16,
        IOCTL_NW24_SET_RTC_COUNTER = 0x17,
        IOCTL_NW24_GET_TIME_DIFF = 0x18,
    };

    int32_t ipcResult = 0;
    switch (cmd) {
    case IOCTL_NW24_GET_UNIVERSAL_TIME:
        if (outBuf && outLen >= 0x0C) {
            Memory::Write64(outBuf + 4u, KdAdjustedUtcSeconds());
        }
        break;
    case IOCTL_NW24_SET_UNIVERSAL_TIME:
        if (inBuf && inLen >= 8) {
            const uint64_t requestedUtc = Memory::Read64(inBuf);
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            g_kdTimeUtcDiff = static_cast<int64_t>(requestedUtc) - now;
        }
        break;
    case IOCTL_NW24_SET_RTC_COUNTER:
        if (inBuf && inLen >= 4) {
            g_kdTimeRtc = Memory::Read32(inBuf);
        }
        break;
    case IOCTL_NW24_GET_TIME_DIFF:
        if (outBuf && outLen >= 0x0C) {
            Memory::Write64(outBuf + 4u, KdAdjustedUtcSeconds() - g_kdTimeRtc);
        }
        break;
    case IOCTL_NW24_UNIMPLEMENTED:
        ipcResult = -9;
        break;
    default:
        break;
    }

    WriteReturn(outBuf, outLen, 0);
    return ipcResult;
}

int32_t HandleKdIoctl(uint32_t cmd, uint32_t inBuf, uint32_t inLen, uint32_t outBuf,
                             uint32_t outLen) {
    switch (cmd) {
    case 0x01: // NWC24 suspend scheduler
        WriteReturn(outBuf, outLen, 0);
        return 0;
    case 0x03: // resume scheduler
        // MKW performs a suspend/user-ID/resume initialization cycle after its
        // first boot-time try-suspend probe. The next probe still observes the
        // scheduler transition as pending; later requests are operational.
        if (g_kdBootProbeSeen && g_kdTrySuspendPhase == KdTrySuspendPhase::Boot) {
            g_kdTrySuspendPhase = KdTrySuspendPhase::PostResumeProbe;
        }
        WriteReturn(outBuf, outLen, 0);
        return 0;
    case 0x04: // time triggers
        // Dolphin writes the scheduler spans (minutes) after the result word;
        // leaving them unwritten hands the guest stale reply-buffer bytes.
        WriteReturn(outBuf, outLen, 0);
        if (outBuf && outLen >= 0x0C) {
            Memory::Write32(outBuf + 4u, 1); // mail span
            Memory::Write32(outBuf + 8u, 2); // download span
        }
        return 0;
    case 0x05: // schedule span
    case 0x08: // lock socket
    case 0x09: // unlock socket
    case 0x1E: // scheduler stat
        WriteReturn(outBuf, outLen, 0);
        return 0;
    case 0x02: // try suspend scheduler
        // Our synchronous IOS bridge reuses the reply buffer, so acknowledging
        // this like Dolphin does (unchanged output) leaks cmd 3's zero result
        // into MKW's post-resume probe and makes the SDK panic. Preserve the
        // two pending startup probes, then report success once ready.
        if (g_kdTrySuspendPhase == KdTrySuspendPhase::Boot) {
            g_kdBootProbeSeen = true;
            WriteReturn(outBuf, outLen, -42);
        } else if (g_kdTrySuspendPhase == KdTrySuspendPhase::PostResumeProbe) {
            WriteReturn(outBuf, outLen, -42);
            g_kdTrySuspendPhase = KdTrySuspendPhase::Ready;
        } else {
            WriteReturn(outBuf, outLen, 0);
        }
        return 0;
    case 0x06: // startup socket
        if (outBuf && outLen) {
            ZeroMemoryRange(outBuf, outLen);
        }
        WriteReturn(outBuf, outLen, 0);
        if (outBuf && outLen >= 8) {
            Memory::Write32(outBuf + 4, 0);
        }
        EnsureSocketRuntime();
        return 0;
    case 0x07: // cleanup socket
        // Dolphin's IOCTL_NWC24_CLEANUP_SOCKET calls WiiSockMan::Clean().
        CleanupAllWiiSockets();
        WriteReturn(outBuf, outLen, 0);
        return 0;
    case 0x0F: // request generated user id
        // Mirrors Dolphin's reply shape: result word, u64 user id at +4,
        // NWC24CreationStage at +0xC (Initial=0/Generated=1/Registered=2). The
        // id is a stable constant so regenerating it can't rebind the DWC
        // account to a different console identity.
        if (outBuf && outLen >= 0x10) {
            ZeroMemoryRange(outBuf, outLen);
            WriteReturn(outBuf, outLen, 0);
            Memory::Write64(outBuf + 4, RuntimeGeneratedUserId());
            Memory::Write32(outBuf + 0x0C, 1); // NWC24CreationStage::Generated
        } else {
            WriteReturn(outBuf, outLen, 0);
        }
        return 0;
    default:
        WriteReturn(outBuf, outLen, 0);
        return 0;
    }
}

int32_t HandleKdIoctlv(uint32_t cmd, const std::vector<IoVector>& in, const std::vector<IoVector>& out) {
    const uint32_t inBuf = in.empty() ? 0 : in[0].address;
    const uint32_t inLen = in.empty() ? 0 : in[0].size;
    const uint32_t outBuf = out.empty() ? 0 : out[0].address;
    const uint32_t outLen = out.empty() ? 0 : out[0].size;
    return HandleKdIoctl(cmd, inBuf, inLen, outBuf, outLen);
}

int32_t HandleNcdIoctlv(uint32_t cmd, const std::vector<IoVector>& in,
                               const std::vector<IoVector>& out) {
    (void)in;
    switch (cmd) {
    case 1: // LOCKWIRELESSDRIVER
        WriteVectorReturn(out, 0, 0);
        if (!out.empty() && out[0].address && out[0].size >= 8) {
            Memory::Write32(out[0].address + 4, 1);
        }
        return 0;
    case 2: // UNLOCKWIRELESSDRIVER
        WriteVectorReturn(out, 0, 0);
        return 0;
    case 3: // GETCONFIG
        if (!out.empty()) {
            WriteNcdConfig(out[0].address, out[0].size);
        }
        WriteVectorReturn(out, out.size() > 1 ? 1 : 0, 0);
        if (out.size() > 1 && out[1].size >= 8) {
            Memory::Write32(out[1].address + 4, 0);
        }
        return 0;
    case 7: // GETLINKSTATUS
        if (!out.empty() && out[0].address && out[0].size >= 8) {
            Memory::Write32(out[0].address, 0);
            Memory::Write32(out[0].address + 4, 3); // LINK_WIRED
        }
        return 0;
    case 8: { // GETWIRELESSMACADDRESS
        const std::array<uint8_t, 6>& mac = RuntimeMacAddress();
        if (out.size() > 1 && out[1].address && out[1].size >= mac.size()) {
            CopyToGuest(out[1].address, mac.data(), static_cast<uint32_t>(mac.size()));
        }
        WriteVectorReturn(out, 0, 0);
        return 0;
    }
    case 4: // SETCONFIG
    case 6: // WRITECONFIG
        WriteVectorReturn(out, out.size() > 1 ? 1 : 0, 0);
        return 0;
    case 5: // READCONFIG
        if (!out.empty()) {
            WriteNcdConfig(out[0].address, out[0].size);
        }
        WriteVectorReturn(out, out.size() > 1 ? 1 : 0, 0);
        if (out.size() > 1 && out[1].size >= 8) {
            Memory::Write32(out[1].address + 4, 0);
        }
        return 0;
    default:
        WriteVectorReturn(out, out.size() > 1 ? 1 : 0, 0);
        return 0;
    }
}

}  // namespace NetworkHle
