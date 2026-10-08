#include "effects.h"
#include <cmath>

namespace sonic {

// s_dash texture shown on frame N of a streak's life (texlists at 0x927BF8 + 8*N,
// the variant the game uses while homing)
static const char* kStreakSeq[19] = {
    "s_dash12", "s_dash01", "s_dash13", "s_dash14", "s_dash14", "s_dash14", "s_dash14", "s_dash14", "s_dash14", "s_dash14",
    "s_dash14", "s_dash15", "s_dash16", "s_dash17", "s_dash18", "s_dash19", "s_dash20", "s_dash21", "s_dash22"};

bool EffectSystem::init(const Assets& assets) {
    ok_ = false;
    clear();
    const PeImage* pe = assets.chrModels();
    if (!pe) return false;
    u32 objs = pe->exportVA("___SONIC_OBJECTS");
    if (!objs) return false;
    NinjaReader nr(*pe);
    spriteSk_ = NjSkeleton();
    tubeSk_ = NjSkeleton();
    if (!nr.readSkeleton(pe->u32at(objs + 56 * 4), spriteSk_) || !nr.readSkeleton(pe->u32at(objs + 57 * 4), tubeSk_)) {
        SONIC_LOGW("Sonic effect models not found");
        return false;
    }
    names_.clear();
    for (auto& t : assets.textures(TEXSET_EFFECTS)) names_.push_back(toLower(t.name));
    if (names_.empty()) return false;
    ok_ = true;
    return true;
}

std::vector<NjModel*> EffectSystem::allModels() {
    std::vector<NjModel*> r;
    for (auto* sk : {&spriteSk_, &tubeSk_})
        for (auto& m : sk->models)
            if (m) r.push_back(m.get());
    return r;
}

int EffectSystem::texIndex(const char* name) const {
    std::string n = toLower(name);
    for (size_t i = 0; i < names_.size(); i++)
        if (names_[i] == n) return (int)i;
    return -1;
}

void EffectSystem::spawnTrail(const Player& p) {
    // 0x4A09F0: Sonic's centre plus a random direction of length 1..3
    Sprite s;
    float len = rnd() * 2.0f + 1.0f;
    float a = rnd() * 6.2831853f, b = rnd() * 6.2831853f;
    Vec3 dir(std::cos(a) * std::cos(b), std::sin(b), std::sin(a) * std::cos(b));
    s.offset = p.up * 5.0f + dir * len;
    s.base = p.pos;
    s.list = rnd() > 0.5f ? 0 : 1;
    s.life = 5;
    s.fade = 0.2f;
    sprites_.push_back(s);
}

void EffectSystem::update(const Player& p) {
    if (!ok_) return;
    Vec3 c = center(p);
    // ---- age streaks (0x4A1260) ----
    for (size_t i = 0; i < streaks_.size();) {
        Streak& s = streaks_[i];
        s.count++;
        bool kill = s.count > 18;
        if (s.count == 1) {
            s.head = c;
            if (length(s.tail - s.head) * 0.125f >= 5.0f) kill = true;  // teleport / respawn
        } else if (s.count == 2) {
            s.head = c;  // the head ring follows Sonic for one more frame
        }
        if (kill) {
            streaks_[i] = streaks_.back();
            streaks_.pop_back();
        } else {
            i++;
        }
    }
    // ---- age sprites (0x4A0F80 / 0x4A10E0) ----
    for (size_t i = 0; i < sprites_.size();) {
        Sprite& s = sprites_[i];
        int prev = s.count++;
        if (prev >= s.life) {
            sprites_[i] = sprites_.back();
            sprites_.pop_back();
            continue;
        }
        if (s.follow) s.base = p.pos;
        s.scale -= s.scaleStep;
        i++;
    }
    // ---- spawn (Sonic_Main: modes 14 = homing, 7 = rolling) ----
    if (p.evHoming) {
        // 0x4A2A70: burst at the start of the attack
        Sprite b;
        b.base = c;
        b.follow = false;
        b.scale = 3.0f;
        b.scaleStep = 0.4f;
        b.life = 10;
        b.fade = 0.1f;
        b.list = rnd() > 0.5f ? 0 : 1;
        sprites_.push_back(b);
    }
    // homing (mode 14) always; rolling / spin dash (mode 5) while spd.x >= run_speed
    if (p.mode == MD_Homing || (p.mode == MD_Roll && p.spd.x >= p.P.run_speed)) {
        Streak s;
        s.tail = s.head = c;
        streaks_.push_back(s);
        spawnTrail(p);
    }
}

void EffectSystem::draw(DrawList& out) {
    if (!ok_) return;
    // streaks: SONIC_OBJECTS[57] is an 8-unit hexagonal tube along +X
    for (auto& s : streaks_) {
        if (s.count < 1) continue;
        Vec3 d = s.tail - s.head;
        float len = length(d);
        if (len < 0.01f) continue;
        float yaw = std::atan2(-d.z, d.x);
        float pitch = std::atan2(d.y, std::sqrt(d.x * d.x + d.z * d.z));
        Mat4 m = Mat4::translate(s.head) * Mat4::rotY(yaw) * Mat4::rotZ(pitch) * Mat4::scale(Vec3(len * 0.125f, 1, 1));
        int ti = texIndex(kStreakSeq[std::min(s.count, 18)]);
        if (ti < 0) continue;
        for (auto& mdl : tubeSk_.models) {
            if (!mdl) continue;
            DrawItem it;
            it.model = mdl.get();
            it.world = m;
            it.textureSet = TEXSET_EFFECTS;
            it.forceTexture = ti;
            it.alpha = 0.999f;
            out.items.push_back(it);
        }
    }
    // glow balls: SONIC_OBJECTS[56] (4.5-unit sphere); its material's texture id is
    // applied to a one-entry s_ball texlist, so like the original it lands on the
    // entries that follow (s_dash06 / s_dash07)
    for (auto& s : sprites_) {
        float f = 1.0f - s.fade * float(s.count);
        if (f <= 0.01f || s.scale <= 0.01f) continue;
        Mat4 m = Mat4::translate(s.base + s.offset) * Mat4::scale(Vec3(s.scale, s.scale, s.scale));
        for (auto& mdl : spriteSk_.models) {
            if (!mdl || mdl->mats.empty()) continue;
            int ti = s.list + int(mdl->mats[0].texId);
            if (ti < 0 || ti >= (int)names_.size()) ti = s.list;
            u8 g = u8(std::min(std::max(f, 0.0f), 1.0f) * 255);
            DrawItem it;
            it.model = mdl.get();
            it.world = m;
            it.textureSet = TEXSET_EFFECTS;
            it.forceTexture = ti;
            it.alpha = std::min(f, 0.999f);
            it.tint = u32(g) | (u32(g) << 8) | (u32(g) << 16) | 0xFF000000u;
            out.items.push_back(it);
        }
    }
}

}  // namespace sonic
