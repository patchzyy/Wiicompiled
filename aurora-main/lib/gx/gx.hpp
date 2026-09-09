#pragma once
#include <dolphin/gx.h>
#include <aurora/math.hpp>

#include "../internal.hpp"
#include "../gfx/common.hpp"
#include "../gfx/texture.hpp"

#include <absl/container/flat_hash_map.h>
#include <algorithm>
#include <type_traits>
#include <cstring>
#include <bitset>
#include <memory>
#include <array>
#include <cfloat>
#include <cmath>

#define M_PIF 3.14159265358979323846f

namespace GX {
constexpr u8 MaxLights = 8;
using LightMask = std::bitset<MaxLights>;
} // namespace GX

struct GXLightObj_ {
  GXColor color;
  float a0 = 1.f;
  float a1 = 0.f;
  float a2 = 0.f;
  float k0 = 1.f;
  float k1 = 0.f;
  float k2 = 0.f;
  float px = 0.f;
  float py = 0.f;
  float pz = 0.f;
  float nx = 0.f;
  float ny = 0.f;
  float nz = 0.f;
};
static_assert(sizeof(GXLightObj_) <= sizeof(GXLightObj), "GXLightObj too small!");

#if GX_IS_WII
constexpr float GX_LARGE_NUMBER = -1.0e+18f;
#else
constexpr float GX_LARGE_NUMBER = -1048576.0f;
#endif

namespace aurora::gx {
constexpr bool UsePerPixelLighting = false;
constexpr bool UseReversedZ = true;

constexpr u32 MaxTextures = GX_MAX_TEXMAP;
constexpr u32 MaxTluts = 20;
constexpr u32 MaxTevStages = GX_MAX_TEVSTAGE;
constexpr u32 MaxColorChannels = 4;
constexpr u32 MaxTevRegs = 4; // TEVPREV, TEVREG0-2
constexpr u32 MaxKColors = GX_MAX_KCOLOR;
constexpr u32 MaxTexMtx = 10;
constexpr u32 MaxPTTexMtx = 20;
constexpr u32 MaxTexCoord = GX_MAX_TEXCOORD;
constexpr u32 MaxVtxAttr = GX_VA_MAX_ATTR;
constexpr u32 MaxTevSwap = GX_MAX_TEVSWAP;
constexpr u32 MaxIndStages = GX_MAX_INDTEXSTAGE;
constexpr u32 MaxIndTexMtxs = 3;
constexpr u32 MaxVtxFmt = GX_MAX_VTXFMT;
constexpr u32 MaxPnMtx = (GX_PNMTX9 / 3) + 1;
// Position and texture matrices share one shader array (`ubuf.postex_mtx`), mirroring XF matrix memory: rows 0..29 (slots 0..9) are position matrices and rows 30..59 (slots 10..19) are texture matrices.
constexpr u32 MaxPostexMtx = MaxPnMtx + MaxTexMtx;
constexpr u32 MaxIndexAttr = 12; // VA_POS -> VA_TEX7
constexpr u32 MaxUniformSize = 3840;

extern wgpu::BindGroup g_emptyTextureBindGroup;

template <typename Arg, Arg Default>
struct TevPass {
  Arg a = Default;
  Arg b = Default;
  Arg c = Default;
  Arg d = Default;

  bool operator==(const TevPass& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const TevPass& rhs) const { return !(*this == rhs); }
};
static_assert(std::has_unique_object_representations_v<TevPass<GXTevColorArg, GX_CC_ZERO>>);
static_assert(std::has_unique_object_representations_v<TevPass<GXTevAlphaArg, GX_CA_ZERO>>);
struct TevOp {
  GXTevOp op = GX_TEV_ADD;
  GXTevBias bias = GX_TB_ZERO;
  GXTevScale scale = GX_CS_SCALE_1;
  GXTevRegID outReg = GX_TEVPREV;
  bool clamp = true;
  u8 _p1 = 0;
  u8 _p2 = 0;
  u8 _p3 = 0;

  bool operator==(const TevOp& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const TevOp& rhs) const { return !(*this == rhs); }
};
static_assert(std::has_unique_object_representations_v<TevOp>);
struct TevStage {
  TevPass<GXTevColorArg, GX_CC_ZERO> colorPass;
  TevPass<GXTevAlphaArg, GX_CA_ZERO> alphaPass;
  TevOp colorOp;
  TevOp alphaOp;
  GXTevKColorSel kcSel = GX_TEV_KCSEL_1;
  GXTevKAlphaSel kaSel = GX_TEV_KASEL_1;
  GXTexCoordID texCoordId = GX_TEXCOORD_NULL;
  GXTexMapID texMapId = GX_TEXMAP_NULL;
  GXChannelID channelId = GX_COLOR_NULL;
  GXTevSwapSel tevSwapRas = GX_TEV_SWAP0;
  GXTevSwapSel tevSwapTex = GX_TEV_SWAP0;
  GXIndTexStageID indTexStage = GX_INDTEXSTAGE0;
  GXIndTexFormat indTexFormat = GX_ITF_8;
  GXIndTexBiasSel indTexBiasSel = GX_ITB_NONE;
  GXIndTexAlphaSel indTexAlphaSel = GX_ITBA_OFF;
  GXIndTexMtxID indTexMtxId = GX_ITM_OFF;
  GXIndTexWrap indTexWrapS = GX_ITW_OFF;
  GXIndTexWrap indTexWrapT = GX_ITW_OFF;
  bool indTexUseOrigLOD = false;
  bool indTexAddPrev = false;
  u8 _p1 = 0;
  u8 _p2 = 0;

  bool operator==(const TevStage& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const TevStage& rhs) const { return !(*this == rhs); }
};
static_assert(std::has_unique_object_representations_v<TevStage>);
struct IndStage {
  GXTexCoordID texCoordId;
  GXTexMapID texMapId;
  GXIndTexScale scaleS;
  GXIndTexScale scaleT;
};
static_assert(std::has_unique_object_representations_v<IndStage>);
// For shader generation
struct ColorChannelConfig {
  GXColorSrc matSrc = GX_SRC_REG;
  GXColorSrc ambSrc = GX_SRC_REG;
  GXDiffuseFn diffFn = GX_DF_NONE;
  GXAttnFn attnFn = GX_AF_NONE;
  bool lightingEnabled = false;
  u8 _p1 = 0;
  u8 _p2 = 0;
  u8 _p3 = 0;

  bool operator==(const ColorChannelConfig& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const ColorChannelConfig& rhs) const { return !(*this == rhs); }
};
static_assert(std::has_unique_object_representations_v<ColorChannelConfig>);
// For uniform generation
struct ColorChannelState {
  Vec4<float> matColor;
  Vec4<float> ambColor;
  GX::LightMask lightMask;
};
struct TcgConfig {
  GXTexGenType type = GX_TG_MTX2x4;
  GXTexGenSrc src = GX_MAX_TEXGENSRC;
  GXTexMtx mtx = GX_IDENTITY;
  GXPTTexMtx postMtx = GX_PTIDENTITY;
  bool normalize = false;
  bool inputFormAB11 = false;
  u8 _p2 = 0;
  u8 _p3 = 0;

  bool operator==(const TcgConfig& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const TcgConfig& rhs) const { return !(*this == rhs); }
};
static_assert(std::has_unique_object_representations_v<TcgConfig>);
struct FogState {
  GXFogType type = GX_FOG_NONE;
  float a = 0.f;
  float b = 0.5f;
  float c = 0.f;
  float aRaw = 0.f;
  Vec4<float> color;
  // Raw encoded register values for A/B reconstruction across separate BP writes
  u32 fog0Raw = 0; // 0xEE: encoded A parameter
  u32 fog1Raw = 0; // 0xEF: B mantissa
  u32 fog2Raw = 0; // 0xF0: B shift
  u32 bMagnitude = 1;
  u32 bShift = 1;

  bool operator==(const FogState& rhs) const {
    return type == rhs.type && a == rhs.a && b == rhs.b && c == rhs.c && aRaw == rhs.aRaw && color == rhs.color &&
           bMagnitude == rhs.bMagnitude && bShift == rhs.bShift;
  }
  bool operator!=(const FogState& rhs) const { return !(*this == rhs); }
};
struct TevSwap {
  GXTevColorChan red = GX_CH_RED;
  GXTevColorChan green = GX_CH_GREEN;
  GXTevColorChan blue = GX_CH_BLUE;
  GXTevColorChan alpha = GX_CH_ALPHA;

  bool operator==(const TevSwap& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const TevSwap& rhs) const { return !(*this == rhs); }
  explicit operator bool() const { return !(*this == TevSwap{}); }
};
static_assert(std::has_unique_object_representations_v<TevSwap>);
struct AlphaCompare {
  GXCompare comp0 = GX_ALWAYS;
  u32 ref0 = 0; // would be u8 but extended to avoid padding bytes
  GXAlphaOp op = GX_AOP_AND;
  GXCompare comp1 = GX_ALWAYS;
  u32 ref1 = 0;

  bool operator==(const AlphaCompare& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
  bool operator!=(const AlphaCompare& rhs) const { return !(*this == rhs); }
  explicit operator bool() const { return comp0 != GX_ALWAYS || comp1 != GX_ALWAYS; }
};
static_assert(std::has_unique_object_representations_v<AlphaCompare>);
struct IndTexMtxInfo {
  Mat3x2<float> mtx;
  s8 scaleExp = 0;
  // Accumulated adjScale bits from BP registers (2 bits per row, 3 rows)
  u8 adjScaleRaw = 0;

  bool operator==(const IndTexMtxInfo& rhs) const { return mtx == rhs.mtx && scaleExp == rhs.scaleExp; }
  bool operator!=(const IndTexMtxInfo& rhs) const { return !(*this == rhs); }
};
struct TexCoordScale {
  u16 scaleS = 0; // texture width - 1
  u16 scaleT = 0; // texture height - 1
  bool biasS = false;
  bool biasT = false;
  bool cylWrapS = false;
  bool cylWrapT = false;
  bool lineOffset = false;
  bool pointOffset = false;

  bool operator==(const TexCoordScale& rhs) const {
    return scaleS == rhs.scaleS && scaleT == rhs.scaleT && biasS == rhs.biasS && biasT == rhs.biasT &&
           cylWrapS == rhs.cylWrapS && cylWrapT == rhs.cylWrapT && lineOffset == rhs.lineOffset &&
           pointOffset == rhs.pointOffset;
  }
  bool operator!=(const TexCoordScale& rhs) const { return !(*this == rhs); }
};
struct VtxAttrFmt {
  GXCompCnt cnt;
  GXCompType type;
  u8 frac;
  u8 _p1 = 0;
  u8 _p2 = 0;
  u8 _p3 = 0;
};
static_assert(std::has_unique_object_representations_v<VtxAttrFmt>);
struct VtxFmt {
  std::array<VtxAttrFmt, MaxVtxAttr> attrs;
};
static_assert(std::has_unique_object_representations_v<VtxFmt>);
struct PnMtx {
  Mat3x4<float> pos;
  Mat3x4<float> nrm;
};
static_assert(sizeof(PnMtx) == sizeof(Mat3x4<float>) * 2);
struct Light {
  Vec4<float> pos{0.f, 0.f, 0.f};
  Vec4<float> dir{0.f, 0.f, 0.f};
  Vec4<float> color{0.f, 0.f, 0.f, 0.f};
  Vec4<float> cosAtt{0.f, 0.f, 0.f};
  Vec4<float> distAtt{0.f, 0.f, 0.f};

  bool operator==(const Light& rhs) const {
    return pos == rhs.pos && dir == rhs.dir && color == rhs.color && cosAtt == rhs.cosAtt && distAtt == rhs.distAtt;
  }
  bool operator!=(const Light& rhs) const { return !(*this == rhs); }
};
static_assert(sizeof(Light) == 80);
struct Fog {
  Vec4<float> color;
  float a = 0.f;
  float b = 0.5f;
  float c = 0.f;
  float pad = FLT_MAX;
  Vec4<float> rangeBase;
  std::array<Vec4<float>, 3> rangeK;
};
static_assert(sizeof(Fog) == 96);
struct AttrArray {
  const void* data;
  u32 size;
  u8 stride;
  bool le = true;
  gfx::Range cachedRange;
};
inline bool operator==(const AttrArray& lhs, const AttrArray& rhs) {
  return lhs.data == rhs.data && lhs.size == rhs.size && lhs.stride == rhs.stride && lhs.le == rhs.le;
}
inline bool operator!=(const AttrArray& lhs, const AttrArray& rhs) { return !(lhs == rhs); }

// Stamps GXState::pipelineStateGeneration, both when a GXState is constructed and on every pipeline-state write.
inline u32 next_gx_state_epoch() noexcept {
  static u32 epoch = 0;
  return ++epoch;
}

struct GXState {
  struct CopyTextureRef {
    gfx::TextureHandle handle;
    u32 revision = 0;
    u32 lastProducedFrame = 0;
    u32 lastSampledFrame = 0;
    u32 width = 0;
    u32 height = 0;
    u32 dataSize = 0;
    GXTexFmt format = GX_TF_I4;
    bool sampledThisFrame = false;

    operator bool() const noexcept { return handle.operator bool(); }
  };
  std::array<PnMtx, MaxPnMtx> pnMtx;
  u32 currentPnMtx;
  Mat4x4<float> proj;
  GXProjectionType projType; // for GXGetProjectionv
  FogState fog;
  GXCullMode cullMode = GX_CULL_BACK;
  u8 lineWidth = 0;
  u8 pointSize = 0;
  GXTexOffset lineTexOffset = GX_TO_ZERO;
  GXTexOffset pointTexOffset = GX_TO_ZERO;
  bool lineHalfAspect = false;
  GXBlendMode blendMode = GX_BM_NONE;
  GXBlendFactor blendFacSrc = GX_BL_SRCALPHA;
  GXBlendFactor blendFacDst = GX_BL_INVSRCALPHA;
  GXLogicOp blendOp = GX_LO_CLEAR;
  GXCompare depthFunc = GX_LEQUAL;
  Vec4<float> clearColor{0.f, 0.f, 0.f, 1.f};
  u32 clearDepth = 0xFFFFFF;
  GXPixelFmt pixelFmt = GX_PF_RGB8_Z24;
  GXZFmt16 zFmt = GX_ZC_LINEAR;
  bool zCompLocBeforeTex = false;
  GXZTexOp zTextureOp = GX_ZT_DISABLE;
  u8 zTextureFmt = 0;
  u32 zTextureBias = 0;
  u32 dstAlpha; // u8; UINT32_MAX = disabled
  AlphaCompare alphaCompare;
  std::array<Vec4<float>, MaxTevRegs> colorRegs;
  std::array<Vec4<float>, GX_MAX_KCOLOR> kcolors;
  std::array<ColorChannelConfig, MaxColorChannels> colorChannelConfig;
  std::array<ColorChannelState, MaxColorChannels> colorChannelState;
  std::array<Light, GX::MaxLights> lights;
  // Light direction sanitization/normalization is independent of the draw.
  std::array<Light, GX::MaxLights> preparedLights;
  bool preparedLightsDirty = true;
  std::array<TevStage, MaxTevStages> tevStages;
  std::array<gfx::TextureBind, MaxTextures> textures;
  std::array<GXTexObj_, MaxTextures> loadedTextures;
  std::array<GXTlutObj_, MaxTluts> loadedTluts;
  AuroraViewportPolicy viewportPolicy = AURORA_VIEWPORT_FIT;
  gfx::Viewport logicalViewport{0.f, 0.f, 640.f, 480.f, 0.f, 1.f};
  gfx::Viewport renderViewport{0.f, 0.f, 640.f, 480.f, 0.f, 1.f};
  gfx::ClipRect logicalScissor{0, 0, 640, 480};
  gfx::ClipRect renderScissor{0, 0, 640, 480};
  std::array<float, 6> xfViewport{320.f, -240.f, 16777215.f, 660.f, 580.f, 16777215.f};
  std::array<float, 6> xfProjection{};
  std::array<Mat3x4<float>, MaxTexMtx> texMtxs;
  std::array<Mat3x4<float>, MaxPTTexMtx> ptTexMtxs;
  std::array<TcgConfig, MaxTexCoord> tcgs;
  std::array<TexCoordScale, MaxTexCoord> texCoordScales;
  u16 lastVtxSize = 0;
  GXVtxFmt lastVtxFmt = GX_MAX_VTXFMT;
  std::array<GXAttrType, MaxVtxAttr> vtxDesc;
  std::array<GXAttrType, MaxVtxAttr> sourceVtxDesc;
  std::array<VtxFmt, MaxVtxFmt> vtxFmts;
  std::array<TevSwap, MaxTevSwap> tevSwapTable{
      TevSwap{},
      TevSwap{GX_CH_RED, GX_CH_RED, GX_CH_RED, GX_CH_ALPHA},
      TevSwap{GX_CH_GREEN, GX_CH_GREEN, GX_CH_GREEN, GX_CH_ALPHA},
      TevSwap{GX_CH_BLUE, GX_CH_BLUE, GX_CH_BLUE, GX_CH_ALPHA},
  };
  std::array<IndStage, MaxIndStages> indStages;
  std::array<IndTexMtxInfo, MaxIndTexMtxs> indTexMtxs;
  u8 indTexMask = 0;
  std::array<u16, 4> boundingBox{1023, 0, 1023, 0};
  std::array<AttrArray, MaxVtxAttr> arrays;
  gfx::ClipRect texCopySrc;
  gfx::ClipRect dispCopySrc;
  bool texCopySrcRenderSpace = false;
  GXTexFmt texCopyFmt;
  u16 texCopyDstWidth = 0;
  u16 texCopyDstHeight = 0;
  bool texCopyHalfScale = false;
  u16 dispCopyDstWidth = 640;
  u16 dispCopyDstHeight = 480;
  f32 dispCopyYScale = 1.f;
  GXGamma dispCopyGamma = GX_GM_1_0;
  GXBool copyFilterAa = GX_FALSE;
  GXBool copyFilterVf = GX_FALSE;
  std::array<std::array<u8, 2>, 12> copyFilterSamplePattern{};
  std::array<u8, 7> copyFilterVFilter{};
  GXFBClamp copyClamp = GX_CLAMP_NONE;
  u32 dispCopyFrame2Field = 0;
  u32 fieldMask = 0;
  u32 fieldMode = 0;
  u32 revBits = 0;
  std::array<u32, 6> fogRange{};
  u32 xfError = 0;
  u32 dualTex = 0;
  float zScale = 1.f;
  float zOffset = 0.f;
  s32 scissorOffsetX = 0;
  s32 scissorOffsetY = 0;
  struct CopyTextureKey {
    const void* dest = nullptr;
    u32 width = 0;
    u32 height = 0;
    GXTexFmt format = GX_TF_I4;

    bool operator==(const CopyTextureKey& rhs) const {
      return dest == rhs.dest && width == rhs.width && height == rhs.height && format == rhs.format;
    }

    template <typename H>
    friend H AbslHashValue(H h, const CopyTextureKey& key) {
      return H::combine(std::move(h), key.dest, key.width, key.height, key.format);
    }
  };
  absl::flat_hash_map<const void*, CopyTextureRef> copyTextures;
  absl::flat_hash_map<CopyTextureKey, CopyTextureRef> copyTextureCache;
  gfx::TextureHandle displayCopyTexture;
  wgpu::BindGroup displayCopyBindGroup;
  u32 displayCopyWidth = 0;
  u32 displayCopyHeight = 0;
  bool depthCompare = true;
  bool depthUpdate = true;
  bool colorUpdate = true;
  bool alphaUpdate = true;
  u8 numChans = 0;
  u8 numIndStages = 0;
  u8 numTevStages = 0;
  u8 numTexGens = 0;
  bool stateDirty = true;
  // Bumped by the decoded register writes that feed populate_pipeline_config, and by nothing else.
  u32 pipelineStateGeneration = next_gx_state_epoch();
  std::array<u32, 0x100> bpRegCache = [] {
    std::array<u32, 0x100> regs{};
    regs[0xFE] = 0x00FFFFFF;
    return regs;
  }();
  // Covers XF 0x00-0x5F: the scalar bank, viewport/projection, and the TexGen and post-transform registers that every material's display list re-emits.
  std::array<u32, 0x60> xfRegCache{};
  std::array<u64, 2> xfRegCacheValid{};

  [[nodiscard]] bool xfRegMatches(u32 reg, u32 val) const {
    return (xfRegCacheValid[reg >> 6] >> (reg & 63) & 1) != 0 && xfRegCache[reg] == val;
  }
  void storeXfReg(u32 reg, u32 val) {
    xfRegCache[reg] = val;
    xfRegCacheValid[reg >> 6] |= u64{1} << (reg & 63);
  }
  // For state that another register bank writes behind the XF cache's back.
  void invalidateXfReg(u32 reg) { xfRegCacheValid[reg >> 6] &= ~(u64{1} << (reg & 63)); }

  void clearVtxSizeCache() { lastVtxFmt = GX_MAX_VTXFMT; }
};
extern GXState g_gxState;
struct ShaderInfo;
struct MappedRenderState {
  gfx::Viewport viewport{};
  gfx::ClipRect scissor{};
};

void initialize() noexcept;
void shutdown() noexcept;
void clear_copy_texture_cache() noexcept;
void clear_display_copy_cache() noexcept;
void set_display_copy_present_source() noexcept;
void evict_copy_texture(const void* dest) noexcept;
// Drops retired GX copy targets so they cannot outlive the destination they were recycled for.
void prune_copy_texture_pool(const void* dest) noexcept;
void evict_texture_object(u32 texObjId) noexcept;
void evict_tlut_object(u32 tlutObjId) noexcept;
void invalidate_static_texture_cache() noexcept;
Vec2<uint32_t> logical_fb_size() noexcept;
MappedRenderState map_logical_render_state() noexcept;
gfx::Viewport map_logical_viewport(const gfx::Viewport& logicalViewport) noexcept;
gfx::ClipRect map_logical_scissor(const gfx::ClipRect& logicalScissor) noexcept;
void set_logical_viewport(const gfx::Viewport& viewport) noexcept;
void set_render_viewport(const gfx::Viewport& viewport) noexcept;
void set_logical_scissor(const gfx::ClipRect& scissor) noexcept;
void set_render_scissor(const gfx::ClipRect& scissor) noexcept;
const gfx::TextureBind& get_texture(GXTexMapID id) noexcept;
void resolve_sampled_textures(const ShaderInfo& info) noexcept;

inline float clear_depth_value() {
  // g_gxState.clearDepth is in GX's own distance terms (0 = near, larger = farther), independent of
  // how UseReversedZ encodes that as a host depth value - it must be re-mapped the same way the
  // projection matrix and depth compare function are, or the buffer clears to the wrong extreme
  // (verified directly: matches upstream aurora's clear_depth_value, which does this same inversion
  // and was the second missing piece alongside to_compare_function's compare-op inversion).
  const float normalizedDepth =
      std::min(static_cast<float>(g_gxState.clearDepth) / 16777216.f, 16777215.f / 16777216.f);
  return UseReversedZ ? (1.f - normalizedDepth) : normalizedDepth;
}

inline bool render_target_has_alpha(GXPixelFmt pixelFmt) noexcept { return pixelFmt == GX_PF_RGBA6_Z24; }

inline float indirect_matrix_scale_multiplier(s8 scaleExp) {
  return std::exp2f(static_cast<float>(scaleExp));
}

inline s32 indirect_matrix_mantissa(float value) noexcept {
  return static_cast<s32>(std::lround(value * 1024.0f));
}

inline s32 indirect_matrix_shift(s8 scaleExp) noexcept { return -static_cast<s32>(scaleExp); }

static inline bool is_palette_format(u32 fmt) { return fmt == GX_TF_C4 || fmt == GX_TF_C8 || fmt == GX_TF_C14X2; }
static inline bool is_depth_format(u32 fmt) {
  return fmt == GX_TF_Z8 || fmt == GX_TF_Z16 || fmt == GX_TF_Z24X8 || fmt == GX_CTF_Z4 || fmt == GX_CTF_Z8M ||
         fmt == GX_CTF_Z8L || fmt == GX_CTF_Z16L;
}
static inline bool copy_texture_format_compatible(GXTexFmt copyFmt, u32 sampleFmt) noexcept {
  if (static_cast<u32>(copyFmt) == sampleFmt) {
    return true;
  }

  switch (copyFmt) {
  case GX_CTF_R4:
    return sampleFmt == GX_TF_I4;
  case GX_CTF_R8:
  case GX_CTF_G8:
  case GX_CTF_B8:
  case GX_CTF_A8:
    return sampleFmt == GX_TF_I8 || sampleFmt == GX_TF_A8;
  case GX_CTF_RA4:
    return sampleFmt == GX_TF_IA4;
  case GX_CTF_RA8:
  case GX_CTF_RG8:
  case GX_CTF_GB8:
  case GX_TF_Z16:
    return sampleFmt == GX_TF_IA8;
  case GX_CTF_YUVA8:
    return sampleFmt == GX_TF_RGBA8;
  case GX_TF_Z24X8:
    return sampleFmt == GX_TF_RGBA8;
  default:
    return false;
  }
}

struct AttrConfig {
  u8 attrType = GX_NONE; // GXAttrType
  u8 cnt = 0xFF;         // Actual count; not GXCompCnt
  u8 compType = 0xFF;    // GXCompType
  u8 offset = 0;         // Offset within vertex
  u8 stride = 0;         // Array stride
  u8 frac = 0;
  bool le = true;
  u8 nrmIndexCount = 0;  // GX_NRM_NBT3 stores three separate normal/tangent/binormal indices.
};
struct ShaderConfig {
  u8 fogType = GX_FOG_NONE;
  u8 vtxStride = 0;
  u8 lineMode : 2 = 0; // 1 = GX_LINES, 2 = GX_LINESTRIP, 3 = GX_POINTS
  u8 dualTexEnabled : 1 = 0;
  u8 fogRangeAdjust : 1 = 0;
  u8 pad1 : 4 = 0;
  u8 numTexGens = 0;
  u32 zTexture = 0; // bias[0:23], format[24:25], op[26:27]; 0 disables shader depth output.
  std::array<AttrConfig, MaxVtxAttr> attrs;
  std::array<TevSwap, MaxTevSwap> tevSwapTable;
  std::array<TevStage, MaxTevStages> tevStages;
  u32 tevStageCount = 0;
  std::array<ColorChannelConfig, MaxColorChannels> colorChannels;
  std::array<TcgConfig, MaxTexCoord> tcgs;
  AlphaCompare alphaCompare;
  std::array<IndStage, MaxIndStages> indStages{};
  u32 numIndStages = 0;

  bool operator==(const ShaderConfig& rhs) const { return memcmp(this, &rhs, sizeof(*this)) == 0; }
};
static_assert(std::has_unique_object_representations_v<ShaderConfig>);

// GX supplies an opaque white color when a draw omits vertex colors.
inline int shader_vertex_color_attr(const ShaderConfig& config, u32 channel) noexcept {
  const bool hasColor0 = config.attrs[GX_VA_CLR0].attrType != GX_NONE;
  const bool hasColor1 = config.attrs[GX_VA_CLR1].attrType != GX_NONE;
  if (channel == 0) {
    if (hasColor0) {
      return GX_VA_CLR0;
    }
    if (hasColor1) {
      return GX_VA_CLR1;
    }
  } else if (channel == 1 && hasColor0 && hasColor1) {
    return GX_VA_CLR1;
  }
  return -1;
}

// The indirect TEV coordinate is persistent across stages.
inline bool tev_stage_needs_fixed_texcoord_state(const ShaderConfig& config, u32 stageIdx) noexcept {
  if (stageIdx >= config.tevStageCount) {
    return false;
  }

  const auto& stage = config.tevStages[stageIdx];
  const bool hasCoordOperation = stage.indTexMtxId != GX_ITM_OFF || stage.indTexWrapS != GX_ITW_OFF ||
                                 stage.indTexWrapT != GX_ITW_OFF || stage.indTexAddPrev;
  const bool feedsNextAddPrev =
      stageIdx + 1 < config.tevStageCount && config.tevStages[stageIdx + 1].indTexAddPrev;
  return hasCoordOperation || feedsNextAddPrev;
}

inline bool tev_stage_has_texture_map(const TevStage& stage) noexcept {
  return stage.texMapId != GX_TEXMAP_NULL && stage.texMapId < MaxTextures;
}

// Returns -1 for GX_ITW_OFF, an integer fixed-point mask for the wrapping modes, and 0 for GX_ITW_0.
inline int tev_indirect_wrap_mask(GXIndTexWrap wrap) noexcept {
  switch (wrap) {
  case GX_ITW_OFF:
    return -1;
  case GX_ITW_256:
    return (256 << 7) - 1;
  case GX_ITW_128:
    return (128 << 7) - 1;
  case GX_ITW_64:
    return (64 << 7) - 1;
  case GX_ITW_32:
    return (32 << 7) - 1;
  case GX_ITW_16:
    return (16 << 7) - 1;
  case GX_ITW_0:
    return 0;
  default:
    return underlying(wrap) >= underlying(GX_ITW_0) ? 0 : -2;
  }
}

inline constexpr s32 tev_s24_wrap(s32 value) noexcept {
  const u32 wrapped = static_cast<u32>(value) & 0x00ffffffu;
  return (wrapped & 0x00800000u) != 0 ? static_cast<s32>(wrapped) - 0x01000000
                                      : static_cast<s32>(wrapped);
}

// GX falls back to texcoord 0 when a TEV order names GX_TEXCOORD_NULL or a coordinate beyond the configured texgen count.
inline int tev_effective_texcoord(const ShaderConfig& config, GXTexCoordID texCoordId) noexcept {
  if (config.numTexGens == 0) {
    return -1;
  }

  const u32 rawTexCoord = underlying(texCoordId);
  return rawTexCoord < config.numTexGens && rawTexCoord < MaxTexCoord ? static_cast<int>(rawTexCoord) : 0;
}

inline bool tev_stage_combiner_uses_texture(const TevStage& stage) noexcept {
  const auto& color = stage.colorPass;
  const auto& alpha = stage.alphaPass;
  return color.a == GX_CC_TEXC || color.a == GX_CC_TEXA || color.b == GX_CC_TEXC ||
         color.b == GX_CC_TEXA || color.c == GX_CC_TEXC || color.c == GX_CC_TEXA ||
         color.d == GX_CC_TEXC || color.d == GX_CC_TEXA || alpha.a == GX_CA_TEXA ||
         alpha.b == GX_CA_TEXA || alpha.c == GX_CA_TEXA || alpha.d == GX_CA_TEXA;
}

struct TevStageTextureDependency {
  int texCoordId = -1;
  int texMapId = -1;
  bool needsFixedTexcoordState = false;
  bool combinerUsesTexture = false;
  bool canSampleTexture = false;
};

inline bool tev_texture_sample_enabled(const TevStageTextureDependency& dependency,
                                       bool sampleRequested) noexcept {
  return sampleRequested && dependency.canSampleTexture;
}

// Pure TEV order/dependency analysis used by both ShaderInfo and WGSL generation.
inline TevStageTextureDependency tev_stage_texture_dependency(const ShaderConfig& config,
                                                               u32 stageIdx) noexcept {
  if (stageIdx >= config.tevStageCount) {
    return {};
  }

  const auto& stage = config.tevStages[stageIdx];
  TevStageTextureDependency dependency{
      .texCoordId = tev_effective_texcoord(config, stage.texCoordId),
      .texMapId = tev_stage_has_texture_map(stage) ? static_cast<int>(underlying(stage.texMapId)) : -1,
      .needsFixedTexcoordState = tev_stage_needs_fixed_texcoord_state(config, stageIdx),
      .combinerUsesTexture = tev_stage_combiner_uses_texture(stage),
  };
  dependency.canSampleTexture = dependency.texCoordId >= 0 && dependency.texMapId >= 0;
  return dependency;
}

inline bool shader_uses_fixed_texcoord_state(const ShaderConfig& config) noexcept {
  for (u32 i = 0; i < config.tevStageCount; ++i) {
    if (tev_stage_texture_dependency(config, i).needsFixedTexcoordState) {
      return true;
    }
  }
  return false;
}

inline bool tev_z_texture_enabled(const ShaderConfig& config) noexcept {
  return ((config.zTexture >> 26) & 0x3u) != GX_ZT_DISABLE;
}

inline int tev_z_texture_stage(const ShaderConfig& config) noexcept {
  if (!tev_z_texture_enabled(config)) {
    return -1;
  }
  for (int i = static_cast<int>(config.tevStageCount) - 1; i >= 0; --i) {
    if (tev_stage_texture_dependency(config, static_cast<u32>(i)).canSampleTexture) {
      return i;
    }
  }
  return -1;
}

struct PipelineConfig;

struct GXBindGroups {
  gfx::BindGroupRef textureBindGroup;
  // Bind group resolved at draw-build time so that gx::render does not have to hash-map the ref again for every draw.
  WGPUBindGroup resolvedTextureBindGroup = nullptr;
};
// Which matrix-memory slots a generated shader can actually read, and where each one lives in the compacted uniform arrays.
struct UniformMatrixLayout {
  // `postexSlots`/`nrmSlots` entry meaning "the matrix selected by the current matrix index", which is only known per draw.
  static constexpr u8 kCurrentPnMtx = 0xFE;
  // `postexRemap` entry for a slot that this shader never reads.
  static constexpr u8 kAbsent = 0xFF;

  // Compact index -> absolute postex slot (or kCurrentPnMtx).
  std::array<u8, MaxPostexMtx> postexSlots{};
  // Absolute postex slot -> compact index (or kAbsent).
  std::array<u8, MaxPostexMtx> postexRemap{};
  // Compact index -> absolute position slot (or kCurrentPnMtx).
  std::array<u8, MaxPnMtx> nrmSlots{};
  u8 postexCount = 0;
  u8 nrmCount = 0;
  // Position slots 0..MaxPnMtx-1 are uploaded 1:1 at compact indices 0..9, so `current_pnmtx` and any per-vertex index need no remapping.
  bool absolutePosRegion = false;
};
// Output info from shader generation
struct ShaderInfo {
  std::bitset<MaxTexCoord> sampledTexCoords;
  std::bitset<MaxTextures> sampledTextures;
  std::bitset<MaxKColors> sampledKColors;
  std::bitset<MaxColorChannels / 2> sampledColorChannels;
  // TEV color and alpha outputs are independently addressable.
  std::bitset<MaxTevRegs> loadsTevRegRgb;
  std::bitset<MaxTevRegs> loadsTevRegAlpha;
  std::bitset<MaxTevRegs> writesTevRegRgb;
  std::bitset<MaxTevRegs> writesTevRegAlpha;
  std::bitset<MaxPTTexMtx> usesPTTexMtx;
  std::bitset<MaxVtxAttr> indexAttr;
  std::bitset<MaxIndStages> usedIndStages;
  std::bitset<MaxTextures> sampledIndTextures;
  std::bitset<MaxIndTexMtxs> usedIndTexMtxs;
  UniformMatrixLayout matrixLayout;
  u32 uniformSize = 0;
  bool usesFog : 1 = false;
  bool lightingEnabled : 1 = false;
  u8 lineMode : 2 = 0;
};
struct BindGroupRanges {
  std::array<gfx::Range, MaxIndexAttr> vaRanges{};
};
void populate_pipeline_config(PipelineConfig& config, GXPrimitive primitive, GXVtxFmt fmt) noexcept;
wgpu::RenderPipeline build_pipeline(const PipelineConfig& config, ArrayRef<wgpu::VertexBufferLayout> vtxBuffers,
                                    wgpu::ShaderModule shader, const char* label) noexcept;
wgpu::ShaderModule build_shader(const ShaderConfig& config) noexcept;
GXBindGroups build_bind_groups(const ShaderInfo& info) noexcept;
void notify_copy_texture_created() noexcept;

u8 comp_type_size(GXAttr attr, GXCompType type) noexcept;
u8 comp_cnt_count(GXAttr attr, GXCompCnt cnt) noexcept;
} // namespace aurora::gx
