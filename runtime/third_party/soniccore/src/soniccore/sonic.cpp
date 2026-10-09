#include "sonic.h"
#include <cmath>

namespace sonic {

bool Sonic::init(const Assets& assets) {
    if (!model_.load(assets, "SONIC")) {
        SONIC_LOGE("Sonic's model could not be loaded");
        return false;
    }
    model_.setAnimTable(kSonicAnims, kSonicAnimCount);
    model_.playAnim(SA_Stand, true);
    effects_.init(assets);
    return true;
}

void Sonic::reset(const Vec3& feet, int yaw) {
    StartPos sp;
    sp.pos = feet;
    sp.yaw = yaw;
    player_.reset(sp);
    model_.playAnim(SA_Stand, true);
    effects_.clear();
    idleTime_ = 0;
}

void Sonic::addHomingTarget(int id, const Vec3& pos) {
    float d = length(pos - player_.center());
    if (d < homingRange) player_.targets.push_back({id, pos, d});
}

void Sonic::update(const InputState& input, const Vec3& cameraForward, const ICollision& collision, float killY) {
    sounds_.clear();
    player_.update(input, cameraForward, collision, killY);
    player_.targets.clear();
    const Player& p = player_;
    auto snd = [&](const std::string& r, float v) {
        if (!r.empty()) sounds_.push_back({r, v});
    };
    if (p.evJumped) snd(soundMap.jump, 0.8f);
    if (p.evHoming) snd(soundMap.homing, 0.8f);
    if (p.evSpinCharge) snd(soundMap.spinCharge, 0.8f);
    if (p.evSpinDash) snd(soundMap.spinDash, 0.8f);
    if (p.evSkid) snd(soundMap.skid, 0.7f);
    if (p.evLanded) snd(soundMap.land, 0.6f);
    if (p.evRespawn) effects_.clear();
    effects_.update(player_);
    updateAnimation();
}

void Sonic::puppet(const Vec3& feet, const Vec3& worldVel, const Vec3& forward, bool onGround, bool curled) {
    sounds_.clear();
    player_.targets.clear();
    player_.setPuppet(feet, worldVel, forward, Vec3(0, 1, 0), onGround, curled);
    effects_.update(player_);
    updateAnimation();
}

void Sonic::updateAnimation() {
    // Picks animation IDs the way Sonic's state machine does in the original game;
    // the IDs index the game's own SonicAnimData table (transitions, blend and
    // playback speed all come from it).
    Player& p = player_;
    float spd = length(projectOnPlane(p.vel, p.up));
    int cur = model_.animId();
    bool curDone = model_.animFinished();
    int want = cur;
    float animSpeed = spd;
    if (p.grounded && p.state == PlayerState::Normal && spd < 0.05f) idleTime_ += 1.0f / 60.0f;
    else idleTime_ = 0;
    switch (p.state) {
        case PlayerState::Homing:
        case PlayerState::Jump: want = SA_Jump; animSpeed = std::max(p.speed(), 2.0f); break;
        case PlayerState::Roll: want = SA_Roll; animSpeed = std::max(spd, 1.0f); break;
        case PlayerState::SpinCharge: want = SA_SpinCharge; animSpeed = 4.0f + p.spinCharge * 2.0f; break;
        case PlayerState::Spring: want = SA_Spring; break;
        case PlayerState::Hurt: want = SA_Hurt; break;
        default:
            if (!p.grounded) {
                if (cur != SA_FallStart && cur != SA_Fall && cur != SA_Spring) want = SA_FallStart;
                if (cur == SA_Spring && p.vel.y < -0.5f) want = SA_FallStart;
            } else if (p.evLanded && spd < 0.5f && cur != SA_Jump) {
                want = SA_Land;
            } else if (p.braking && spd > 1.0f) {
                want = SA_Brake;
            } else if (spd < 0.05f) {
                bool oneShotPlaying = (cur == SA_Land || cur == SA_IdleLook || cur == 1 || cur == 2) && !curDone;
                if (!oneShotPlaying) want = SA_Stand;
                if (idleTime_ > 8.0f) {
                    want = SA_IdleLook;
                    idleTime_ = 0;
                }
            } else if (spd < 0.6f) want = SA_Walk;
            else if (spd < 1.39f) want = SA_Walk2;
            else if (spd < 2.3f) want = SA_Jog;
            else if (spd < 5.0f) want = SA_Run;
            else want = SA_Dash;
            break;
    }
    if (want != cur) model_.playAnim(want);
    model_.tickAnim(animSpeed);
    p.anim = model_.animId();
    p.animFrame = model_.animFrame();
}

void Sonic::draw(DrawList& out) {
    if (!model_.loaded()) return;
    const Player& p = player_;
    Vec3 fwd = normalize(projectOnPlane(p.facing, p.up), Vec3(1, 0, 0));
    // character models face -X
    Vec3 mx = -fwd;
    Vec3 z = cross(mx, p.up);
    Mat4 root = Mat4::translate(p.feet()) * Mat4::basis(mx, p.up, z) * Mat4::translate(Vec3(0, -model_.footOffset(), 0));
    // Sonic_Display (0x4948C0): rolling on the ground squashes and tilts the ball
    int anim = model_.animId();
    if (anim == SA_Roll && p.grounded)
        root = root * Mat4::translate(Vec3(0, -1, 0)) * Mat4::rotZ(bamsToRad(0x2000)) * Mat4::scale(Vec3(0.7f, 1.1f, 0.8f));
    // While curled up the game alternates between the curled model and the DX spin
    // ball (animation 32 -> SONIC_ACTIONS[21]): the ball is shown whenever
    // (pwp->0x7E & 0x11) != 0, i.e. 24 of every 32 frames.
    bool ball = dxSpinBall && (p.flags & 0x100) && anim != 0x91 && (p.ballTimer & 0x11);
    const AnimDef* ballDef = model_.animDef(32);
    if (ball && ballDef && model_.hasAction(ballDef->action))
        model_.drawAction(out, ballDef->action, model_.animFrame(), root);
    else
        model_.drawAnimated(out, root);
    if (showEffects) effects_.draw(out);
}

TouchResult Sonic::touchEnemy(const Vec3& c, float radius, float bounceSpeed) {
    Vec3 sc = player_.center();
    float r = radius + player_.P.rad + 1.0f;
    if (length2(c - sc) > r * r) return TouchResult::None;
    if (player_.attacking()) {
        if (!player_.grounded) player_.bounce(bounceSpeed);
        return TouchResult::Attacked;
    }
    if (player_.state != PlayerState::Hurt) {
        player_.hurt(c);
        return TouchResult::Damaged;
    }
    return TouchResult::None;
}

std::vector<NjModel*> Sonic::allModels() {
    auto a = model_.allModels();
    auto b = effects_.allModels();
    a.insert(a.end(), b.begin(), b.end());
    return a;
}

}  // namespace sonic
