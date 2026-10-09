// sonic_course.cpp - see sonic_course.h.
#include "sonic/sonic_course.h"

#include <cmath>
#include <cstring>
#include <deque>
#include <mutex>

namespace sonic_mkw {

namespace course_detail {

using sonic::Vec3;

std::mutex g_mutex;
struct RecentCourse {
    std::string dvdPath;
    std::filesystem::path hostPath;
};
std::deque<RecentCourse> g_recent;  // newest first
uint64_t g_generation = 0;

Vec3 ReadVec(const uint8_t* p) { return Vec3(BeReadF32(p), BeReadF32(p + 4), BeReadF32(p + 8)); }

bool IsCoursePath(const std::string& dvdPath) {
    const std::string lower = ToLowerAscii(dvdPath);
    if (lower.rfind("/race/course/", 0) != 0) return false;
    const size_t n = lower.size();
    return (n > 4 && (lower.compare(n - 4, 4, ".szs") == 0 || lower.compare(n - 4, 4, ".arc") == 0));
}

}  // namespace course_detail

bool KclTypeIsSolid(uint32_t type) {
    switch (type) {
        case KCL_ITEM_ROAD:
        case KCL_ITEM_WALL:
        case KCL_FALL_BOUNDARY:
        case KCL_CANNON:
        case KCL_FORCE_RECALC:
        case KCL_SOUND_TRIGGER:
        case KCL_EFFECT_TRIGGER:
        case KCL_ITEM_STATE:
            return false;
        default:
            return true;
    }
}

// KCL layout (Wiimm's documentation):
//   0x00 u32 vertex positions   0x04 u32 normals
//   0x08 u32 prisms - 0x10 (prisms are indexed from 1)   0x0C u32 spatial index
//   prism (0x10): f32 height, u16 position, u16 face normal, u16 edge normals A/B/C, u16 attribute
//   v1 = pos; crossA = cross(enA, fn); crossB = cross(enB, fn)
//   v2 = v1 + crossB * (height / dot(crossB, enC)); v3 = v1 + crossA * (height / dot(crossA, enC))
bool DecodeKcl(const uint8_t* data, size_t size, std::vector<KclTriangle>& out) {
    using namespace course_detail;
    out.clear();
    if (size < 0x3C) return false;
    const uint32_t posOff = BeRead32(data);
    const uint32_t nrmOff = BeRead32(data + 4);
    const uint32_t prismBase = BeRead32(data + 8);
    const uint32_t indexOff = BeRead32(data + 12);
    const uint64_t first = uint64_t(prismBase) + 0x10;
    if (posOff >= size || nrmOff >= size || first > size || indexOff > size || indexOff < first) return false;
    const size_t count = size_t((indexOff - first) / 0x10);
    out.reserve(count);
    auto vecAt = [&](uint32_t base, uint32_t index, Vec3& v) {
        const uint64_t off = uint64_t(base) + uint64_t(index) * 12;
        if (off + 12 > size) return false;
        v = ReadVec(data + off);
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    };
    for (size_t i = 0; i < count; ++i) {
        const uint8_t* p = data + first + i * 0x10;
        const float height = BeReadF32(p);
        Vec3 v1, fn, ea, eb, ec;
        if (!vecAt(posOff, BeRead16(p + 4), v1) || !vecAt(nrmOff, BeRead16(p + 6), fn) ||
            !vecAt(nrmOff, BeRead16(p + 8), ea) || !vecAt(nrmOff, BeRead16(p + 10), eb) ||
            !vecAt(nrmOff, BeRead16(p + 12), ec)) {
            continue;
        }
        const Vec3 crossA = sonic::cross(ea, fn);
        const Vec3 crossB = sonic::cross(eb, fn);
        const float dB = sonic::dot(crossB, ec);
        const float dA = sonic::dot(crossA, ec);
        if (std::fabs(dA) < 1e-9f || std::fabs(dB) < 1e-9f || !std::isfinite(height)) continue;
        const Vec3 v2 = v1 + crossB * (height / dB);
        const Vec3 v3 = v1 + crossA * (height / dA);
        KclTriangle t;
        for (int k = 0; k < 3; ++k) {
            t.a[k] = v1[k];
            t.b[k] = v2[k];
            t.c[k] = v3[k];
            t.normal[k] = fn[k];
        }
        t.attribute = BeRead16(p + 14);
        out.push_back(t);
    }
    return !out.empty();
}

bool CourseCollision::Load(const std::vector<KclTriangle>& tris, float scale) {
    using course_detail::Vec3;
    ready_ = false;
    world_.clear();
    scale_ = scale > 0 ? scale : 1.0f;
    const float inv = 1.0f / scale_;
    float minY = 1e30f;
    for (const KclTriangle& t : tris) {
        const uint32_t type = t.attribute & 0x1F;
        if (!KclTypeIsSolid(type)) continue;
        const Vec3 a(t.a[0] * inv, t.a[1] * inv, t.a[2] * inv);
        const Vec3 b(t.b[0] * inv, t.b[1] * inv, t.b[2] * inv);
        const Vec3 c(t.c[0] * inv, t.c[1] * inv, t.c[2] * inv);
        // SonicCore takes a triangle's normal from (b - a) x (c - a); wind every
        // triangle so that it agrees with the KCL face normal (out of the surface).
        const Vec3 fn(t.normal[0], t.normal[1], t.normal[2]);
        const uint32_t flags = sonic::SURF_SOLID | (uint32_t(t.attribute) << 16);
        if (sonic::dot(sonic::cross(b - a, c - a), fn) >= 0) {
            world_.addTriangle(a, b, c, flags);
        } else {
            world_.addTriangle(a, c, b, flags);
        }
        minY = std::min(minY, std::min(a.y, std::min(b.y, c.y)));
    }
    if (world_.triangleCount() == 0) return false;
    world_.build();
    killY_ = minY - 400.0f;
    ready_ = true;
    return true;
}

bool CourseCollision::LoadArchive(const Bytes& archive, float scale, std::string& error) {
    Bytes raw;
    const Bytes* u8 = &archive;
    if (IsYaz0(archive.data(), archive.size())) {
        if (!Yaz0Decode(archive.data(), archive.size(), raw)) {
            error = "course archive: bad Yaz0 data";
            return false;
        }
        u8 = &raw;
    }
    U8Archive arc;
    if (!arc.Parse(u8->data(), u8->size())) {
        error = "course archive: not a U8 archive";
        return false;
    }
    for (size_t index : arc.Files()) {
        if (ToLowerAscii(arc.At(index).name) != "course.kcl") continue;
        const Bytes& kcl = arc.At(index).data;
        std::vector<KclTriangle> tris;
        if (!DecodeKcl(kcl.data(), kcl.size(), tris)) {
            error = "course.kcl could not be decoded";
            return false;
        }
        if (!Load(tris, scale)) {
            error = "course.kcl has no solid surfaces";
            return false;
        }
        return true;
    }
    error = "no course.kcl in the archive";
    return false;
}

bool CourseCollision::GroundBelow(const sonic::Vec3& from, float maxDrop, sonic::RayHit& hit) const {
    if (!ready_) return false;
    return world_.raycast(from, sonic::Vec3(0, -1, 0), maxDrop, hit);
}

void NotifyDvdFileRead(const std::string& dvdPath, const std::filesystem::path& hostPath) {
    using namespace course_detail;
    if (!IsCoursePath(dvdPath)) return;
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_recent.empty() && g_recent.front().dvdPath == dvdPath) return;
    for (auto it = g_recent.begin(); it != g_recent.end(); ++it) {
        if (it->dvdPath == dvdPath) {
            g_recent.erase(it);
            break;
        }
    }
    g_recent.push_front({dvdPath, hostPath});
    while (g_recent.size() > 4) g_recent.pop_back();
    ++g_generation;
}

bool RecentCourseArchives(std::vector<std::pair<std::string, std::filesystem::path>>& out, uint64_t& generation) {
    using namespace course_detail;
    std::lock_guard<std::mutex> lock(g_mutex);
    generation = g_generation;
    out.clear();
    for (const auto& c : g_recent) out.emplace_back(c.dvdPath, c.hostPath);
    return !out.empty();
}

}  // namespace sonic_mkw
