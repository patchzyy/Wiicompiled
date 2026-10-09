// sonic_render.h - Sonic's model for the Mario Kart Wii integration.
//
// Owns SonicCore (loaded from the player's own Sonic Adventure DX files), poses
// Sonic for the two situations Mario Kart Wii needs (driving and standing in the
// menus) and turns a pose into plain triangle batches. Two consumers use the
// batches: the GX draw hook (sonic_draw_hook.cpp) and the software rasterizer
// below, which renders the roster icons.
//
// Pure host code: no guest memory, no GX. It can be built and tested on its own.
#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include "soniccore/sonic.h"

namespace sonic_mkw {

// Sonic's "canonical" space used by everything below: Y up, facing +Z, the
// pose's lowest point at y = 0, centred on x = z = 0, SADX units.
enum class SonicPose {
    Driving,   // seated, legs forward, hands on a wheel
    Standing,  // menus and award ceremony
};

struct MeshVertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    uint32_t rgba;  // vertex colour (R in the low byte); only used when `vertexColor`
};

struct MeshBatch {
    int texture = -1;       // index into the texture set, -1 = untextured
    int textureSet = 0;     // sonic::TEXSET_SONIC or TEXSET_EFFECTS
    uint32_t tint = 0xFFFFFFFFu;  // RGBA multiplier (R in the low byte)
    float alpha = 1.0f;     // fade
    bool env = false;       // environment mapped: UVs from the view-space normal
    bool blend = false;     // alpha blended (drawn after the opaque batches)
    bool alphaTest = false; // texture with cut-out alpha
    bool lit = true;
    bool vertexColor = false;
    int wrapS = 1;          // GX wrap mode: 0 clamp, 1 repeat, 2 mirror
    int wrapT = 1;
    int srcBlend = 4;       // Ninja blend factors (see soniccore render_types.h)
    int dstBlend = 5;
    float diffuse[4] = {1, 1, 1, 1};
    float boost = 1.0f;
    std::vector<MeshVertex> tris;  // triangle list
};

struct PosedSonic {
    std::vector<MeshBatch> batches;
    std::vector<sonic::Vec3> joints;  // node origins of the pose (canonical space)
    sonic::Vec3 boxMin, boxMax;       // of the vertices
    sonic::Vec3 jointMin, jointMax;   // of the joints
};

struct RgbaImage {
    int width = 0;
    int height = 0;
    std::vector<uint8_t> rgba;  // tightly packed, straight alpha
};

// Vertex shading shared by the icon renderer and the in-game draw so both look
// the same: a key light from the camera's upper left plus a soft fill.
// `nView` is the view-space normal. Returns RGBA8 (R in the low byte).
uint32_t ShadeVertex(const MeshBatch& batch, const MeshVertex& v, const float nView[3]);

class SonicResources {
public:
    static SonicResources& Get();

    // Loads SonicCore from `configured` (SADX install or SonicAssets folder) or,
    // when it is empty, from the usual places next to the program and config file.
    bool Load(const std::string& configured, const std::vector<std::string>& fallbacks);
    bool Ready() const { return ready_; }
    const std::string& Error() const { return error_; }
    const std::string& Folder() const { return folder_; }

    const std::vector<sonic::TextureImage>& Textures(int set = sonic::TEXSET_SONIC) const;
    const sonic::Assets& Assets() const { return assets_; }

    // Poses Sonic (thread-safe, serialised). `time` is in 60 Hz frames.
    bool Pose(SonicPose pose, float time, PosedSonic& out);

    // Any raw SONIC_ACTIONS entry (tools); same normalisation as Pose().
    bool PoseAction(int action, float frame, PosedSonic& out);
    int ActionCount() const { return ready_ ? model_.actionCount() : 0; }
    // Tuning knobs for the driving pose (tools).
    static void SetDrivingMix(int armAction, float armFrame, int headAction);
    static void SetDrivingMixMode(int mode);
    static void SetDrivingMirror(int legs, int arms);

    std::mutex& Mutex() { return mutex_; }

private:
    bool PoseLocked(int action, float frame, PosedSonic& out);
    bool PoseDrivingLocked(float frame, PosedSonic& out);
    bool FinishPose(const sonic::DrawList& list, PosedSonic& out);
    bool ready_ = false;
    std::string error_;
    std::string folder_;
    sonic::Assets assets_;
    sonic::CharacterModel model_;
    std::mutex mutex_;
};

// Turns a SonicCore draw list into batches, transforming every vertex by
// `toWorld` (no normalisation; for drawing live characters).
void BuildBatches(const sonic::DrawList& list, const sonic::Mat4& toWorld, PosedSonic& out);

// ---- icons ------------------------------------------------------------------------
struct IconStyle {
    float yawDegrees = 22.0f;     // camera position around Sonic (0 = straight on)
    float pitchDegrees = 6.0f;    // camera height
    float outlinePixels = 0.0f;   // 0 = automatic (proportional to the icon size)
    uint32_t outlineRgba = 0xFF140A0Au;  // R in the low byte
    bool bust = true;             // head and shoulders instead of the full body
    float bustFraction = 0.46f;   // how much of Sonic's height (from the top) a bust frames
    float sideCrop = 0.86f;       // fraction of the bust's width that must fit
};
// Renders Sonic's portrait into a width x height image with a transparent background.
bool RenderSonicIcon(int width, int height, const IconStyle& style, RgbaImage& out);

// Orthographic software rasterizer over posed batches (exposed for tools/tests).
// `view` maps canonical space to view space (camera looks down -Z); the window
// [left,right]x[bottom,top] in view space fills the image.
void RasterizeSonic(const PosedSonic& posed, const sonic::Mat4& view, float left, float right, float bottom,
                    float top, int width, int height, int supersample, RgbaImage& out);

}  // namespace sonic_mkw
