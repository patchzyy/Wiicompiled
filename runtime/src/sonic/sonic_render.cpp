// sonic_render.cpp - see sonic_render.h.
#include "sonic/sonic_render.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <map>
#include <tuple>

namespace sonic_mkw {

namespace render_detail {

using sonic::Mat4;
using sonic::Vec3;

// SONIC_ACTIONS entries used for the two poses (see soniccore anim_table.h).
struct PoseDef {
    int action;
    float frame;      // starting frame
    float speed;      // frames advanced per 60 Hz tick (0 = hold)
};
constexpr PoseDef kStandingPose = {1, 0.0f, 0.5f};  // SA_Stand idle loop
constexpr PoseDef kPortraitPose = {1, 0.0f, 0.0f};
// Driving is a mix of two SONIC_ACTIONS on the main skeleton: the body, head and
// legs of "sitting, legs forward" (108, SonicAnimData 122) and the arms of a
// seated "holding on" pose (89), which puts the hands forward at chest height.
// The right leg and right arm are then mirrored onto the left side so both feet
// reach forward and both hands meet on the wheel.
constexpr PoseDef kDrivingPose = {108, 0.0f, 0.0f};
int g_armAction = 89;
float g_armFrame = 0.0f;
int g_headAction = -1;
int g_mixMode = 0;  // 0: arms only (default), 1: all but legs, 2: all but legs and pelvis
// Roots of the arm subtrees of Sonic's main skeleton (73 nodes): left/right
// shoulder. Node 11 is the neck (head subtree).
constexpr int kMainSkeletonNodes = 73;
constexpr int kArmRoots[] = {24, 35};
constexpr int kHeadRoot = 11;
constexpr int kLegRoots[] = {46, 56};
constexpr int kLegNodes = 10;  // 46..55 mirror 56..65
constexpr int kArmNodes = 11;  // 24..34 mirror 35..45
int g_legMirror = 2;  // both legs forward, like the right leg of 108
int g_armMirror = 2;  // both hands together in front of the chest
// Hip and knee joints of both legs: the thighs point forward when seated, which
// gives the pose's facing (the sitting action itself is turned sideways).
constexpr int kHips[] = {47, 57};
constexpr int kKnees[] = {49, 59};

bool InSubtree(const sonic::NjSkeleton& sk, int node, int root) {
    for (int n = node; n >= 0; n = sk.nodes[size_t(n)].parent) {
        if (n == root) return true;
    }
    return false;
}

Vec3 V(const float* p) { return {p[0], p[1], p[2]}; }

// Folder names are UTF-8 (like SonicCore's); never the Windows ANSI code page.
std::filesystem::path Utf8Path(const std::string& text) {
#if defined(__cpp_char8_t)
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
#else
    return std::filesystem::u8path(text);
#endif
}

float Clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

}  // namespace render_detail

using render_detail::Mat4;
using render_detail::Vec3;

// =====================================================================================
// shading
// =====================================================================================

uint32_t ShadeVertex(const MeshBatch& batch, const MeshVertex& v, const float nView[3]) {
    float base[4];
    if (batch.vertexColor) {
        base[0] = float(v.rgba & 0xFF) / 255.0f;
        base[1] = float((v.rgba >> 8) & 0xFF) / 255.0f;
        base[2] = float((v.rgba >> 16) & 0xFF) / 255.0f;
        base[3] = float(v.rgba >> 24) / 255.0f;
    } else {
        std::memcpy(base, batch.diffuse, sizeof(base));
    }
    float light[3] = {1, 1, 1};
    if (batch.lit) {
        // Key light from the upper left in front of the camera, warm; cool fill
        // from the opposite side so the shadow side keeps Sonic's blue readable.
        static const float kKey[3] = {-0.42f, 0.70f, 0.58f};
        static const float kFill[3] = {0.62f, 0.10f, 0.30f};
        auto nd = [&](const float* l) {
            const float len = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
            return std::max(0.0f, (nView[0] * l[0] + nView[1] * l[1] + nView[2] * l[2]) / len);
        };
        const float key = nd(kKey);
        const float fill = nd(kFill);
        const float rim = std::pow(1.0f - std::max(0.0f, nView[2]), 3.0f) * 0.18f;
        // SADX draws characters brighter than the level (light boost 1.55); the
        // DX body texture is a deep navy that needs it to read as Sonic blue.
        const float boost = 0.85f + 0.15f * batch.boost;
        light[0] = (0.74f + 0.58f * key + 0.14f * fill + rim) * boost;
        light[1] = (0.74f + 0.56f * key + 0.15f * fill + rim) * boost;
        light[2] = (0.78f + 0.54f * key + 0.18f * fill + rim) * boost;
    }
    // DrawItem tint (RGBA, R in the low byte) and fade alpha.
    const float tint[4] = {float(batch.tint & 0xFF) / 255.0f, float((batch.tint >> 8) & 0xFF) / 255.0f,
                           float((batch.tint >> 16) & 0xFF) / 255.0f, float(batch.tint >> 24) / 255.0f};
    uint32_t out = 0;
    for (int i = 0; i < 3; ++i) {
        out |= uint32_t(render_detail::Clamp01(base[i] * light[i] * tint[i]) * 255.0f + 0.5f) << (i * 8);
    }
    out |= uint32_t(render_detail::Clamp01(base[3] * tint[3] * batch.alpha) * 255.0f + 0.5f) << 24;
    return out;
}

// =====================================================================================
// resources and posing
// =====================================================================================

SonicResources& SonicResources::Get() {
    static SonicResources instance;
    return instance;
}

bool SonicResources::Load(const std::string& configured, const std::vector<std::string>& fallbacks) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (ready_) return true;
    std::vector<std::string> candidates;
    if (!configured.empty()) {
        candidates.push_back(configured);
    } else {
        candidates = fallbacks;
    }
    std::string errors;
    for (const auto& folder : candidates) {
        std::error_code ec;
        if (folder.empty() || !std::filesystem::is_directory(render_detail::Utf8Path(folder), ec)) continue;
        sonic::Assets assets;
        if (!assets.load(folder)) {
            errors += folder + ": " + assets.error() + "\n";
            continue;
        }
        assets_ = std::move(assets);
        if (!model_.load(assets_)) {
            errors += folder + ": Sonic's model could not be read\n";
            continue;
        }
        folder_ = folder;
        ready_ = true;
        return true;
    }
    error_ = errors.empty() ? std::string("no Sonic Adventure DX files were found") : errors;
    return false;
}

const std::vector<sonic::TextureImage>& SonicResources::Textures(int set) const {
    return assets_.textures(set);
}

namespace render_detail {

// Batch key: everything that changes GX state.
using BatchKey = std::tuple<int, int, uint32_t, uint32_t, bool, float, uint32_t, float>;

void AppendItem(const sonic::DrawItem& item, const Mat4& canonical, std::map<BatchKey, size_t>& index,
                PosedSonic& out) {
    const sonic::NjModel& model = *item.model;
    const Mat4 world = canonical * item.world;
    const auto& textures = SonicResources::Get().Textures(item.textureSet);
    for (const sonic::NjMeshPart& part : model.parts) {
        if (part.material < 0 || part.material >= int(model.mats.size())) continue;
        const sonic::NjMaterial& mat = model.mats[size_t(part.material)];
        const sonic::MaterialInfo info(mat);
        const uint32_t texId = item.forceTexture >= 0 ? uint32_t(item.forceTexture) : mat.texId;
        const int texture = (info.useTexture() && texId < textures.size()) ? int(texId) : -1;
        const BatchKey key{item.textureSet, texture, mat.flags & 0xFFFF0000u, mat.diffuse, part.hasVColor,
                           item.lightBoost, item.tint, item.alpha};
        auto found = index.find(key);
        if (found == index.end()) {
            MeshBatch batch;
            batch.texture = texture;
            batch.textureSet = item.textureSet;
            batch.tint = item.tint;
            batch.alpha = item.alpha;
            batch.env = info.env();
            batch.blend = info.useAlpha() || item.alpha < 0.999f;
            batch.alphaTest = !batch.blend && texture >= 0 && textures[size_t(texture)].image.hasAlpha;
            batch.vertexColor = part.hasVColor;
            batch.lit = !(part.hasVColor || info.ignoreLight());
            batch.wrapS = info.clampU() ? 0 : (info.flipU() ? 2 : 1);
            batch.wrapT = info.clampV() ? 0 : (info.flipV() ? 2 : 1);
            batch.srcBlend = info.srcBlend();
            batch.dstBlend = info.dstBlend();
            batch.diffuse[0] = float((mat.diffuse >> 16) & 0xFF) / 255.0f;
            batch.diffuse[1] = float((mat.diffuse >> 8) & 0xFF) / 255.0f;
            batch.diffuse[2] = float(mat.diffuse & 0xFF) / 255.0f;
            batch.diffuse[3] = float(mat.diffuse >> 24) / 255.0f;
            batch.boost = item.lightBoost;
            found = index.emplace(key, out.batches.size()).first;
            out.batches.push_back(std::move(batch));
        }
        MeshBatch& batch = out.batches[found->second];
        for (uint32_t i = 0; i < part.indexCount; ++i) {
            const uint32_t vi = model.indices[part.firstIndex + i];
            if (vi >= model.verts.size()) continue;
            const sonic::NjVertex& src = model.verts[vi];
            const Vec3 p = world.transformPoint(src.pos);
            const Vec3 n = sonic::normalize(world.transformDir(src.nrm));
            MeshVertex v;
            v.pos[0] = p.x; v.pos[1] = p.y; v.pos[2] = p.z;
            v.nrm[0] = n.x; v.nrm[1] = n.y; v.nrm[2] = n.z;
            v.uv[0] = src.u;
            v.uv[1] = src.v;
            v.rgba = src.color;
            batch.tris.push_back(v);
        }
    }
    out.joints.push_back(world.transformPoint(Vec3(0, 0, 0)));
}

}  // namespace render_detail

bool SonicResources::Pose(SonicPose pose, float time, PosedSonic& out) {
    using namespace render_detail;
    std::lock_guard<std::mutex> lock(mutex_);
    out = PosedSonic{};
    if (!ready_) return false;

    PoseDef def = pose == SonicPose::Driving ? kDrivingPose : kStandingPose;
    if (pose == SonicPose::Standing && time < 0) def = kPortraitPose;
    if (!model_.hasAction(def.action)) def = kStandingPose;
    const float frames = model_.actionFrames(def.action);
    const float frame = std::fmod(def.frame + std::max(time, 0.0f) * def.speed, frames);
    if (pose == SonicPose::Driving) return PoseDrivingLocked(frame, out);
    return PoseLocked(def.action, frame, out);
}

void SonicResources::SetDrivingMixMode(int mode) { render_detail::g_mixMode = mode; }
void SonicResources::SetDrivingMirror(int legs, int arms) {
    render_detail::g_legMirror = legs;
    render_detail::g_armMirror = arms;
}

void SonicResources::SetDrivingMix(int armAction, float armFrame, int headAction) {
    render_detail::g_armAction = armAction;
    render_detail::g_armFrame = armFrame;
    render_detail::g_headAction = headAction;
}

bool SonicResources::PoseDrivingLocked(float frame, PosedSonic& out) {
    using namespace render_detail;
    const int skel = model_.actionSkeleton(kDrivingPose.action);
    const sonic::NjSkeleton* sk = model_.skeleton(skel);
    std::vector<sonic::NjLocal> body, arms, head;
    if (!sk || sk->nodes.size() != size_t(kMainSkeletonNodes) || model_.actionSkeleton(g_armAction) != skel ||
        !model_.actionLocals(kDrivingPose.action, frame, body) || !model_.actionLocals(g_armAction, g_armFrame, arms)) {
        return PoseLocked(kDrivingPose.action, frame, out);
    }
    const bool mixHead = g_headAction >= 0 && model_.actionSkeleton(g_headAction) == skel &&
                         model_.actionLocals(g_headAction, 0.0f, head);
    for (size_t n = 0; n < body.size(); ++n) {
        bool arm = false;
        for (int root : kArmRoots) arm = arm || InSubtree(*sk, int(n), root);
        bool leg = false;
        for (int root : kLegRoots) leg = leg || InSubtree(*sk, int(n), root);
        if (g_mixMode == 1) {
            if (!leg) body[n] = arms[n];
        } else if (g_mixMode == 2) {
            if (!leg && n > 1) body[n] = arms[n];
        } else if (arm) {
            body[n] = arms[n];
        }
        if (mixHead && InSubtree(*sk, int(n), kHeadRoot)) body[n] = head[n];
    }
    // Symmetry: copy one leg (and one arm) onto the other side, mirrored across
    // Sonic's sagittal plane (z -> -z: X and Y rotations and the Z offset flip).
    auto mirror = [&](int from, int to, int count) {
        for (int k = 0; k < count; ++k) {
            sonic::NjLocal m = body[size_t(from + k)];
            m.ang[0] = -m.ang[0];
            m.ang[1] = -m.ang[1];
            m.pos.z = -m.pos.z;
            body[size_t(to + k)] = m;
        }
    };
    if (g_legMirror == 1) mirror(kLegRoots[0], kLegRoots[1], kLegNodes);
    if (g_legMirror == 2) mirror(kLegRoots[1], kLegRoots[0], kLegNodes);
    if (g_armMirror == 1) mirror(kArmRoots[0], kArmRoots[1], kArmNodes);
    if (g_armMirror == 2) mirror(kArmRoots[1], kArmRoots[0], kArmNodes);

    // Turn the pose so the thighs point along the model's forward axis (-X).
    std::vector<Mat4> world;
    sonic::composePose(*sk, body, Mat4(), world);
    Vec3 thighs(0, 0, 0);
    for (int i = 0; i < 2; ++i) thighs += world[size_t(kKnees[i])].translation() - world[size_t(kHips[i])].translation();
    Mat4 root;
    if (thighs.x * thighs.x + thighs.z * thighs.z > 1e-6f) {
        // rotY(a) maps (x, z) to (x cos a + z sin a, -x sin a + z cos a); solve for -X.
        const float current = std::atan2(-thighs.z, thighs.x);
        const float wanted = std::atan2(0.0f, -1.0f);
        root = Mat4::rotY(wanted - current);
    }
    sonic::DrawList list;
    model_.drawLocals(list, skel, body, root);
    return FinishPose(list, out);
}

void BuildBatches(const sonic::DrawList& list, const sonic::Mat4& toWorld, PosedSonic& out) {
    using namespace render_detail;
    out = PosedSonic{};
    std::map<BatchKey, size_t> index;
    for (const auto& item : list.items) {
        if (item.model) AppendItem(item, toWorld, index, out);
    }
}

bool SonicResources::PoseAction(int action, float frame, PosedSonic& out) {
    std::lock_guard<std::mutex> lock(mutex_);
    out = PosedSonic{};
    if (!ready_ || !model_.hasAction(action)) return false;
    return PoseLocked(action, std::fmod(std::max(frame, 0.0f), model_.actionFrames(action)), out);
}

bool SonicResources::PoseLocked(int action, float frame, PosedSonic& out) {
    using namespace render_detail;

    sonic::DrawList list;
    model_.draw(list, action, frame, Mat4());
    return FinishPose(list, out);
}

bool SonicResources::FinishPose(const sonic::DrawList& list, PosedSonic& out) {
    using namespace render_detail;
    // Character models face -X; turn them to face +Z (see soniccore sonic.cpp).
    const Mat4 facePlusZ = Mat4::basis(Vec3(0, 0, -1), Vec3(0, 1, 0), Vec3(1, 0, 0));
    std::map<BatchKey, size_t> index;
    for (const auto& item : list.items) {
        if (item.model) AppendItem(item, facePlusZ, index, out);
    }

    // Normalise: lowest vertex at y = 0, joint box centred on x/z.
    Vec3 vmin(1e30f, 1e30f, 1e30f), vmax(-1e30f, -1e30f, -1e30f);
    for (const auto& batch : out.batches) {
        for (const auto& v : batch.tris) {
            vmin = sonic::vmin(vmin, V(v.pos));
            vmax = sonic::vmax(vmax, V(v.pos));
        }
    }
    Vec3 jmin(1e30f, 1e30f, 1e30f), jmax(-1e30f, -1e30f, -1e30f);
    for (const auto& j : out.joints) {
        jmin = sonic::vmin(jmin, j);
        jmax = sonic::vmax(jmax, j);
    }
    if (vmin.x > vmax.x || jmin.x > jmax.x) return false;
    const Vec3 shift(-(jmin.x + jmax.x) * 0.5f, -vmin.y, -(jmin.z + jmax.z) * 0.5f);
    for (auto& batch : out.batches) {
        for (auto& v : batch.tris) {
            v.pos[0] += shift.x;
            v.pos[1] += shift.y;
            v.pos[2] += shift.z;
        }
    }
    for (auto& j : out.joints) j += shift;
    out.boxMin = vmin + shift;
    out.boxMax = vmax + shift;
    out.jointMin = jmin + shift;
    out.jointMax = jmax + shift;
    return true;
}

// =====================================================================================
// software rasterizer
// =====================================================================================

namespace render_detail {

struct Canvas {
    int width = 0, height = 0;
    std::vector<float> rgb, alpha, depth;
    void Init(int w, int h) {
        width = w;
        height = h;
        rgb.assign(size_t(w) * h * 3, 0.0f);
        alpha.assign(size_t(w) * h, 0.0f);
        depth.assign(size_t(w) * h, 1e30f);
    }
};

struct TexSampler {
    const sonic::Image* image = nullptr;
    int wrapS = 1, wrapT = 1;
    static int Wrap(int c, int size, int mode) {
        if (mode == 0) return std::clamp(c, 0, size - 1);
        if (mode == 2) {
            const int period = size * 2;
            int m = ((c % period) + period) % period;
            return m < size ? m : period - 1 - m;
        }
        return ((c % size) + size) % size;
    }
    void Sample(float u, float v, float out[4]) const {
        if (!image || image->width <= 0) {
            out[0] = out[1] = out[2] = out[3] = 1.0f;
            return;
        }
        const int w = image->width, h = image->height;
        const float x = u * float(w) - 0.5f, y = v * float(h) - 0.5f;
        const int x0 = int(std::floor(x)), y0 = int(std::floor(y));
        const float fx = x - float(x0), fy = y - float(y0);
        float acc[4] = {0, 0, 0, 0};
        for (int k = 0; k < 4; ++k) {
            const int xx = Wrap(x0 + (k & 1), w, wrapS);
            const int yy = Wrap(y0 + (k >> 1), h, wrapT);
            const float wgt = ((k & 1) ? fx : 1 - fx) * ((k >> 1) ? fy : 1 - fy);
            const uint8_t* p = &image->rgba[(size_t(yy) * w + xx) * 4];
            for (int c = 0; c < 4; ++c) acc[c] += wgt * float(p[c]) / 255.0f;
        }
        std::memcpy(out, acc, sizeof(acc));
    }
};

struct ScreenVertex {
    float x, y, z;
    float u, v;
    float c[4];
};

void DrawTriangle(Canvas& cv, const ScreenVertex& a, const ScreenVertex& b, const ScreenVertex& c,
                  const TexSampler& tex, bool alphaTest, bool blend) {
    const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (std::fabs(area) < 1e-8f) return;
    const int minX = std::max(0, int(std::floor(std::min({a.x, b.x, c.x}))));
    const int maxX = std::min(cv.width - 1, int(std::ceil(std::max({a.x, b.x, c.x}))));
    const int minY = std::max(0, int(std::floor(std::min({a.y, b.y, c.y}))));
    const int maxY = std::min(cv.height - 1, int(std::ceil(std::max({a.y, b.y, c.y}))));
    const float inv = 1.0f / area;
    for (int y = minY; y <= maxY; ++y) {
        const float py = float(y) + 0.5f;
        for (int x = minX; x <= maxX; ++x) {
            const float px = float(x) + 0.5f;
            float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) * inv;
            float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) * inv;
            float w2 = 1.0f - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            const float z = w0 * a.z + w1 * b.z + w2 * c.z;
            const size_t idx = size_t(y) * cv.width + x;
            if (z >= cv.depth[idx]) continue;
            float t[4];
            tex.Sample(w0 * a.u + w1 * b.u + w2 * c.u, w0 * a.v + w1 * b.v + w2 * c.v, t);
            float col[4];
            for (int k = 0; k < 4; ++k) col[k] = t[k] * (w0 * a.c[k] + w1 * b.c[k] + w2 * c.c[k]);
            if (alphaTest && t[3] < 0.5f) continue;
            if (blend) {
                const float al = Clamp01(col[3]);
                for (int k = 0; k < 3; ++k) {
                    float& d = cv.rgb[idx * 3 + k];
                    d = col[k] * al + d * (1.0f - al);
                }
                cv.alpha[idx] = al + cv.alpha[idx] * (1.0f - al);
            } else {
                for (int k = 0; k < 3; ++k) cv.rgb[idx * 3 + k] = col[k];
                cv.alpha[idx] = 1.0f;
                cv.depth[idx] = z;
            }
        }
    }
}

void RasterizeToCanvas(const PosedSonic& posed, const Mat4& view, float left, float right, float bottom, float top,
                       Canvas& cv) {
    const auto& resources = SonicResources::Get();
    const float sx = float(cv.width) / (right - left);
    const float sy = float(cv.height) / (top - bottom);
    for (int pass = 0; pass < 2; ++pass) {
        for (const MeshBatch& batch : posed.batches) {
            if (batch.blend != (pass == 1)) continue;
            TexSampler tex;
            if (batch.texture >= 0 && size_t(batch.texture) < resources.Textures(batch.textureSet).size()) {
                tex.image = &resources.Textures(batch.textureSet)[size_t(batch.texture)].image;
            }
            tex.wrapS = batch.wrapS;
            tex.wrapT = batch.wrapT;
            for (size_t i = 0; i + 2 < batch.tris.size(); i += 3) {
                ScreenVertex sv[3];
                for (int k = 0; k < 3; ++k) {
                    const MeshVertex& mv = batch.tris[i + size_t(k)];
                    const Vec3 p = view.transformPoint(V(mv.pos));
                    const Vec3 n = sonic::normalize(view.transformDir(V(mv.nrm)));
                    const float nv[3] = {n.x, n.y, n.z};
                    const uint32_t shaded = ShadeVertex(batch, mv, nv);
                    sv[k].x = (p.x - left) * sx;
                    sv[k].y = (top - p.y) * sy;
                    sv[k].z = -p.z;
                    if (batch.env) {
                        sv[k].u = n.x * 0.5f + 0.5f;
                        sv[k].v = -n.y * 0.5f + 0.5f;
                    } else {
                        sv[k].u = mv.uv[0];
                        sv[k].v = mv.uv[1];
                    }
                    for (int c = 0; c < 4; ++c) sv[k].c[c] = float((shaded >> (c * 8)) & 0xFF) / 255.0f;
                }
                DrawTriangle(cv, sv[0], sv[1], sv[2], tex, batch.alphaTest, batch.blend);
            }
        }
    }
}

// Dark outline around the silhouette, like Mario Kart Wii's roster portraits.
void ApplyOutline(Canvas& cv, float radius, const float color[3]) {
    if (radius <= 0) return;
    const int r = int(std::ceil(radius));
    std::vector<float> cover(cv.alpha.size(), 0.0f);
    std::vector<std::pair<int, int>> offsets;
    std::vector<float> weights;
    for (int dy = -r; dy <= r; ++dy) {
        for (int dx = -r; dx <= r; ++dx) {
            const float d = std::sqrt(float(dx * dx + dy * dy));
            const float w = Clamp01(radius + 0.5f - d);
            if (w > 0) {
                offsets.emplace_back(dx, dy);
                weights.push_back(w);
            }
        }
    }
    for (int y = 0; y < cv.height; ++y) {
        for (int x = 0; x < cv.width; ++x) {
            float best = 0;
            for (size_t k = 0; k < offsets.size() && best < 1.0f; ++k) {
                const int xx = x + offsets[k].first, yy = y + offsets[k].second;
                if (xx < 0 || yy < 0 || xx >= cv.width || yy >= cv.height) continue;
                best = std::max(best, cv.alpha[size_t(yy) * cv.width + xx] * weights[k]);
            }
            cover[size_t(y) * cv.width + x] = best;
        }
    }
    for (size_t i = 0; i < cv.alpha.size(); ++i) {
        const float a = cv.alpha[i];
        const float o = cover[i];
        // character over outline
        const float outA = a + o * (1.0f - a);
        if (outA <= 0) continue;
        for (int k = 0; k < 3; ++k) {
            const float premul = cv.rgb[i * 3 + k] * a + color[k] * o * (1.0f - a);
            cv.rgb[i * 3 + k] = premul / outA;
        }
        cv.alpha[i] = outA;
    }
}

void Downsample(const Canvas& cv, int ss, RgbaImage& out) {
    out.width = cv.width / ss;
    out.height = cv.height / ss;
    out.rgba.assign(size_t(out.width) * out.height * 4, 0);
    const float n = float(ss * ss);
    for (int y = 0; y < out.height; ++y) {
        for (int x = 0; x < out.width; ++x) {
            float acc[4] = {0, 0, 0, 0};
            for (int sy = 0; sy < ss; ++sy) {
                for (int sx = 0; sx < ss; ++sx) {
                    const size_t i = size_t(y * ss + sy) * cv.width + size_t(x * ss + sx);
                    const float a = cv.alpha[i];
                    for (int k = 0; k < 3; ++k) acc[k] += cv.rgb[i * 3 + k] * a;
                    acc[3] += a;
                }
            }
            uint8_t* p = &out.rgba[(size_t(y) * out.width + x) * 4];
            const float a = acc[3] / n;
            for (int k = 0; k < 3; ++k) {
                const float c = acc[3] > 0 ? acc[k] / acc[3] : 0.0f;
                p[k] = uint8_t(Clamp01(c) * 255.0f + 0.5f);
            }
            p[3] = uint8_t(Clamp01(a) * 255.0f + 0.5f);
        }
    }
}

}  // namespace render_detail

void RasterizeSonic(const PosedSonic& posed, const Mat4& view, float left, float right, float bottom, float top,
                    int width, int height, int supersample, RgbaImage& out) {
    render_detail::Canvas cv;
    supersample = std::max(1, supersample);
    cv.Init(width * supersample, height * supersample);
    render_detail::RasterizeToCanvas(posed, view, left, right, bottom, top, cv);
    render_detail::Downsample(cv, supersample, out);
}

bool RenderSonicIcon(int width, int height, const IconStyle& style, RgbaImage& out) {
    using namespace render_detail;
    if (width <= 0 || height <= 0) return false;
    PosedSonic posed;
    if (!SonicResources::Get().Pose(SonicPose::Standing, -1.0f, posed)) return false;

    const float yaw = style.yawDegrees * 3.14159265f / 180.0f;
    const float pitch = style.pitchDegrees * 3.14159265f / 180.0f;
    const Vec3 dir(std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch));
    const float height3d = posed.boxMax.y - posed.boxMin.y;
    const Vec3 target(0, height3d * 0.75f, 0);
    const Mat4 view = Mat4::lookAt(target + dir * 200.0f, target, Vec3(0, 1, 0));

    // Frame the head and shoulders (or the whole body): the view-space box of the
    // vertices above the cut line.
    const float cut = style.bust ? posed.boxMax.y - height3d * style.bustFraction : -1e30f;
    float minX = 1e30f, maxX = -1e30f, minY = 1e30f, maxY = -1e30f;
    for (const auto& batch : posed.batches) {
        for (const auto& v : batch.tris) {
            if (v.pos[1] < cut) continue;
            const Vec3 p = view.transformPoint(V(v.pos));
            minX = std::min(minX, p.x);
            maxX = std::max(maxX, p.x);
            minY = std::min(minY, p.y);
            maxY = std::max(maxY, p.y);
        }
    }
    if (minX > maxX) return false;

    const int ss = 4;
    const float outline = style.outlinePixels > 0 ? style.outlinePixels : std::max(1.25f, float(std::min(width, height)) / 26.0f);
    const float margin = outline + 1.0f;  // pixels kept free around the silhouette
    const float spanX = maxX - minX, spanY = maxY - minY;
    const float aspect = float(width) / float(height);
    // Fit the box, keeping the margin, then anchor the top of the head at the top.
    const float usableW = float(width) - 2 * margin, usableH = float(height) - 2 * margin;
    // A portrait may crop the tips of the quills at the sides and the body at the
    // bottom, but never the top of the head.
    const float scale = style.bust ? std::min(usableW / (spanX * style.sideCrop), usableH / spanY)
                                   : std::min(usableW / spanX, usableH / spanY);  // pixels per unit
    const float viewW = float(width) / scale;
    const float viewH = viewW / aspect;
    const float cx = (minX + maxX) * 0.5f;
    const float top = maxY + margin / scale;
    const float left = cx - viewW * 0.5f;

    Canvas cv;
    cv.Init(width * ss, height * ss);
    RasterizeToCanvas(posed, view, left, left + viewW, top - viewH, top, cv);
    const float color[3] = {float(style.outlineRgba & 0xFF) / 255.0f, float((style.outlineRgba >> 8) & 0xFF) / 255.0f,
                            float((style.outlineRgba >> 16) & 0xFF) / 255.0f};
    ApplyOutline(cv, outline * float(ss), color);
    Downsample(cv, ss, out);
    return true;
}

}  // namespace sonic_mkw
