// sonic_racer.h - Sonic racing on foot in a Mario Kart Wii race.
//
// A SonicRacer is one Sonic player. Every race frame the game's kart for that
// player still runs (laps, positions, items, camera, the AI's view of the field
// all keep working), but the racer owns its movement: Sonic runs SonicCore's
// ported SADX physics on the course collision with the player's input, and the
// result is written back into the kart, which becomes an invisible stand-in.
//
// Phases where the game must drive the kart (countdown, Lakitu respawn, cannons,
// after the finish line) turn Sonic into a passenger who follows the kart; when
// the game lets go he resumes from wherever the kart is.
//
// Pure host code (no guest memory): sonic_race_hook.cpp does the game plumbing.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "sonic/sonic_course.h"
#include "sonic/sonic_render.h"
#include "soniccore/sonic.h"

namespace sonic_mkw {

// The player's race controls, as the game reports them.
struct RacerInput {
    float stickX = 0, stickY = 0;  // -1..1
    bool accelerate = false;       // A / 2
    bool brake = false;            // B / 1
    bool drift = false;            // R / B trigger (hop)
    bool item = false;
    bool trick = false;            // d-pad / shake flick this frame
};

// What the game's kart is doing this frame (Mario Kart units, world space).
struct KartView {
    sonic::Vec3 pos;
    float rot[4] = {0, 0, 0, 1};   // quaternion x, y, z, w
    sonic::Vec3 vel;               // per frame
    bool raceStarted = false;      // countdown over
    bool finished = false;         // crossed the line; the AI drives now
    bool respawning = false;       // fell off: Lakitu is carrying the kart
    bool inCannon = false;
    bool boosting = false;         // mushroom, boost panel or mini-turbo
    bool hitByItem = false;        // shell, banana, bomb... this frame
    sonic::Vec3 cameraForward;     // horizontal direction the camera looks, or zero
};

// What to write back into the kart.
struct KartWrite {
    bool write = false;
    sonic::Vec3 pos;
    float rot[4] = {0, 0, 0, 1};
    sonic::Vec3 vel;
    float speed = 0;               // horizontal, Mario Kart units per frame
};

// Tuning shared by every racer.
struct RacerTuning {
    float scale = 9.0f;        // Mario Kart units per SADX unit (Sonic is ~10 SADX units tall)
    float speed = 1.0f;        // multiplier on Sonic's speeds and accelerations
    float kartTopSpeed = 86.0f;  // the fastest 150cc kart's top speed, Mario Kart units per frame
    float rideHeight = 0.0f;   // kart position above Sonic's feet, Mario Kart units
    int forwardAxis = 2;       // kart local axis that points forward: 0 +X, 1 -X, 2 +Z, 3 -Z
    // The physics Sonic runs with (SonicRacer::StockParams or Calibrate). When
    // not set, SADX's own values are used unchanged.
    bool calibrated = false;
    sonic::PhysicsParams params;
};

enum class RacerPhase { Waiting, Running, Passenger };

// One piece of Sonic's body for collisions (Mario Kart units, world space).
struct BodySphere {
    sonic::Vec3 center;
    float radius = 0;
};

class SonicRacer {
public:
    bool Init(const sonic::Assets& assets);
    void SetTuning(const RacerTuning& tuning);

    // One 60 Hz race frame (after the game's kart update).
    KartWrite Update(const RacerInput& input, const KartView& kart, const CourseCollision* course);

    // Opponents Sonic can home in on (Mario Kart world positions), for this frame.
    void AddHomingTarget(int id, const sonic::Vec3& mkwPos);

    // Sonic and his effects as world-space batches (Mario Kart units).
    void BuildDraw(PosedSonic& out);
    // Poses Sonic once for the frame (after Update): his draw batches and his
    // collision body, one sphere per part of his model (head, torso, arms, legs,
    // shoes, quills... or the spin ball), so collisions follow his animation.
    void RefreshPose();
    const PosedSonic& Posed() const { return posed_; }
    const std::vector<BodySphere>& Body() const { return body_; }
    // Bounding sphere of the whole body (Mario Kart units).
    BodySphere BodyBounds() const;

    // Something shoved Sonic (a kart bumping into him): move him by `offset` and
    // add `addVelocity` (Mario Kart units, per frame).
    void Push(const sonic::Vec3& offset, const sonic::Vec3& addVelocity);
    // Hit by an item or hazard coming from `from` (Mario Kart units).
    void Hurt(const sonic::Vec3& from);
    sonic::Vec3 Velocity() const;  // Mario Kart units per frame
    const std::vector<sonic::SoundEvent>& Sounds() const { return sonic_.sounds(); }

    RacerPhase Phase() const { return phase_; }
    sonic::Vec3 WorldPosition() const;  // Mario Kart units (feet)
    sonic::Sonic& Core() { return sonic_; }

    // SADX Sonic's steady running speed on flat ground (SADX units per frame).
    static float MeasureTopSpeed(const sonic::Assets& assets);
    // SADX's own physics with every speed and acceleration multiplied by `speed`
    // (1 = exactly SADX).
    static sonic::PhysicsParams StockParams(const sonic::Assets& assets, float speed = 1.0f);
    // SADX's physics with only the flat-ground speed cap raised, so that Sonic's
    // steady running speed on flat ground equals the speed SADX itself gives him
    // running down a steep (30 degree) slope. Acceleration, turning and every
    // other value stay SADX's. `topOut` receives that speed (SADX units/frame).
    // `acceleration` multiplies how hard he can push off the ground (SADX's
    // traction limit and run acceleration); 1 = SADX, where flat-ground
    // acceleration is held to ~0.023 units/frame^2 by traction.
    static sonic::PhysicsParams DownhillParams(const sonic::Assets& assets, float speed = 1.0f,
                                               float acceleration = 1.0f, float* topOut = nullptr);
    // SADX Sonic's steady speed running down a slope of `degrees` (SADX units/frame).
    static float MeasureDownhillSpeed(const sonic::Assets& assets, const sonic::PhysicsParams& params,
                                      float degrees);
    // Physics whose steady running speed is `targetTopSpeed` (SADX units/frame),
    // with quicker acceleration to match a kart's.
    static sonic::PhysicsParams Calibrate(const sonic::Assets& assets, float targetTopSpeed, float* factorOut = nullptr);

private:
    void ApplyTuning();
    void Resume(const KartView& kart, const CourseCollision* course);
    void Follow(const KartView& kart, const CourseCollision* course);
    sonic::Vec3 KartForward(const float q[4]) const;
    void MakeKartRotation(const sonic::Vec3& forward, const sonic::Vec3& up, float out[4]) const;
    void ApplySurface(const KartView& kart);

    sonic::Sonic sonic_;
    RacerTuning tuning_;
    PosedSonic posed_;
    std::vector<BodySphere> body_;
    RacerPhase phase_ = RacerPhase::Waiting;
    bool prevDrift_ = false, prevBrake_ = false, prevTrick_ = false;
    bool chargeBuffered_ = false;
    float boostSpeed_ = 0;  // SADX units/frame a boost panel or mushroom pushes Sonic to  // B went down while Sonic couldn't spin dash yet
    int boostFrames_ = 0;
    int hurtCooldown_ = 0;
    sonic::Vec3 lastKartPos_;
    bool initialized_ = false;
    // passenger animation
    float passengerSpeed_ = 0;
    // last ground Sonic stood on, for courses without a fall boundary below
    sonic::Vec3 lastSafe_, lastSafeForward_{1, 0, 0};
    bool haveSafe_ = false;
    int safeTimer_ = 0, fallFrames_ = 0;
};

}  // namespace sonic_mkw
