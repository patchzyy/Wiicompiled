// Sonic's movement, ported from the original game's player code in sonic.exe
// (SADX PC, US). Function addresses are given for every ported routine so the
// behaviour can be checked against a disassembly. Velocities are in the
// character's local frame (x = forward, y = up, z = side), units per 60 Hz frame,
// exactly like the original.
#include "player.h"
#include <cmath>

namespace sonic {

static const Vec3 kGravity(0, -1, 0);  // DAT_03b0f0f8 (world gravity direction)

// ---- small helpers from the original -----------------------------------------
static inline int wrapAng(int a) { return a & 0xFFFF; }
static inline int sdiff(int from, int to) { return s16((to - from) & 0xFFFF); }
// FUN_004383b0: absolute 16-bit angle difference
static inline int angDiffAbs(int a, int b) { return std::abs(sdiff(a, b)); }
// FUN_00438350: move angle `cur` toward `target` by at most `step`
static inline int adjustAngle(int cur, int target, int step) {
    int d = sdiff(cur, target);
    if (d > step) d = step;
    if (d < -step) d = -step;
    return wrapAng(cur + d);
}
static inline int radToAng(float r) { return int(std::lround(r * 65536.0f / (2.0f * PI))); }

bool PhysicsParams::loadFromExe(const PeImage& exe, int character) {
    // PhysicsArray: 8 x 0x84 bytes; located by Sonic's distinctive values
    const auto& d = exe.data();
    static const float sig[4] = {16.0f, 16.0f, 3.0f, 0.6f};
    for (size_t o = 0; o + 0x84 * 8 < d.size(); o += 4) {
        if (memcmp(&d[o], sig, 16) != 0) continue;
        if (o < 8) continue;
        size_t b = o - 8 + size_t(character) * 0x84;
        if (rd<s32>(&d[o - 8]) != 60) continue;
        jump2_timer = rd<s32>(&d[b]);
        float* f = &pos_error;
        for (int i = 0; i < 32; i++) f[i] = rd<float>(&d[b + 4 + i * 4]);
        SONIC_LOGI("Physics table found at 0x%X (character %d)", exe.offsetToVA(o - 8), character);
        return true;
    }
    SONIC_LOGW("Physics table not found, using built-in Sonic values");
    return false;
}

// ---- orientation ---------------------------------------------------------------
Mat4 Player::localToWorld() const {
    // FUN_0043ec90: njRotateZ(z), njRotateX(x), njRotateY(-y)
    return Mat4::rotZ(bamsToRad(ang[2])) * Mat4::rotX(bamsToRad(ang[0])) * Mat4::rotY(bamsToRad(-ang[1]));
}
Vec3 Player::toLocal(const Vec3& v) const {
    // FUN_0043ec00: njRotateY(y), njRotateX(-x), njRotateZ(-z)
    Mat4 m = Mat4::rotY(bamsToRad(ang[1])) * Mat4::rotX(bamsToRad(-ang[0])) * Mat4::rotZ(bamsToRad(-ang[2]));
    return m.transformDir(v);
}
Vec3 Player::gravityLocal() const { return toLocal(kGravity * P.weight); }

void Player::alignToFloor(const Vec3& n) {
    // ang.x/ang.z tilt (applied after yaw) so that the local up axis equals n
    Vec3 nn = normalize(n);
    ang[0] = wrapAng(radToAng(std::asin(clampf(nn.z, -1.0f, 1.0f))));
    ang[2] = wrapAng(radToAng(std::atan2(-nn.x, nn.y)));
}

bool Player::stick(int* aim, float* mag) const {
    if (!hasInput_ || nocontimer > 0) return false;
    if (aim) *aim = stickAng_;
    if (mag) *mag = stickMag_;
    return true;
}

// ---- turning -------------------------------------------------------------------
// FUN_00443c50
void Player::turnNormal(int aim) {
    Vec3 old = spd;
    Vec3 w = toWorld(spd);
    int d = angDiffAbs(ang[1], aim);
    int step = d <= 0x1000 ? (d >> 3) : d <= 0x2000 ? (d >> 2) : 0x800;
    ang[1] = adjustAngle(ang[1], aim, step);
    spd = toLocal(w);  // world velocity preserved in the new frame
    float f;
    if (!(flags & 3)) {
        spd = spd * 0.1f + old * 0.9f;
        return;
    }
    const float weight = 1.0f;  // mwp->weight
    if (!stick()) f = 0.05f * (weight < 1 ? weight : 1);
    else if (upY <= 0.4f) f = 0.5f * (weight < 1 ? weight : 1);
    else f = 0.99f * (weight < 1 ? weight : 1);
    spd = spd * f + old * (1.0f - f);
}

// FUN_00443e60
void Player::turnFast(int aim) {
    Vec3 old = spd;
    Vec3 w = toWorld(spd);
    int step = 0x100;
    if (spd.x > P.dash_speed) step = std::max(0x40, 0x100 - int(std::sqrt((spd.x - P.dash_speed) * 0.0625f) * 64.0f));
    ang[1] = adjustAngle(ang[1], aim, step);
    spd = toLocal(w);
    float f = 0.5f, g = 0.5f;
    if (upY > 0.4f) {
        f = 0.99f;
        g = 0.01f;
    }
    spd = spd * f + old * g;
}

// FUN_00443df0: turn without affecting the local velocity
void Player::turnKeep(int aim) { ang[1] = adjustAngle(ang[1], aim, 0x2000); }

// FUN_004491e0: on slopes with no input, slowly face downhill
void Player::slopeTurn() {
    if (nocontimer) return;
    if (length(wvel) > P.jog_speed) {
        Vec3 h = normalize(wvel);
        if (dot(h, kGravity) < -5.00488e-05f) return;  // moving uphill: keep heading
    }
    Vec3 g = toLocal(kGravity);
    if (g.y < 0 && g.y > -0.87f) {
        float gx = std::fabs(g.x);
        int a = radToAng(std::atan2(g.z, gx));
        int rate = radToAng(std::fabs(g.z) > 0 ? std::atan2(std::fabs(g.z), 1.0f) : 0.0f);
        angAim = adjustAngle(angAim, wrapAng(ang[1] - a), std::max(rate, 1));
        turnNormal(angAim);
        return;
    }
    angAim = ang[1];
    turnNormal(angAim);
}

// ---- rotation (stand) FUN_0044bb60 -------------------------------------------------
void Player::pGetRotation() {
    int aim;
    if (!stick(&aim)) {
        if (flags & 3) {
            slopeTurn();
            return;
        }
        aim = ang[1];
    }
    angAim = aim;
    turnNormal(angAim);
}

// ---- ground acceleration FUN_0044c270 ---------------------------------------------
void Player::pGetAcceleration() {
    const PhysicsParams& p = P;
    const float w = 1.0f;  // mwp->weight
    Vec3 g = gravityLocal();
    float gx = g.x, gy = g.y, gz = g.z;
    float extraY = 0, lateral = 0, fwd = 0;
    int aim = 0;
    float mag = 0;
    bool input = stick(&aim, &mag);

    // cross(world velocity, floor normal): sideways running on walls
    Vec3 c = cross(wvel, groundNormal);
    float cl = length(c);
    float cy = cl > 1e-6f ? c.y / cl : 0;

    if (upY >= 0.1f || std::fabs(cy) <= 0.6f || spd.x <= 1.16f) {
        float push = 0;
        bool doPush = false;
        if (upY >= -0.4f || spd.x <= 1.16f) {
            if (upY >= -0.3f || spd.x <= 1.16f) {
                if (upY >= -0.1f || spd.x <= 1.16f) {
                    if (upY >= 0.5f || spd.x >= p.run_speed || spd.x <= -p.run_speed) {
                        if (upY >= 0.7f || spd.x >= p.run_speed || spd.x <= -p.run_speed) {
                            if (upY < 0.87f && spd.x < p.jog_speed && -p.run_speed < spd.x) gz *= 1.4f;
                        } else {
                            gz += gz;
                        }
                    } else {
                        gx *= 4.225f;
                        gz *= 4.225f;
                    }
                } else {
                    push = p.weight * 0.4f;
                    doPush = true;
                }
            } else {
                push = p.weight * 0.8f;
                doPush = true;
            }
        } else {
            push = p.weight * 5.0f;
            doPush = true;
        }
        if (doPush) gy -= push;
    } else {
        gx = 0;
        gy = -p.weight;
        gz = 0;
    }

    // air resistance on the forward axis
    auto resist42d = [&]() {
        if (p.max_x_spd < spd.x) gx += (spd.x - p.max_x_spd) * p.air_resist;
        else if (spd.x < 0) gx += p.air_resist * spd.x;
    };
    if (!input) {
        if (p.run_speed < spd.x) gx += p.air_resist * spd.x;
        else resist42d();
    } else {
        if (spd.x <= p.max_x_spd || upY <= 0.96f) resist42d();
        else gx += (spd.x - p.max_x_spd) * p.air_resist * 1.7f;
    }
    gy += p.air_resist_y * spd.y;
    gz += p.air_resist_z * spd.z;

    if (!input) {
        if (upY >= 0.71f) {
            if (spd.x <= 0) {
                if (spd.x < 0) fwd = -p.slow_down;
            } else {
                fwd = p.slow_down;
            }
        } else {
            slopeTurn();
        }
    } else {
        // stick: choose acceleration from the speed band
        float a = mag;
        if (spd.x >= p.max_x_spd) {
            a = upY >= 0 ? a * p.run_accel * 0.4f : a * p.run_accel;
        } else if (spd.x >= p.jog_speed) {
            if (spd.x >= p.run_speed) {
                if (spd.x >= p.rush_speed) a = a * p.run_accel;
                else {
                    bool soft = a <= 0.9f;
                    a = a * p.run_accel;
                    if (soft) a *= 0.3f;
                }
            } else {
                a = a * p.run_accel;  // (a <= 0.7 path reaches the same value below run_speed)
            }
        } else {
            if (a <= 0.5f && p.jog_speed * 0.4f <= spd.x) a = 0;
            else a = a * p.run_accel;
        }
        int d = angDiffAbs(ang[1], aim);
        braking = false;
        if (spd.x != 0 || d <= 0x1000) {
            float mid = (p.run_speed + p.jog_speed) * 0.5f;
            if (spd.x < mid || d <= 0x1000) {
                if (spd.x < p.jog_speed || d >= 0x1000) {
                    if (p.jog_speed <= spd.x && spd.x <= p.rush_speed && d > 0x2000) a *= 0.8f;
                    angAim = aim;
                    turnNormal(aim);
                } else {
                    angAim = aim;
                    turnFast(aim);
                }
            } else {
                // sharp turn at speed: decelerate while turning
                angAim = aim;
                a = p.slow_down;
                braking = d > 0x4000;
                turnNormal(aim);
            }
        } else {
            angAim = aim;
            a = 0;
            turnKeep(aim);
        }
        fwd = a;
    }

    // forward axis: traction / static friction
    float outX;
    if (upY < 0.71f && spd.x < p.jog_speed && -p.jog_speed < spd.x && !input) {
        gz *= 10.0f;
        outX = gx * 10.0f;
    } else if (spd.x == 0) {
        float lim = p.lim_frict * w * gy;
        outX = gx + fwd;
        if (!input && ((outX < lim && -lim < outX) || (outX < 0.051f && -0.051f < outX))) outX = 0;
    } else if (fwd >= 0) {
        float lim = p.lim_frict * w * gy;
        if (spd.x < 0 && gx < 0.051f && -0.051f < gx) {
            outX = gx + fwd;
            if ((gx + fwd) * fwd < 0) outX = 0;
        } else if (lim <= 0 || lim >= fwd) {
            if (input || spd.x > p.jog_speed || !(gx < 0.051f && -0.051f < gx)) outX = gx + fwd;
            else outX = 0;
        } else {
            outX = gx + fwd;
            if (lim < outX) outX = lim;  // traction limit
        }
    } else {
        // decelerating (slow_down)
        if (spd.x <= 0) {
            float v = p.lim_frict * w * gy + fwd;
            if ((!input && spd.x <= p.jog_speed && gx < 0.051f && -0.051f < gx) || v >= 0) outX = 0;
            else outX = v;
        } else {
            outX = gx + fwd;
            if (fwd * (gx + fwd) < 0) outX = 0;
        }
    }

    // side axis: ground friction
    float outZ = gz;
    if (spd.z != 0) {
        float fr = gy >= 0 ? 0.0f : p.grd_frict_z * w * gy;
        if (gz <= 0) {
            if (gz < 0) lateral = fr;
        } else {
            lateral = -fr;
        }
        outZ = gz + lateral;
        if (gz != 0 && lateral != 0 && gz * outZ < 0) outZ = 0;
    } else {
        float lim = p.lim_frict * w * gy;
        if (gz < lim && -lim < gz) outZ = 0;
    }
    acc = Vec3(outX, gy + extraY, outZ);
}

// ---- air acceleration FUN_0044b9c0 --------------------------------------------------
void Player::pGetAccelerationAir() {
    const PhysicsParams& p = P;
    Vec3 g = gravityLocal();
    g.x += p.air_resist_air * spd.x;
    g.y += p.air_resist_y * spd.y;
    g.z += p.air_resist_z * spd.z;
    int aim;
    float mag = 0, a = 0;
    if (!stick(&aim, &mag)) {
        angAim = ang[1];
    } else {
        int d = angDiffAbs(ang[1], aim);
        if (spd.x <= p.run_speed || d <= 0x6000) {
            angAim = aim;
            if (d <= 0x1000) {
                a = mag * p.air_accel;
                if (wvel.y < 0) a += a;
            }
        } else {
            a = mag * p.air_break;
        }
        turnNormal(angAim);
    }
    acc = Vec3(a + g.x, g.y, g.z);
}

// ---- rolling FUN_00443650 -----------------------------------------------------------
void Player::pGetAccelerationRoll() {
    const PhysicsParams& p = P;
    Vec3 g = gravityLocal();
    if (p.run_speed < spd.x && upY < 0) g.y *= -8.0f;  // stick to loops when upside down
    if (!(flags & 0x100) || upY > 0.98f) g.x += p.air_resist * spd.x;
    else g.x -= spd.x * 0.0002f;
    acc = Vec3(g.x, p.air_resist_y * spd.y + g.y, p.air_resist_z * spd.z + g.z);
}

// ---- braking (spin dash charge / skid) FUN_00448e50 -------------------------------------
void Player::pGetAccelerationBrake() {
    const PhysicsParams& p = P;
    const float w = 1.0f;
    Vec3 g = gravityLocal();
    float gx = p.air_resist * spd.x + g.x;
    float gy = p.air_resist_y * spd.y + g.y;
    float gz = p.air_resist_z * spd.z + g.z;
    float bx = 0, bz = 0;
    if (spd.x > 0) bx = p.run_break * w;
    else if (spd.x < 0) bx = -(p.run_break * w);
    if (spd.z > 0) bz = p.run_break * w;
    else if (spd.z < 0) bz = -(p.run_break * w);
    if (flags & 3) {
        if (spd.x != 0) {
            float fr = gy < 0 ? p.grd_frict * w * gy : 0.0f;
            gx += bx;
            if (gx > 0) {
                gx -= fr;
                if (gx < 0) gx = 0;
            } else if (gx < 0) {
                gx += fr;
                if (gx > 0) gx = 0;
            }
            if ((spd.x > 0 && -gx > spd.x) || (spd.x < 0 && -gx < spd.x)) gx = -spd.x;
        } else {
            float lim = p.lim_frict * w * gy;
            if (gx < lim && -lim < gx) gx = 0;
        }
        if (spd.z != 0) {
            float fr = gy < 0 ? p.grd_frict_z * w * gy : 0.0f;
            if (gz > 0) bz -= fr;
            else if (gz < 0) bz += fr;
            gz += bz;
            if ((spd.z > 0 && spd.z < -gz) || (spd.z < 0 && -gz < spd.z)) gz = -spd.z;
        } else {
            float lim = p.lim_frict * w * gy;
            if (gz < lim && -lim < gz) gz = 0;
        }
    }
    turnNormal(angAim);
    acc = Vec3(gx, gy, gz);
}

// ---- return upright in the air FUN_00443ad0 ------------------------------------------
void Player::pAirUpright() {
    float s2 = length2(spd);
    if (s2 <= P.dash_speed * P.dash_speed) {
        Vec3 w = toWorld(spd);
        ang[0] = adjustAngle(ang[0], 0, 0x800);
        ang[2] = adjustAngle(ang[2], 0, 0x800);
        spd = toLocal(w);
    }
}

// ---- integrate velocity FUN_00443f50 ---------------------------------------------------
void Player::pGetSpeed() {
    bool input = stick();
    float j = P.jog_speed * 0.87f;
    float old = spd.x, nw = spd.x + acc.x;
    spd.x = nw;
    if (old <= 0) {
        if (old < 0) {
            if (nw <= 0) {
                if (!input && -j < spd.x && upY > 0.7f) spd.x = 0;
            } else {
                spd.x = 0;
            }
        }
    } else if (nw >= 0) {
        if (!input && spd.x < j && upY > 0.7f) spd.x = 0;
    } else {
        spd.x = 0;
    }
    spd.y += acc.y;
    old = spd.z;
    nw = spd.z + acc.z;
    spd.z = nw;
    if (old <= 0) {
        if (old < 0) {
            if (-j < old && old == nw) spd.z = 0;  // small drift with no acceleration
            else if (old <= -j && -j < nw) spd.z = 0;
        }
    } else if (old >= j || old != nw) {
        if (j <= old && nw < j) spd.z = 0;
    } else {
        spd.z = 0;
    }
    wvel = toWorld(spd);
}

// ---- position / collision (FUN_0044cdf0 + level collision) ------------------------------
void Player::pSetPosition(const ICollision& col) {
    const float R = P.rad;
    bool wasGround = (flags & 3) != 0;
    Vec3 upAxis = toWorld(Vec3(0, 1, 0));
    Vec3 center = pos + upAxis * R;
    float sp = length(wvel);
    int steps = std::min(16, std::max(1, (int)std::ceil(sp / (R * 0.5f))));
    Vec3 step = wvel / float(steps);
    std::vector<Contact> contacts;
    bool touchedGround = false;
    Vec3 groundCand;
    for (int s = 0; s < steps; s++) {
        center += step;
        if (col.resolveSphere(center, R, contacts)) {
            for (auto& ct : contacts) {
                bool isGround = wasGround ? dot(ct.normal, upAxis) > 0.5f : dot(ct.normal, Vec3(0, 1, 0)) > 0.55f;
                if (isGround) {
                    touchedGround = true;
                    groundCand = ct.normal;
                    continue;
                }
                float vn = dot(wvel, ct.normal);
                if (vn < 0) {
                    // wall: the original stops dead on head-on hits above crash speed,
                    // otherwise slides along the wall (FUN_0043c580)
                    Vec3 head = normalize(wvel);
                    if (wasGround && spd.x >= P.crash_speed && dot(head, ct.normal) < -0.98f) wvel = Vec3();
                    else wvel -= ct.normal * vn;
                    step = wvel / float(steps);
                }
                if (ct.flags & SURF_HURT) hurt(center - ct.normal * 4.0f);
            }
        }
    }
    // floor probe
    Vec3 probeDir = wasGround ? -upAxis : Vec3(0, -1, 0);
    float snap = wasGround ? (P.pos_error + length(wvel) * 0.6f) : 0.6f;
    RayHit h;
    bool hit = false;
    if (wasGround || touchedGround || wvel.y <= 0.3f) {
        hit = col.raycast(center, probeDir, R + snap, h);
        if (hit) {
            float ref = dot(h.normal, wasGround ? upAxis : Vec3(0, 1, 0));
            bool away = !wasGround && dot(wvel, h.normal) > 0.3f;
            if (ref < 0.45f || away) hit = false;
        }
    }
    if (hit) {
        groundNormal = normalize(h.normal);
        groundFlags = h.flags;
        flags |= 3;
        alignToFloor(groundNormal);
        Vec3 nu = toWorld(Vec3(0, 1, 0));
        pos = h.pos;
        (void)nu;
        wvel = projectOnPlane(wvel, groundNormal);
        if (!wasGround) evLanded = true;
    } else if (touchedGround && !wasGround && wvel.y <= 0) {
        groundNormal = groundCand;
        flags |= 3;
        alignToFloor(groundNormal);
        pos = center - groundNormal * R;
        evLanded = true;
    } else {
        flags &= ~3;
        groundNormal = Vec3(0, 1, 0);
        pos = center - upAxis * R;
        if (wvel.y < -P.lim_v_spd) wvel.y = -P.lim_v_spd;
    }
}

// ---- back to local velocity FUN_0043ee70 --------------------------------------------------
void Player::pResetPosition() {
    Vec3 l = toLocal(wvel);
    if (!(flags & 3)) {
        if (l.x < 0.001f && -0.001f <= l.x) l.x = 0;
        if (l.z < 0.001f && -0.001f <= l.z) l.z = 0;
    } else {
        l.y = 0;
        float f = P.run_accel * 0.9f;
        if (l.x < 0.01f && -0.01f < l.x) l.x = 0;
        if (l.z < f && -f < l.z) l.z = 0;
    }
    spd = l;
    upY = (flags & 3) ? toWorld(Vec3(0, 1, 0)).y : 1.0f;
}

// ---- action checks --------------------------------------------------------------------------
bool Player::stickReversed() const {
    int aim;
    if (!stick(&aim)) return false;
    return angDiffAbs(ang[1], aim) > 0x6000;
}

bool Player::checkFall() {  // FUN_00494f70
    if (flags & 3) return false;
    if (flags & 0x100) mode = MD_Jump;
    else mode = MD_Fall;
    return true;
}

bool Player::checkJump() {  // FUN_00495e60
    if (!jumpPressed_) return false;
    mode = MD_Jump;
    spd.y = P.jmp_y_spd;  // along the character's own up axis
    spindashSpeed = 5.0f;
    flags = (flags & ~3) | 0x500;  // leaves the ground (the original clears bit 1; its collision clears bit 0)
    jumpTimer = 0;
    evJumped = true;
    return true;
}

bool Player::checkSpinDash() {  // FUN_00496ee0
    if (!bPressed_) return false;
    mode = MD_SpinCharge;
    flags |= 0x500;
    spindashSpeed = spd.x <= 2.0f ? 2.0f : spd.x;
    ballTimer = 0;
    evSpinCharge = true;
    return true;
}

bool Player::checkStop() {  // FUN_00494ff0
    if (stick() || spd.x != 0) return false;
    mode = MD_Stand;
    return true;
}

void Player::landed() {
    // modes 8/12 on touching the ground
    if (stickReversed() && spd.x > 0) {
        mode = MD_Skid;
        flags &= ~0x500;
        evSkid = true;
        return;
    }
    flags &= ~0x500;
    if (!checkStop()) mode = MD_Run;
}

// ---- Sonic_RunsActions (FUN_00496f50), the parts relevant to the ported modes --------------
void Player::runActions() {
    bool ground = (flags & 3) != 0;
    switch (mode) {
        case MD_Stand:
            if (checkFall() || checkJump() || checkSpinDash()) return;
            if (stick()) mode = MD_Run;
            break;
        case MD_Run:
            if (checkFall() || checkJump() || checkSpinDash() || checkStop()) return;
            if (stickReversed() && P.jog_speed <= spd.x) {
                mode = MD_Skid;
                evSkid = true;
            }
            break;
        case MD_SpinCharge:  // FUN_00495080
            if (checkFall()) return;
            if (bHeld_) {
                if (spindashSpeed < 10.0f) spindashSpeed += 0.4f;
            } else {
                mode = MD_Roll;
                spd.x = spindashSpeed;
                spindashSpeed = 0;
                evSpinDash = true;
            }
            break;
        case MD_Roll:
            if (checkFall() || checkJump()) return;
            if (spd.x < P.run_speed) {  // FUN_004930d0
                flags &= ~0x500;
                mode = spd.x <= 0 ? MD_Stand : MD_Run;
                return;
            }
            if (bPressed_) {
                flags &= ~0x500;
                mode = MD_Run;
            }
            break;
        case MD_Jump:
            if (ground) {
                landed();
                return;
            }
            if (bPressed_) {  // FUN_00492f50: uncurl
                mode = MD_Fall;
                flags &= ~0x500;
                spindashSpeed = 0;
                return;
            }
            if (jumpPressed_) {  // FUN_0043bf40 -> mode 0x0E (homing attack / jump dash)
                mode = MD_Homing;
                homingStart();
                return;
            }
            if (jumpHeld_ && jumpTimer++ < P.jump2_timer) spd.y += P.jmp_addit * 0.8f;  // FUN_0043bf90
            break;
        case MD_Homing:  // Sonic_RunsActions case 0x0E
            if (++homingFrames_ > 0x167) {
                homingTimer_ = -1;
                mode = MD_Jump;
                return;
            }
            if (homingTimer_ >= 6 || bPressed_) {  // dash ran out, or B: uncurl and fall
                homingTimer_ = 0;
                mode = MD_Fall;
                flags &= ~0x500;
                homingTarget = -1;
                return;
            }
            if (ground) {
                homingTarget = -1;
                landed();
            }
            break;
        case MD_Fall:
        case MD_Spring:
            if (ground) landed();
            else if (mode == MD_Spring && wvel.y < 0) mode = MD_Fall;
            break;
        case MD_Skid:
            if (checkFall() || checkJump()) return;
            if (spd.x <= 0) {
                int aim;
                if (stickReversed() && stick(&aim)) {
                    ang[1] = aim;  // snap round, as the original does
                    angAim = aim;
                    mode = MD_Run;
                } else {
                    mode = stick() ? MD_Run : MD_Stand;
                }
            }
            break;
        case MD_Hurt:
            if (ground && hurtTimer_ <= 0) mode = MD_Stand;
            break;
    }
}

// ---- homing attack ---------------------------------------------------------------------------
// FUN_00494b80: entering mode 0x0E
void Player::homingStart() {
    homingTimer_ = -1;
    homingFrames_ = 0;
    evHoming = true;
    if (targets.empty() && homingTarget < 0) {
        spd.x = 5.0f;  // no target: jump dash straight ahead
        homingTimer_ = 1;
        return;
    }
    homingStep();
    if (homingTimer_ != 0) spd.x = 5.0f;
}

// FUN_00492300: per-frame homing
void Player::homingStep() {
    auto dashOn = [&]() {
        spd.x *= 0.9f;
        homingTimer_++;
        pGetAcceleration();
        pGetSpeed();
    };
    if (homingTimer_ > 0 || targets.empty()) {
        dashOn();
        return;
    }
    Vec3 from = center();
    auto yawTo = [&](const Vec3& t) { return wrapAng(radToAng(std::atan2(t.z - from.z, t.x - from.x))); };
    // keep the current lock while it stays in front and in range
    const HomingTarget* best = nullptr;
    float bestDist = 10000.0f;
    for (auto& t : targets)
        if (t.id == homingTarget && angDiffAbs(ang[1], yawTo(t.pos)) <= 0x5000 && t.dist < 10000.0f) {
            best = &t;
            bestDist = t.dist;
        }
    for (auto& t : targets)
        if (angDiffAbs(ang[1], yawTo(t.pos)) <= 0x5000 && t.dist < bestDist) {
            best = &t;
            bestDist = t.dist;
        }
    if (!best) {
        homingTimer_ = 1;
        homingTarget = -1;
        dashOn();
        return;
    }
    homingTarget = best->id;
    Vec3 d = best->pos - from;
    ang[1] = adjustAngle(ang[1], yawTo(best->pos), 0x800);
    float dy = d.y;
    if (dy > 0 && homingTimer_ < 0) {  // the original won't start homing upward
        dy = 0;
        homingTimer_ = 1;
    }
    float len = std::sqrt(d.x * d.x + dy * dy + d.z * d.z);
    float ny = len != 0 ? dy / len : 0.0f;
    float h = std::sqrt(std::max(0.0f, 1.0f - ny * ny));
    float speedH = 5.0f;
    if (homingFrames_ > 0xB4) speedH *= 0.7f + 0.1f * float(rand() & 0x7FFF) / 32768.0f;
    float a = bamsToRad(ang[1]);
    wvel = Vec3(std::cos(a) * h * speedH, ny * speedH, std::sin(a) * h * speedH);
    spd = toLocal(wvel);
    if (homingTimer_ < 0) homingTimer_ = 0;
}

void Player::bounce(float upSpeed) {
    mode = MD_Jump;
    flags = (flags & ~3) | 0x500;
    ang[0] = ang[2] = 0;
    wvel = Vec3(0, upSpeed, 0);
    spd = toLocal(wvel);
    jumpTimer = P.jump2_timer;  // no extra hold height after a bounce
    homingTimer_ = 0;
    homingTarget = -1;
    evBounce = true;
    syncView();
}

// ---- public -------------------------------------------------------------------------------
void Player::reset(const StartPos& sp) {
    start_ = sp;
    pos = sp.pos;
    ang[0] = ang[2] = 0;
    ang[1] = wrapAng(sp.yaw);
    angAim = ang[1];
    flags = 0;
    mode = MD_Fall;
    wvel = acc = spd = Vec3();
    upY = 1;
    spindashSpeed = 0;
    jumpTimer = 0;
    nocontimer = 0;
    rings = 0;
    hurtTimer_ = 0;
    syncView();
}

void Player::launch(const Vec3& v, int lockFrames, PlayerState st) {
    wvel = v;
    if (st == PlayerState::Spring) {
        flags &= ~0x503;
        ang[0] = ang[2] = 0;
        Vec3 h(v.x, 0, v.z);
        if (length(h) > 0.1f) ang[1] = angAim = wrapAng(radToAng(std::atan2(h.z, h.x)));
        mode = MD_Spring;
        pos += normalize(v) * 1.0f;
    } else if (st == PlayerState::Roll) {
        mode = MD_Roll;
    } else {
        Vec3 h = projectOnPlane(v, groundNormal);
        if (length(h) > 0.1f) {
            Vec3 l = toLocal(h);
            ang[1] = angAim = wrapAng(ang[1] + radToAng(std::atan2(l.z, l.x)));
        }
        if (mode != MD_Roll) mode = MD_Run;
    }
    spd = toLocal(wvel);
    nocontimer = lockFrames;
    syncView();
}

void Player::hurt(const Vec3& from) {
    if (mode == MD_Hurt) return;
    Vec3 away = normalize(Vec3(pos.x - from.x, 0, pos.z - from.z), -facing);
    wvel = away * 1.5f + Vec3(0, 2.0f, 0);
    flags &= ~0x503;
    ang[0] = ang[2] = 0;
    spd = toLocal(wvel);
    mode = MD_Hurt;
    hurtTimer_ = 40;
    nocontimer = 40;
    rings = 0;
}

void Player::setPuppet(const Vec3& feet, const Vec3& worldVel, const Vec3& forward, const Vec3& upDir, bool onGround,
                       bool curled) {
    pos = feet;
    wvel = worldVel;
    Vec3 f = normalize(Vec3(forward.x, 0, forward.z), facing);
    ang[0] = ang[2] = 0;
    ang[1] = angAim = wrapAng(radToAng(std::atan2(f.z, f.x)));
    (void)upDir;
    flags = u16(flags & ~0x503);
    if (onGround) flags |= 1;
    if (curled) flags |= 0x100 | 0x400;
    if (curled) mode = MD_Jump;
    else if (!onGround) mode = MD_Fall;
    else mode = length(Vec3(worldVel.x, 0, worldVel.z)) > 0.05f ? MD_Run : MD_Stand;
    spd = toLocal(wvel);
    groundNormal = Vec3(0, 1, 0);
    upY = 1;
    nocontimer = 0;
    hurtTimer_ = 0;
    evJumped = evLanded = evSpinDash = evRespawn = evSkid = evSpinCharge = false;
    evHoming = evBounce = false;
    braking = false;
    if (flags & 0x100) ballTimer = (ballTimer + 1) & 0xFFFF;
    syncView();
}

void Player::syncView() {
    Mat4 m = localToWorld();
    up = normalize(m.transformDir(Vec3(0, 1, 0)));
    facing = normalize(m.transformDir(Vec3(1, 0, 0)));
    grounded = (flags & 3) != 0;
    vel = wvel;
    spinCharge = spindashSpeed;
    controlLock = nocontimer;
    switch (mode) {
        case MD_Jump: state = PlayerState::Jump; break;
        case MD_Roll: state = PlayerState::Roll; break;
        case MD_SpinCharge: state = PlayerState::SpinCharge; break;
        case MD_Spring: state = PlayerState::Spring; break;
        case MD_Hurt: state = PlayerState::Hurt; break;
        case MD_Homing: state = PlayerState::Homing; break;
        default: state = PlayerState::Normal; break;
    }
    if (mode == MD_Skid) braking = true;
}

void Player::update(const InputState& in, const Vec3& camForward, const ICollision& col, float killY) {
    evJumped = evLanded = evSpinDash = evRespawn = evSkid = evSpinCharge = false;
    evHoming = evBounce = false;
    braking = false;
    if (flags & 0x100) ballTimer = (ballTimer + 1) & 0xFFFF;
    if (nocontimer > 0) nocontimer--;
    if (hurtTimer_ > 0) hurtTimer_--;

    // controller -> world angle + magnitude (the game's per-player input record)
    Vec3 cf = normalize(Vec3(camForward.x, 0, camForward.z), facing);
    Vec3 cr = normalize(cross(cf, Vec3(0, 1, 0)), Vec3(1, 0, 0));
    Vec3 wish = cr * in.moveX + cf * in.moveY;
    stickMag_ = clampf(std::sqrt(in.moveX * in.moveX + in.moveY * in.moveY), 0, 1);
    hasInput_ = stickMag_ > 0.0f;
    if (hasInput_) stickAng_ = wrapAng(radToAng(std::atan2(wish.z, wish.x)));
    jumpPressed_ = in.jump;
    jumpHeld_ = in.jumpHeld;
    bPressed_ = in.action;
    bHeld_ = in.actionHeld;

    runActions();

    // Sonic_Main physics per mode (FUN_0049a9b0)
    switch (mode) {
        case MD_Stand:
            pGetRotation();
            pGetAcceleration();
            pGetSpeed();
            break;
        case MD_Run:
            pGetAcceleration();
            pGetSpeed();
            break;
        case MD_SpinCharge:
            pGetRotation();
            pGetAccelerationBrake();
            pGetSpeed();
            break;
        case MD_Roll:
            // Sonic_Main mode 5 (spin dash / roll): steer with the stick, then roll physics
            pGetRotation();
            pGetAccelerationRoll();
            pGetSpeed();
            break;
        case MD_Skid:
            pGetAccelerationBrake();
            pGetSpeed();
            break;
        case MD_Homing:
            pAirUpright();
            homingStep();
            break;
        case MD_Jump:
        case MD_Fall:
        case MD_Spring:
        case MD_Hurt:
            pAirUpright();
            pGetAccelerationAir();
            pGetSpeed();
            break;
    }
    pSetPosition(col);
    pResetPosition();
    syncView();

    if (pos.y < killY) {
        deaths++;
        reset(start_);
        evRespawn = true;
    }
}

}  // namespace sonic
