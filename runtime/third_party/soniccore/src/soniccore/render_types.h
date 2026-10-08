// SonicCore rendering contract.
//
// SonicCore never talks to a graphics API. Each frame it fills a DrawList with
// mesh parts (Ninja models already converted to triangle lists) plus a world
// matrix, and you draw them with your own renderer. Textures are handed over
// once as RGBA8 images (Assets::textures()).
#pragma once
#include "formats/ninja.h"
#include "formats/pvr.h"

namespace sonic {

// Texture sets SonicCore draws with
enum TextureSetId : int {
    TEXSET_SONIC = 0,    // SONIC.PVM (body, eyes, the DX spin ball's stx_newspin)
    TEXSET_EFFECTS = 1,  // SON_EFF.PVM (homing/spin dash streak, afterimages)
    TEXSET_COUNT
};

struct TextureImage {
    std::string name;
    Image image;
};

struct DrawItem {
    NjModel* model = nullptr;  // vertices/indices/parts/materials; pointer stays valid while the Sonic lives
    Mat4 world;                // model -> world
    int textureSet = TEXSET_SONIC;
    int forceTexture = -1;     // >= 0: every material uses this texture of the set instead of its own texId
    float alpha = 1.0f;        // extra alpha multiplier (fading effects)
    u32 tint = 0xFFFFFFFF;     // RGBA multiplier, R in the low byte (additive effects fade through it)
    float lightBoost = 1.0f;   // lighting multiplier (Sonic is drawn a little brighter than the level)
};

struct DrawList {
    std::vector<DrawItem> items;
    void clear() { items.clear(); }
};

// ---- how to interpret NjMaterial (Ninja / SADX material flags) ----------------------
// diffuse: ARGB8888 colour multiplied with the texture (and lighting unless ignoreLight).
// Vertex colours (NjMeshPart::hasVColor) replace the diffuse colour and disable lighting.
// Blend factors index this table (src = srcBlend(), dst = dstBlend()):
//   0 ZERO, 1 ONE, 2 OTHER_COLOR, 3 INV_OTHER_COLOR, 4 SRC_ALPHA, 5 INV_SRC_ALPHA, 6 DST_ALPHA, 7 INV_DST_ALPHA
// Only materials with useAlpha() need sorting/blending; the rest are opaque.
// env(): environment-mapped; derive UVs from the view-space normal: uv = n.xy * (0.5, -0.5) + 0.5
struct MaterialInfo {
    u32 flags;
    explicit MaterialInfo(const NjMaterial& m) : flags(m.flags) {}
    bool useTexture() const { return (flags & NJD_FLAG_USE_TEXTURE) != 0; }
    bool useAlpha() const { return (flags & NJD_FLAG_USE_ALPHA) != 0; }
    bool env() const { return (flags & NJD_FLAG_USE_ENV) != 0; }
    bool doubleSided() const { return (flags & NJD_FLAG_DOUBLE_SIDE) != 0; }
    bool ignoreLight() const { return (flags & NJD_FLAG_IGNORE_LIGHT) != 0; }
    bool clampU() const { return (flags & NJD_FLAG_CLAMP_U) != 0; }
    bool clampV() const { return (flags & NJD_FLAG_CLAMP_V) != 0; }
    bool flipU() const { return (flags & NJD_FLAG_FLIP_U) != 0; }
    bool flipV() const { return (flags & NJD_FLAG_FLIP_V) != 0; }
    int srcBlend() const { return int((flags >> 29) & 7); }
    int dstBlend() const { return int((flags >> 26) & 7); }
};

}  // namespace sonic
