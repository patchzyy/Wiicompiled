// sonic_race_hook.cpp - Sonic players race on foot (see sonic_game.h).
//
// Kart::Manager::Update (0x8058FFE8) runs every kart once per race frame. The
// wrapper below lets the game do that, then, for every player who picked Sonic:
//   1. reads what the game's kart is doing (position, rotation, respawn, cannon,
//      finish, boosts, item hits) and the player's controller;
//   2. runs SonicRacer (SADX physics on the course's own collision);
//   3. writes Sonic's position, rotation and speed back into the kart, so laps,
//      positions, items, the camera and the AI's view of the field all follow him.
// With [sonic] collision = "model" the kart is also shrunk (Kart::Movement::
// UpdateScale) so it no longer touches anything itself; instead other karts
// bump into Sonic's body (spheres on his model's parts, SonicRacer::Body) and
// items test against it (Item::Obj::CheckKartCollision).
//
// Guest layouts (PAL), from the community's Mario Kart Wii documentation:
//   Kart::Manager       sInstance 0x809C18F8: players +0x20, count +0x24
//   Kart::Player        Link (Pointers* at +0), Pointers embedded at +0x1C
//   Kart::Pointers      values +0, status +4, movement +0x28, collision +0x30
//   Kart::Physics       position +0x68, speed0 (external) +0x74, speed2 +0xB0,
//                       speed3 +0xC8, speed +0xD4, speedNorm +0xE0, mainRot +0xF0,
//                       fullRot +0x100, engineSpeed +0x14C
//   Kart::PhysicsHolder position +0x18, transform Mtx34 +0x9C
//   Kart::Movement      engineSpeed +0x20, scale +0x164, timeInRespawn +0x234
//   Kart::Status        bitfield0 +4 (0x10 out of bounds), bitfield1 +8 (0x1 hit by
//                       an item or object, 0x10 in a cannon)
//   Kart::Collision     timeBeforeRespawn +0x48
//   BSP                 hitbox[16] at +4, 0x18 each: enable u16, center Vec3 +4, radius +0x10
//   Racedata            sInstance 0x809BD728, race scenario players at +0x28, 0xF0 each:
//                       hudSlotId +5, characterId +0xC, playerType +0x10 (0 = local)
//   Raceinfo            sInstance 0x809BD730: players +0xC, stage +0x28 (2 = race);
//                       RaceinfoPlayer stateFlags +0x38, controller holder +0x48
//   ControllerHolder    current InputState at +0x28: actions +4 (1 accelerate,
//                       2 brake, 4 item, 8 drift), stickX +8, stickY +0xC, flick +0x12
//   Item::Obj           position +0x44, owner +0x6C, entity +0xB0 (radius +4)
#include "abi_bridge.h"
#include "ppc_runtime.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "sonic/sonic_course.h"
#include "sonic/sonic_game.h"
#include "sonic/sonic_guest.h"
#include "sonic/sonic_mkw.h"
#include "sonic/sonic_racer.h"
#include "sonic/sonic_render.h"

#include <chrono>
#include <cmath>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#ifndef RT_TAG_SONIC
#define RT_TAG_SONIC "sonic"
#endif

extern "C" void func_8058ffe8(CpuContext* ctx);  // Kart::Manager::Update
extern "C" void func_8058160c(CpuContext* ctx);  // Kart::Movement::UpdateScale
extern "C" void func_807a14d4(CpuContext* ctx);  // Item::Obj::CheckKartCollision
extern "C" void func_8058fdd4(CpuContext* ctx);  // Kart::Manager::~Manager

namespace sonic_mkw {
namespace race_detail {

using sonic::Vec3;
namespace g = guest;

constexpr uint32_t kKartManager = 0x809C18F8u;
constexpr uint32_t kRacedata = 0x809BD728u;
constexpr uint32_t kRaceinfo = 0x809BD730u;
constexpr uint32_t kLinkGetPhysics = 0x805903CCu;
constexpr uint32_t kLinkGetPhysicsHolder = 0x805903ACu;
constexpr uint32_t kLinkGetBsp = 0x80590888u;
constexpr uint32_t kLinkGetPlayerIdx = 0x80590A5Cu;
constexpr uint32_t kModelsVisibilitySet = 0x8056A300u;  // Kart::ModelsVisibility::SetModelsVisibility(bool)
constexpr int kStageRace = 2;
constexpr float kSadxToMkw = 9.0f;  // Mario Kart units per SADX unit at scale 1

struct Kart {
    int idx = -1;
    uint32_t player = 0, pointers = 0, physics = 0, holder = 0, movement = 0, status = 0, collision = 0, bsp = 0;
    int racer = -1;  // index into State::racers, or -1
};

struct Racer {
    int kart = -1;  // index into State::karts
    int hud = 0;
    std::unique_ptr<SonicRacer> sonic;
    int writes = 0;
    bool reportedHit = false;
    Vec3 gamePos;
    int lakituFrames = 0;
    bool trustTimers = true;
};

struct State {
    std::mutex mutex;  // guards what the draw hook reads
    uint32_t key = 0;
    std::vector<Kart> karts;
    std::vector<Racer> racers;
    CourseCollision course;
    uint64_t courseGeneration = ~0ull;  // generation last tried (loaded or failed)
    std::string courseName;
    float courseScale = 0;
    RacerTuning tuning;
    std::string tuningKey;
    uint32_t frame = 0;
    bool baseIsSonic = false;
    bool modelCollision = true;
    float kartScale = 0.1f;
    std::vector<game::RacerDraw> draws;
    int itemLogs = 0, bumpLogs = 0;
};

State& S() {
    static State state;
    return state;
}

// ---- helpers --------------------------------------------------------------------

Vec3 QuatRotate(const float q[4], const Vec3& v) {
    const Vec3 u(q[0], q[1], q[2]);
    const Vec3 t = sonic::cross(u, v) * 2.0f;
    return v + t * q[3] + sonic::cross(u, t);
}

void ReadQuat(uint32_t address, float q[4]) {
    for (int i = 0; i < 4; ++i) q[i] = g::F32(address + 4 * i);
    const float len = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(len > 1e-4f) || !std::isfinite(len)) {
        q[0] = q[1] = q[2] = 0;
        q[3] = 1;
        return;
    }
    for (int i = 0; i < 4; ++i) q[i] /= len;
}

void WriteQuat(uint32_t address, const float q[4]) {
    for (int i = 0; i < 4; ++i) g::SetF32(address + 4 * i, q[i]);
}

bool FiniteVec(const Vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

std::string Lower(std::string s) {
    for (char& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

// The physics every Sonic racer runs with, from [sonic] physics / speed / acceleration.
const RacerTuning& Tuning() {
    State& s = S();
    namespace cfg = RuntimeConfigFile;
    const std::string mode = Lower(cfg::SonicPhysics());
    const float speed = cfg::SonicSpeed();
    const float accel = cfg::SonicAcceleration();
    const float scale = kSadxToMkw * cfg::SonicScale();
    char key[160];
    std::snprintf(key, sizeof(key), "%s|%.3f|%.3f|%.3f", mode.c_str(), speed, accel, scale);
    if (s.tuningKey == key) return s.tuning;
    s.tuningKey = key;
    const sonic::Assets& assets = SonicResources::Get().Assets();
    RacerTuning t;
    t.scale = scale;
    t.speed = speed;
    t.calibrated = true;
    const auto started = std::chrono::steady_clock::now();
    float top = 0;
    if (mode == "sadx") {
        t.params = SonicRacer::StockParams(assets, speed);
        top = SonicRacer::MeasureTopSpeed(assets) * speed;
    } else if (mode == "kart") {
        float factor = 0;
        t.params = SonicRacer::Calibrate(assets, t.kartTopSpeed * speed / scale, &factor);
        top = t.kartTopSpeed * speed / scale;
    } else {
        if (mode != "downhill") {
            RT_LOG(RT_TAG_SONIC) << "[sonic] physics = \"" << mode << "\" is not one of downhill, sadx, kart; using downhill"
                                 << std::endl;
        }
        t.params = SonicRacer::DownhillParams(assets, speed, accel, &top);
    }
    if ((mode == "sadx" || mode == "kart") && std::fabs(accel - 1.0f) > 1e-3f) {
        t.params.lim_frict *= accel;
        t.params.run_accel *= accel;
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
    RT_LOGF(RT_TAG_SONIC, "racer physics \"%s\": top speed on flat ground %.2f SADX = %.1f Mario Kart units/frame "
            "(speed %.2f, acceleration %.2f, scale %.1f; %lld ms)\n",
            mode.c_str(), top, top * scale, speed, accel, scale, static_cast<long long>(ms.count()));
    s.tuning = t;
    return s.tuning;
}

// Loads the course collision from the newest course archive the game read.
void LoadCourse(float scale) {
    State& s = S();
    std::vector<std::pair<std::string, std::filesystem::path>> recent;
    uint64_t generation = 0;
    if (!RecentCourseArchives(recent, generation)) {
        if (s.courseGeneration != 0) RT_LOG(RT_TAG_SONIC) << "no course archive seen yet; Sonic waits" << std::endl;
        s.courseGeneration = 0;
        return;
    }
    if (generation == s.courseGeneration && scale == s.courseScale) return;  // loaded, or failed: wait for a new one
    s.courseGeneration = generation;
    s.courseScale = scale;
    for (const auto& [dvdPath, hostPath] : recent) {
        const auto started = std::chrono::steady_clock::now();
        std::ifstream file(hostPath, std::ios::binary);
        Bytes data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
        std::string error;
        if (data.empty()) {
            error = "could not read the file";
        } else if (s.course.LoadArchive(data, scale, error)) {
            s.courseName = dvdPath;
            const auto ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);
            RT_LOGF(RT_TAG_SONIC, "course collision %s: %zu triangles (%lld ms)\n", dvdPath.c_str(),
                    s.course.TriangleCount(), static_cast<long long>(ms.count()));
            return;
        }
        RT_LOGF(RT_TAG_SONIC, "course %s: %s\n", dvdPath.c_str(), error.c_str());
    }
    s.course = CourseCollision();
}

bool IsSonicPlayer(int playerIdx, int& hudOut, int& typeOut, int& charOut) {
    const uint32_t racedata = g::Ptr(kRacedata);
    if (!racedata) return false;
    const uint32_t p = racedata + 0x28u + uint32_t(playerIdx) * 0xF0u;
    const int hud = int(int8_t(g::U8(p + 5)));
    const int character = int(g::U32(p + 0xC));
    const int type = int(g::U32(p + 0x10));
    hudOut = hud;
    typeOut = type;
    charOut = character;
    if (type != 0 || character != game::BaseCharacter()) return false;
    if (Lower(RuntimeConfigFile::SonicSelect()) == "base") return true;
    return hud >= 0 && hud < game::kHuds && game::ChoseSonic(hud);
}

// The race is over (Kart::Manager destroyed): forget its players and course.
void Reset() {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.key = 0;
    s.karts.clear();
    s.racers.clear();
    s.draws.clear();
    s.baseIsSonic = false;
    s.course = CourseCollision();
    s.courseGeneration = ~0ull;
}

void Setup(CpuContext* ctx, uint32_t manager, uint32_t key) {
    State& s = S();
    std::lock_guard<std::mutex> lock(s.mutex);
    s.key = key;
    s.karts.clear();
    s.racers.clear();
    s.draws.clear();
    s.course = CourseCollision();
    s.courseGeneration = ~0ull;
    s.itemLogs = s.bumpLogs = 0;
    s.baseIsSonic = false;
    if (!SonicResources::Get().Ready()) return;
    const uint32_t players = g::Ptr(manager + 0x20);
    const int count = std::min<int>(g::U8(manager + 0x24), 24);
    if (!players || count <= 0) return;
    namespace cfg = RuntimeConfigFile;
    s.modelCollision = Lower(cfg::SonicCollision()) != "kart";
    s.kartScale = cfg::SonicKartScale();
    bool otherBase = false;
    for (int i = 0; i < count; ++i) {
        Kart k;
        k.player = g::Ptr(players + 4u * uint32_t(i));
        if (!k.player) continue;
        k.idx = int(g::Call(ctx, kLinkGetPlayerIdx, {k.player}) & 0xFF);
        k.pointers = k.player + 0x1C;
        k.status = g::Ptr(k.pointers + 4);
        k.movement = g::Ptr(k.pointers + 0x28);
        k.collision = g::Ptr(k.pointers + 0x30);
        k.physics = g::Call(ctx, kLinkGetPhysics, {k.player});
        k.holder = g::Call(ctx, kLinkGetPhysicsHolder, {k.player});
        k.bsp = g::Call(ctx, kLinkGetBsp, {k.player});
        if (!g::Valid(k.physics, 0x160)) continue;
        int hud = 0, type = 0, character = 0;
        if (IsSonicPlayer(k.idx, hud, type, character)) {
            Racer r;
            r.kart = int(s.karts.size());
            r.hud = hud;
            r.sonic = std::make_unique<SonicRacer>();
            if (!r.sonic->Init(SonicResources::Get().Assets())) continue;
            k.racer = int(s.racers.size());
            s.racers.push_back(std::move(r));
        } else if (character == game::BaseCharacter()) {
            otherBase = true;
        }
        if (DebugLogging()) {
            RT_LOGF(RT_TAG_SONIC, "race player %d: kart 0x%08X physics 0x%08X type %d hud %d character %d%s\n", k.idx,
                    k.player, k.physics, type, hud, character, k.racer >= 0 ? " -> SONIC" : "");
        }
        s.karts.push_back(k);
    }
    if (s.racers.empty()) return;
    s.baseIsSonic = !otherBase;
    const RacerTuning& tuning = Tuning();
    for (Racer& r : s.racers) r.sonic->SetTuning(tuning);
    LoadCourse(tuning.scale);
    RT_LOGF(RT_TAG_SONIC, "race: %zu Sonic player(s) of %zu, collision %s%s\n", s.racers.size(), s.karts.size(),
            s.modelCollision ? "model" : "kart", s.course.Ready() ? "" : " (no course collision yet)");
}

RacerInput ReadInput(int playerIdx, bool& finished) {
    RacerInput in;
    finished = false;
    const uint32_t raceinfo = g::Ptr(kRaceinfo);
    const uint32_t players = raceinfo ? g::Ptr(raceinfo + 0xC) : 0;
    const uint32_t player = players ? g::Ptr(players + 4u * uint32_t(playerIdx)) : 0;
    if (!player) return in;
    finished = (g::U32(player + 0x38) & 0x22u) != 0;
    const uint32_t holder = g::Ptr(player + 0x48);
    if (!holder) return in;
    const uint32_t state = holder + 0x28;
    const uint16_t actions = g::U16(state + 4);
    in.accelerate = (actions & 1) != 0;
    in.brake = (actions & 2) != 0;
    in.item = (actions & 4) != 0;
    in.drift = (actions & 8) != 0;
    in.stickX = sonic::clampf(g::F32(state + 8), -1, 1);
    in.stickY = sonic::clampf(g::F32(state + 0xC), -1, 1);
    if (!std::isfinite(in.stickX)) in.stickX = 0;
    if (!std::isfinite(in.stickY)) in.stickY = 0;
    in.trick = g::U8(state + 0x12) != 0;
    return in;
}

// `trustTimers`: use the respawn timers (Movement +0x234, Collision +0x48); a
// racer stops trusting them if they never clear (wrong offsets on some build).
KartView ReadKart(const Kart& k, int stage, bool trustTimers, bool& lakituOut) {
    KartView v;
    v.pos = g::Vec(k.physics + 0x68);
    ReadQuat(k.physics + 0xF0, v.rot);
    v.vel = g::Vec(k.physics + 0xD4);
    if (!FiniteVec(v.vel)) v.vel = Vec3();
    v.raceStarted = stage >= kStageRace;
    const uint32_t b0 = g::U32(k.status + 4);
    const uint32_t b1 = g::U32(k.status + 8);
    v.inCannon = (b1 & 0x10u) != 0;
    v.boosting = (b0 & (0x100000u | 0x2000000u | 0x80000000u)) != 0;
    v.hitByItem = (b1 & 1u) != 0;
    const bool oob = (b0 & 0x10u) != 0;
    const bool lakitu = g::U16(k.movement + 0x234) != 0 || g::S16(k.collision + 0x48) > 0;
    v.respawning = oob || (lakitu && trustTimers) || (b1 & 2u) != 0;
    lakituOut = lakitu;
    return v;
}

void WriteKart(const Kart& k, const KartWrite& w) {
    if (!FiniteVec(w.pos) || !FiniteVec(w.vel)) return;
    g::SetVec(k.physics + 0x68, w.pos);
    g::SetVec(k.physics + 0x74, Vec3());
    g::SetVec(k.physics + 0xB0, Vec3());
    g::SetVec(k.physics + 0xC8, Vec3());
    g::SetVec(k.physics + 0xD4, w.vel);
    g::SetF32(k.physics + 0xE0, sonic::length(w.vel));
    g::SetVec(k.physics + 0x14C, w.vel);
    WriteQuat(k.physics + 0xF0, w.rot);
    WriteQuat(k.physics + 0x100, w.rot);
    g::SetF32(k.movement + 0x20, w.speed);
    if (g::Valid(k.holder, 0xCC)) {
        g::SetVec(k.holder + 0x18, w.pos);
        // transform: rows of a 3x4 matrix (rotation | translation)
        const Vec3 x = QuatRotate(w.rot, Vec3(1, 0, 0));
        const Vec3 y = QuatRotate(w.rot, Vec3(0, 1, 0));
        const Vec3 z = QuatRotate(w.rot, Vec3(0, 0, 1));
        const float m[12] = {x.x, y.x, z.x, w.pos.x, x.y, y.y, z.y, w.pos.y, x.z, y.z, z.z, w.pos.z};
        for (int i = 0; i < 12; ++i) g::SetF32(k.holder + 0x9C + 4u * uint32_t(i), m[i]);
    }
}

// ---- kart bumps against Sonic's body ---------------------------------------------

struct Ball {
    Vec3 c;
    float r;
};

void KartHitboxes(const Kart& k, std::vector<Ball>& out) {
    out.clear();
    if (!g::Valid(k.bsp, 4 + 16 * 0x18)) return;
    const Vec3 pos = g::Vec(k.physics + 0x68);
    float q[4];
    ReadQuat(k.physics + 0xF0, q);
    float scale = g::F32(k.movement + 0x164);
    if (!(scale > 0.01f && scale < 10.0f)) scale = 1.0f;
    for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t h = k.bsp + 4 + i * 0x18;
        if (g::U16(h) == 0) continue;
        const Vec3 local = g::Vec(h + 4);
        const float radius = g::F32(h + 0x10);
        if (!FiniteVec(local) || !(radius > 0 && radius < 1000)) continue;
        out.push_back({pos + QuatRotate(q, local * scale), radius * scale});
    }
}

// Pushes `body` out of `other`; returns the push direction (from other to body)
// weighted by depth, and the deepest overlap.
float Overlap(const std::vector<BodySphere>& body, const std::vector<Ball>& other, Vec3& normal) {
    float deepest = 0;
    Vec3 sum;
    for (const BodySphere& b : body) {
        for (const Ball& o : other) {
            Vec3 d = b.center - o.c;
            const float dist = sonic::length(d);
            const float pen = b.radius + o.r - dist;
            if (pen <= 0) continue;
            if (dist < 1e-3f) d = Vec3(0, 1, 0);
            sum += sonic::normalize(d, Vec3(0, 1, 0)) * pen;
            deepest = std::max(deepest, pen);
        }
    }
    normal = sum;
    return deepest;
}

void Bumps() {
    State& s = S();
    std::vector<Ball> hitboxes;
    for (Racer& r : s.racers) {
        if (r.sonic->Phase() != RacerPhase::Running) continue;
        const BodySphere bounds = r.sonic->BodyBounds();
        for (const Kart& k : s.karts) {
            const int self = int(&r - s.racers.data());
            if (k.racer == self) continue;
            if (k.racer >= 0 && k.racer < self) continue;  // Sonic/Sonic pairs: handled once
            const Vec3 kartPos = g::Vec(k.physics + 0x68);
            if (sonic::length(kartPos - bounds.center) > bounds.radius + 400.0f) continue;
            std::vector<Ball> other;
            float otherMass = 2.0f;
            if (k.racer >= 0) {
                const Racer& o = s.racers[size_t(k.racer)];
                if (o.sonic->Phase() != RacerPhase::Running) continue;
                for (const BodySphere& b : o.sonic->Body()) other.push_back({b.center, b.radius});
                otherMass = 1.0f;
            } else {
                KartHitboxes(k, hitboxes);
                other = hitboxes;
            }
            if (other.empty()) continue;
            Vec3 n;
            const float depth = Overlap(r.sonic->Body(), other, n);
            if (depth <= 0) continue;
            n.y *= 0.3f;  // shove sideways, not into the ground
            n = sonic::normalize(n, Vec3(1, 0, 0));
            const float sonicMass = 1.0f;
            const float total = sonicMass + otherMass;
            const Vec3 sonicVel = r.sonic->Velocity();
            const Vec3 otherVel = k.racer >= 0 ? s.racers[size_t(k.racer)].sonic->Velocity() : g::Vec(k.physics + 0xD4);
            const float closing = sonic::dot(sonicVel - otherVel, n);  // < 0: moving into each other
            float impulse = closing < 0 ? -(1.0f + 0.4f) * closing / (1.0f / sonicMass + 1.0f / otherMass) : 0.0f;
            impulse = std::max(impulse, 3.0f * sonicMass);  // always a noticeable shove
            r.sonic->Push(n * (depth * otherMass / total), n * (impulse / sonicMass));
            if (k.racer >= 0) {
                s.racers[size_t(k.racer)].sonic->Push(n * (-depth * sonicMass / total), n * (-impulse / otherMass));
            } else {
                g::SetVec(k.physics + 0x68, kartPos - n * (depth * sonicMass / total));
                g::SetVec(k.physics + 0x74, g::Vec(k.physics + 0x74) - n * (impulse / otherMass));
            }
            if (DebugLogging() && s.bumpLogs < 40) {
                ++s.bumpLogs;
                RT_LOGF(RT_TAG_SONIC, "bump: Sonic (player %d) and player %d, depth %.1f, impulse %.1f\n",
                        s.karts[size_t(r.kart)].idx, k.idx, depth, impulse);
            }
        }
    }
}

void Tick(CpuContext* ctx) {
    State& s = S();
    ++s.frame;
    if (!RuntimeConfigFile::SonicEnabled() || !SonicResources::Get().Ready()) return;
    const uint32_t manager = g::Ptr(kKartManager);
    const uint32_t players = manager ? g::Ptr(manager + 0x20) : 0;
    if (!manager || !players) {
        if (s.key) Reset();
        return;
    }
    const uint32_t key = manager ^ (players << 1) ^ g::U8(manager + 0x24);
    if (key != s.key) Setup(ctx, manager, key);
    if (s.racers.empty()) return;
    if (!s.course.Ready()) LoadCourse(s.tuning.scale);

    const uint32_t raceinfo = g::Ptr(kRaceinfo);
    const int stage = raceinfo ? int(g::U32(raceinfo + 0x28)) : 0;
    const CourseCollision* course = s.course.Ready() ? &s.course : nullptr;
    for (Racer& r : s.racers) {
        const Kart& k = s.karts[size_t(r.kart)];
        for (const Kart& other : s.karts) {
            if (&other != &k) r.sonic->AddHomingTarget(other.idx, g::Vec(other.physics + 0x68));
        }
        bool finished = false;
        const RacerInput in = ReadInput(k.idx, finished);
        bool lakitu = false;
        KartView view = ReadKart(k, stage, r.trustTimers, lakitu);
        r.gamePos = view.pos;
        r.lakituFrames = (lakitu && stage >= kStageRace) ? r.lakituFrames + 1 : 0;
        if (r.trustTimers && r.lakituFrames > 600) {
            r.trustTimers = false;
            RT_LOGF(RT_TAG_SONIC, "Sonic (player %d): the respawn timers never clear; ignoring them\n", k.idx);
        }
        view.finished = finished || stage > kStageRace + 1;
        if (view.hitByItem && !r.reportedHit && DebugLogging()) {
            RT_LOGF(RT_TAG_SONIC, "Sonic (player %d) was hit\n", k.idx);
        }
        r.reportedHit = view.hitByItem;
        const KartWrite w = r.sonic->Update(in, view, course);
        if (w.write) {
            WriteKart(k, w);
            ++r.writes;
        }
        // The stand-in kart's own models (body, wheels) are hidden by the game;
        // the driver is skipped by the draw hook.
        if (const uint32_t visibility = g::Ptr(k.pointers + 0x58)) {
            g::Call(ctx, kModelsVisibilitySet, {visibility, 0u});
        }
    }
    if (s.modelCollision) Bumps();

    std::lock_guard<std::mutex> lock(s.mutex);
    s.draws.clear();
    for (Racer& r : s.racers) {
        r.sonic->RefreshPose();
        game::RacerDraw d;
        d.kartPos = g::Vec(s.karts[size_t(r.kart)].physics + 0x68);
        d.gamePos = r.gamePos;
        d.posed = &r.sonic->Posed();
        d.id = s.karts[size_t(r.kart)].idx;
        d.hideRadius = s.modelCollision ? 40.0f : 150.0f;
        s.draws.push_back(d);
    }
}

int RacerForPlayer(uint32_t player) {
    for (const Kart& k : S().karts) {
        if (k.player == player) return k.racer;
    }
    return -1;
}

}  // namespace race_detail

namespace game {

void RacersForDraw(std::vector<RacerDraw>& out) {
    auto& s = race_detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    out = s.draws;
}

bool RaceActive() {
    auto& s = race_detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return !s.racers.empty();
}

bool RaceBaseIsSonic() {
    auto& s = race_detail::S();
    std::lock_guard<std::mutex> lock(s.mutex);
    return !s.racers.empty() && s.baseIsSonic;
}

uint32_t FrameNumber() { return race_detail::S().frame; }

}  // namespace game
}  // namespace sonic_mkw

// Kart::Manager::Update: every kart, then the Sonic racers on top.
extern "C" void Kart_Manager_Update_Sonic_8058ffe8(CpuContext* ctx) {
    const uint32_t self = ctx->gpr[3];
    func_8058ffe8(ctx);
    (void)self;
    try {
        sonic_mkw::race_detail::Tick(ctx);
    } catch (const std::exception& error) {
        static int reports = 0;
        if (reports++ < 5) RT_LOGF(RT_TAG_SONIC, "race update failed: %s\n", error.what());
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8058FFE8, Kart_Manager_Update_Sonic_8058ffe8, "Kart_Manager_Update_Sonic_8058ffe8");

// Kart::Movement::UpdateScale: a Sonic player's kart stays tiny (collision = model).
extern "C" void Kart_Movement_UpdateScale_Sonic_8058160c(CpuContext* ctx) {
    const uint32_t movement = ctx->gpr[3];
    func_8058160c(ctx);
    using namespace sonic_mkw;
    auto& s = race_detail::S();
    if (s.racers.empty() || !s.modelCollision) return;
    const uint32_t pointers = guest::Ptr(movement);
    for (const auto& k : s.karts) {
        if (k.racer < 0 || k.pointers != pointers) continue;
        const float scale = s.kartScale;
        guest::SetVec(movement + 0x164, sonic::Vec3(scale, scale, scale));
        static bool logged = false;
        if (!logged) {
            logged = true;
            RT_LOGF(RT_TAG_SONIC, "kart of Sonic (player %d) shrunk to %.2f\n", k.idx, scale);
        }
        break;
    }
}
REGISTER_NATIVE_FUNCTION_AS(0x8058160C, Kart_Movement_UpdateScale_Sonic_8058160c,
                            "Kart_Movement_UpdateScale_Sonic_8058160c");

// Item::Obj::CheckKartCollision: for a Sonic player, items hit his body.
extern "C" void Item_Obj_CheckKartCollision_Sonic_807a14d4(CpuContext* ctx) {
    using namespace sonic_mkw;
    namespace g = guest;
    auto& s = race_detail::S();
    const uint32_t item = ctx->gpr[3];
    const uint32_t player = ctx->gpr[4];
    const int racerIndex = (!s.racers.empty() && s.modelCollision) ? race_detail::RacerForPlayer(player) : -1;
    if (racerIndex < 0) {
        func_807a14d4(ctx);
        return;
    }
    const auto& racer = s.racers[size_t(racerIndex)];
    const auto& kart = s.karts[size_t(racer.kart)];
    const int owner = g::U8(item + 0x6C);
    if (owner == kart.idx || racer.sonic->Phase() != RacerPhase::Running) {
        func_807a14d4(ctx);  // his own items, or the game moving him: the game decides
        return;
    }
    if (g::U32(item + 0x74) & 1u) {  // already killed
        ctx->gpr[3] = 0;
        return;
    }
    const sonic::Vec3 pos = g::Vec(item + 0x44);
    const uint32_t entity = g::Ptr(item + 0xB0);
    float radius = entity ? g::F32(entity + 4) : 0.0f;
    if (!(radius > 1.0f && radius < 2000.0f)) radius = 40.0f;
    bool hit = false;
    for (const auto& b : racer.sonic->Body()) {
        if (sonic::length(b.center - pos) < b.radius + radius) {
            hit = true;
            break;
        }
    }
    if (DebugLogging() && s.itemLogs < 60 && (hit || sonic::length(pos - racer.sonic->BodyBounds().center) < 400.0f)) {
        ++s.itemLogs;
        RT_LOGF(RT_TAG_SONIC, "item 0x%08X (owner %d, radius %.0f) vs Sonic (player %d): %s\n", item, owner, radius,
                kart.idx, hit ? "HIT" : "miss");
    }
    ctx->gpr[3] = hit ? 1u : 0u;
}
REGISTER_NATIVE_FUNCTION_AS(0x807A14D4, Item_Obj_CheckKartCollision_Sonic_807a14d4,
                            "Item_Obj_CheckKartCollision_Sonic_807a14d4");

// Kart::Manager::~Manager: the race is over.
extern "C" void Kart_Manager_dtor_Sonic_8058fdd4(CpuContext* ctx) {
    sonic_mkw::race_detail::Reset();
    func_8058fdd4(ctx);
}
REGISTER_NATIVE_FUNCTION_AS(0x8058FDD4, Kart_Manager_dtor_Sonic_8058fdd4, "Kart_Manager_dtor_Sonic_8058fdd4");
