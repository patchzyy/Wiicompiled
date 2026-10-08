// SonicCore public API: one header to include.
//
//   sonic::Assets assets;   assets.load("path/to/SonicAssets");   // once
//   sonic::Sonic  sonic;    sonic.init(assets);  sonic.reset({x, y, z}, 0);
//   every 60 Hz tick:   sonic.setHomingTargets(...); sonic.update(input, camForward, collision);
//                       for (auto& s : sonic.sounds()) play(assets.sound(s.ref), s.volume);
//   every frame:        sonic.draw(drawList);  -> render the DrawItems
//
// Units and axes follow Sonic Adventure: Y is up, 1 unit is about Sonic's ankle
// height (he is ~10 units tall), speeds are units per 1/60 s frame, and angles are
// BAMS (0x10000 = 360 degrees). Sonic's position is at his feet.
#pragma once
#include "assets.h"
#include "character.h"
#include "collision.h"
#include "effects.h"
#include "player.h"

namespace sonic {

struct SoundEvent {
    std::string ref;  // "BANK:ENTRY", look it up with Assets::sound()
    float volume = 1.0f;
};

// Which sound plays for what (sound IDs from sonic.exe mapped through its bank table).
struct SoundMap {
    std::string jump = "COMMON_BANK00:B00_00_17";              // 0x11
    std::string homing = "P_SONICTAILS_BANK03:B03_00_00";      // 0x2FA
    std::string spinCharge = "P_SONICTAILS_BANK03:B03_00_05";  // 0x2FF
    std::string spinDash = "P_SONICTAILS_BANK03:B03_00_06";    // 0x300
    std::string skid = "COMMON_BANK00:B00_00_18";              // 0x12
    std::string land = "COMMON_BANK00:B00_00_33";              // 0x21
};

enum class TouchResult { None, Attacked, Damaged };

class Sonic {
public:
    bool init(const Assets& assets);
    bool ready() const { return model_.loaded(); }

    // Place Sonic (feet position) facing `yaw` (BAMS; 0 = +X, 0x4000 = +Z).
    void reset(const Vec3& feet, int yaw = 0);

    // Homing attack candidates for the next update(): id is yours, pos is the point
    // to aim at (the target's centre). Cleared after every update().
    void addHomingTarget(int id, const Vec3& pos);
    float homingRange = 100.0f;  // targets further than this from Sonic are ignored

    // One 60 Hz game tick. `cameraForward` turns the stick into a world direction
    // (stick up = away from the camera). Falling below `killY` respawns Sonic.
    void update(const InputState& input, const Vec3& cameraForward, const ICollision& collision, float killY = -1e30f);

    // Add Sonic (and his effects) to a draw list.
    void draw(DrawList& out);

    // Sounds started during the last update().
    const std::vector<SoundEvent>& sounds() const { return sounds_; }
    SoundMap soundMap;

    // ---- gameplay hooks for your objects --------------------------------------------
    // Sonic touching an enemy-like sphere: destroys it when he is attacking (rolling,
    // curled up, homing; bouncing him off it when airborne) and hurts him otherwise.
    TouchResult touchEnemy(const Vec3& center, float radius, float bounceSpeed = 2.0f);
    bool attacking() const { return player_.attacking(); }
    void bounce(float upSpeed = 2.0f) { player_.bounce(upSpeed); }
    void hurt(const Vec3& from) { player_.hurt(from); }
    // springs, dash panels: world velocity, frames without control
    void launch(const Vec3& worldVelocity, int lockFrames, PlayerState st = PlayerState::Spring) { player_.launch(worldVelocity, lockFrames, st); }

    // ---- state ---------------------------------------------------------------------
    Vec3 position() const { return player_.pos; }       // feet
    Vec3 center() const { return player_.center(); }    // body centre (collision sphere)
    Vec3 velocity() const { return player_.vel; }       // world units / frame
    Vec3 up() const { return player_.up; }
    Vec3 facing() const { return player_.facing; }
    bool grounded() const { return player_.grounded; }
    PlayerState state() const { return player_.state; }
    int animation() const { return model_.animId(); }   // SonicAnimData index

    // Full access to the ported physics (modes, local velocity, PhysicsParams...)
    Player& physics() { return player_; }
    const Player& physics() const { return player_; }
    CharacterModel& model() { return model_; }
    EffectSystem& effects() { return effects_; }

    bool dxSpinBall = true;  // false: Dreamcast-style curled Sonic only
    bool showEffects = true;

    // Every mesh SonicCore may put in a draw list (to upload up front).
    std::vector<NjModel*> allModels();

private:
    void updateAnimation();
    Player player_;
    CharacterModel model_;
    EffectSystem effects_;
    std::vector<SoundEvent> sounds_;
    float idleTime_ = 0;
};

}  // namespace sonic
