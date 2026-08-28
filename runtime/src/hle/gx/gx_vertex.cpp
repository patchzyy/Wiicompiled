// gx_vertex.cpp - Vertex Descriptor and Attribute Functions
#include "gx_internal.h"
#include "gx_stream_common.h"
#include "fiber_manager.h"
#include "guest_interrupt_context.h"
#include "runtime_log.h"

#include <cstdio>
#include <cstdlib>

namespace {
uint32_t CanonicalVtxAttr(uint32_t attr) {
    return attr == GX_VA_NBT ? GX_VA_NRM : attr;
}

// There is deliberately no indexed-aurora path here. The immediate begin path
// streams vertex data incrementally, and aurora's indexed-array upload must
// precede the draw carrying the max-index bounds, which cannot be known without
// buffering the whole primitive. Indexed attributes are therefore expanded into
// the packed direct stream below (GX_INDEX8/16 -> GX_DIRECT).
void ApplyAuroraVtxStateForBegin(GXVtxFmt fmt) {
    GxStream::PublishAuroraVtxState(fmt,
                                    GxStream::AuroraVtxPublishOptions{
                                        /*includeNbt=*/false,
                                        /*fmtLoopFirst=*/static_cast<int>(GX_VA_POS),
                                        /*fmtLoopLast=*/static_cast<int>(GX_VA_TEX7)});
}
}

// ============================================================================
// Vertex Descriptor
// ============================================================================

extern "C" void GX__ClearVtxDesc_8016dc34() {
    bool changed = false;
    for(int i=0; i<26; ++i){
        changed |= g_hleGxState.vtxDesc[i] != GX_NONE;
        g_hleGxState.vtxDesc[i]=GX_NONE;
    }
    if (changed) g_hleGxState.InvalidateVtxLayoutHash();
    // GXClearVtxDesc resets descriptors only; array base/stride state persists.
    GXClearVtxDesc();
}
PPC_NATIVE_OVERRIDE_VOID(8016dc34, GX__ClearVtxDesc_8016dc34, (), ());

extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr>=26||attr==GX_VA_NULL) return;
    const GXAttrType oldType = g_hleGxState.vtxDesc[attr];
    g_hleGxState.vtxDesc[attr]=(GXAttrType)t;
    if (a == GX_VA_NBT) {
        g_hleGxState.vtxDesc[GX_VA_NBT] = GX_NONE;
    }
    if (g_hleGxState.vtxDesc[attr] != oldType) g_hleGxState.InvalidateVtxLayoutHash();
    if(IsMatrixIndexAttr((GXAttr)attr)) return;
    GXSetVtxDesc((GXAttr)a, (t==GX_INDEX8||t==GX_INDEX16)?GX_DIRECT:(GXAttrType)t);
}
PPC_NATIVE_OVERRIDE_VOID(8016d3a4, GX__SetVtxDesc_8016d3a4, (uint32_t a, uint32_t t), (a, t));

extern "C" void GX__SetVtxDescv_8016d608(uint32_t la) {
    if(!la) return; uint32_t p=la;
    // Real tables terminate with the 0xFF sentinel well within GX_VA_MAX_ATTR (26) entries; cap
    // the walk generously above that and use TryRead32 so a missing/corrupted terminator on a
    // guest table can't run off the end of mapped memory (crash) or loop indefinitely.
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t a=0, t=0;
        if(!Memory::TryRead32(p, a) || a==0xFFu) break;
        if(!Memory::TryRead32(p+4, t)) break;
        GX__SetVtxDesc_8016d3a4(a, t);
        p+=8;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016d608, GX__SetVtxDescv_8016d608, (uint32_t la), (la));

extern "C" void GX__GetVtxDesc_8016d9f0(uint32_t a, uint32_t tp) {
    GXAttrType type = GX_NONE;
    const uint32_t attr = CanonicalVtxAttr(a);
    if (attr < GX_VA_MAX_ATTR && attr != GX_VA_NULL) {
        type = g_hleGxState.vtxDesc[attr];
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(type));
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016d9f0, GX__GetVtxDesc_8016d9f0, (uint32_t a, uint32_t tp), (a, tp));

extern "C" void GX__GetVtxDescv_8016dba4(uint32_t la) {
    if (!la) {
        return;
    }

    uint32_t p = la;
    for (uint32_t a = GX_VA_PNMTXIDX; a <= GX_VA_TEX7; ++a) {
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(g_hleGxState.vtxDesc[a]));
        p += 8;
    }
    Memory::Write32(p, GX_VA_NBT);
    Memory::Write32(p + 4, static_cast<uint32_t>(g_hleGxState.vtxDesc[GX_VA_NRM]));
    p += 8;
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8016dba4, GX__GetVtxDescv_8016dba4, (uint32_t la), (la));

// ============================================================================
// Vertex Attribute Format
// ============================================================================

extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(vf<8&&attr<26){
        const VtxAttrFmt oldFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        g_hleGxState.vtxAttrFmt[vf][attr].cnt=(GXCompCnt)c;
        g_hleGxState.vtxAttrFmt[vf][attr].type=(GXCompType)t;
        g_hleGxState.vtxAttrFmt[vf][attr].frac=(u8)fr;
        if (a == GX_VA_NBT) {
            g_hleGxState.vtxAttrFmt[vf][GX_VA_NBT] = {};
        }
        const auto& newFmt = g_hleGxState.vtxAttrFmt[vf][attr];
        if (oldFmt.cnt != newFmt.cnt || oldFmt.type != newFmt.type || oldFmt.frac != newFmt.frac) {
            g_hleGxState.InvalidateVtxLayoutHash();
        }
    }
    if (vf >= GX_MAX_VTXFMT || a < GX_VA_POS || a >= GX_VA_MAX_ATTR) {
        return;
    }
    GXSetVtxAttrFmt((GXVtxFmt)vf, (GXAttr)a, (GXCompCnt)c, (GXCompType)t, (u8)fr);
}
PPC_NATIVE_OVERRIDE_VOID(8016dc68, GX__SetVtxAttrFmt_8016dc68, (uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr), (vf, a, c, t, fr));

extern "C" void GX__SetVtxAttrFmtv_8016de08(uint32_t vf, uint32_t la) {
    if(!la) return; uint32_t p=la;
    // See GX__SetVtxDescv_8016d608 above: bounded, fault-tolerant walk instead of an unguarded
    // while(true) driven purely by a guest-supplied terminator.
    for (int guard = 0; guard < 64; ++guard) {
        uint32_t a=0, c=0, t=0, fr=0;
        if(!Memory::TryRead32(p, a) || a==0xFFu) break;
        if(!Memory::TryRead32(p+4, c) || !Memory::TryRead32(p+8, t) || !Memory::TryRead32(p+12, fr)) break;
        GX__SetVtxAttrFmt_8016dc68(vf, a, c, t, fr&0xFFu);
        p+=16;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016de08, GX__SetVtxAttrFmtv_8016de08, (uint32_t vf, uint32_t la), (vf, la));

extern "C" void GX__GetVtxAttrFmt_8016e04c(uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp) {
    VtxAttrFmt fmt{};
    const uint32_t attr = CanonicalVtxAttr(a);
    if (vf < 8 && attr < GX_VA_MAX_ATTR && attr != GX_VA_NULL) {
        fmt = g_hleGxState.vtxAttrFmt[vf][attr];
    }
    if (cp) {
        Memory::Write32(cp, static_cast<uint32_t>(fmt.cnt));
    }
    if (tp) {
        Memory::Write32(tp, static_cast<uint32_t>(fmt.type));
    }
    if (fp) {
        Memory::Write8(fp, fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(8016e04c, GX__GetVtxAttrFmt_8016e04c, (uint32_t vf, uint32_t a, uint32_t cp, uint32_t tp, uint32_t fp), (vf, a, cp, tp, fp));

extern "C" void GX__GetVtxAttrFmtv_8016e2b8(uint32_t vf, uint32_t la) {
    if (!la) {
        return;
    }

    uint32_t p = la;
    for (uint32_t a = GX_VA_POS; a <= GX_VA_TEX7; ++a) {
        VtxAttrFmt fmt{};
        if (vf < 8) {
            fmt = g_hleGxState.vtxAttrFmt[vf][a];
        }
        Memory::Write32(p, a);
        Memory::Write32(p + 4, static_cast<uint32_t>(fmt.cnt));
        Memory::Write32(p + 8, static_cast<uint32_t>(fmt.type));
        Memory::Write8(p + 12, fmt.frac);
        p += 16;
    }
    Memory::Write32(p, GX_VA_NULL);
}
PPC_NATIVE_OVERRIDE_VOID(8016e2b8, GX__GetVtxAttrFmtv_8016e2b8, (uint32_t vf, uint32_t la), (vf, la));

// ============================================================================
// Vertex Arrays
// ============================================================================

extern "C" void GX__SetArray_8016e32c(uint32_t a, uint32_t ba, uint32_t str) {
    const uint32_t attr = CanonicalVtxAttr(a);
    if(attr<26){ g_hleGxState.vtxArray[attr].base=ba; g_hleGxState.vtxArray[attr].stride=str; }
}
PPC_NATIVE_OVERRIDE_VOID(8016e32c, GX__SetArray_8016e32c, (uint32_t a, uint32_t ba, uint32_t str), (a, ba, str));

// Switch-artifact entry point for the same SDK function; forwards rather than
// repeating the body.
extern "C" void GX__SetArray_8016e1c4(uint32_t a, uint32_t b, uint32_t s) { GX__SetArray_8016e32c(a, b, s); }
PPC_NATIVE_OVERRIDE_VOID(8016e1c4, GX__SetArray_8016e1c4, (uint32_t a, uint32_t b, uint32_t s), (a, b, s));

// ============================================================================
// Texture Coordinate Generation
// ============================================================================

extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t n) {
    GXSetNumTexGens((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(8016e5a4, GX__SetNumTexGens_8016e5a4, (uint32_t n), (n));

extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm) {
    if (sp >= static_cast<uint32_t>(GX_MAX_TEXGENSRC)) {
        uint32_t pc = 0;
        uint32_t lr = 0;
        if (auto* cpu = TryGetCpuContext()) {
            pc = cpu->pc;
            lr = cpu->lr;
        }
        RT_LOGF(RT_TAG_GX,
                "GXSetTexCoordGen2 invalid src=%u dst=%u type=%u mtx=%u norm=%u post=%u PC=0x%08X LR=0x%08X\n",
                sp, dc, f, m, n, pm, pc, lr);
    }
    GXSetTexCoordGen2((GXTexCoordID)dc, (GXTexGenType)f, (GXTexGenSrc)sp, m, (GXBool)n, pm);
}
PPC_NATIVE_OVERRIDE_VOID(8016e37c, GX__SetTexCoordGen2_8016e37c, (uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm), (dc, f, sp, m, n, pm));

extern "C" void GX__EnableTexOffsets_8016f37c(uint32_t coord, uint32_t lineEnable, uint32_t pointEnable) {
    GXEnableTexOffsets(static_cast<GXTexCoordID>(coord),
                       lineEnable ? GX_TRUE : GX_FALSE,
                       pointEnable ? GX_TRUE : GX_FALSE);
}
PPC_NATIVE_OVERRIDE_VOID(8016f37c, GX__EnableTexOffsets_8016f37c, (uint32_t coord, uint32_t lineEnable, uint32_t pointEnable), (coord, lineEnable, pointEnable));

extern "C" void GX__SetLineWidth_8016f314(uint32_t width, uint32_t texOffsets) {
    GXSetLineWidth(static_cast<u8>(width), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8016f314, GX__SetLineWidth_8016f314, (uint32_t width, uint32_t texOffsets), (width, texOffsets));

extern "C" void GX__SetPointSize_8016f348(uint32_t pointSize, uint32_t texOffsets) {
    GXSetPointSize(static_cast<u8>(pointSize), static_cast<GXTexOffset>(texOffsets));
}
PPC_NATIVE_OVERRIDE_VOID(8016f348, GX__SetPointSize_8016f348, (uint32_t pointSize, uint32_t texOffsets), (pointSize, texOffsets));

// ============================================================================
// Begin/End Drawing
// ============================================================================

namespace {
thread_local bool g_inGxBeginOrFifo = false;
thread_local std::chrono::steady_clock::time_point g_nextDeferredTimingPoll{};

void ServiceDeferredTimingDuringGxWork() {
    if (g_inGxBeginOrFifo) {
        return;
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < g_nextDeferredTimingPoll) {
        return;
    }
    g_nextDeferredTimingPoll = now + std::chrono::milliseconds(1);

    g_inGxBeginOrFifo = true;
    try {
        if (Fiber::GuestFiberManager::IsInitialized()) {
            // VI callbacks are what advance EGG::AsyncDisplay on NTSC/PAL60;
            // PAL50 uses an OS alarm. Keep both clocks moving while a slow
            // host frame is still being submitted, but defer fiber switches
            // and Aurora work until the native GX call has unwound.
            VI_HLE_ProcessRetracesDeferred(8);
            OS_HLE_ProcessAlarmsDeferred(8);
        } else if (TryGetCpuContext() != nullptr) {
            // Same interrupt isolation as the deferred paths above: this GX call
            // is mid-execution of a translated function, so the retrace
            // callbacks must not publish their registers into its context.
            GuestInterruptCallbackContext interrupt;
            VI_HLE_PollRetrace(interrupt.get());
        }
    } catch (...) {
        g_inGxBeginOrFifo = false;
        throw;
    }
    g_inGxBeginOrFifo = false;
}
}

extern "C" void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv) {
    if(IsDisplayListActive()){
        WriteDisplayListData((u8)(t|vf), 1);
        WriteDisplayListData((u16)nv, 2);
        return;
    }
    ServiceDeferredTimingDuringGxWork();
    ApplyAuroraVtxStateForBegin(static_cast<GXVtxFmt>(vf));
    g_hleGxState.currentVtxFmt=(GXVtxFmt)vf; g_hleGxState.currentPrim=(GXPrimitive)t;
    g_hleGxState.vertsRemaining=nv; g_hleGxState.inBegin=true; g_hleGxState.auroraBeginCalled=false;
    g_hleGxState.fifoReadOffset = 0;
    g_hleGxState.fifoByteCount=0; g_hleGxState.ResetVertex();
}
PPC_NATIVE_OVERRIDE_VOID(8016f0f0, GX__Begin_8016f0f0, (uint32_t t, uint32_t vf, uint32_t nv), (t, vf, nv));

extern "C" void GX__End_80044b30() { g_hleGxState.inBegin=false; GXEnd(); }
PPC_NATIVE_OVERRIDE_VOID(80044b30, GX__End_80044b30, (), ());

extern "C" void GX__End_80048c30() { GX__End_80044b30(); }
PPC_NATIVE_OVERRIDE_VOID(80048c30, GX__End_80048c30, (), ());

extern "C" void GX__DrawSphere_80172a30(uint32_t numMajor, uint32_t numMinor) {
    constexpr uint32_t kAttrCount = 26;
    constexpr float kSphereRadius = 1.0f;
    constexpr float kPi = 3.14159265358979323846f;
    constexpr float kTwoPi = kPi * 2.0f;

    std::array<GXAttrType, kAttrCount> savedVtxDesc{};
    std::array<VtxAttrFmt, kAttrCount> savedFmt3{};
    for (uint32_t i = 0; i < kAttrCount; ++i) {
        savedVtxDesc[i] = g_hleGxState.vtxDesc[i];
        savedFmt3[i] = g_hleGxState.vtxAttrFmt[GX_VTXFMT3][i];
    }

    const bool hadTex0 = savedVtxDesc[GX_VA_TEX0] != GX_NONE;

    bool drawOpen = false;
    try {
        EnsureAuroraFrameActive();

        for (uint32_t i = 0; i < kAttrCount; ++i) {
            g_hleGxState.vtxDesc[i] = GX_NONE;
        }
        g_hleGxState.InvalidateVtxLayoutHash();
        GXClearVtxDesc();

        GX__SetVtxDesc_8016d3a4(GX_VA_POS, GX_DIRECT);
        GX__SetVtxDesc_8016d3a4(GX_VA_NRM, GX_DIRECT);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        if (hadTex0) {
            GX__SetVtxDesc_8016d3a4(GX_VA_TEX0, GX_DIRECT);
            GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        }

        const float majorStep = kPi / static_cast<float>(numMajor);
        const float minorStep = kTwoPi / static_cast<float>(numMinor);
        const float invRadius = 1.0f / kSphereRadius;

        for (uint32_t major = 0; major < numMajor; ++major) {
            const float major0 = static_cast<float>(major) * majorStep;
            const float major1 = major0 + majorStep;

            const float ring0 = kSphereRadius * std::sin(major0);
            const float ring1 = kSphereRadius * std::sin(major1);
            const float z0 = kSphereRadius * std::cos(major0);
            const float z1 = kSphereRadius * std::cos(major1);

            const uint16_t vertexCount = static_cast<uint16_t>(((numMinor + 1u) * 2u) & 0xFFFEu);
            GXBegin(GX_TRIANGLESTRIP, GX_VTXFMT3, vertexCount);
            drawOpen = true;

            const float nz0 = z0 * invRadius;
            const float nz1 = z1 * invRadius;

            for (uint32_t minor = 0; minor <= numMinor; ++minor) {
                const float minorAngle = static_cast<float>(minor) * minorStep;
                const float c = std::cos(minorAngle);
                const float s = std::sin(minorAngle);

                const float x1 = c * ring1;
                const float y1 = s * ring1;
                GXPosition3f32(x1, y1, z1);
                GXNormal3f32(x1 * invRadius, y1 * invRadius, nz1);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major + 1u) / static_cast<float>(numMajor));
                }

                const float x0 = c * ring0;
                const float y0 = s * ring0;
                GXPosition3f32(x0, y0, z0);
                GXNormal3f32(x0 * invRadius, y0 * invRadius, nz0);
                if (hadTex0) {
                    GXTexCoord2f32(static_cast<float>(minor) / static_cast<float>(numMinor),
                                   static_cast<float>(major) / static_cast<float>(numMajor));
                }
            }
            GXEnd();
            drawOpen = false;
        }
    } catch (...) {
        if (drawOpen) {
            GXEnd();
            drawOpen = false;
        }
    }

    for (uint32_t i = 0; i < kAttrCount; ++i) {
        g_hleGxState.vtxDesc[i] = GX_NONE;
    }
    g_hleGxState.InvalidateVtxLayoutHash();
    GXClearVtxDesc();
    for (uint32_t attr = 0; attr < kAttrCount; ++attr) {
        const GXAttrType type = savedVtxDesc[attr];
        if (type != GX_NONE) {
            GX__SetVtxDesc_8016d3a4(attr, static_cast<uint32_t>(type));
        }
    }
    for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
        const VtxAttrFmt& fmt = savedFmt3[attr];
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT3, attr, static_cast<uint32_t>(fmt.cnt), static_cast<uint32_t>(fmt.type), fmt.frac);
    }
}
PPC_NATIVE_OVERRIDE_VOID(80172a30, GX__DrawSphere_80172a30, (uint32_t numMajor, uint32_t numMinor), (numMajor, numMinor));
