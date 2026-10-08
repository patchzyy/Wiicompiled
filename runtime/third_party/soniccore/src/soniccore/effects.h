#pragma once
#include "player.h"
#include "assets.h"

namespace sonic {

// Sonic's SADX visual effects, ported from sonic.exe:
//  - homing / dash streak (task 0x4A2AA0: SONIC_OBJECTS[57] tube, s_dash textures)
//  - glowing ball afterimages (task 0x4A2A40: SONIC_OBJECTS[56], s_ball_a/b)
//  - homing start burst (task 0x4A2A70)
class EffectSystem {
public:
    bool init(const Assets& assets);
    std::vector<NjModel*> allModels();
    void clear() { streaks_.clear(); sprites_.clear(); }
    // one 60 Hz tick, after the player has moved
    void update(const Player& p);
    void draw(DrawList& out);
    size_t activeCount() const { return streaks_.size() + sprites_.size(); }

private:
    struct Streak {
        Vec3 tail, head;  // tube runs from head (x=0 ring) back to tail (x=8 ring)
        int count = 0;
    };
    struct Sprite {
        Vec3 offset, base;  // drawn at base + offset; base follows Sonic while `follow`
        int count = 0, life = 5;
        float scale = 1, scaleStep = 0, fade = 0.2f;
        int list = 0;       // 0 = s_ball_a texlist, 1 = s_ball_b texlist
        bool follow = true;
    };
    static Vec3 center(const Player& p) { return p.pos + p.up * 5.0f; }
    void spawnTrail(const Player& p);

    std::vector<Streak> streaks_;
    std::vector<Sprite> sprites_;
    NjSkeleton spriteSk_, tubeSk_;
    int texIndex(const char* name) const;
    std::vector<std::string> names_;
    u32 rng_ = 12345;
    float rnd() { rng_ = rng_ * 1103515245u + 12345u; return float((rng_ >> 16) & 0x7FFF) / 32768.0f; }
    bool ok_ = false;
};

}  // namespace sonic
