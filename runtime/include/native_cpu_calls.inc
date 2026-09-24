// Native call catalog used by InvokeDirectCpu. Keep project-specific HLE
// address mappings here; abi_bridge.h contains only the generic ABI machinery.
//
// This is a .inc, not a header: it has no includes of its own and references
// CpuContext, KnownTypedNativeCpuCall<> and KnownNativeCpuCall<>, all of which
// are declared above its single mid-file include site in abi_bridge.h. It
// cannot compile standalone and must never be included anywhere else.

extern "C" void nw4r__lyt__detail__DrawQuad_800847c0(CpuContext* ctx);
extern "C" void nw4r__lyt__detail__DrawQuad_80084d20(CpuContext* ctx);

extern "C" void GX__SetVtxDesc_8016d3a4(uint32_t a, uint32_t t);
extern "C" void GX__ClearVtxDesc_8016dc34();
extern "C" void GX__SetVtxAttrFmt_8016dc68(uint32_t vf, uint32_t a, uint32_t c, uint32_t t, uint32_t fr);
extern "C" void GX__SetTexCoordGen2_8016e37c(uint32_t dc, uint32_t f, uint32_t sp, uint32_t m, uint32_t n, uint32_t pm);
extern "C" void GX__SetNumTexGens_8016e5a4(uint32_t n);
extern "C" void GX__DrawDone_8016eab0();
extern "C" void GX__Begin_8016f0f0(uint32_t t, uint32_t vf, uint32_t nv);
extern "C" void GX__SetLineWidth_8016f314(uint32_t width, uint32_t texOffsets);
extern "C" void GX__SetPointSize_8016f348(uint32_t pointSize, uint32_t texOffsets);
extern "C" void GX__SetCullMode_8016f3b8(uint32_t m);
extern "C" void GX__SetNumChans_8017054c(uint32_t n);
extern "C" void GX__SetChanCtrl_80170570(uint32_t ch, uint32_t en, uint32_t as, uint32_t ms, uint32_t lm, uint32_t df, uint32_t af);
extern "C" void GX__InitTexObj_801707f8(uint32_t oa, uint32_t da, uint32_t w, uint32_t h, uint32_t f, uint32_t ws, uint32_t wt, uint32_t m);
extern "C" void GX__InitTexObjLOD_80170a4c(uint32_t oa, uint32_t mif, uint32_t maf, float mil, float mal, float lb, uint32_t bc, uint32_t el, uint32_t ma);
extern "C" void GX__InvalidateTexAll_80171110();
extern "C" void GX__LoadTexObj_80170f2c(uint32_t oa, uint32_t tid);
extern "C" void GX__SetNumIndStages_80171b38(uint32_t n);
extern "C" void GX__SetTevDirect_80171b58(uint32_t s);
extern "C" void GX__SetTevColorIn_80171ce0(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevAlphaIn_80171d20(uint32_t s, uint32_t a, uint32_t b, uint32_t c, uint32_t d);
extern "C" void GX__SetTevColorOp_80171d60(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevAlphaOp_80171db8(uint32_t s, uint32_t op, uint32_t b, uint32_t sc, uint32_t cl, uint32_t or_);
extern "C" void GX__SetTevColorS10_80171e70(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColor_80171ed4(uint32_t id, uint32_t cp);
extern "C" void GX__SetTevKColorSel_80171f30(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevKAlphaSel_80171f80(uint32_t s, uint32_t sel);
extern "C" void GX__SetTevSwapMode_80171fd0(uint32_t s, uint32_t rs, uint32_t ts);
extern "C" void GX__SetTevSwapModeTable_8017200c(uint32_t id, uint32_t r, uint32_t g, uint32_t b, uint32_t a);
extern "C" void GX__SetTevOrder_8017214c(uint32_t s, uint32_t c, uint32_t m, uint32_t col);
extern "C" void GX__SetNumTevStages_801722a8(uint32_t n);
extern "C" void GX__SetBlendMode_8017277c(uint32_t t, uint32_t s, uint32_t d, uint32_t op);
extern "C" void GX__LoadPosMtxImm_8017310c(uint32_t ma, uint32_t id);
extern "C" void GX__SetCurrentMtx_80173214(uint32_t id);

#define MKW_KNOWN_TYPED_NATIVE(addr, fn, ...) \
template <> struct KnownTypedNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static void Invoke(CpuContext* cpu) { (void)cpu; fn(__VA_ARGS__); } \
}

MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016D3A4), GX__SetVtxDesc_8016d3a4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016DC34), GX__ClearVtxDesc_8016dc34);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016DC68), GX__SetVtxAttrFmt_8016dc68, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016E37C), GX__SetTexCoordGen2_8016e37c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016E5A4), GX__SetNumTexGens_8016e5a4, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016EAB0), GX__DrawDone_8016eab0);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016F0F0), GX__Begin_8016f0f0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016F314), GX__SetLineWidth_8016f314, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016F348), GX__SetPointSize_8016f348, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8016F3B8), GX__SetCullMode_8016f3b8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8017054C), GX__SetNumChans_8017054c, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80170570), GX__SetChanCtrl_80170570, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(801707F8), GX__InitTexObj_801707f8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8], cpu->gpr[9], cpu->gpr[10]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80170A4C), GX__InitTexObjLOD_80170a4c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], static_cast<float>(cpu->fpr[1].d), static_cast<float>(cpu->fpr[2].d), static_cast<float>(cpu->fpr[3].d), cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171110), GX__InvalidateTexAll_80171110);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80170F2C), GX__LoadTexObj_80170f2c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171B38), GX__SetNumIndStages_80171b38, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171B58), GX__SetTevDirect_80171b58, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171CE0), GX__SetTevColorIn_80171ce0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171D20), GX__SetTevAlphaIn_80171d20, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171D60), GX__SetTevColorOp_80171d60, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171DB8), GX__SetTevAlphaOp_80171db8, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7], cpu->gpr[8]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171E70), GX__SetTevColorS10_80171e70, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171ED4), GX__SetTevKColor_80171ed4, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171F30), GX__SetTevKColorSel_80171f30, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171F80), GX__SetTevKAlphaSel_80171f80, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80171FD0), GX__SetTevSwapMode_80171fd0, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8017200C), GX__SetTevSwapModeTable_8017200c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6], cpu->gpr[7]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8017214C), GX__SetTevOrder_8017214c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(801722A8), GX__SetNumTevStages_801722a8, cpu->gpr[3]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8017277C), GX__SetBlendMode_8017277c, cpu->gpr[3], cpu->gpr[4], cpu->gpr[5], cpu->gpr[6]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(8017310C), GX__LoadPosMtxImm_8017310c, cpu->gpr[3], cpu->gpr[4]);
MKW_KNOWN_TYPED_NATIVE(MKW_GADDR(80173214), GX__SetCurrentMtx_80173214, cpu->gpr[3]);

#undef MKW_KNOWN_TYPED_NATIVE

#define MKW_KNOWN_NATIVE_CPU_CALL(addr, fn) \
template <> struct KnownNativeCpuCall<addr> { \
    static constexpr bool kAvailable = true; \
    static constexpr uint32_t kNonvolatileFprWriteMask = 0; \
    static constexpr void (*Entry)(CpuContext*) = &fn; \
}

MKW_KNOWN_NATIVE_CPU_CALL(MKW_GADDR(800847C0), nw4r__lyt__detail__DrawQuad_800847c0);
MKW_KNOWN_NATIVE_CPU_CALL(MKW_GADDR(80084D20), nw4r__lyt__detail__DrawQuad_80084d20);

#undef MKW_KNOWN_NATIVE_CPU_CALL
