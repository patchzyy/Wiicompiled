// gx_tev.cpp - TEV Stage Configuration
#include "gx_internal.h"
#include "runtime_log.h"

// Aurora's CHECK() on these tev array indices compiles to nothing under NDEBUG, so this
// guest->native boundary must reject out-of-range IDs itself; a bad ID here is a malformed
// display list, never legitimate traffic.
namespace {

bool GxTevIdOk(uint32_t value, uint32_t limit, const char* what) {
    if (value < limit) return true;
    RT_LOGF(RT_TAG_GX, "%s out of range: %u (max %u), ignoring\n", what, value, limit - 1u);
    return false;
}

inline bool TevStageOk(uint32_t s) { return GxTevIdOk(s, GX_MAX_TEVSTAGE, "TEV stage"); }
inline bool TevRegOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVREG, "TEV register"); }
inline bool TevKColorOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_KCOLOR, "TEV konstant color"); }
inline bool TevSwapOk(uint32_t id) { return GxTevIdOk(id, GX_MAX_TEVSWAP, "TEV swap selector"); }

} // namespace

// ============================================================================
// TEV Stage Count and Order
// ============================================================================

extern "C" void GX__SetNumTevStages_801722a8(uint32_t n) {
    GX_DEFER_CALL(GX__SetNumTevStages_801722a8, n);
    // GXSetNumTevStages takes a count, not an index, so the inclusive bound is
    // GX_MAX_TEVSTAGE itself.
    if (n > GX_MAX_TEVSTAGE) {
        RT_LOGF(RT_TAG_GX, "GXSetNumTevStages: invalid count %u, ignoring\n", n);
        return;
    }
    GXSetNumTevStages((u8)n);
}
PPC_NATIVE_OVERRIDE_VOID(801722a8, GX__SetNumTevStages_801722a8, (uint32_t n), (n));

extern "C" void GX__SetTevOp_80171c4c(uint32_t s, uint32_t m) {
    GX_DEFER_CALL(GX__SetTevOp_80171c4c, s, m);
    if (!TevStageOk(s)) return;
    GXSetTevOp((GXTevStageID)s, (GXTevMode)m);
}
PPC_NATIVE_OVERRIDE_VOID(80171c4c, GX__SetTevOp_80171c4c, (uint32_t s, uint32_t m), (s, m));

extern "C" void GX__SetTevOrder_8017214c(uint32_t s, uint32_t c, uint32_t m, uint32_t col) {
    GX_DEFER_CALL(GX__SetTevOrder_8017214c, s, c, m, col);
    if (!TevStageOk(s)) return;
    GXSetTevOrder((GXTevStageID)s, (GXTexCoordID)c, (GXTexMapID)m, (GXChannelID)col);
}
PPC_NATIVE_OVERRIDE_VOID(8017214c, GX__SetTevOrder_8017214c, (uint32_t s, uint32_t c, uint32_t m, uint32_t col), (s, c, m, col));

// ============================================================================
// TEV Color/Alpha Inputs
// ============================================================================

extern "C" void GX__SetTevColorIn_80171ce0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    GX_DEFER_CALL(GX__SetTevColorIn_80171ce0, s, a, b, c, d);
    if (!TevStageOk(s)) return;
    GXSetTevColorIn((GXTevStageID)s, (GXTevColorArg)a, (GXTevColorArg)b, (GXTevColorArg)c, (GXTevColorArg)d);
}
PPC_NATIVE_OVERRIDE_VOID(80171ce0, GX__SetTevColorIn_80171ce0, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

extern "C" void GX__SetTevAlphaIn_80171d20(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    GX_DEFER_CALL(GX__SetTevAlphaIn_80171d20, s, a, b, c, d);
    if (!TevStageOk(s)) return;
    GXSetTevAlphaIn((GXTevStageID)s, (GXTevAlphaArg)a, (GXTevAlphaArg)b, (GXTevAlphaArg)c, (GXTevAlphaArg)d);
}
PPC_NATIVE_OVERRIDE_VOID(80171d20, GX__SetTevAlphaIn_80171d20, (uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d), (s, a, b, c, d));

// ============================================================================
// TEV Color/Alpha Operations
// ============================================================================

extern "C" void GX__SetTevColorOp_80171d60(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    GX_DEFER_CALL(GX__SetTevColorOp_80171d60, s, op, b, sc, cl, or_);
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevColorOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
PPC_NATIVE_OVERRIDE_VOID(80171d60, GX__SetTevColorOp_80171d60, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

extern "C" void GX__SetTevAlphaOp_80171db8(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_) {
    GX_DEFER_CALL(GX__SetTevAlphaOp_80171db8, s, op, b, sc, cl, or_);
    if (!TevStageOk(s) || !TevRegOk(or_)) return;
    GXSetTevAlphaOp((GXTevStageID)s, (GXTevOp)op, (GXTevBias)b, (GXTevScale)sc, (GXBool)cl, (GXTevRegID)or_);
}
PPC_NATIVE_OVERRIDE_VOID(80171db8, GX__SetTevAlphaOp_80171db8, (uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_), (s, op, b, sc, cl, or_));

// ============================================================================
// TEV Color Registers
// ============================================================================

extern "C" void GX__SetTevColor_80171e10(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 4); GXColor c; c.r=p[0]; c.g=p[1]; c.b=p[2]; c.a=p[3];
    GxWorker::Run([id, c] { GXSetTevColor((GXTevRegID)id, c); });
}
PPC_NATIVE_OVERRIDE_VOID(80171e10, GX__SetTevColor_80171e10, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t cp) {
    if (!TevRegOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 8); GXColorS10 c;
    c.r=(p[0]<<8)|p[1]; c.g=(p[2]<<8)|p[3]; c.b=(p[4]<<8)|p[5]; c.a=(p[6]<<8)|p[7];
    GxWorker::Run([id, c] { GXSetTevColorS10((GXTevRegID)id, c); });
}
PPC_NATIVE_OVERRIDE_VOID(80171e70, GX__SetTevColorS10_80171e70, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t cp) {
    if (!TevKColorOk(id)) return;
    const uint8_t* p=Memory::GetPointer(cp, 4); GXColor c; c.r=p[0]; c.g=p[1]; c.b=p[2]; c.a=p[3];
    GxWorker::Run([id, c] { GXSetTevKColor((GXTevKColorID)id, c); });
}
PPC_NATIVE_OVERRIDE_VOID(80171ed4, GX__SetTevKColor_80171ed4, (uint32_t id, uint32_t cp), (id, cp));

extern "C" void GX__SetTevKColorSel_80171f30(uint32_t s, uint32_t sel) {
    GX_DEFER_CALL(GX__SetTevKColorSel_80171f30, s, sel); if (!TevStageOk(s)) return; GXSetTevKColorSel((GXTevStageID)s, (GXTevKColorSel)sel); }
PPC_NATIVE_OVERRIDE_VOID(80171f30, GX__SetTevKColorSel_80171f30, (uint32_t s, uint32_t sel), (s, sel));

extern "C" void GX__SetTevKAlphaSel_80171f80(uint32_t s, uint32_t sel) {
    GX_DEFER_CALL(GX__SetTevKAlphaSel_80171f80, s, sel); if (!TevStageOk(s)) return; GXSetTevKAlphaSel((GXTevStageID)s, (GXTevKAlphaSel)sel); }
PPC_NATIVE_OVERRIDE_VOID(80171f80, GX__SetTevKAlphaSel_80171f80, (uint32_t s, uint32_t sel), (s, sel));

// ============================================================================
// TEV Swap Tables
// ============================================================================

extern "C" void GX__SetTevSwapModeTable_8017200c(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    GX_DEFER_CALL(GX__SetTevSwapModeTable_8017200c, id, r, g, b, a);
    if (!TevSwapOk(id)) return;
    GXSetTevSwapModeTable((GXTevSwapSel)id, (GXTevColorChan)r, (GXTevColorChan)g, (GXTevColorChan)b, (GXTevColorChan)a);
}
PPC_NATIVE_OVERRIDE_VOID(8017200c, GX__SetTevSwapModeTable_8017200c, (uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a), (id, r, g, b, a));

extern "C" void GX__SetTevSwapMode_80171fd0(uint32_t s, uint32_t rs, uint32_t ts) {
    GX_DEFER_CALL(GX__SetTevSwapMode_80171fd0, s, rs, ts);
    if (!TevStageOk(s) || !TevSwapOk(rs) || !TevSwapOk(ts)) return;
    GXSetTevSwapMode((GXTevStageID)s, (GXTevSwapSel)rs, (GXTevSwapSel)ts);
}
PPC_NATIVE_OVERRIDE_VOID(80171fd0, GX__SetTevSwapMode_80171fd0, (uint32_t s, uint32_t rs, uint32_t ts), (s, rs, ts));
