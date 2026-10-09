// sonic_draw_hook.cpp - draw-side half of the Sonic integration (see sonic_mkw.h).
//
// nw4r::g3d::DrawResMdlDirectly (0x80069000) draws every g3d model in the game:
//   r3 ResMdl (by value: a pointer to a word holding the MDL0 address)
//   r4 viewPosMtxArray   MTX34[] (big-endian), indexed by node matrix ID
//   r5 viewNrmMtxArray, r6 viewTexMtxArray
//   r7 byteCodeOpa, r8 byteCodeXlu (the pass being drawn is the non-null one)
//   r9 DrawResMdlReplacement*, r10 drawMode
// The wrapper keeps the original translated function for every model, except:
//   * in races, near a Sonic player's (shrunk) kart: the base character's driver
//     model is skipped and, once per camera and frame, Sonic is drawn running
//     (SonicRacer's posed batches, world space, through the current camera
//     matrix, G3DState::GetCameraMtxPtr);
//   * in the menus while Sonic is picked: the base character's models (recognised
//     by geometry fingerprint, see PatchDisc) are drawn as Sonic instead, posed
//     seated or standing to match the model's skeleton.
// Lighting is baked per vertex on the CPU; the GX state it changes is handed back
// to g3d with G3DState::Invalidate.
#include "abi_bridge.h"
#include "hle/gx/gx_internal.h"
#include "memory.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "sonic/sonic_formats.h"
#include "sonic/sonic_game.h"
#include "sonic/sonic_guest.h"
#include "sonic/sonic_mkw.h"
#include "sonic/sonic_place.h"
#include "sonic/sonic_render.h"

#include <chrono>
#include <cmath>
#include <map>
#include <unordered_map>

#ifndef RT_TAG_SONIC
#define RT_TAG_SONIC "sonic"
#endif

extern "C" void func_80069000(CpuContext* ctx);

namespace sonic_mkw {
namespace draw_detail {

using sonic::Mat4;
using sonic::Vec3;

constexpr uint32_t kG3DStateInvalidate = 0x80064450u;  // nw4r::g3d::G3DState::Invalidate(u32 flags)
constexpr uint32_t kG3DStateCameraMtx = 0x80064180u;   // nw4r::g3d::G3DState::GetCameraMtxPtr()
constexpr uint32_t kInvalidateAll = 0x7FFu;
constexpr uint32_t kMdl0Magic = 0x4D444C30u;  // "MDL0"
// Sonic is drawn this much smaller than the driver he replaces: SADX Sonic's
// head and shoes make him bulkier than Mario Kart's humans at the same height.
constexpr float kSizeRatio = 0.92f;

struct ModelEntry {
    uint32_t size = 0;
    uint8_t head[64] = {};
    uint8_t middle[32] = {};
    bool isBase = false;  // one of the base character's models
    Mdl0Summary summary;
    uint32_t rootMatrix = 0;  // matrix ID of the first node
    DriverState state;
    bool drawnThisFrame = true;  // last opaque pass drew Sonic
};

std::unordered_map<uint32_t, ModelEntry> g_models;

// ---- guest memory helpers ----------------------------------------------------------

bool ReadMtx34(uint32_t addr, Mat4& out) {
    if (!Memory::Contains(addr, 48)) return false;
    const uint8_t* p = Memory::GetPointer(addr, 48);
    if (!p) return false;
    out = Mat4();
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) out.at(r, c) = BeReadF32(p + (r * 4 + c) * 4);
    }
    for (int i = 0; i < 16; ++i) {
        if (!std::isfinite(out.m[i])) return false;
    }
    return true;
}

uint32_t ResolveMdl0(uint32_t r3) {
    if (r3 && Memory::Contains(r3, 4)) {
        if (Memory::Read32(r3) == kMdl0Magic) return r3;
        const uint32_t inner = Memory::Read32(r3);
        if (inner && Memory::Contains(inner, 4) && Memory::Read32(inner) == kMdl0Magic) return inner;
    }
    return 0;
}

const uint8_t* MdlBytes(uint32_t mdl, size_t& avail) {
    if (!Memory::Contains(mdl, 0x50)) return nullptr;
    const uint32_t size = Memory::Read32(mdl + 4);
    if (size < 0x50 || size > 0x2000000 || !Memory::Contains(mdl, size)) return nullptr;
    // Node names live in the BRRES string table right after the model.
    avail = Memory::Contains(mdl, size_t(size) + 0x10000) ? size_t(size) + 0x10000 : size_t(size);
    return Memory::GetPointer(mdl, avail);
}

ModelEntry* LookupModel(uint32_t mdl) {
    size_t avail = 0;
    const uint8_t* bytes = MdlBytes(mdl, avail);
    if (!bytes) return nullptr;
    const uint32_t size = BeRead32(bytes + 4);
    auto it = g_models.find(mdl);
    if (it != g_models.end()) {
        ModelEntry& e = it->second;
        if (e.size == size && std::memcmp(e.head, bytes, sizeof(e.head)) == 0 &&
            std::memcmp(e.middle, bytes + size / 2, sizeof(e.middle)) == 0) {
            return &e;
        }
        g_models.erase(it);  // something else was loaded at this address
    }
    ModelEntry e;
    e.size = size;
    std::memcpy(e.head, bytes, sizeof(e.head));
    std::memcpy(e.middle, bytes + size / 2, sizeof(e.middle));
    const uint64_t print = ModelSwapActive() ? Mdl0Fingerprint(bytes, avail) : 0;
    e.isBase = print != 0 && IsBaseModelFingerprint(print);
    Mdl0Summarize(bytes, avail, e.summary);
    if (!e.summary.nodes.empty()) e.rootMatrix = e.summary.nodes[0].matrixId;
    if (DebugLogging() && e.isBase) {
        const Mdl0Summary& s = e.summary;
        std::string firstNodes;
        for (size_t i = 0; i < s.nodes.size() && i < 6; ++i) firstNodes += " " + s.nodes[i].name;
        RT_LOGF(RT_TAG_SONIC, "model 0x%08X size %u nodes %zu fingerprint %016llx (base character):%s\n", mdl, size,
                s.nodes.size(), static_cast<unsigned long long>(print), firstNodes.c_str());
    }
    if (g_models.size() > 4096) g_models.clear();
    return &g_models.emplace(mdl, std::move(e)).first->second;
}

// ---- Sonic's poses ------------------------------------------------------------------

struct PoseCache {
    PosedSonic posed;
    int key = -1;
    bool ok = false;
};

float TicksNow() {
    static const auto start = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::steady_clock::now() - start;
    return float(std::chrono::duration<double>(elapsed).count() * 60.0);
}

const PosedSonic* SonicPose_(SonicPose pose) {
    static PoseCache driving, standing;
    PoseCache& cache = pose == SonicPose::Driving ? driving : standing;
    const float now = TicksNow();
    const int key = pose == SonicPose::Driving ? 0 : int(now);
    if (cache.key != key) {
        cache.ok = SonicResources::Get().Pose(pose, now, cache.posed);
        cache.key = key;
    }
    return cache.ok ? &cache.posed : nullptr;
}

float SonicStandingHeight() {
    static float height = 0;
    if (height <= 0) {
        PosedSonic posed;
        if (SonicResources::Get().Pose(SonicPose::Standing, 0.0f, posed)) height = posed.boxMax.y - posed.boxMin.y;
        if (height <= 0) height = 10.0f;
    }
    return height;
}

// ---- placement ----------------------------------------------------------------------

struct Placement {
    Mat4 sonicToView;
    SonicPose pose = SonicPose::Driving;
    bool ok = false;
};

Placement Place(ModelEntry& model, uint32_t viewPosArray) {
    Placement out;
    const auto& nodes = model.summary.nodes;
    if (nodes.empty()) return out;
    Mat4 root;
    if (!ReadMtx34(viewPosArray + nodes[0].matrixId * 48u, root)) return out;
    std::vector<Vec3> joints;
    joints.reserve(nodes.size());
    for (size_t i = 1; i < nodes.size(); ++i) {
        Mat4 m;
        if (ReadMtx34(viewPosArray + nodes[i].matrixId * 48u, m)) joints.push_back(m.translation());
    }
    DriverFrame frame;
    frame.root = root;
    frame.jointsView = std::move(joints);
    if (model.summary.boxValid) frame.bindHeight = model.summary.boxMax[1] - model.summary.boxMin[1];
    const DriverPlacement placed =
        PlaceSonicOnDriver(frame, model.state, kSizeRatio * RuntimeConfigFile::SonicScale(), SonicPose_,
                           SonicStandingHeight());
    out.sonicToView = placed.sonicToView;
    out.pose = placed.pose;
    out.ok = placed.ok;
    return out;
}

// ---- GX ---------------------------------------------------------------------------

GXTexObj* TextureFor(int set, int texture, int wrapS, int wrapT) {
    static std::map<int, GXTexObj> objects;
    const auto& textures = SonicResources::Get().Textures(set);
    if (texture < 0 || size_t(texture) >= textures.size()) return nullptr;
    const int key = (set * 4096 + texture) * 16 + wrapS * 4 + wrapT;
    auto it = objects.find(key);
    if (it == objects.end()) {
        const sonic::Image& image = textures[size_t(texture)].image;
        if (image.width <= 0 || image.rgba.empty()) return nullptr;
        GXTexObj obj{};
        GXInitTexObj(&obj, image.rgba.data(), u16(image.width), u16(image.height), GX_TF_RGBA8_PC,
                     GXTexWrapMode(wrapS), GXTexWrapMode(wrapT), GX_FALSE);
        GXInitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
        it = objects.emplace(key, obj).first;
    }
    return &it->second;
}

void SetCommonState() {
    static const float kIdentity[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
    GXLoadPosMtxImm(const_cast<float(*)[4]>(kIdentity), GX_PNMTX0);
    GXLoadNrmMtxImm(const_cast<float(*)[4]>(kIdentity), GX_PNMTX0);
    GXSetCurrentMtx(GX_PNMTX0);

    GXSetNumChans(1);
    GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetChanCtrl(GX_COLOR1A1, GX_FALSE, GX_SRC_REG, GX_SRC_REG, 0, GX_DF_NONE, GX_AF_NONE);
    GXSetNumTexGens(1);
    GXSetTexCoordGen2(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY, GX_FALSE, GX_PTIDENTITY);
    GXSetNumIndStages(0);
    GXSetTevDirect(GX_TEVSTAGE0);
    GXSetNumTevStages(1);
    GXSetTevSwapModeTable(GX_TEV_SWAP0, GX_CH_RED, GX_CH_GREEN, GX_CH_BLUE, GX_CH_ALPHA);
    GXSetTevSwapMode(GX_TEVSTAGE0, GX_TEV_SWAP0, GX_TEV_SWAP0);
    GXSetZTexture(GX_ZT_DISABLE, GX_TF_Z8, 0);
    GXSetCullMode(GX_CULL_NONE);
    GXSetCoPlanar(GX_FALSE);
    GXSetColorUpdate(GX_TRUE);
}

void SetBatchState(const MeshBatch& batch) {
    GXTexObj* tex = batch.texture >= 0 ? TextureFor(batch.textureSet, batch.texture, batch.wrapS, batch.wrapT) : nullptr;
    if (tex) {
        GXLoadTexObj(tex, GX_TEXMAP0);
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    } else {
        GXSetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD_NULL, GX_TEXMAP_NULL, GX_COLOR0A0);
        GXSetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    }
    if (batch.alphaTest && tex) {
        GXSetAlphaCompare(GX_GEQUAL, 128, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_FALSE);
    } else {
        GXSetAlphaCompare(GX_ALWAYS, 0, GX_AOP_AND, GX_ALWAYS, 0);
        GXSetZCompLoc(GX_TRUE);
    }
    if (batch.blend) {
        // Ninja blend factor indices line up with GXBlendFactor.
        GXSetBlendMode(GX_BM_BLEND, GXBlendFactor(batch.srcBlend), GXBlendFactor(batch.dstBlend), GX_LO_CLEAR);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_FALSE);
    } else {
        GXSetBlendMode(GX_BM_NONE, GX_BL_ONE, GX_BL_ZERO, GX_LO_CLEAR);
        GXSetZMode(GX_TRUE, GX_LEQUAL, GX_TRUE);
    }
}

void EmitBatch(const MeshBatch& batch, const Mat4& toView, bool& drawOpen) {
    constexpr size_t kMaxVerts = 3 * 20000;
    size_t done = 0;
    const size_t total = batch.tris.size() - batch.tris.size() % 3;
    while (done < total) {
        const size_t count = std::min(kMaxVerts, total - done);
        GXBegin(GX_TRIANGLES, GX_VTXFMT7, u16(count));
        drawOpen = true;
        for (size_t i = done; i < done + count; ++i) {
            const MeshVertex& v = batch.tris[i];
            const Vec3 p = toView.transformPoint(Vec3(v.pos[0], v.pos[1], v.pos[2]));
            const Vec3 n = sonic::normalize(toView.transformDir(Vec3(v.nrm[0], v.nrm[1], v.nrm[2])));
            const float nv[3] = {n.x, n.y, n.z};
            const uint32_t c = ShadeVertex(batch, v, nv);
            GXPosition3f32(p.x, p.y, p.z);
            GXColor4u8(u8(c), u8(c >> 8), u8(c >> 16), u8(c >> 24));
            if (batch.env) {
                GXTexCoord2f32(n.x * 0.5f + 0.5f, -n.y * 0.5f + 0.5f);
            } else {
                GXTexCoord2f32(v.uv[0], v.uv[1]);
            }
        }
        GXEnd();
        drawOpen = false;
        done += count;
    }
}

void DrawPosed(const PosedSonic& posed, const Mat4& toView) {
    constexpr uint32_t kAttrCount = 26;
    std::array<GXAttrType, kAttrCount> savedDesc{};
    std::array<VtxAttrFmt, kAttrCount> savedFmt{};
    for (uint32_t i = 0; i < kAttrCount; ++i) {
        savedDesc[i] = g_hleGxState.vtxDesc[i];
        savedFmt[i] = g_hleGxState.vtxAttrFmt[GX_VTXFMT7][i];
    }

    // Like GX__DrawSphere: whatever happens while drawing, close an open GXBegin
    // and restore the vertex state below.
    bool drawOpen = false;
    try {
        EnsureAuroraFrameActive();
        GXMarkFrameWork();
        for (uint32_t i = 0; i < kAttrCount; ++i) g_hleGxState.vtxDesc[i] = GX_NONE;
        g_hleGxState.InvalidateVtxLayoutHash();
        GXClearVtxDesc();
        GX__SetVtxDesc_8016d3a4(GX_VA_POS, GX_DIRECT);
        GX__SetVtxDesc_8016d3a4(GX_VA_CLR0, GX_DIRECT);
        GX__SetVtxDesc_8016d3a4(GX_VA_TEX0, GX_DIRECT);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT7, GX_VA_POS, GX_POS_XYZ, GX_F32, 0);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT7, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT7, GX_VA_TEX0, GX_TEX_ST, GX_F32, 0);

        SetCommonState();
        for (int pass = 0; pass < 2; ++pass) {
            for (const MeshBatch& batch : posed.batches) {
                if (batch.blend != (pass == 1) || batch.tris.empty()) continue;
                SetBatchState(batch);
                EmitBatch(batch, toView, drawOpen);
            }
        }
    } catch (const std::exception& error) {
        if (drawOpen) GXEnd();
        static int reports = 0;
        if (reports++ < 8) RT_LOGF(RT_TAG_SONIC, "draw failed: %s\n", error.what());
    } catch (...) {
        if (drawOpen) GXEnd();
    }

    for (uint32_t i = 0; i < kAttrCount; ++i) g_hleGxState.vtxDesc[i] = GX_NONE;
    g_hleGxState.InvalidateVtxLayoutHash();
    GXClearVtxDesc();
    for (uint32_t attr = 0; attr < kAttrCount; ++attr) {
        if (savedDesc[attr] != GX_NONE) GX__SetVtxDesc_8016d3a4(attr, uint32_t(savedDesc[attr]));
    }
    for (uint32_t attr = GX_VA_POS; attr <= GX_VA_TEX7; ++attr) {
        const VtxAttrFmt& fmt = savedFmt[attr];
        GX__SetVtxAttrFmt_8016dc68(GX_VTXFMT7, attr, uint32_t(fmt.cnt), uint32_t(fmt.type), fmt.frac);
    }
    {
        // Our texture went to TEXMAP0 behind the HLE's back: make its binding cache
        // reload whatever the game binds there next.
        std::lock_guard<std::mutex> guard(g_texObjMutex);
        g_boundTexMaps[GX_TEXMAP0] = BoundTexInfo{};
    }
}

// Tell g3d its cached GX state is gone, so the next material reloads everything.
void InvalidateG3DState(CpuContext* ctx) {
    const CpuContext saved = *ctx;
    ctx->gpr[3] = kInvalidateAll;
    InvokeIndirectCpu(kG3DStateInvalidate, ctx);
    *ctx = saved;
}

// Menus: the base character's model drawn as Sonic.
bool TryDrawMenuSonic(CpuContext* ctx, ModelEntry& model) {
    const uint32_t viewPos = ctx->gpr[4];
    const uint32_t opa = ctx->gpr[7];
    if (!viewPos) return false;
    // Sonic is drawn whole in the opaque pass; the translucent pass is skipped
    // unless that failed and the original model was drawn instead.
    if (!opa) return model.drawnThisFrame;
    model.drawnThisFrame = false;
    const Placement placement = Place(model, viewPos);
    if (!placement.ok) return false;
    const PosedSonic* posed = SonicPose_(placement.pose);
    if (!posed) return false;
    DrawPosed(*posed, placement.sonicToView);
    InvalidateG3DState(ctx);
    model.drawnThisFrame = true;
    return true;
}

// ---- races ---------------------------------------------------------------------

struct RaceFrame {
    uint32_t frame = ~0u;
    std::vector<game::RacerDraw> racers;
    std::vector<std::pair<int, uint64_t>> drawn;  // (racer, camera) pairs drawn this frame
    int logs = 0;
};

RaceFrame& Race() {
    static RaceFrame race;
    return race;
}

uint64_t HashMatrix(const Mat4& m) {
    uint64_t h = 1469598103934665603ull;
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) {
            uint32_t bits;
            const float f = m.at(r, c);
            std::memcpy(&bits, &f, 4);
            h = (h ^ bits) * 1099511628211ull;
        }
    }
    return h;
}

bool CameraMatrix(CpuContext* ctx, Mat4& out) {
    const uint32_t mtx = guest::Call(ctx, kG3DStateCameraMtx);
    return mtx && ReadMtx34(mtx, out);
}

// Returns true when the model must not be drawn (Sonic's stand-in).
bool RaceModel(CpuContext* ctx, ModelEntry* model) {
    RaceFrame& race = Race();
    const uint32_t frame = game::FrameNumber();
    if (race.frame != frame) {
        race.frame = frame;
        game::RacersForDraw(race.racers);
        race.drawn.clear();
    }
    if (race.racers.empty() || !model) return false;
    const uint32_t viewPos = ctx->gpr[4];
    Mat4 root;
    if (!viewPos || !ReadMtx34(viewPos + model->rootMatrix * 48u, root)) return false;
    Mat4 camera;
    if (!CameraMatrix(ctx, camera)) return false;
    const Vec3 at = root.translation();
    for (const game::RacerDraw& racer : race.racers) {
        // The kart's models are posed either before or after Sonic moves the kart
        // in the frame: either position marks them.
        const float near = std::min(sonic::length(at - camera.transformPoint(racer.kartPos)),
                                    sonic::length(at - camera.transformPoint(racer.gamePos)));
        if (near > racer.hideRadius) continue;
        // Something of Sonic's stand-in kart: Sonic is drawn here, once per camera.
        if (ctx->gpr[7] && racer.posed && !racer.posed->batches.empty()) {
            const std::pair<int, uint64_t> key(racer.id, HashMatrix(camera));
            if (std::find(race.drawn.begin(), race.drawn.end(), key) == race.drawn.end()) {
                race.drawn.push_back(key);
                DrawPosed(*racer.posed, camera);
                InvalidateG3DState(ctx);
                if (DebugLogging() && race.logs < 4) {
                    ++race.logs;
                    RT_LOGF(RT_TAG_SONIC, "drawing Sonic (player %d) with model 0x%08X as the anchor\n", racer.id,
                            ctx->gpr[3]);
                }
            }
        }
        return racer.hideAll || model->isBase;
    }
    return false;
}

bool TryDraw(CpuContext* ctx) {
    const uint32_t mdl = ResolveMdl0(ctx->gpr[3]);
    if (!mdl) return false;
    ModelEntry* model = LookupModel(mdl);
    if (game::RaceActive()) return RaceModel(ctx, model);
    if (model && model->isBase && game::MenuShowsSonic()) return TryDrawMenuSonic(ctx, *model);
    return false;
}

}  // namespace draw_detail
}  // namespace sonic_mkw

// nw4r::g3d::DrawResMdlDirectly, with Sonic drawn in races and over the base character in menus.
extern "C" void nw4r_g3d_DrawResMdlDirectly_Sonic_80069000(CpuContext* ctx) {
    if (ctx && sonic_mkw::SonicResources::Get().Ready() && sonic_mkw::draw_detail::TryDraw(ctx)) {
        return;
    }
    func_80069000(ctx);
}

REGISTER_NATIVE_FUNCTION_AS(0x80069000, nw4r_g3d_DrawResMdlDirectly_Sonic_80069000,
                            "nw4r_g3d_DrawResMdlDirectly_Sonic_80069000");
