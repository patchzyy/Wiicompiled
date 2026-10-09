// sonic_course.h - Mario Kart Wii course collision (KCL) for Sonic's physics.
//
// Sonic runs SonicCore's own physics, so he needs the course's collision mesh.
// The course archive (Race/Course/<name>.szs, "course.kcl" inside) is read from
// the host file the DVD layer last served for that path; the KCL prisms are
// turned back into triangles and fed to SonicCore's TriangleCollision, scaled
// from Mario Kart units into SADX units.
#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "sonic/sonic_formats.h"
#include "soniccore/collision.h"

namespace sonic_mkw {

// KCL collision types (low 5 bits of a prism's attribute).
enum KclType : uint32_t {
    KCL_ROAD = 0x00,
    KCL_SLIPPERY_ROAD = 0x01,
    KCL_WEAK_OFFROAD = 0x02,
    KCL_OFFROAD = 0x03,
    KCL_HEAVY_OFFROAD = 0x04,
    KCL_SLIPPERY_ROAD_2 = 0x05,
    KCL_BOOST_PANEL = 0x06,
    KCL_BOOST_RAMP = 0x07,
    KCL_JUMP_PAD = 0x08,
    KCL_ITEM_ROAD = 0x09,
    KCL_SOLID_FALL = 0x0A,
    KCL_MOVING_WATER = 0x0B,
    KCL_WALL = 0x0C,
    KCL_INVISIBLE_WALL = 0x0D,
    KCL_ITEM_WALL = 0x0E,
    KCL_WALL_3 = 0x0F,
    KCL_FALL_BOUNDARY = 0x10,
    KCL_CANNON = 0x11,
    KCL_FORCE_RECALC = 0x12,
    KCL_HALFPIPE_RAMP = 0x13,
    KCL_PLAYER_WALL = 0x14,
    KCL_MOVING_ROAD = 0x15,
    KCL_STICKY_ROAD = 0x16,
    KCL_ROAD_2 = 0x17,
    KCL_SOUND_TRIGGER = 0x18,
    KCL_WEAK_WALL = 0x19,
    KCL_EFFECT_TRIGGER = 0x1A,
    KCL_ITEM_STATE = 0x1B,
    KCL_HALFPIPE_WALL = 0x1C,
    KCL_ROTATING_ROAD = 0x1D,
    KCL_SPECIAL_WALL = 0x1E,
    KCL_INVISIBLE_WALL_2 = 0x1F,
};

// Collision flags for SonicCore: SURF_SOLID plus the KCL attribute in the high
// 16 bits, so the racer can read the surface type under Sonic's feet.
inline uint32_t KclAttributeOf(uint32_t surfaceFlags) { return surfaceFlags >> 16; }
inline uint32_t KclTypeOf(uint32_t surfaceFlags) { return (surfaceFlags >> 16) & 0x1F; }
// Types Sonic stands on or bumps into (triggers and item-only surfaces are not).
bool KclTypeIsSolid(uint32_t type);

struct KclTriangle {
    float a[3], b[3], c[3];
    float normal[3];  // the prism's face normal (points out of the surface)
    uint16_t attribute;
};
// Decodes a KCL file into triangles (Mario Kart units). False if malformed.
bool DecodeKcl(const uint8_t* data, size_t size, std::vector<KclTriangle>& out);

// The collision world of the current course, in SADX units (MKW / scale).
class CourseCollision {
public:
    // Builds the collision from a course archive (.szs or .arc). `scale` is MKW
    // units per SADX unit.
    bool LoadArchive(const Bytes& archive, float scale, std::string& error);
    bool Load(const std::vector<KclTriangle>& tris, float scale);

    bool Ready() const { return ready_; }
    float Scale() const { return scale_; }
    const sonic::TriangleCollision& World() const { return world_; }
    float KillY() const { return killY_; }  // SADX units, well below the course
    size_t TriangleCount() const { return world_.triangleCount(); }

    // First solid surface below `from` (SADX units) within `maxDrop`.
    bool GroundBelow(const sonic::Vec3& from, float maxDrop, sonic::RayHit& hit) const;

private:
    sonic::TriangleCollision world_;
    float scale_ = 1.0f;
    float killY_ = -1e30f;
    bool ready_ = false;
};

// Remembers which course archive the game read most recently (called by the DVD
// layer for every read that starts at offset 0).
void NotifyDvdFileRead(const std::string& dvdPath, const std::filesystem::path& hostPath);
// Race/Course archives the game read recently, newest first. `generation`
// changes whenever a new one is read.
bool RecentCourseArchives(std::vector<std::pair<std::string, std::filesystem::path>>& out, uint64_t& generation);

}  // namespace sonic_mkw
