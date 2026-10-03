// gx_egg.cpp - EGG Library Helper Stubs
#include "gx_internal.h"

// Vertex helper declarations not used by the direct-call catalog.
extern "C" void GX__SetArray_8016e32c(uint32_t a, uint32_t ba, uint32_t str);
extern "C" void GX_HLE_FIFO_WriteFloat(float val);
extern "C" void GX_HLE_FIFO_Write8(uint8_t val);

// ============================================================================
// EGG::DrawGX Functions
// ============================================================================

// Use translated implementations for the main DrawGX setup routines.
// Keep HLE fallbacks only for known missing alias entry points.
static void EGG__DrawGX__SetVtxState_HLE(uint32_t state) {
    GX__ClearVtxDesc_8016dc34();
    auto setDesc = [](uint32_t attr, uint32_t type) { GX__SetVtxDesc_8016d3a4(attr, type); };
    auto setFmt = [](uint32_t attr, uint32_t cnt, uint32_t type, uint32_t frac) { GX__SetVtxAttrFmt_8016dc68(0, attr, cnt, type, frac); };
    auto setArray = [](uint32_t attr, uint32_t addr, uint32_t stride) { if (addr && stride) GX__SetArray_8016e32c(attr, addr, stride); };

    switch (state) {
    case 0:
        setFmt(GX_VA_POS, GX_POS_XYZ, GX_S16, 0xE);
        setFmt(GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0xE);
        setArray(GX_VA_POS, MKW_GADDR(802574a0), 6);
        setArray(GX_VA_NRM, MKW_GADDR(802574e0), 6);
        setDesc(GX_VA_POS, GX_INDEX8);
        setDesc(GX_VA_NRM, GX_INDEX8);
        break;
    case 1:
        setDesc(GX_VA_POS, GX_DIRECT);
        setFmt(GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        break;
    case 2: case 3: case 4: case 5:
        setDesc(GX_VA_POS, GX_DIRECT); setDesc(GX_VA_NRM, GX_DIRECT);
        setFmt(GX_VA_POS, GX_POS_XYZ, GX_F32, 0); setFmt(GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        break;
    case 6:
        setDesc(GX_VA_POS, GX_DIRECT); setDesc(GX_VA_NRM, GX_DIRECT); setDesc(GX_VA_CLR0, GX_DIRECT);
        setFmt(GX_VA_POS, GX_POS_XYZ, GX_F32, 0); setFmt(GX_VA_NRM, GX_NRM_XYZ, GX_F32, 0);
        setFmt(GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        break;
    case 7: case 8:
        setDesc(GX_VA_POS, GX_INDEX8); setDesc(GX_VA_NRM, GX_INDEX8);
        if (state == 7) setDesc(GX_VA_TEX0, GX_DIRECT);
        setFmt(GX_VA_POS, GX_POS_XY, GX_S16, 0xE); setFmt(GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0xE);
        if (state == 7) setFmt(GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        setArray(GX_VA_POS, MKW_GADDR(80257520), 4); setArray(GX_VA_NRM, MKW_GADDR(80388b80), 6);
        break;
    case 9:
        setDesc(GX_VA_POS, GX_INDEX8); setDesc(GX_VA_NRM, GX_INDEX8); setDesc(GX_VA_TEX0, GX_DIRECT);
        setFmt(GX_VA_POS, GX_POS_XYZ, GX_S16, 0xE); setFmt(GX_VA_NRM, GX_NRM_XYZ, GX_S16, 0xE);
        setFmt(GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);
        setArray(GX_VA_POS, MKW_GADDR(80257540), 6); setArray(GX_VA_NRM, MKW_GADDR(80388ba0), 6);
        break;
    case 10: case 11: case 12: case 13: {
        const bool useAltPos = (state == 11 || state == 13);
        const bool useTex = (state == 10 || state == 11);
        setDesc(GX_VA_POS, GX_INDEX8);
        if (useTex) setDesc(GX_VA_TEX0, GX_INDEX8);
        const uint32_t posBase = useAltPos ? MKW_GADDR(80388be0) : MKW_GADDR(80388bc0);
        setArray(GX_VA_POS, posBase, 2);
        if (useTex) setArray(GX_VA_TEX0, MKW_GADDR(80388be0), 2);
        setFmt(GX_VA_POS, GX_POS_XY, GX_U8, 0);
        if (useTex) setFmt(GX_VA_TEX0, GX_TEX_ST, GX_U8, 0);
        break;
    }
    default: break;
    }
}
PPC_NATIVE_OVERRIDE_VOID(8021b344, EGG__DrawGX__SetVtxState_HLE, (uint32_t state), (state));
PPC_NATIVE_OVERRIDE_VOID(8021b688, EGG__DrawGX__SetVtxState_HLE, (uint32_t state), (state));

extern "C" void EGG__LightTexture__SetupTevFinish_HLE_8022e2bc(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    const uint32_t stageCount = Memory::Read8(self + 0x75);
    const uint32_t tevCount = Memory::Read16(self + 0x78);

    if (stageCount != 0) {
        uint32_t remainder = tevCount % stageCount;
        if (remainder > 0) {
            const GXColor black{0, 0, 0, 255};
            while (remainder < stageCount) {
                GXSetTevColor(static_cast<GXTevRegID>(remainder + 1), black);
                GXSetTevKColor(static_cast<GXTevKColorID>(remainder), black);
                ++remainder;
            }

            // The three fog constants live in .sdata2 and are named by identity rather than by
            // an r2 offset: NTSC-U puts them 8 bytes further from r2 than PAL/NTSC-J/NTSC-K do,
            // so the hardcoded PAL offset used to read 176.0 there instead of 0.0 / 0.99.
            constexpr uint32_t kFogOriginDefault = MKW_GADDR(80388DD0);
            constexpr uint32_t kFogOriginMode2 = MKW_GADDR(80388DF4);
            constexpr uint32_t kFogSpan = MKW_GADDR(80388DE0);
            const uint32_t mode = Memory::Read32(self + 0x44);
            const float origin =
                Memory::ReadFloat32((mode == 2) ? kFogOriginMode2 : kFogOriginDefault);
            const float span = Memory::ReadFloat32(kFogSpan);
            const float end = static_cast<float>(origin + span);
            const float lower = static_cast<float>(origin - span);

            auto writeVertex = [](uint8_t posIdx, float s, float t) {
                GX_HLE_FIFO_Write8(posIdx);
                GX_HLE_FIFO_WriteFloat(s);
                GX_HLE_FIFO_WriteFloat(t);
            };

            GX__Begin_8016f0f0(GX_QUADS, GX_VTXFMT0, 4);
            writeVertex(0, origin, origin);
            writeVertex(1, origin, lower);
            writeVertex(2, end, lower);
            writeVertex(3, end, origin);
        }
    }

    Memory::Write8(self + 0x74, 2);
}
PPC_NATIVE_OVERRIDE_VOID(8022e2bc, EGG__LightTexture__SetupTevFinish_HLE_8022e2bc, (CpuContext* ctx), (ctx));

// ============================================================================
// EGG::AsyncDisplay
// ============================================================================

extern "C" void EGG__AsyncDisplay__endRender_HLE_8020ff9c(CpuContext* ctx) {
    uint32_t p = ctx->gpr[3]; ctx->gpr[3] = p; ctx->lr = MKW_GADDR(8020FF9C);
    InvokeIndirectCpu(MKW_GADDR(80219FB4), ctx);
    InvokeIndirectCpu(MKW_GADDR(8016ED50), ctx);
}
PPC_NATIVE_OVERRIDE_VOID(8020FF9C, EGG__AsyncDisplay__endRender_HLE_8020ff9c, (CpuContext* ctx), (ctx));
