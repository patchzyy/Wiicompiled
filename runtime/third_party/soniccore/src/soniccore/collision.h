#pragma once
#include "core/common.h"
#include "core/math.h"
#include <unordered_map>

namespace sonic {

// SADX COL surface flags (subset)
enum SurfaceFlags : u32 {
    SURF_SOLID = 0x1,
    SURF_WATER = 0x2,
    SURF_NOFRICTION = 0x4,
    SURF_NOACCEL = 0x8,
    SURF_INCACCEL = 0x80,
    SURF_DIGGABLE = 0x100,
    SURF_UNCLIMBABLE = 0x1000,
    SURF_HURT = 0x10000,
    SURF_FOOTPRINTS = 0x100000,
    SURF_VISIBLE = 0x80000000,
};

struct ColTri {
    Vec3 a, b, c;
    Vec3 n;
    u32 flags;
};

struct RayHit {
    float t = 0;
    Vec3 pos, normal;
    u32 flags = 0;
    int tri = -1;
};

struct Contact {
    Vec3 normal;
    float depth;
    u32 flags;
    int tri;
};

// What Sonic needs from the host's collision system. Implement this to run Sonic
// against your own level collision, or use TriangleCollision below.
class ICollision {
public:
    virtual ~ICollision() = default;
    // First hit along from + dir * t, t in [0, maxDist]; dir is normalised.
    virtual bool raycast(const Vec3& from, const Vec3& dir, float maxDist, RayHit& hit, u32 mask = SURF_SOLID) const = 0;
    // Push a sphere out of the geometry: update `center`, append one Contact per
    // touching surface (normal pointing out of the surface) and return the count.
    virtual int resolveSphere(Vec3& center, float radius, std::vector<Contact>& contacts, u32 mask = SURF_SOLID) const = 0;
};

// Ready-made triangle soup collision with a spatial hash.
class TriangleCollision : public ICollision {
public:
    void clear();
    void addTriangle(const Vec3& a, const Vec3& b, const Vec3& c, u32 flags);
    void build();  // build spatial hash
    bool raycast(const Vec3& from, const Vec3& dir, float maxDist, RayHit& hit, u32 mask = SURF_SOLID) const override;
    // Push a sphere out of geometry. Returns contacts; `center` is updated.
    int resolveSphere(Vec3& center, float radius, std::vector<Contact>& contacts, u32 mask = SURF_SOLID) const override;
    size_t triangleCount() const { return tris_.size(); }
    const std::vector<ColTri>& triangles() const { return tris_; }
    Vec3 boundsMin() const { return bmin_; }
    Vec3 boundsMax() const { return bmax_; }
    void gather(const Vec3& mn, const Vec3& mx, std::vector<int>& out) const;

private:
    struct Key {
        int x, y, z;
        bool operator==(const Key& o) const { return x == o.x && y == o.y && z == o.z; }
    };
    struct KeyHash {
        size_t operator()(const Key& k) const { return size_t((k.x * 73856093) ^ (k.y * 19349663) ^ (k.z * 83492791)); }
    };
    std::vector<ColTri> tris_;
    std::unordered_map<Key, std::vector<int>, KeyHash> grid_;
    float cell_ = 64.0f;
    Vec3 bmin_, bmax_;
    mutable std::vector<u32> stamp_;
    mutable u32 stampId_ = 0;
};

using CollisionWorld = TriangleCollision;

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c);

}  // namespace sonic
