// sonic_racer.cpp - see sonic_racer.h.
#include "sonic/sonic_racer.h"

#include <algorithm>
#include <cmath>

namespace sonic_mkw {

namespace racer_detail {

using sonic::Mat4;
using sonic::Vec3;

// Kart-matched mode only: a kart reaches its top speed in ~2-3 s. SADX Sonic
// takes ~3.6 s to reach his own (much lower) one, and with his speeds doubled he
// would take far longer, so accelerations get an extra push there.
constexpr float kAccelBoost = 4.0f;

Vec3 QuatRotate(const float q[4], const Vec3& v) {
    // v' = v + 2w (u x v) + 2 u x (u x v), u = (x, y, z)
    const Vec3 u(q[0], q[1], q[2]);
    const Vec3 t = sonic::cross(u, v) * 2.0f;
    return v + t * q[3] + sonic::cross(u, t);
}

void QuatFromBasis(const Vec3& x, const Vec3& y, const Vec3& z, float out[4]) {
    // Columns x, y, z of a rotation matrix.
    const float m00 = x.x, m10 = x.y, m20 = x.z;
    const float m01 = y.x, m11 = y.y, m21 = y.z;
    const float m02 = z.x, m12 = z.y, m22 = z.z;
    const float trace = m00 + m11 + m22;
    float qx, qy, qz, qw;
    if (trace > 0) {
        const float s = std::sqrt(trace + 1.0f) * 2.0f;
        qw = 0.25f * s;
        qx = (m21 - m12) / s;
        qy = (m02 - m20) / s;
        qz = (m10 - m01) / s;
    } else if (m00 > m11 && m00 > m22) {
        const float s = std::sqrt(1.0f + m00 - m11 - m22) * 2.0f;
        qw = (m21 - m12) / s;
        qx = 0.25f * s;
        qy = (m01 + m10) / s;
        qz = (m02 + m20) / s;
    } else if (m11 > m22) {
        const float s = std::sqrt(1.0f + m11 - m00 - m22) * 2.0f;
        qw = (m02 - m20) / s;
        qx = (m01 + m10) / s;
        qy = 0.25f * s;
        qz = (m12 + m21) / s;
    } else {
        const float s = std::sqrt(1.0f + m22 - m00 - m11) * 2.0f;
        qw = (m10 - m01) / s;
        qx = (m02 + m20) / s;
        qy = (m12 + m21) / s;
        qz = 0.25f * s;
    }
    const float len = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
    out[0] = qx / len;
    out[1] = qy / len;
    out[2] = qz / len;
    out[3] = qw / len;
}

const Vec3 kAxes[4] = {Vec3(1, 0, 0), Vec3(-1, 0, 0), Vec3(0, 0, 1), Vec3(0, 0, -1)};

Vec3 Horizontal(const Vec3& v, const Vec3& fallback) {
    return sonic::normalize(Vec3(v.x, 0, v.z), fallback);
}

int YawOf(const Vec3& forward) {
    return sonic::radToBams(std::atan2(forward.z, forward.x)) & 0xFFFF;
}

// A flat floor big enough to reach top speed on, for MeasureTopSpeed().
class FlatFloor : public sonic::ICollision {
public:
    bool raycast(const Vec3& from, const Vec3& dir, float maxDist, sonic::RayHit& hit,
                 sonic::u32 mask) const override {
        (void)mask;
        if (dir.y >= -1e-6f || from.y < 0) return false;
        const float t = -from.y / dir.y;
        if (t > maxDist) return false;
        hit.t = t;
        hit.pos = from + dir * t;
        hit.normal = Vec3(0, 1, 0);
        hit.flags = sonic::SURF_SOLID;
        hit.tri = 0;
        return true;
    }
    int resolveSphere(Vec3& center, float radius, std::vector<sonic::Contact>& contacts,
                      sonic::u32 mask) const override {
        (void)mask;
        if (center.y >= radius) return 0;
        const float depth = radius - center.y;
        center.y = radius;
        contacts.push_back({Vec3(0, 1, 0), depth, sonic::SURF_SOLID, 0});
        return 1;
    }
};

// An endless plane through the origin, falling along +X at `degrees`.
class Slope : public sonic::ICollision {
public:
    explicit Slope(float degrees) {
        const float a = degrees * 3.14159265f / 180.0f;
        normal_ = Vec3(std::sin(a), std::cos(a), 0);
    }
    bool raycast(const Vec3& from, const Vec3& dir, float maxDist, sonic::RayHit& hit,
                 sonic::u32 mask) const override {
        (void)mask;
        const float d0 = sonic::dot(from, normal_);
        const float dd = sonic::dot(dir, normal_);
        if (dd >= -1e-6f || d0 < 0) return false;
        const float t = -d0 / dd;
        if (t > maxDist) return false;
        hit.t = t;
        hit.pos = from + dir * t;
        hit.normal = normal_;
        hit.flags = sonic::SURF_SOLID;
        hit.tri = 0;
        return true;
    }
    int resolveSphere(Vec3& center, float radius, std::vector<sonic::Contact>& contacts,
                      sonic::u32 mask) const override {
        (void)mask;
        const float d = sonic::dot(center, normal_);
        if (d >= radius) return 0;
        center = center + normal_ * (radius - d);
        contacts.push_back({normal_, radius - d, sonic::SURF_SOLID, 0});
        return 1;
    }

private:
    Vec3 normal_;
};

}  // namespace racer_detail

using racer_detail::Vec3;

namespace racer_detail {

// Steady running speed (SADX units per frame) with the given physics.
float MeasureTopSpeedWith(const sonic::Assets& assets, const sonic::PhysicsParams* params) {
    sonic::Sonic s;
    if (!s.init(assets)) return 6.0f;
    if (params) s.physics().P = *params;
    FlatFloor floor;
    s.reset(Vec3(0, 0, 0), 0);
    if (params) s.physics().P = *params;
    sonic::InputState in;
    in.moveY = 1.0f;
    float sum = 0;
    int n = 0;
    for (int frame = 0; frame < 1500; ++frame) {
        s.update(in, Vec3(1, 0, 0), floor);
        if (frame >= 1400) {
            const Vec3 v = s.velocity();
            sum += std::sqrt(v.x * v.x + v.z * v.z);
            ++n;
        }
    }
    const float top = n ? sum / float(n) : 6.0f;
    return top > 0.2f ? top : 6.0f;
}

sonic::PhysicsParams ScaledParams(const sonic::PhysicsParams& base, float f, float accel) {
    sonic::PhysicsParams p = base;
    for (float* v : {&p.max_x_spd, &p.max_psh_spd, &p.nocon_speed, &p.slide_speed, &p.jog_speed, &p.run_speed,
                     &p.rush_speed, &p.crash_speed, &p.dash_speed, &p.lim_h_spd, &p.slow_down, &p.run_break,
                     &p.air_break}) {
        *v *= f;
    }
    p.run_accel *= f * accel;
    p.air_accel *= f * accel;
    return p;
}

}  // namespace racer_detail

sonic::PhysicsParams SonicRacer::StockParams(const sonic::Assets& assets, float speed) {
    sonic::Sonic probe;
    sonic::PhysicsParams base;
    if (probe.init(assets)) base = probe.physics().P;
    if (std::fabs(speed - 1.0f) < 1e-4f) return base;
    return racer_detail::ScaledParams(base, sonic::clampf(speed, 0.25f, 4.0f), 1.0f);
}

float SonicRacer::MeasureDownhillSpeed(const sonic::Assets& assets, const sonic::PhysicsParams& params,
                                       float degrees) {
    sonic::Sonic s;
    if (!s.init(assets)) return 0;
    racer_detail::Slope slope(degrees);
    s.reset(Vec3(0, 0, 0), 0);
    s.physics().P = params;
    sonic::InputState in;
    in.moveY = 1.0f;
    float sum = 0;
    int n = 0;
    for (int frame = 0; frame < 1800; ++frame) {
        s.update(in, Vec3(1, 0, 0), slope);
        if (frame >= 1700) {
            sum += sonic::length(s.velocity());
            ++n;
        }
    }
    return n ? sum / float(n) : 0;
}

sonic::PhysicsParams SonicRacer::DownhillParams(const sonic::Assets& assets, float speed, float acceleration,
                                                float* topOut) {
    // On flat ground, above max_x_spd SADX pushes Sonic forward with
    // run_accel * 0.4 and drags him back with (speed - max_x_spd) * air_resist * 1.7,
    // so his top speed is max_x_spd + 0.4 * run_accel / (1.7 * -air_resist). Raise
    // max_x_spd until that lands on the downhill speed, then check by measuring.
    sonic::PhysicsParams p = StockParams(assets, speed);
    const float target = MeasureDownhillSpeed(assets, p, 30.0f);
    if (topOut) *topOut = target;
    const float accel = sonic::clampf(acceleration, 0.25f, 8.0f);
    p.lim_frict *= accel;
    p.run_accel *= accel;
    if (!(target > MeasureTopSpeed(assets) * speed)) return p;  // nothing to raise
    const float push = p.air_resist < 0 ? 0.4f * p.run_accel / (1.7f * -p.air_resist) : 1.5f;
    p.max_x_spd = std::max(p.max_x_spd, target - push);
    for (int i = 0; i < 3; ++i) {
        const float measured = racer_detail::MeasureTopSpeedWith(assets, &p);
        if (std::fabs(measured - target) < 0.05f) break;
        p.max_x_spd = std::max(0.5f, p.max_x_spd + (target - measured));
    }
    return p;
}

float SonicRacer::MeasureTopSpeed(const sonic::Assets& assets) {
    return racer_detail::MeasureTopSpeedWith(assets, nullptr);
}

sonic::PhysicsParams SonicRacer::Calibrate(const sonic::Assets& assets, float targetTopSpeed, float* factorOut) {
    // Scale speeds and accelerations until Sonic's steady running speed on flat
    // ground matches the target (SADX units per frame).
    sonic::Sonic probe;
    sonic::PhysicsParams base;
    if (probe.init(assets)) base = probe.physics().P;
    float f = targetTopSpeed / std::max(MeasureTopSpeed(assets), 0.5f);
    sonic::PhysicsParams p = racer_detail::ScaledParams(base, f, racer_detail::kAccelBoost);
    for (int i = 0; i < 4; ++i) {
        const float measured = racer_detail::MeasureTopSpeedWith(assets, &p);
        const float ratio = targetTopSpeed / measured;
        if (std::fabs(ratio - 1.0f) < 0.02f) break;
        f *= sonic::clampf(ratio, 0.5f, 2.0f);
        p = racer_detail::ScaledParams(base, f, racer_detail::kAccelBoost);
    }
    if (factorOut) *factorOut = f;
    return p;
}

bool SonicRacer::Init(const sonic::Assets& assets) {
    if (!sonic_.init(assets)) return false;
    sonic_.homingRange = 60.0f;
    initialized_ = false;
    phase_ = RacerPhase::Waiting;
    return true;
}

void SonicRacer::SetTuning(const RacerTuning& tuning) {
    tuning_ = tuning;
    ApplyTuning();
}

void SonicRacer::ApplyTuning() {
    if (tuning_.calibrated) sonic_.physics().P = tuning_.params;
    // Boost panels and mushrooms: a quarter above his flat-ground top speed (MKW's
    // mushroom adds 40%, a panel ~30%), never less than SADX's dash speed * 1.45.
    const sonic::PhysicsParams& p = sonic_.physics().P;
    const float flatTop = p.max_x_spd + (p.air_resist < 0 ? 0.4f * p.run_accel / (1.7f * -p.air_resist) : 1.5f);
    boostSpeed_ = std::max(p.dash_speed * 1.45f, flatTop * 1.25f);
}

Vec3 SonicRacer::KartForward(const float q[4]) const {
    const int axis = std::clamp(tuning_.forwardAxis, 0, 3);
    return racer_detail::QuatRotate(q, racer_detail::kAxes[axis]);
}

void SonicRacer::MakeKartRotation(const Vec3& forward, const Vec3& up, float out[4]) const {
    const Vec3 u = sonic::normalize(up, Vec3(0, 1, 0));
    Vec3 f = sonic::normalize(forward - u * sonic::dot(forward, u), Vec3(0, 0, 1));
    // Local axes of the kart: forward axis -> f, +Y -> u, the third completes a
    // right-handed basis.
    Vec3 x, z;
    switch (std::clamp(tuning_.forwardAxis, 0, 3)) {
        case 0: x = f; z = sonic::cross(x, u); break;            // +X forward
        case 1: x = -f; z = sonic::cross(x, u); break;           // -X forward
        case 3: z = -f; x = sonic::cross(u, z); break;           // -Z forward
        default: z = f; x = sonic::cross(u, z); break;           // +Z forward
    }
    racer_detail::QuatFromBasis(sonic::normalize(x), u, sonic::normalize(z), out);
}

Vec3 SonicRacer::WorldPosition() const {
    return sonic_.position() * tuning_.scale;
}

void SonicRacer::AddHomingTarget(int id, const Vec3& mkwPos) {
    const float inv = 1.0f / std::max(tuning_.scale, 0.1f);
    // aim at the middle of the kart, roughly Sonic's chest height
    sonic_.addHomingTarget(id, mkwPos * inv + Vec3(0, 3, 0));
}

void SonicRacer::Follow(const KartView& kart, const CourseCollision* course) {
    const float inv = 1.0f / std::max(tuning_.scale, 0.1f);
    const Vec3 forward = racer_detail::Horizontal(KartForward(kart.rot), Vec3(0, 0, 1));
    Vec3 feet = (kart.pos - Vec3(0, tuning_.rideHeight, 0)) * inv;
    bool onGround = false;
    if (course && course->Ready() && !kart.inCannon && !kart.respawning) {
        sonic::RayHit hit;
        if (course->GroundBelow(feet + Vec3(0, 3, 0), 8.0f, hit)) {
            feet = hit.pos;
            onGround = true;
        }
    }
    const Vec3 vel = kart.vel * inv;
    sonic_.puppet(feet, onGround ? vel : vel, forward, onGround, kart.inCannon);
    passengerSpeed_ = std::sqrt(vel.x * vel.x + vel.z * vel.z);
}

void SonicRacer::Resume(const KartView& kart, const CourseCollision* course) {
    const float inv = 1.0f / std::max(tuning_.scale, 0.1f);
    const Vec3 forward = racer_detail::Horizontal(KartForward(kart.rot), Vec3(0, 0, 1));
    Vec3 feet = (kart.pos - Vec3(0, tuning_.rideHeight, 0)) * inv;
    if (course && course->Ready()) {
        sonic::RayHit hit;
        if (course->GroundBelow(feet + Vec3(0, 3, 0), 8.0f, hit)) feet = hit.pos;
    }
    sonic_.reset(feet, racer_detail::YawOf(forward));
    ApplyTuning();  // reset() keeps P, but be safe if a respawn re-read it
    const Vec3 vel = kart.vel * inv;
    if (sonic::length(vel) > 0.2f) sonic_.launch(vel, 0, sonic::PlayerState::Normal);
    boostFrames_ = 0;
}

void SonicRacer::ApplySurface(const KartView& kart) {
    sonic::Player& p = sonic_.physics();
    if (kart.boosting) boostFrames_ = std::max(boostFrames_, 2);
    if (p.grounded) {
        const uint32_t type = KclTypeOf(p.groundFlags);
        switch (type) {
            case KCL_BOOST_PANEL:
            case KCL_BOOST_RAMP:
                boostFrames_ = std::max(boostFrames_, 60);
                break;
            case KCL_JUMP_PAD:
                if (p.state != sonic::PlayerState::Spring) {
                    const Vec3 fwd = racer_detail::Horizontal(p.facing, Vec3(1, 0, 0));
                    sonic_.launch(fwd * std::max(sonic::length(p.vel), p.P.dash_speed) + Vec3(0, 4.0f, 0), 15);
                }
                break;
            // Grass, sand and dirt (offroad) slow karts, not Sonic: he is on foot.
            default:
                break;
        }
    }
    if (boostFrames_ > 0) {
        --boostFrames_;
        if (p.grounded && p.state != sonic::PlayerState::Hurt) {
            p.spd.x = std::max(p.spd.x, boostSpeed_);
        }
    }
}

KartWrite SonicRacer::Update(const RacerInput& input, const KartView& kart, const CourseCollision* course) {
    KartWrite out;
    const bool courseReady = course && course->Ready();
    const bool gameDrives = !kart.raceStarted || kart.finished || kart.respawning || kart.inCannon || !courseReady;
    if (hurtCooldown_ > 0) --hurtCooldown_;
    if (!initialized_ || gameDrives) {
        initialized_ = true;
        phase_ = kart.raceStarted ? RacerPhase::Passenger : RacerPhase::Waiting;
        Follow(kart, course);
        prevDrift_ = input.drift;
        prevBrake_ = input.brake;
        prevTrick_ = input.trick;
        chargeBuffered_ = input.brake;
        lastKartPos_ = kart.pos;
        return out;
    }
    if (phase_ != RacerPhase::Running) {
        Resume(kart, course);
        phase_ = RacerPhase::Running;
    }

    sonic::InputState in;
    float sx = sonic::clampf(input.stickX, -1, 1);
    float sy = sonic::clampf(input.stickY, -1, 1);
    // Holding accelerate runs forward, so wheel and remote players can steer with
    // the tilt alone; the stick's own up/down still works.
    if (input.accelerate && sy < 0.9f) {
        sy = std::max(sy, 1.0f - std::fabs(sx) * 0.2f);
        sx *= 0.6f;  // gentler aim while running forward: steer, don't spin
    }
    const float mag = std::sqrt(sx * sx + sy * sy);
    if (mag > 1.0f) {
        sx /= mag;
        sy /= mag;
    }
    if (mag < 0.15f) sx = sy = 0;
    in.moveX = sx;
    in.moveY = sy;
    in.jump = (input.drift && !prevDrift_) || (input.trick && !prevTrick_);
    in.jumpHeld = input.drift;
    sonic::Player& p = sonic_.physics();
    // B is the spin dash. A press that came while Sonic was in the air or still
    // a passenger (held through the countdown, say) charges as soon as he can.
    const bool brakePressed = input.brake && !prevBrake_;
    const bool canCharge = p.grounded && (p.mode == sonic::MD_Stand || p.mode == sonic::MD_Run);
    if (!input.brake) chargeBuffered_ = false;
    in.action = brakePressed || (chargeBuffered_ && canCharge);
    if (brakePressed && !canCharge && p.mode != sonic::MD_Jump && p.mode != sonic::MD_Roll &&
        p.mode != sonic::MD_Homing) {
        chargeBuffered_ = true;  // in those three B means "uncurl"
    }
    if (in.action && canCharge) chargeBuffered_ = false;
    in.actionHeld = input.brake;
    prevDrift_ = input.drift;
    prevBrake_ = input.brake;
    prevTrick_ = input.trick;

    Vec3 camForward = racer_detail::Horizontal(kart.cameraForward, Vec3(0, 0, 0));
    if (sonic::length(camForward) < 0.5f) camForward = racer_detail::Horizontal(p.facing, Vec3(1, 0, 0));

    if (kart.hitByItem && hurtCooldown_ == 0) {
        sonic_.hurt(sonic_.position() + racer_detail::Horizontal(p.facing, Vec3(1, 0, 0)) * 3.0f);
        hurtCooldown_ = 60;
        boostFrames_ = 0;
    }

    // The game handles falling off the course (Lakitu); SonicCore's own respawn
    // is disabled. If no fall boundary catches Sonic, put him back on the last
    // ground he stood on.
    sonic_.update(in, camForward, course->World(), -1e30f);
    ApplySurface(kart);
    if (p.grounded) {
        if (++safeTimer_ >= 30) {
            safeTimer_ = 0;
            lastSafe_ = p.pos;
            lastSafeForward_ = p.facing;
            haveSafe_ = true;
        }
        fallFrames_ = 0;
    } else if (p.pos.y < course->KillY()) {
        if (++fallFrames_ > 90 && haveSafe_) {
            sonic_.reset(lastSafe_ + Vec3(0, 2, 0), racer_detail::YawOf(lastSafeForward_));
            ApplyTuning();
            fallFrames_ = 0;
        }
    }

    const float scale = tuning_.scale;
    out.write = true;
    out.pos = p.pos * scale + p.up * tuning_.rideHeight;
    out.vel = p.vel * scale;
    out.speed = std::sqrt(out.vel.x * out.vel.x + out.vel.z * out.vel.z);
    const Vec3 up = p.grounded ? p.up : Vec3(0, 1, 0);
    const Vec3 forward = sonic::length(Vec3(p.vel.x, 0, p.vel.z)) > 0.3f ? Vec3(p.vel.x, 0, p.vel.z) : p.facing;
    MakeKartRotation(racer_detail::Horizontal(forward, Vec3(1, 0, 0)), up, out.rot);
    lastKartPos_ = out.pos;
    return out;
}

void SonicRacer::RefreshPose() {
    sonic::DrawList list;
    sonic_.draw(list);
    const float scale = tuning_.scale;
    BuildBatches(list, sonic::Mat4::scale(Vec3(scale, scale, scale)), posed_);
    body_.clear();
    for (const sonic::DrawItem& item : list.items) {
        if (!item.model || item.textureSet != sonic::TEXSET_SONIC || item.model->verts.empty()) continue;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        for (const sonic::NjVertex& v : item.model->verts) {
            const Vec3 w = item.world.transformPoint(v.pos);
            lo = sonic::vmin(lo, w);
            hi = sonic::vmax(hi, w);
        }
        BodySphere sphere;
        sphere.center = (lo + hi) * (0.5f * scale);
        // Half the box's middle extent: a sphere around a limb's box overshoots
        // along its length but a long thin part is covered by its neighbours.
        Vec3 ext = (hi - lo) * 0.5f;
        float e[3] = {ext.x, ext.y, ext.z};
        std::sort(e, e + 3);
        sphere.radius = std::max(e[1], 0.5f * e[2]) * scale;
        if (sphere.radius < 0.3f * scale) continue;  // eyes, teeth, tiny bits
        body_.push_back(sphere);
    }
    if (body_.empty()) {
        // No model parts (should not happen): a capsule-ish pair of spheres.
        const sonic::Player& p = sonic_.physics();
        body_.push_back({(p.pos + p.up * 2.5f) * scale, 2.5f * scale});
        body_.push_back({(p.pos + p.up * 7.0f) * scale, 3.0f * scale});
    }
}

BodySphere SonicRacer::BodyBounds() const {
    BodySphere out;
    if (body_.empty()) return out;
    Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
    for (const BodySphere& b : body_) {
        lo = sonic::vmin(lo, b.center - Vec3(b.radius, b.radius, b.radius));
        hi = sonic::vmax(hi, b.center + Vec3(b.radius, b.radius, b.radius));
    }
    out.center = (lo + hi) * 0.5f;
    out.radius = sonic::length(hi - lo) * 0.5f;
    return out;
}

void SonicRacer::Push(const Vec3& offset, const Vec3& addVelocity) {
    if (phase_ != RacerPhase::Running) return;
    const float inv = 1.0f / std::max(tuning_.scale, 0.1f);
    sonic_.physics().push(offset * inv, addVelocity * inv);
}

void SonicRacer::Hurt(const Vec3& from) {
    if (phase_ != RacerPhase::Running || hurtCooldown_ > 0) return;
    const float inv = 1.0f / std::max(tuning_.scale, 0.1f);
    sonic_.hurt(from * inv);
    hurtCooldown_ = 60;
    boostFrames_ = 0;
}

Vec3 SonicRacer::Velocity() const { return sonic_.physics().vel * tuning_.scale; }

void SonicRacer::BuildDraw(PosedSonic& out) {
    sonic::DrawList list;
    sonic_.draw(list);
    BuildBatches(list, sonic::Mat4::scale(Vec3(tuning_.scale, tuning_.scale, tuning_.scale)), out);
}

}  // namespace sonic_mkw
