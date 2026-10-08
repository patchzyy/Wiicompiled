#include "collision.h"
#include <cmath>

namespace sonic {

Vec3 closestPointOnTriangle(const Vec3& p, const Vec3& a, const Vec3& b, const Vec3& c) {
    // Ericson, Real-Time Collision Detection 5.1.5
    Vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = dot(ab, ap), d2 = dot(ac, ap);
    if (d1 <= 0 && d2 <= 0) return a;
    Vec3 bp = p - b;
    float d3 = dot(ab, bp), d4 = dot(ac, bp);
    if (d3 >= 0 && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0 && d1 >= 0 && d3 <= 0) {
        float v = d1 / (d1 - d3);
        return a + ab * v;
    }
    Vec3 cp = p - c;
    float d5 = dot(ab, cp), d6 = dot(ac, cp);
    if (d6 >= 0 && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0 && d2 >= 0 && d6 <= 0) {
        float w = d2 / (d2 - d6);
        return a + ac * w;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + (c - b) * w;
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom, w = vc * denom;
    return a + ab * v + ac * w;
}

void TriangleCollision::clear() {
    tris_.clear();
    grid_.clear();
}

void TriangleCollision::addTriangle(const Vec3& a, const Vec3& b, const Vec3& c, u32 flags) {
    Vec3 n = cross(b - a, c - a);
    float l = length(n);
    if (l < 1e-6f) return;
    tris_.push_back({a, b, c, n / l, flags});
}

void TriangleCollision::build() {
    grid_.clear();
    bmin_ = Vec3(1e30f, 1e30f, 1e30f);
    bmax_ = Vec3(-1e30f, -1e30f, -1e30f);
    for (int i = 0; i < (int)tris_.size(); i++) {
        auto& t = tris_[i];
        Vec3 mn = vmin(t.a, vmin(t.b, t.c)), mx = vmax(t.a, vmax(t.b, t.c));
        bmin_ = vmin(bmin_, mn);
        bmax_ = vmax(bmax_, mx);
        int x0 = (int)std::floor(mn.x / cell_), x1 = (int)std::floor(mx.x / cell_);
        int y0 = (int)std::floor(mn.y / cell_), y1 = (int)std::floor(mx.y / cell_);
        int z0 = (int)std::floor(mn.z / cell_), z1 = (int)std::floor(mx.z / cell_);
        long cells = long(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1);
        if (cells > 200000) continue;  // absurdly large triangle
        for (int x = x0; x <= x1; x++)
            for (int y = y0; y <= y1; y++)
                for (int z = z0; z <= z1; z++) grid_[{x, y, z}].push_back(i);
    }
    stamp_.assign(tris_.size(), 0);
}

void TriangleCollision::gather(const Vec3& mn, const Vec3& mx, std::vector<int>& out) const {
    out.clear();
    if (++stampId_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0);
        stampId_ = 1;
    }
    int x0 = (int)std::floor(mn.x / cell_), x1 = (int)std::floor(mx.x / cell_);
    int y0 = (int)std::floor(mn.y / cell_), y1 = (int)std::floor(mx.y / cell_);
    int z0 = (int)std::floor(mn.z / cell_), z1 = (int)std::floor(mx.z / cell_);
    if (long(x1 - x0 + 1) * (y1 - y0 + 1) * (z1 - z0 + 1) > 4096) return;
    for (int x = x0; x <= x1; x++)
        for (int y = y0; y <= y1; y++)
            for (int z = z0; z <= z1; z++) {
                auto it = grid_.find({x, y, z});
                if (it == grid_.end()) continue;
                for (int i : it->second)
                    if (stamp_[i] != stampId_) {
                        stamp_[i] = stampId_;
                        out.push_back(i);
                    }
            }
}

bool TriangleCollision::raycast(const Vec3& from, const Vec3& dir, float maxDist, RayHit& hit, u32 mask) const {
    Vec3 to = from + dir * maxDist;
    static thread_local std::vector<int> cand;
    // march in chunks to keep candidate sets small for long rays
    float step = cell_ * 4;
    float best = maxDist;
    bool found = false;
    for (float s = 0; s < maxDist && !found; s += step) {
        float e = std::min(maxDist, s + step);
        Vec3 a = from + dir * s, b = from + dir * e;
        gather(vmin(a, b) - Vec3(1, 1, 1), vmax(a, b) + Vec3(1, 1, 1), cand);
        for (int i : cand) {
            const ColTri& t = tris_[i];
            if (!(t.flags & mask)) continue;
            // Moller-Trumbore (two sided)
            Vec3 e1 = t.b - t.a, e2 = t.c - t.a;
            Vec3 p = cross(dir, e2);
            float det = dot(e1, p);
            if (std::fabs(det) < 1e-9f) continue;
            float inv = 1.0f / det;
            Vec3 tv = from - t.a;
            float u = dot(tv, p) * inv;
            if (u < 0 || u > 1) continue;
            Vec3 q = cross(tv, e1);
            float v = dot(dir, q) * inv;
            if (v < 0 || u + v > 1) continue;
            float tt = dot(e2, q) * inv;
            if (tt >= 0 && tt < best) {
                best = tt;
                hit.t = tt;
                hit.pos = from + dir * tt;
                hit.normal = dot(t.n, dir) > 0 ? -t.n : t.n;
                hit.flags = t.flags;
                hit.tri = i;
                found = true;
            }
        }
    }
    (void)to;
    return found;
}

int TriangleCollision::resolveSphere(Vec3& center, float radius, std::vector<Contact>& contacts, u32 mask) const {
    contacts.clear();
    static thread_local std::vector<int> cand;
    for (int iter = 0; iter < 8; iter++) {
        Vec3 r(radius, radius, radius);
        gather(center - r, center + r, cand);
        bool moved = false;
        // resolve deepest first
        float bestDepth = 0;
        Vec3 bestN;
        int bestTri = -1;
        for (int i : cand) {
            const ColTri& t = tris_[i];
            if (!(t.flags & mask)) continue;
            Vec3 q = closestPointOnTriangle(center, t.a, t.b, t.c);
            Vec3 d = center - q;
            float dist2 = length2(d);
            if (dist2 >= radius * radius) continue;
            float dist = std::sqrt(dist2);
            Vec3 n = dist > 1e-5f ? d / dist : t.n;
            // one-sided-ish: if we are behind the face's plane and the face points away, use face normal
            float depth = radius - dist;
            if (depth > bestDepth) {
                bestDepth = depth;
                bestN = n;
                bestTri = i;
            }
        }
        if (bestTri >= 0) {
            center += bestN * (bestDepth + 0.001f);
            contacts.push_back({bestN, bestDepth, tris_[bestTri].flags, bestTri});
            moved = true;
        }
        if (!moved) break;
    }
    return (int)contacts.size();
}

}  // namespace sonic
