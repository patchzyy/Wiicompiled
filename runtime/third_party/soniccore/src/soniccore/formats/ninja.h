#pragma once
#include "../core/common.h"
#include "../core/math.h"
#include "pe_image.h"
#include <unordered_map>

namespace sonic {

// ---- Ninja (Sega Dreamcast SDK) model structures as used by SADX PC --------
//
// NJS_OBJECT   : evalflags, NJS_MODEL*, pos[3], ang[3] (BAMS), scl[3], child*, sibling*
// NJS_MODEL    : points*, normals*, nbPoint, meshsets*, mats*, nbMeshset, nbMat, center, r
// NJS_MESHSET  : type_matId, nbMesh, meshes*, attrs*, normals*, vertcolor*, vertuv*, buffer*  (0x1C, SADX)
// NJS_MATERIAL : diffuse, specular, exponent, attr_texId, attrflags
// NJS_MOTION   : mdata*, nbFrame, type, inp_fn

enum NjEvalFlags : u32 {
    NJD_EVAL_UNIT_POS = 0x01,
    NJD_EVAL_UNIT_ANG = 0x02,
    NJD_EVAL_UNIT_SCL = 0x04,
    NJD_EVAL_HIDE = 0x08,
    NJD_EVAL_BREAK = 0x10,
    NJD_EVAL_ZXY_ANG = 0x20,
    NJD_EVAL_SKIP = 0x40,
    NJD_EVAL_SHAPE_SKIP = 0x80,
};

enum NjMatFlags : u32 {
    NJD_FLAG_CLAMP_V = 0x00010000,
    NJD_FLAG_CLAMP_U = 0x00020000,
    NJD_FLAG_FLIP_V = 0x00040000,
    NJD_FLAG_FLIP_U = 0x00080000,
    NJD_FLAG_USE_ALPHA = 0x00100000,
    NJD_FLAG_USE_TEXTURE = 0x00200000,
    NJD_FLAG_USE_ENV = 0x00400000,
    NJD_FLAG_DOUBLE_SIDE = 0x00800000,
    NJD_FLAG_USE_FLAT = 0x01000000,
    NJD_FLAG_IGNORE_LIGHT = 0x02000000,
};

struct NjMaterial {
    u32 diffuse = 0xFFFFFFFF;  // ARGB
    u32 specular = 0;
    float exponent = 0;
    u32 texId = 0;
    u32 flags = 0;
    int srcBlend() const { return int((flags >> 29) & 7); }
    int dstBlend() const { return int((flags >> 26) & 7); }
};

struct NjVertex {
    Vec3 pos;
    Vec3 nrm;
    float u = 0, v = 0;
    u32 color = 0xFFFFFFFF;  // RGBA8 (R in low byte)
};

struct NjMeshPart {
    int material = 0;
    u32 firstIndex = 0;
    u32 indexCount = 0;
    bool hasVColor = false;
    bool hasUV = false;
};

struct NjModel {
    u32 va = 0;
    std::vector<NjVertex> verts;
    std::vector<u32> indices;
    // original point/normal arrays and, per vertex, which point it came from
    // (needed for vertex welding, which addresses points by index)
    std::vector<Vec3> points, normals;
    std::vector<int> pointOf;
    std::vector<NjMeshPart> parts;
    std::vector<NjMaterial> mats;
    Vec3 center;
    float radius = 0;
    Vec3 bmin, bmax;
    // Host renderer bookkeeping: `gpu` is yours to store a handle in; `version`
    // increases whenever SonicCore rewrites the vertices (limb welds), so re-upload
    // the vertex buffer when it differs from the version you uploaded.
    void* gpu = nullptr;
    u32 version = 0;
};

struct NjNode {
    u32 va = 0;
    u32 flags = 0;
    Vec3 pos;
    int ang[3] = {0, 0, 0};
    Vec3 scl{1, 1, 1};
    int model = -1;   // index into NjSkeleton::models
    int parent = -1;
};

// A flattened object hierarchy in Ninja depth-first order (same order as motion data).
struct NjSkeleton {
    std::vector<NjNode> nodes;
    std::vector<std::shared_ptr<NjModel>> models;
    bool valid() const { return !nodes.empty(); }
};

struct NjKey3f {
    u32 frame;
    Vec3 v;
};
struct NjKey3i {
    u32 frame;
    int a[3];
};
struct NjTrack {
    std::vector<NjKey3f> pos;
    std::vector<NjKey3i> ang;
    std::vector<NjKey3f> scl;
};
struct NjMotion {
    u32 va = 0;
    u32 frames = 0;
    std::vector<NjTrack> tracks;  // one per node
    bool valid() const { return frames > 0 && !tracks.empty(); }
};

// Local (per node) transform, used for pose blending
struct NjLocal {
    Vec3 pos;
    int ang[3] = {0, 0, 0};
    Vec3 scl{1, 1, 1};
};
void evalLocal(const NjSkeleton& sk, const NjMotion* mot, float frame, std::vector<NjLocal>& out);
// Blend b into a with weight t (0 = a, 1 = b); angles take the shortest path.
void blendLocal(std::vector<NjLocal>& a, const std::vector<NjLocal>& b, float t);
void composePose(const NjSkeleton& sk, const std::vector<NjLocal>& locals, const Mat4& root, std::vector<Mat4>& out);

// Pose evaluation result: world matrix per node
void evalPose(const NjSkeleton& sk, const NjMotion* mot, float frame, const Mat4& root, std::vector<Mat4>& out);

class NinjaReader {
public:
    explicit NinjaReader(const PeImage& pe) : pe_(pe) {}
    bool readSkeleton(u32 objVA, NjSkeleton& out, int maxNodes = 512);
    std::shared_ptr<NjModel> readModel(u32 modelVA);
    bool readMotion(u32 motionVA, size_t nodeCount, NjMotion& out);
    bool looksLikeObject(u32 va) const;
    bool looksLikeModel(u32 va) const;
    std::vector<std::string> readTexlistNames(u32 texlistVA) const;

private:
    const PeImage& pe_;
    std::unordered_map<u32, std::shared_ptr<NjModel>> modelCache_;
};

}  // namespace sonic
