// gx_lighting.cpp - Lighting and Channel Control
#include "gx_internal.h"
#include "runtime_log.h"

#include <cstdio>

namespace {

// GXLightObj is an opaque 64-byte guest object. The Revolution SDK leaves
// three words at the front unused, then stores the exact 13 XF light words
// loaded by GXLoadLightObjImm. Aurora's host-side GXLightObj implementation is
// compact (it starts at color), so a guest pointer must never be passed to the
// host implementation directly.
constexpr uint32_t kLightObjSize = 0x40;
constexpr uint32_t kColorOffset = 0x0c;
constexpr uint32_t kAttnAOffset = 0x10;
constexpr uint32_t kAttnKOffset = 0x1c;
constexpr uint32_t kPositionOffset = 0x28;
constexpr uint32_t kDirectionOffset = 0x34;

bool ValidateGuestLightObj(uint32_t addr, const char* operation) {
    if (addr != 0 && Memory::Contains(addr, kLightObjSize)) {
        return true;
    }
    RT_LOGF(RT_TAG_GX, "%s: invalid GXLightObj @0x%08X\n", operation, addr);
    std::fflush(stderr);
    return false;
}

float ReadGuestLightFloat(uint32_t addr, uint32_t offset) {
    return Memory::ReadFloat32(addr + offset);
}

void InitializeHostLightFromGuest(GXLightObj& host, uint32_t guestAddr) {
    GXInitLightColor(&host, DecodeGxColor(Memory::Read32(guestAddr + kColorOffset)));
    GXInitLightAttn(
        &host,
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 0),
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 4),
        ReadGuestLightFloat(guestAddr, kAttnAOffset + 8),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 0),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 4),
        ReadGuestLightFloat(guestAddr, kAttnKOffset + 8));
    GXInitLightPos(
        &host,
        ReadGuestLightFloat(guestAddr, kPositionOffset + 0),
        ReadGuestLightFloat(guestAddr, kPositionOffset + 4),
        ReadGuestLightFloat(guestAddr, kPositionOffset + 8));

    // GXInitLightDir negates its arguments before storing them. The guest
    // object already contains those stored XF direction components.
    GXInitLightDir(
        &host,
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 0),
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 4),
        -ReadGuestLightFloat(guestAddr, kDirectionOffset + 8));
}

} // namespace

// ============================================================================
// Light Object Load
// ============================================================================

extern "C" void GX__LoadLightObjImm_80170320(uint32_t la, uint32_t lid) {
    if (!ValidateGuestLightObj(la, "GXLoadLightObjImm")) return;
    GXLightObj host{};
    InitializeHostLightFromGuest(host, la);
    GXLoadLightObjImm(&host, static_cast<GXLightID>(lid));
}
PPC_NATIVE_OVERRIDE_VOID(80170320, GX__LoadLightObjImm_80170320, (uint32_t la, uint32_t lid), (la, lid));

// ============================================================================
// Channel Control
// ============================================================================

extern "C" void GX__SetChanAmbColor_8017039c(uint32_t c, uint32_t cp) {
    EnsureAuroraFrameActive();
    uint32_t raw = 0;
    if (!Memory::TryRead32(cp, raw)) return;
    GXColor color = DecodeGxColor(raw);
    GXSetChanAmbColor((GXChannelID)c, color);
}
PPC_NATIVE_OVERRIDE_VOID(8017039c, GX__SetChanAmbColor_8017039c, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetChanMatColor_80170474(uint32_t c, uint32_t cp) {
    EnsureAuroraFrameActive();
    uint32_t raw = 0;
    if (!Memory::TryRead32(cp, raw)) return;
    GXColor color = DecodeGxColor(raw);
    GXSetChanMatColor((GXChannelID)c, color);
}
PPC_NATIVE_OVERRIDE_VOID(80170474, GX__SetChanMatColor_80170474, (uint32_t c, uint32_t cp), (c, cp));

extern "C" void GX__SetNumChans_8017054c(uint32_t n) { GXSetNumChans((u8)n); }
PPC_NATIVE_OVERRIDE_VOID(8017054c, GX__SetNumChans_8017054c, (uint32_t n), (n));

extern "C" void GX__SetChanCtrl_80170570(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af) {
    GXSetChanCtrl((GXChannelID)ch, en!=0, (GXColorSrc)as, (GXColorSrc)ms, lm, (GXDiffuseFn)df, (GXAttnFn)af);
}
PPC_NATIVE_OVERRIDE_VOID(80170570, GX__SetChanCtrl_80170570, (uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af), (ch, en, as, ms, lm, df, af));
