#pragma once
#include "collision.h"
#include "formats/pe_image.h"

namespace sonic {

// Where Sonic (re)spawns. yaw is a BAMS angle (0x10000 = 360 degrees, 0 = facing +X).
struct StartPos {
    Vec3 pos;
    int yaw = 0;
};

struct InputState {
    float moveX = 0, moveY = 0;  // stick: x = right, y = forward (magnitude <= 1)
    float lookX = 0, lookY = 0;  // camera look delta (radians this frame)
    bool jump = false, jumpHeld = false;
    bool action = false, actionHeld = false;  // B / X: spin dash
    bool boost = false;
};

// The original game's per-character physics table (PhysicsData, 0x84 bytes).
// Values are loaded from sonic.exe at runtime; these defaults are Sonic's.
struct PhysicsParams {
    int jump2_timer = 60;
    float pos_error = 2, lim_h_spd = 16, lim_v_spd = 16, max_x_spd = 3, max_psh_spd = 0.6f;
    float jmp_y_spd = 1.66f, nocon_speed = 3, slide_speed = 0.23f, jog_speed = 0.46f, run_speed = 1.39f;
    float rush_speed = 2.3f, crash_speed = 3.7f, dash_speed = 5.09f, jmp_addit = 0.076f, run_accel = 0.05f;
    float air_accel = 0.031f, slow_down = -0.06f, run_break = -0.18f, air_break = -0.17f;
    float air_resist_air = -0.028f, air_resist = -0.008f, air_resist_y = -0.01f, air_resist_z = -0.4f;
    float grd_frict = -0.1f, grd_frict_z = -0.6f, lim_frict = -0.2825f, rat_bound = 0.3f;
    float rad = 4, height = 10, weight = 0.08f, eyes_height = 7, center_height = 5.4f;
    bool loadFromExe(const PeImage& exe, int character);
};

// Gameplay-facing state (derived from the original action mode)
enum class PlayerState { Normal, Jump, Roll, SpinCharge, Spring, Hurt, Homing };

// Homing attack candidates (the game's per-frame target list, DAT_03b259c0)
struct HomingTarget {
    int id;
    Vec3 pos;
    float dist;
};

// Original Sonic action modes (twp->mode) that this port implements
enum SonicMode {
    MD_Stand = 1, MD_Run = 2, MD_SpinCharge = 4, MD_Roll = 5, MD_Jump = 8, MD_Spring = 9,
    MD_Fall = 12, MD_Skid = 13, MD_Homing = 14, MD_Hurt = 0x2D,
};

class Player {
public:
    void reset(const StartPos& sp);
    void update(const InputState& in, const Vec3& camForward, const ICollision& col, float killY);

    // ---- original state ------------------------------------------------------
    int mode = MD_Stand;
    Vec3 pos;                  // twp->pos (at the feet)
    int ang[3] = {0, 0, 0};    // twp->ang (BAMS); local->world = Rz(z) Rx(x) Ry(-y)
    u16 flags = 0;             // twp->flag: 1|2 = on ground, 0x100 = ball, 0x400 = attack
    Vec3 wvel;                 // mwp->spd (world velocity)
    int angAim = 0;            // mwp->ang_aim
    Vec3 acc;                  // pwp->acc (local)
    Vec3 spd;                  // pwp->spd (local: x forward, y up, z side)
    float upY = 1;             // pwp+0x1C: world-up component of the character's up axis
    float spindashSpeed = 0;   // pwp+0x00
    int jumpTimer = 0;         // pwp+0x08
    int nocontimer = 0;        // pwp+0x0A (input lock)
    int ballTimer = 0;         // pwp+0x7E: frames spent curled up (drives the DX spin-ball flicker)
    PhysicsParams P;

    // ---- engine-facing view --------------------------------------------------
    Vec3 vel;                 // world velocity (units/frame)
    Vec3 up{0, 1, 0};
    Vec3 facing{1, 0, 0};
    Vec3 groundNormal{0, 1, 0};
    bool grounded = false;
    bool braking = false;
    PlayerState state = PlayerState::Normal;
    int rings = 0, deaths = 0;
    float spinCharge = 0;
    int controlLock = 0;
    u32 groundFlags = 0;
    int anim = 0;
    float animFrame = 0;

    float speed() const { return length(vel); }
    bool attacking() const { return (flags & 0x400) != 0 || mode == MD_Homing; }
    // filled by the object system each frame before update()
    std::vector<HomingTarget> targets;
    int homingTarget = -1;   // id of the locked target, -1 if none
    // enemy / item box hit while attacking: bounce off it
    void bounce(float upSpeed = 2.0f);
    Vec3 feet() const { return pos; }
    Vec3 center() const { return pos + up * P.rad; }
    // external launch (springs, dash panels): world velocity
    void launch(const Vec3& worldVel, int lockFrames, PlayerState st = PlayerState::Spring);
    void hurt(const Vec3& from);
    // Something outside the physics shoved the character (another racer, say):
    // move it by `offset` and add `addWorldVel` to its velocity.
    void push(const Vec3& offset, const Vec3& addWorldVel);
    // Hosts that move the character themselves (cutscenes, vehicles, replays):
    // place it with a world velocity and facing, grounded or airborne, curled up
    // or not, without running the physics. Animation and effects follow as usual.
    void setPuppet(const Vec3& feet, const Vec3& worldVel, const Vec3& forward, const Vec3& upDir, bool onGround,
                   bool curled);

    // events consumed by the game for sounds
    bool evHoming = false, evBounce = false;
    bool evJumped = false, evLanded = false, evSpinDash = false, evRespawn = false, evSkid = false, evSpinCharge = false;

private:
    // input as the game sees it: world angle + magnitude
    bool stick(int* aim = nullptr, float* mag = nullptr) const;
    bool hasInput_ = false;
    int stickAng_ = 0;
    float stickMag_ = 0;
    bool jumpPressed_ = false, jumpHeld_ = false, bPressed_ = false, bHeld_ = false;

    // orientation helpers (FUN_0043ec90 / FUN_0043ec00)
    Mat4 localToWorld() const;
    Vec3 toWorld(const Vec3& v) const { return localToWorld().transformDir(v); }
    Vec3 toLocal(const Vec3& v) const;
    Vec3 gravityLocal() const;

    // ported routines
    void runActions();                  // Sonic_RunsActions (mode transitions)
    void pGetRotation();                // 0x44BB60
    void pGetAcceleration();            // 0x44C270
    void pGetAccelerationAir();         // 0x44B9C0
    void pGetAccelerationRoll();        // 0x443650
    void pGetAccelerationBrake();       // 0x448E50
    void pAirUpright();                 // 0x443AD0
    void pGetSpeed();                   // 0x443F50
    void pSetPosition(const ICollision& col);  // 0x44CDF0 + level collision
    void pResetPosition();              // 0x43EE70
    void turnNormal(int aim);           // 0x443C50
    void turnFast(int aim);             // 0x443E60
    void turnKeep(int aim);             // 0x443DF0
    void slopeTurn();                   // 0x4491E0
    void alignToFloor(const Vec3& n);
    bool checkFall();                   // 0x494F70
    bool checkJump();                   // 0x495E60
    bool checkSpinDash();               // 0x496EE0
    bool checkStop();                   // 0x494FF0
    void homingStart();                 // 0x494B80
    void homingStep();                  // 0x492300
    int homingTimer_ = 0;               // pwp+0x80
    int homingFrames_ = 0;              // pwp+0x82
    bool stickReversed() const;         // 0x4429C0
    void landed();
    void syncView();

    StartPos start_;
    int hurtTimer_ = 0;
    int skidTimer_ = 0;
};

}  // namespace sonic
