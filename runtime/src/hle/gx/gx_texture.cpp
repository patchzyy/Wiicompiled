// gx_texture.cpp - Texture Object and TLUT Functions
#include "gx_internal.h"
#include "runtime_log.h"

#include <algorithm>
#include <cstdlib>

// Helper to write GXTexObj structure to guest memory in SDK format
// This is needed because other code may read the structure directly
static void WriteGuestTexObj(uint32_t addr, uint32_t dataAddr, uint16_t width, uint16_t height, 
                             uint32_t format, uint32_t wrapS, uint32_t wrapT, bool mipmap,
                             bool writeTlut, uint32_t tlut) {
    dataAddr = CanonicalizeGxMainRamAddress(dataAddr);
    for (int i = 0; i < 8; i++) {
        Memory::Write32(addr + i * 4, 0);
    }

    uint32_t word0 = (wrapS & 0x3u) | ((wrapT & 0x3u) << 2) | 0x10u;
    uint32_t word1 = 0;
    if (!mipmap) {
        word0 = (word0 & 0xFFFFFF10u) | (wrapS & 0x3u) | ((wrapT & 0x3u) << 2) | 0x90u;
    } else {
        word0 = (word0 & 0xFFFFFF10u) | (wrapS & 0x3u) | ((wrapT & 0x3u) << 2) |
                (((format - 8u) < 3u) ? 0xB0u : 0xD0u);
        const uint32_t maxDim = std::max<uint32_t>(width, height);
        const uint32_t maxLod = maxDim > 0 ? (31u - static_cast<uint32_t>(__builtin_clz(maxDim))) : 0u;
        word1 |= (std::min<uint32_t>(maxLod * 16u, 0xFFu) << 8);
    }

    uint32_t blockShiftX = 2;
    uint32_t blockShiftY = 2;
    uint8_t blockType = 2;
    switch (format & 0xFu) {
    case 0:
    case 8:
        blockShiftX = 3;
        blockShiftY = 3;
        blockType = 1;
        break;
    case 1:
    case 2:
    case 9:
        blockShiftX = 3;
        blockShiftY = 2;
        blockType = 2;
        break;
    case 3:
    case 4:
    case 5:
    case 10:
        blockShiftX = 2;
        blockShiftY = 2;
        blockType = 2;
        break;
    case 6:
        blockShiftX = 2;
        blockShiftY = 2;
        blockType = 3;
        break;
    case 14:
        blockShiftX = 3;
        blockShiftY = 3;
        blockType = 0;
        break;
    default:
        blockShiftX = 2;
        blockShiftY = 2;
        blockType = 2;
        break;
    }

    const uint32_t word2 =
        ((width - 1u) & 0x3FFu) |
        (((height - 1u) & 0x3FFu) << 10) |
        ((format & 0xFu) << 20);
    const uint32_t word3 = (dataAddr >> 5) & 0x00FFFFFFu;
    const uint32_t blocksX = (static_cast<uint32_t>(width) + ((1u << blockShiftX) - 1u)) >> blockShiftX;
    const uint32_t blocksY = (static_cast<uint32_t>(height) + ((1u << blockShiftY) - 1u)) >> blockShiftY;
    const uint16_t blockCount = static_cast<uint16_t>((blocksX * blocksY) & 0x7FFFu);
    uint8_t flags = mipmap ? 0x03u : 0x02u;

    Memory::Write32(addr + 0x00, word0);
    Memory::Write32(addr + 0x04, word1);
    Memory::Write32(addr + 0x08, word2);
    Memory::Write32(addr + 0x0C, word3);
    Memory::Write32(addr + 0x14, format);
    if (writeTlut) {
        Memory::Write32(addr + 0x18, tlut);
    }
    Memory::Write16(addr + 0x1C, blockCount);
    Memory::Write8(addr + 0x1E, blockType);
    Memory::Write8(addr + 0x1F, flags);
}

// SDK min-filter enum -> the hardware encoding stored in GXTexObj word0[7:5]
// (GX2HWFiltConv). Entries 6 and 7 have no SDK filter and read back as 0; the
// inverse table (HW -> SDK) lives in gx_objects.cpp.
static const uint8_t kGxToHwMinFilter[8] = {0, 4, 1, 5, 2, 6, 0, 0};

// Helper to update LOD info in guest GXTexObj structure
static void WriteGuestTexObjLOD(uint32_t addr, uint32_t minFilt, uint32_t magFilt,
                                 float minLod, float maxLod, float lodBias,
                                 bool biasClamp, bool edgeLod, uint32_t maxAniso) {
    // Read existing word0 and update relevant bits
    uint32_t word0 = Memory::Read32(addr + 0x00);

    // bits [4] = magFilter, bits [7:5] = hardware minFilter encoding
    uint32_t minFiltBits = (minFilt < 6u) ? kGxToHwMinFilter[minFilt] : 4;
    word0 = (word0 & ~0xF0) | ((magFilt & 1) << 4) | ((minFiltBits & 7) << 5);
    
    // bits [8] = edgeLod (inverted), bits [17:9] = lodBias (signed, scaled by 32)
    int8_t biasScaled = (int8_t)(lodBias * 32.0f);
    word0 = (word0 & ~0x3FF00) | ((edgeLod ? 0 : 1) << 8) | ((uint32_t)(uint8_t)biasScaled << 9);
    
    // bits [20:19] = maxAniso, bit [21] = biasClamp
    word0 = (word0 & ~0x380000) | ((maxAniso & 0x3) << 19) | ((biasClamp ? 1 : 0) << 21);
    
    Memory::Write32(addr + 0x00, word0);
    
    // word1: bits [7:0] = minLod * 16, bits [15:8] = maxLod * 16
    uint8_t minLodScaled = (uint8_t)(minLod * 16.0f);
    uint8_t maxLodScaled = (uint8_t)(maxLod * 16.0f);
    uint32_t word1 = Memory::Read32(addr + 0x04);
    word1 = (word1 & 0xFFFF0000) | minLodScaled | ((uint32_t)maxLodScaled << 8);
    Memory::Write32(addr + 0x04, word1);
}

// ============================================================================
// Texture Object Initialization
// ============================================================================

static uint32_t g_unsupportedFormatLogCount = 0;

// Must cover every format aurora's convert_texture() decodes; anything missing here is refused
// and never reaches aurora (GX_TF_C14X2 was once missing despite aurora decoding it, silently
// dropping C14X2 binds). GX_CTF_YUVA8 stays listed for THP EFB-copy frames, which never go through convert_texture.
static bool IsAuroraLoadableTexFormat(uint32_t fmt) noexcept {
    switch (fmt) {
    case GX_TF_I4:
    case GX_TF_I8:
    case GX_TF_IA4:
    case GX_TF_IA8:
    case GX_TF_C4:
    case GX_TF_C8:
    case GX_TF_C14X2:
    case GX_TF_RGB565:
    case GX_TF_RGB5A3:
    case GX_TF_Z8:
    case GX_TF_Z16:
    case GX_TF_Z24X8:
    case GX_TF_RGBA8:
    case GX_CTF_YUVA8:
    case GX_TF_CMPR:
        return true;
    default:
        return false;
    }
}

static GXTexWrapMode SanitizeWrapMode(uint32_t rawWrap) noexcept {
    switch (rawWrap) {
    case GX_CLAMP:
    case GX_REPEAT:
    case GX_MIRROR:
        return static_cast<GXTexWrapMode>(rawWrap);
    default:
        return GX_CLAMP;
    }
}

static GXTexFilter SanitizeMinFilter(uint32_t rawFilter) noexcept {
    switch (rawFilter) {
    case GX_NEAR:
    case GX_LINEAR:
    case GX_NEAR_MIP_NEAR:
    case GX_LIN_MIP_NEAR:
    case GX_NEAR_MIP_LIN:
    case GX_LIN_MIP_LIN:
        return static_cast<GXTexFilter>(rawFilter);
    // Some decoded hardware encodings surface as 6/7; map to the strongest valid mip filter.
    case 6:
    case 7:
        return GX_LIN_MIP_LIN;
    default:
        return GX_LINEAR;
    }
}

static GXTexFilter SanitizeMagFilter(uint32_t rawFilter) noexcept {
    return (rawFilter == GX_NEAR) ? GX_NEAR : GX_LINEAR;
}

static GXAnisotropy SanitizeAniso(uint32_t rawAniso) noexcept {
    switch (rawAniso) {
    case GX_ANISO_1:
    case GX_ANISO_2:
    case GX_ANISO_4:
        return static_cast<GXAnisotropy>(rawAniso);
    default:
        return GX_ANISO_4;
    }
}

static bool IsReasonableTextureDimensions(uint16_t width, uint16_t height) noexcept {
    // Hardware textures are small; reject absurd dimensions that can destabilize host texture creation.
    constexpr uint16_t kMaxDimension = 4096;
    return width > 0 && height > 0 && width <= kMaxDimension && height <= kMaxDimension;
}

static float SanitizeLodValue(float lod, float fallback) noexcept {
    if (!std::isfinite(lod)) {
        return fallback;
    }
    return std::max(0.0f, lod);
}

static float SanitizeLodBias(float lodBias) noexcept {
    if (!std::isfinite(lodBias)) {
        return 0.0f;
    }
    return std::clamp(lodBias, -4.0f, 3.99f);
}

// Republish the host texobj's LOD/filter block from `meta`, with the three
// fields each caller may be overriding passed in explicitly.
static void ApplyHostTexObjLod(GXTexObj* obj, const TexObjMeta& meta, GXTexFilter minFilter,
                               GXTexFilter magFilter, float lodBias, GXAnisotropy maxAniso) {
    GXInitTexObjLOD(obj, minFilter, magFilter, meta.minLod, meta.maxLod, lodBias,
                    meta.biasClamp ? GX_TRUE : GX_FALSE, meta.edgeLod ? GX_TRUE : GX_FALSE,
                    maxAniso);
}

static uint32_t ComputeMaxMipLevel(uint16_t width, uint16_t height) noexcept {
    uint32_t w = std::max<uint32_t>(1u, width);
    uint32_t h = std::max<uint32_t>(1u, height);
    uint32_t levels = 0;
    while (w > 1u || h > 1u) {
        w = std::max<uint32_t>(1u, w >> 1u);
        h = std::max<uint32_t>(1u, h >> 1u);
        ++levels;
    }
    return levels;
}

// GXInitTexObj* writes the guest struct synchronously; the host half runs on the GX worker with the
// struct bytes from before that write, which is what an inline call's shadow capture sees.
template <typename Hle, typename Guest>
static void InitTexObjSplit(uint32_t oa, Hle&& hle, Guest&& guest) {
    if (!GxWorker::ShouldDefer()) {
        hle();
        guest();
        return;
    }
    std::optional<TexObjBytes> before;
    try {
        before = CaptureTexObjBytes(oa);
    } catch (...) {
    }
    guest();
    GxWorker::Submit([oa, before, hle = std::forward<Hle>(hle)] {
        if (before) {
            TexObjSnapshotScope snapshot(oa, *before);
            hle();
        } else {
            hle();
        }
    });
}

extern "C" void GX__InitTexObj_801707f8(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) {
    if (w == 0 || h == 0) {
        auto* cpu = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX,
                "GXInitTexObj zero size oa=0x%08X da=0x%08X w=%u h=%u f=%u ws=%u wt=%u m=%u pc=0x%08X lr=0x%08X\n",
                oa, da, w, h, f, ws, wt, m, cpu ? cpu->pc : 0u, cpu ? cpu->lr : 0u);
    }
    const uint32_t canonicalDataAddr = CanonicalizeGxMainRamAddress(da);
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex); GXTexObj* obj = CreateHostTexObj(oa); TexObjMeta& meta = GetTexObjMeta(oa);
        meta.dataAddr=canonicalDataAddr; meta.width=(u16)w; meta.height=(u16)h; meta.format=f; meta.wrapS=ws; meta.wrapT=wt; meta.mipmap=(m!=0); meta.userData=0; meta.needsUpload=true;
        GXInitTexObj(obj, GuestToHostPtr(da), (u16)w, (u16)h, (GXTexFmt)f, (GXTexWrapMode)ws, (GXTexWrapMode)wt, (GXBool)m); MarkHostTexObjConstructed(oa);
    }, [=] {
        WriteGuestTexObj(oa, canonicalDataAddr, (u16)w, (u16)h, f, ws, wt, m != 0, false, 0);
    });
}

PPC_NATIVE_OVERRIDE_VOID(801707f8, GX__InitTexObj_801707f8, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_switch_80170938(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(80170938, GX__InitTexObj_switch_80170938, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_0_8017093c(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(8017093c, GX__InitTexObj_caseD_0_8017093c, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_1_80170950(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(80170950, GX__InitTexObj_caseD_1_80170950, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_3_80170964(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(80170964, GX__InitTexObj_caseD_3_80170964, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_6_80170978(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(80170978, GX__InitTexObj_caseD_6_80170978, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_e_8017098c(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(8017098c, GX__InitTexObj_caseD_e_8017098c, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

extern "C" void GX__InitTexObj_caseD_7_801709a0(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m) { GX__InitTexObj_801707f8(oa, da, w, h, f, ws, wt, m); }
PPC_NATIVE_OVERRIDE_VOID(801709a0, GX__InitTexObj_caseD_7_801709a0, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m), (oa, da, w, h, f, ws, wt, m));

// ============================================================================
// Texture Object Configuration
// ============================================================================

extern "C" void GX__InitTexObjCI_80170a04(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m, uint32_t tl) {
    if (w == 0 || h == 0) {
        auto* cpu = TryGetCpuContext();
        RT_LOGF(RT_TAG_GX,
                "GXInitTexObjCI zero size oa=0x%08X da=0x%08X w=%u h=%u f=%u ws=%u wt=%u m=%u tl=%u pc=0x%08X lr=0x%08X\n",
                oa, da, w, h, f, ws, wt, m, tl, cpu ? cpu->pc : 0u, cpu ? cpu->lr : 0u);
    }
    const uint32_t canonicalDataAddr = CanonicalizeGxMainRamAddress(da);
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex); GXTexObj* obj = CreateHostTexObj(oa); TexObjMeta& meta = GetTexObjMeta(oa);
        meta.dataAddr=canonicalDataAddr; meta.width=(u16)w; meta.height=(u16)h; meta.format=f; meta.wrapS=ws; meta.wrapT=wt; meta.mipmap=(m!=0); meta.tlut=tl; meta.userData=0; meta.needsUpload=true;
        GXInitTexObjCI(obj, GuestToHostPtr(da), (u16)w, (u16)h, (GXCITexFmt)f, (GXTexWrapMode)ws, (GXTexWrapMode)wt, (GXBool)m, tl); MarkHostTexObjConstructed(oa);
    }, [=] {
        WriteGuestTexObj(oa, canonicalDataAddr, (u16)w, (u16)h, f, ws, wt, m != 0, true, tl);
        // GXInitTexObjCI clears bit1 in the flags byte; keep guest memory consistent.
        try {
            uint8_t flags = Memory::Read8(oa + 0x1F);
            Memory::Write8(oa + 0x1F, static_cast<uint8_t>(flags & 0xFD));
        } catch (...) {
        }
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170a04, GX__InitTexObjCI_80170a04, (uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m, uint32_t tl), (oa, da, w, h, f, ws, wt, m, tl));

extern "C" void GX__InitTexObjLOD_80170a4c(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma) {
    const float fmal = std::isfinite(mal) && mal >= 0.f ? mal : 0.f, fmil = std::isfinite(mil) && mil >= 0.f ? std::min(mil, fmal) : 0.f;
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex); GXTexObj* obj = GetHostTexObj(oa); TexObjMeta& meta = GetTexObjMeta(oa);
        meta.minFilter=mif; meta.magFilter=maf; meta.minLod=fmil; meta.maxLod=fmal; meta.lodBias=lb; meta.biasClamp=(bc!=0); meta.edgeLod=(el!=0); meta.maxAniso=ma;
        GXInitTexObjLOD(obj, (GXTexFilter)mif, (GXTexFilter)maf, fmil, fmal, lb, (GXBool)bc, (GXBool)el, (GXAnisotropy)ma);
    }, [=] {
        WriteGuestTexObjLOD(oa, mif, maf, fmil, fmal, lb, bc != 0, el != 0, ma);
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170a4c, GX__InitTexObjLOD_80170a4c, (uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma), (oa, mif, maf, mil, mal, lb, bc, el, ma));

extern "C" void GX__InitTexObjWrapMode_80170b50(uint32_t oa, uint32_t ws, uint32_t wt) {
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        GXTexObj* obj = GetHostTexObj(oa);
        TexObjMeta& meta = GetTexObjMeta(oa);
        meta.wrapS = ws;
        meta.wrapT = wt;
        GXInitTexObjWrapMode(obj, (GXTexWrapMode)ws, (GXTexWrapMode)wt);
    }, [=] {
        try {
            uint32_t word0 = Memory::Read32(oa + 0x00);
            word0 = (word0 & ~0xFu) | (ws & 0x3u) | ((wt & 0x3u) << 2);
            Memory::Write32(oa + 0x00, word0);
        } catch (...) {
        }
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170b50, GX__InitTexObjWrapMode_80170b50, (uint32_t oa, uint32_t ws, uint32_t wt), (oa, ws, wt));

extern "C" void GX__InitTexObjTlut_80170b64(uint32_t oa, uint32_t tl) {
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        GXTexObj* obj = GetHostTexObj(oa);
        TexObjMeta& meta = GetTexObjMeta(oa);
        meta.tlut = tl;
        GXInitTexObjTlut(obj, tl);
    }, [=] {
        // Keep guest GXTexObj coherent for later GXLoadTexObj from memory.
        try {
            Memory::Write32(oa + 0x18, tl);
        } catch (...) {
        }
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170b64, GX__InitTexObjTlut_80170b64, (uint32_t oa, uint32_t tl), (oa, tl));

// GXInitTexObjFilter - sets mag/min filter modes
extern "C" void GX__InitTexObjFilter_80170b6c(uint32_t oa, uint32_t minFilter, uint32_t magFilter) {
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        TexObjMeta& meta = GetTexObjMeta(oa);
        meta.minFilter = minFilter;
        meta.magFilter = magFilter;
        // Update host texture object if it exists
        if (GXTexObj* obj = TryGetHostTexObj(oa)) {
            ApplyHostTexObjLod(obj, meta, (GXTexFilter)minFilter, (GXTexFilter)magFilter, meta.lodBias,
                               (GXAnisotropy)meta.maxAniso);
        }
    }, [=] {
        try {
            uint32_t word0 = Memory::Read32(oa + 0x00);
            // bit 4: magFilter - set if magFilter == GX_LINEAR (1), clear otherwise
            // Original code used cntlzw to check: cntlzw(magFilter-1) >> 1 & 0x10
            // This sets bit 4 when magFilter-1 is 0 (i.e., magFilter == 1)
            uint32_t magBit = (magFilter == 1) ? 0x10 : 0;
            word0 = (word0 & 0xFFFFFFEF) | magBit;
            // bits [7:5]: minFilter from LUT
            uint32_t minBits = (kGxToHwMinFilter[minFilter & 7] & 7) << 5;
            word0 = (word0 & 0xFFFFFF0F) | magBit | minBits;
            Memory::Write32(oa + 0x00, word0);
        } catch (...) {}
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170b6c, GX__InitTexObjFilter_80170b6c, (uint32_t oa, uint32_t minFilter, uint32_t magFilter), (oa, minFilter, magFilter));

// GXInitTexObjLODBias - sets LOD bias value
extern "C" void GX__InitTexObjLODBias_80170b94(uint32_t oa, float bias) {
    // Clamp bias to [-4.0, 3.99...] range as per SDK
    constexpr float kMinBias = -4.0f;
    constexpr float kMaxBias = 3.99f; // Actually ~3.990000009536743
    float clampedBias = bias;
    if (clampedBias < kMinBias) clampedBias = kMinBias;
    if (clampedBias >= 4.0f) clampedBias = kMaxBias;

    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        TexObjMeta& meta = GetTexObjMeta(oa);
        meta.lodBias = clampedBias;
        // Update host texture object if it exists
        if (GXTexObj* obj = TryGetHostTexObj(oa)) {
            ApplyHostTexObjLod(obj, meta, (GXTexFilter)meta.minFilter, (GXTexFilter)meta.magFilter,
                               clampedBias, (GXAnisotropy)meta.maxAniso);
        }
    }, [=] {
        // Update guest GXTexObj word0 with bias (scale by 32, store in bits [16:9])
        try {
            uint32_t word0 = Memory::Read32(oa + 0x00);
            int8_t biasScaled = static_cast<int8_t>(clampedBias * 32.0f);
            word0 = (word0 & 0xFFFE01FF) | ((static_cast<uint32_t>(static_cast<uint8_t>(biasScaled)) & 0xFF) << 9);
            Memory::Write32(oa + 0x00, word0);
        } catch (...) {}
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170b94, GX__InitTexObjLODBias_80170b94, (uint32_t oa, float bias), (oa, bias));

extern "C" void GX__InitTexObjUserData_80170be8(uint32_t oa, uint32_t userData) {
    InitTexObjSplit(oa, [=] {
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        TexObjMeta& meta = GetTexObjMeta(oa);
        meta.userData = userData;
        // Aurora keeps an expanded host-side object, so mirror the pointer when
        // that representation has already been constructed.
        if (GXTexObj* obj = TryGetHostTexObj(oa)) {
            GXInitTexObjUserData(obj, GuestToHostPtr(userData));
        }
    }, [=] {
        // The RVL SDK stores this opaque guest pointer verbatim at GXTexObj + 0x10.
        WriteGuest32(oa + 0x10, userData, "GXInitTexObjUserData");
    });
}

PPC_NATIVE_OVERRIDE_VOID(80170be8, GX__InitTexObjUserData_80170be8,
              (uint32_t oa, uint32_t userData), (oa, userData));

// ============================================================================
// Texture Loading
// ============================================================================

// A refused GXLoadTexObj must not leave the previous material's texture bound in that TEV
// slot (rejection paths used to just return, so draws sampled stale, unrelated art). Bind
// aurora's empty texture instead: null image data resolves to a 1x1 transparent placeholder,
// which is also the correct result for the common case of an intentionally-transparent hide texture.
static void BindUnloadableTexturePlaceholder(uint32_t tid) {
    if (tid >= g_boundTexMaps.size()) {
        return;
    }
    // Callers hold g_texObjMutex, so this initialization is serialized.
    static GXTexObj s_placeholder{};
    static bool s_placeholderReady = false;
    if (!s_placeholderReady) {
        GXInitTexObj(&s_placeholder, nullptr, 1, 1, GX_TF_RGBA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
        s_placeholderReady = true;
    }
    // A zeroed binding record can never compare equal to a real load (those
    // always carry a non-zero objAddr), so the CanSkipHostLoad fast path cannot
    // mistake a later genuine bind for a no-op.
    g_boundTexMaps[tid] = BoundTexInfo{};
    GXLoadTexObj(&s_placeholder, (GXTexMapID)tid);
}

static void LoadTexObjImpl(uint32_t oa, uint32_t tid) {
    static uint32_t s_invalidMetaLogCount = 0;
    static uint32_t s_invalidTidLogCount = 0;
    static uint32_t s_invalidDimLogCount = 0;
    static uint32_t s_hostExceptionLogCount = 0;
    static uint32_t s_metaLookupLogCount = 0;
    static uint32_t s_unknownFormatLogCount = 0;
    static uint32_t s_invalidDataLogCount = 0;
    std::lock_guard<std::mutex> guard(g_texObjMutex);

    if (tid >= g_boundTexMaps.size()) {
        if (s_invalidTidLogCount < 128) {
            RT_LOGF(RT_TAG_GX, "GXLoadTexObj: invalid tex map id %u (oa=0x%08X)\n", tid, oa);
            ++s_invalidTidLogCount;
        }
        return;
    }
    
    // Check if we have metadata for this texture, or try to extract it from guest memory
    TexObjMeta meta;
    if (!TryGetOrExtractTexObjMeta(oa, meta)) {
        if (s_metaLookupLogCount++ < 64) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=no-metadata oa=0x%08X tid=%u\n", oa, tid);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }

    if (meta.dataAddr == 0 || meta.width == 0 || meta.height == 0) {
        if (s_invalidMetaLogCount++ < 64) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=invalid-meta oa=0x%08X tid=%u data=0x%08X %ux%u\n",
                    oa, tid, meta.dataAddr, meta.width, meta.height);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }
    if (!IsReasonableTextureDimensions(meta.width, meta.height)) {
        if (s_invalidDimLogCount++ < 128) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=bad-dimensions oa=0x%08X tid=%u %ux%u fmt=0x%X\n",
                    oa, tid, meta.width, meta.height, meta.format);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }
    // One gate, two reasons: every aurora-loadable format is also a known one,
    // so a separate IsKnownTexFormat rejection could never fire on anything this
    // check would let through. The reason string still distinguishes "GX has no
    // such format" from "aurora cannot decode it", which is what triage needs.
    if (!IsAuroraLoadableTexFormat(meta.format)) {
        const bool known = IsKnownTexFormat(meta.format);
        uint32_t& logCount = known ? g_unsupportedFormatLogCount : s_unknownFormatLogCount;
        if (logCount++ < 128) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=%s oa=0x%08X tid=%u fmt=0x%X %ux%u\n",
                    known ? "unsupported-format" : "unknown-format", oa, tid, meta.format,
                    meta.width, meta.height);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }
    const float maxMipLevel = static_cast<float>(ComputeMaxMipLevel(meta.width, meta.height));
    const float minLodSafe = SanitizeLodValue(meta.minLod, 0.0f);
    float maxLodSafe = SanitizeLodValue(meta.maxLod, minLodSafe);
    maxLodSafe = std::min(maxLodSafe, maxMipLevel);
    const float clampedMinLodSafe = std::min(minLodSafe, maxLodSafe);
    const float lodBiasSafe = SanitizeLodBias(meta.lodBias);
    const uint8_t maxLod = (maxLodSafe > 0.0f) ? ((maxLodSafe > 255.0f) ? 255u : static_cast<uint8_t>(maxLodSafe)) : 0u;
    const uint32_t size = GXGetTexBufferSize(meta.width, meta.height, meta.format, (GXBool)meta.mipmap, maxLod);
    if (size == 0 || !Memory::Contains(meta.dataAddr, size)) {
        if (s_invalidDataLogCount++ < 64) {
            auto* cpu = TryGetCpuContext();
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=data-out-of-range "
                    "oa=0x%08X tid=%u data=0x%08X size=0x%X %ux%u fmt=0x%X mip=%u maxLod=%u pc=0x%08X lr=0x%08X\n",
                    oa, tid, meta.dataAddr, size, meta.width, meta.height, meta.format,
                    meta.mipmap ? 1u : 0u, static_cast<uint32_t>(maxLod),
                    cpu ? cpu->pc : 0u, cpu ? cpu->lr : 0u);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }
    const GXTexWrapMode wrapS = SanitizeWrapMode(meta.wrapS);
    const GXTexWrapMode wrapT = SanitizeWrapMode(meta.wrapT);
    const GXTexFilter minFilter = SanitizeMinFilter(meta.minFilter);
    const GXTexFilter magFilter = SanitizeMagFilter(meta.magFilter);
    const GXAnisotropy maxAnisoSafe = SanitizeAniso(meta.maxAniso);
    meta.wrapS = static_cast<uint32_t>(wrapS);
    meta.wrapT = static_cast<uint32_t>(wrapT);
    meta.minFilter = static_cast<uint32_t>(minFilter);
    meta.magFilter = static_cast<uint32_t>(magFilter);
    meta.maxAniso = static_cast<uint32_t>(maxAnisoSafe);
    meta.minLod = clampedMinLodSafe;
    meta.maxLod = maxLodSafe;
    meta.lodBias = lodBiasSafe;
    
    // Check if host texture object exists, create if needed
    GXTexObj* obj = TryGetHostTexObj(oa);
    const bool isPalette = IsPaletteTexFormat(meta.format);
    bool needsInit = (obj == nullptr);
    const auto metaIt = g_TexObjMeta.find(oa);
    if (!needsInit && metaIt != g_TexObjMeta.end()) {
        const TexObjMeta& cached = metaIt->second;
        if (cached.width != meta.width || cached.height != meta.height || cached.format != meta.format ||
            cached.wrapS != meta.wrapS || cached.wrapT != meta.wrapT || cached.mipmap != meta.mipmap ||
            cached.minFilter != meta.minFilter || cached.magFilter != meta.magFilter ||
            cached.minLod != meta.minLod || cached.maxLod != meta.maxLod ||
            cached.lodBias != meta.lodBias || cached.biasClamp != meta.biasClamp ||
            cached.edgeLod != meta.edgeLod || cached.maxAniso != meta.maxAniso ||
            cached.tlut != meta.tlut || cached.userData != meta.userData) {
            needsInit = true;
        }
    }
    bool tlutChanged = false;
    bool textureDataUploaded = false;
    try {
        if (needsInit) {
            if (!obj) {
                obj = CreateHostTexObj(oa);
            }
            void* dp = GuestToHostPtr(meta.dataAddr, size);
            if (dp) {
                if (isPalette) {
                    GXInitTexObjCI(obj, dp, meta.width, meta.height, (GXCITexFmt)meta.format,
                                   wrapS, wrapT,
                                   meta.mipmap ? GX_TRUE : GX_FALSE, meta.tlut);
                } else {
                    GXInitTexObj(obj, dp, meta.width, meta.height, (GXTexFmt)meta.format,
                                 wrapS, wrapT,
                                 meta.mipmap ? GX_TRUE : GX_FALSE);
                }
                ApplyHostTexObjLod(obj, meta, minFilter, magFilter, meta.lodBias, maxAnisoSafe);
                GXInitTexObjUserData(obj, GuestToHostPtr(meta.userData));
            }
            MarkHostTexObjConstructed(oa);
            // Write through GetTexObjMeta so the DCStoreRange interval index is
            // told this entry's backing may have moved.
            GetTexObjMeta(oa) = meta;
        } else if (isPalette && metaIt != g_TexObjMeta.end() && metaIt->second.tlut != meta.tlut) {
            GXInitTexObjTlut(obj, meta.tlut);
            GetTexObjMeta(oa).tlut = meta.tlut;
            tlutChanged = true;
        }

        const void* dp = GuestToHostPtr(meta.dataAddr, size);
        if (dp && meta.needsUpload) {
            GXInitTexObjData(obj, dp);
            meta.needsUpload = false;
            textureDataUploaded = true;
        }
        // Everything the binding contract compares apart from objAddr is the
        // shared sampler state, so copy that base wholesale; only dataAddr needs
        // to be canonicalized on the way in.
        BoundTexInfo newBound{};
        static_cast<GxTextureBindingContract::SamplerState&>(newBound) = meta;
        newBound.objAddr = oa;
        newBound.dataAddr = CanonicalizeGxMainRamAddress(meta.dataAddr);
        const BoundTexInfo& oldBound = g_boundTexMaps[tid];
        const bool canSkipHostLoad = GxTextureBindingContract::CanSkipHostLoad(
            oldBound, newBound, needsInit, tlutChanged, textureDataUploaded);

        g_boundTexMaps[tid] = newBound;
        GetTexObjMeta(oa) = meta;
        if (!canSkipHostLoad) {
            GXLoadTexObj(obj, (GXTexMapID)tid);
        }
    } catch (const std::exception& ex) {
        if (s_hostExceptionLogCount++ < 64) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=host-exception (%s) oa=0x%08X tid=%u fmt=0x%X %ux%u data=0x%08X\n",
                    ex.what(), oa, tid, meta.format, meta.width, meta.height, meta.dataAddr);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    } catch (...) {
        if (s_hostExceptionLogCount++ < 64) {
            RT_LOGF(RT_TAG_GX,
                    "GXLoadTexObj rejected reason=host-exception-unknown oa=0x%08X tid=%u fmt=0x%X %ux%u data=0x%08X\n",
                    oa, tid, meta.format, meta.width, meta.height, meta.dataAddr);
        }
        BindUnloadableTexturePlaceholder(tid);
        return;
    }
}

extern "C" void GX__LoadTexObj_80170f2c(uint32_t oa, uint32_t tid) {
    if (GxWorker::ShouldDefer()) {
        // The CPU-side SDK reads the GXTexObj at call time; the texture data itself is read later.
        std::optional<TexObjBytes> bytes;
        try {
            bytes = CaptureTexObjBytes(oa);
        } catch (...) {
        }
        GxWorker::Submit([oa, tid, bytes] {
            if (bytes) {
                TexObjSnapshotScope snapshot(oa, *bytes);
                LoadTexObjImpl(oa, tid);
            } else {
                LoadTexObjImpl(oa, tid);
            }
        });
    } else {
        LoadTexObjImpl(oa, tid);
    }
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) { Memory::Write32(gd + 0x5FCu, Memory::Read32(gd + 0x5FCu) | 1u); Memory::Write16(gd + 2, 0); } } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80170f2c, GX__LoadTexObj_80170f2c, (uint32_t oa, uint32_t tid), (oa, tid));

extern "C" void GX__LoadTexObjPreLoaded_80170dc8(uint32_t oa, uint32_t region, uint32_t tid) {
    (void)region;
    GX__LoadTexObj_80170f2c(oa, tid);
}
PPC_NATIVE_OVERRIDE_VOID(80170dc8, GX__LoadTexObjPreLoaded_80170dc8, (uint32_t oa, uint32_t region, uint32_t tid), (oa, region, tid));

// ============================================================================
// Texture Object Getters
// ============================================================================

extern "C" void GX__GetTexObjAll_80170bf8(uint32_t oa, uint32_t dp, uint32_t wp, uint32_t hp, uint32_t fp, uint32_t wsp, uint32_t wtp, uint32_t mp) {
    GX_SYNC_INLINE();
    std::lock_guard<std::mutex> guard(g_texObjMutex);
    TexObjMeta meta;
    if (!TryGetOrExtractTexObjMeta(oa, meta)) {
        RT_LOGF(RT_TAG_GX, "GXGetTexObjAll: failed to get metadata for GXTexObj @0x%08X\n", oa);
        // Return zeros on failure
        if (dp) Memory::Write32(dp, 0); if (wp) Memory::Write16(wp, 0); if (hp) Memory::Write16(hp, 0); 
        if (fp) Memory::Write32(fp, 0); if (wsp) Memory::Write32(wsp, 0); if (wtp) Memory::Write32(wtp, 0); if (mp) Memory::Write8(mp, 0);
        return;
    }
    if (dp) Memory::Write32(dp, meta.dataAddr); if (wp) Memory::Write16(wp, meta.width); if (hp) Memory::Write16(hp, meta.height); if (fp) Memory::Write32(fp, meta.format); if (wsp) Memory::Write32(wsp, meta.wrapS); if (wtp) Memory::Write32(wtp, meta.wrapT); if (mp) Memory::Write8(mp, meta.mipmap ? 1 : 0);
}
PPC_NATIVE_OVERRIDE_VOID(80170bf8, GX__GetTexObjAll_80170bf8, (uint32_t oa, uint32_t dp, uint32_t wp, uint32_t hp, uint32_t fp, uint32_t wsp, uint32_t wtp, uint32_t mp), (oa, dp, wp, hp, fp, wsp, wtp, mp));

extern "C" void GX__GetTexObjLODAll_80170cbc(uint32_t oa, uint32_t mifp, uint32_t mafp, uint32_t milp, uint32_t malp, uint32_t lbp, uint32_t bcp, uint32_t elp, uint32_t map) {
    GX_SYNC_INLINE();
    std::lock_guard<std::mutex> guard(g_texObjMutex);
    TexObjMeta meta;
    if (!TryGetOrExtractTexObjMeta(oa, meta)) {
        RT_LOGF(RT_TAG_GX, "GXGetTexObjLODAll: failed to get metadata for GXTexObj @0x%08X\n", oa);
        // Return zeros on failure
        if (mifp) Memory::Write32(mifp, 0); if (mafp) Memory::Write32(mafp, 0); if (milp) Memory::WriteFloat32(milp, 0.0f);
        if (malp) Memory::WriteFloat32(malp, 0.0f); if (lbp) Memory::WriteFloat32(lbp, 0.0f); if (bcp) Memory::Write8(bcp, 0);
        if (elp) Memory::Write8(elp, 0); if (map) Memory::Write32(map, 0);
        return;
    }
    if (mifp) Memory::Write32(mifp, meta.minFilter); if (mafp) Memory::Write32(mafp, meta.magFilter); if (milp) Memory::WriteFloat32(milp, meta.minLod); if (malp) Memory::WriteFloat32(malp, meta.maxLod); if (lbp) Memory::WriteFloat32(lbp, meta.lodBias); if (bcp) Memory::Write8(bcp, meta.biasClamp ? 1 : 0); if (elp) Memory::Write8(elp, meta.edgeLod ? 1 : 0); if (map) Memory::Write32(map, meta.maxAniso);
}
PPC_NATIVE_OVERRIDE_VOID(80170cbc, GX__GetTexObjLODAll_80170cbc, (uint32_t oa, uint32_t mifp, uint32_t mafp, uint32_t milp, uint32_t malp, uint32_t lbp, uint32_t bcp, uint32_t elp, uint32_t map), (oa, mifp, mafp, milp, malp, lbp, bcp, elp, map));

// ============================================================================
// TLUT (Texture Lookup Table)
// ============================================================================

extern "C" void GX__InitTlutObj_80170f80(uint32_t oa, uint32_t da, uint32_t f, uint32_t e) {
    GX_DEFER_CALL(GX__InitTlutObj_80170f80, oa, da, f, e);
    std::lock_guard<std::mutex> guard(g_tlutObjMutex); GXTlutObj* obj = CreateHostTlutObj(oa); TlutObjMeta& meta = GetTlutObjMeta(oa);
    meta.dataAddr=CanonicalizeGxMainRamAddress(da); meta.format=f; meta.entries=(u16)e; meta.dirty=false;
    // Leave the object unconstructed on a bad descriptor. A later GXLoadTlut
    // then takes GetHostTlutObj's soft-fail path and gets a fresh empty object
    // instead of aurora reading entries*2 bytes off an unvalidated pointer.
    if (!ValidateTlutData(oa, meta)) return;
    GXInitTlutObj(obj, GuestToHostPtr(da), (GXTlutFmt)f, (u16)e); MarkHostTlutObjConstructed(oa);
}
PPC_NATIVE_OVERRIDE_VOID(80170f80, GX__InitTlutObj_80170f80, (uint32_t oa, uint32_t da, uint32_t f, uint32_t e), (oa, da, f, e));

static void LoadTlutImpl(uint32_t oa, uint32_t tl) { std::lock_guard<std::mutex> guard(g_tlutObjMutex); if (tl >= kMaxTluts) { RT_LOGF(RT_TAG_GX, "GXLoadTlut: invalid TLUT index %u (oa=0x%08X)\n", tl, oa); return; } auto metaIt = g_TlutObjMeta.find(oa); if (metaIt != g_TlutObjMeta.end() && metaIt->second.dirty) { if (ValidateTlutData(oa, metaIt->second)) { GXTlutObj* rebuild = CreateHostTlutObj(oa); GXInitTlutObj(rebuild, GuestToHostPtr(metaIt->second.dataAddr), (GXTlutFmt)metaIt->second.format, metaIt->second.entries); MarkHostTlutObjConstructed(oa); } metaIt->second.dirty = false; } GXTlutObj* obj = GetHostTlutObj(oa); GXLoadTlut(obj, (GXTlut)tl); }

extern "C" void GX__LoadTlut_80170fa8(uint32_t oa, uint32_t tl) {
    GxWorker::Run([oa, tl] { LoadTlutImpl(oa, tl); });
    try { uint32_t gd = Memory::Read32(kGXDataPtrAddr); if (gd) Memory::Write16(gd + 2, 0); } catch (...) {}
}
PPC_NATIVE_OVERRIDE_VOID(80170fa8, GX__LoadTlut_80170fa8, (uint32_t oa, uint32_t tl), (oa, tl));

// ============================================================================
// Texture Invalidation and Coordinate Control
// ============================================================================

extern "C" void GX__InvalidateTexAll_80171110() {
    GX_DEFER_CALL0(GX__InvalidateTexAll_80171110);
    // Real GX invalidates its internal texture cache here. Aurora forwards the
    // invalidate to the renderer, which keeps unchanged source uploads hot but
    // revalidates reused guest buffers before serving cached texture handles.
    GXInvalidateTexAll();
}
PPC_NATIVE_OVERRIDE_VOID(80171110, GX__InvalidateTexAll_80171110, (), ());

extern "C" void GX__SetTexCoordScaleManually_80171180(uint32_t c, uint32_t en, uint32_t ss, uint32_t ts) { GxWorker::Run([c, en, ss, ts] { GXSetTexCoordScaleManually((GXTexCoordID)c, (GXBool)en, (u16)ss, (u16)ts); }); try{ uint32_t gd=Memory::Read32(kGXDataPtrAddr); if(gd){ Memory::Write32(gd+0x5E4u, (Memory::Read32(gd+0x5E4u)&~(1u<<c))|((en&1u)<<c)); if(en){ uint32_t sa=gd+0x108u+c*4u, ta=gd+0x128u+c*4u; Memory::Write32(sa, (Memory::Read32(sa)&0xFFFF0000u)|((ss-1)&0xFFFFu)); Memory::Write32(ta, (Memory::Read32(ta)&0xFFFF0000u)|((ts-1)&0xFFFFu)); Memory::Write16(gd+2, 0); } } }catch(...){} }
PPC_NATIVE_OVERRIDE_VOID(80171180, GX__SetTexCoordScaleManually_80171180, (uint32_t c, uint32_t en, uint32_t ss, uint32_t ts), (c, en, ss, ts));

extern "C" void GX__SetTexCoordBias_801711fc(uint32_t c, uint32_t se, uint32_t te) { GxWorker::Run([c, se, te] { GXSetTexCoordBias((GXTexCoordID)c, (GXBool)se, (GXBool)te); }); try{ uint32_t gd=Memory::Read32(kGXDataPtrAddr); if(gd){ uint32_t sa=gd+0x108u+c*4u, ta=gd+0x128u+c*4u; Memory::Write32(sa, (Memory::Read32(sa)&0xFFFEFFFFu)|((se&1u)<<16)); Memory::Write32(ta, (Memory::Read32(ta)&0xFFFEFFFFu)|((te&1u)<<16)); if(Memory::Read32(gd+0x5E4u)&(1u<<c)) Memory::Write16(gd+2, 0); } }catch(...){} }
PPC_NATIVE_OVERRIDE_VOID(801711fc, GX__SetTexCoordBias_801711fc, (uint32_t c, uint32_t se, uint32_t te), (c, se, te));
