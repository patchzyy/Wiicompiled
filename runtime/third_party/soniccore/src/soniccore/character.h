#pragma once
#include "assets.h"
#include "anim_table.h"
#include "weld_table.h"

namespace sonic {

// A playable character's model + animation set, read from CHRMODELS(_orig).dll.
// Every instance owns its own copy of the meshes (limb welds rewrite vertices).
class CharacterModel {
public:
    bool load(const Assets& assets, const std::string& prefix = "SONIC");
    void unload();
    // the raw SONIC_ACTIONS[action] at `frame` (model viewers)
    void draw(DrawList& out, int action, float frame, const Mat4& root);
    // a raw action without disturbing the animator's pose (used for the DX spin ball)
    void drawAction(DrawList& out, int action, float frame, const Mat4& root);
    // every mesh, for renderers that want to upload up front
    std::vector<NjModel*> allModels();

    // --- custom poses (mix actions, edit joints) ---
    // Which skeleton an action animates (-1 if the action does not exist).
    int actionSkeleton(int action) const { return hasAction(action) ? actions_[action].skel : -1; }
    const NjSkeleton* skeleton(int index) const { return index >= 0 && index < (int)skels_.size() ? &skels_[index] : nullptr; }
    // The per-node local transforms of an action at `frame` (wrapped into the action).
    bool actionLocals(int action, float frame, std::vector<NjLocal>& out) const;
    // Draw a skeleton in a pose given as per-node local transforms.
    void drawLocals(DrawList& out, int skel, const std::vector<NjLocal>& locals, const Mat4& root);

    // --- animation-ID playback, driven by the game's SonicAnimData table ---
    void setAnimTable(const AnimDef* table, int count) { table_ = table; tableCount_ = count; }
    void playAnim(int id, bool restart = false);
    // Advance one 60 Hz frame. `speed` is the character's speed (units/frame),
    // used by speed-scaled animations (walk/run/roll).
    void tickAnim(float speed);
    void drawAnimated(DrawList& out, const Mat4& root);
    int animId() const { return animId_; }
    float animFrame() const { return animFrame_; }
    bool animFinished() const { return animDone_; }
    const AnimDef* animDef(int id) const { return (table_ && id >= 0 && id < tableCount_) ? &table_[id] : nullptr; }
    int animCount() const { return tableCount_; }
    int actionCount() const { return (int)actions_.size(); }
    bool hasAction(int a) const { return a >= 0 && a < (int)actions_.size() && actions_[a].skel >= 0; }
    float actionFrames(int a) const { return hasAction(a) ? float(std::max<u32>(actions_[a].motion.frames, 1)) : 1.0f; }
    // Approximate height of the bind pose (used for the collision capsule)
    float height() const { return height_; }
    float footOffset() const { return footOffset_; }
    bool loaded() const { return !skels_.empty(); }

private:
    // SADX joins limb segments with small "weld" meshes whose vertices are moved
    // every frame by the game. We approximate this with two-bone rigid skinning.
    struct Weld {
        int node = -1;     // weld mesh node
        int other = -1;    // joint the far end follows
        std::vector<bool> follow;       // per NjVertex: follows `other`
        std::vector<Vec3> localPos, localNrm;  // in `other` joint space (bind pose)
        std::vector<Vec3> origPos, origNrm;
    };
    std::vector<std::vector<Weld>> welds_;  // per skeleton (generic fallback)
    // Exact welds from the game's SonicWeldInfo table
    struct GameWeld {
        int nodeA = -1, nodeB = -1;
        std::vector<std::pair<int, int>> pairs;  // {point in A, point in B}
    };
    std::vector<std::vector<GameWeld>> gameWelds_;  // per skeleton
    std::vector<u32> objectVAs_;                    // ___<CHAR>_OBJECTS entries
    std::vector<std::vector<Vec3>> workPts_, workNrm_;  // per model of the skeleton being drawn
    void setupGameWelds(const std::vector<WeldDef>& defs);
    void setupWelds(int skel);
    struct Action {
        int skel = -1;
        NjMotion motion;
    };
    std::vector<NjSkeleton> skels_;
    std::vector<u32> skelVA_;
    std::vector<Action> actions_;
    std::vector<Mat4> pose_;
    void drawPose(DrawList& out, int skel, const std::vector<NjLocal>& locals, const Mat4& root);
    const AnimDef* table_ = nullptr;
    int tableCount_ = 0;
    int animId_ = -1;
    float animFrame_ = 0;
    bool animDone_ = false;
    std::vector<NjLocal> locals_, prevLocals_, blendScratch_, actionScratch_;
    float boost_ = 1.55f;  // character light boost (effects like the spin ball use 1.0)
    int prevSkel_ = -1;
    float blend_ = 1.0f, blendRate_ = 1.0f;
    float height_ = 12, footOffset_ = 0;
};

}  // namespace sonic
