// sonic_place.cpp - see sonic_place.h.
#include "sonic/sonic_place.h"

#include <algorithm>
#include <cmath>

namespace sonic_mkw {

namespace place_detail {

using sonic::Mat4;
using sonic::Vec3;

// Every driver model shares the game's convention for which way a model faces;
// it can only be measured while seated (feet forward), so it is remembered for the
// standing poses (menus, award ceremony). Mario Kart Wii models face +Z.
Vec3 g_learnedForward(0, 0, 1);

Vec3 SnapAxis(const Vec3& v) {
    if (std::fabs(v.x) > std::fabs(v.z)) return Vec3(v.x > 0 ? 1.0f : -1.0f, 0, 0);
    return Vec3(0, 0, v.z >= 0 ? 1.0f : -1.0f);
}

}  // namespace place_detail

DriverPlacement PlaceSonicOnDriver(const DriverFrame& frame, DriverState& state, float sizeRatio,
                                   PoseProvider poses, float sonicStandingHeight) {
    using namespace place_detail;
    DriverPlacement out;
    const Mat4 rootInv = frame.root.affineInverse();

    std::vector<Vec3> joints;
    joints.reserve(frame.jointsView.size() + 1);
    for (const Vec3& j : frame.jointsView) joints.push_back(rootInv.transformPoint(j));
    joints.push_back(Vec3(0, 0, 0));  // the root bone itself
    Vec3 jmin(1e30f, 1e30f, 1e30f), jmax(-1e30f, -1e30f, -1e30f), all(0, 0, 0);
    for (const Vec3& j : joints) {
        jmin = sonic::vmin(jmin, j);
        jmax = sonic::vmax(jmax, j);
        all += j;
    }
    all = all / float(joints.size());
    const float span = jmax.y - jmin.y;
    if (!(span > 1e-4f) || !std::isfinite(span)) return out;

    // Seated or standing.
    Vec3 low(0, 0, 0), top(0, 0, 0);
    int lowCount = 0, topCount = 0;
    for (const Vec3& j : joints) {
        if (j.y <= jmin.y + span * 0.2f) {
            low += j;
            ++lowCount;
        }
        if (j.y >= jmax.y - span * 0.2f) {
            top += j;
            ++topCount;
        }
    }
    low = low / float(std::max(lowCount, 1));
    top = top / float(std::max(topCount, 1));
    const Vec3 reach(low.x - top.x, 0, low.z - top.z);
    const float ratio = sonic::length(reach) / span;
    if (!state.known) {
        state.seated = ratio > 0.22f;
        state.known = true;
    } else if (state.seated ? ratio < 0.14f : ratio > 0.26f) {
        state.seated = !state.seated;
    }
    out.pose = state.seated ? SonicPose::Driving : SonicPose::Standing;
    if (state.seated) g_learnedForward = SnapAxis(reach);
    const Vec3 forward = g_learnedForward;

    const PosedSonic* sonicPose = poses ? poses(out.pose) : nullptr;
    if (!sonicPose) return out;

    // Scale: prefer the driver's bind-pose height against Sonic standing; fall
    // back to the joint spans of the current poses when the two disagree a lot.
    const float sonicJointSpan = std::max(sonicPose->jointMax.y - sonicPose->jointMin.y, 1e-3f);
    const float jointScale = span / sonicJointSpan;
    float scale = jointScale;
    if (frame.bindHeight > 0 && sonicStandingHeight > 0) {
        const float boxScale = frame.bindHeight / sonicStandingHeight;
        // Skeletons differ in where their top joint sits (neck, head, hat), so the
        // joint estimate is only a sanity check for the bind-pose box.
        if (boxScale > jointScale * 0.4f && boxScale < jointScale * 2.5f) scale = boxScale;
    }
    scale *= sizeRatio;

    // Lean sideways with the driver's upper body while driving.
    float roll = 0;
    if (state.seated) {
        const Vec3 right = sonic::cross(forward, Vec3(0, 1, 0));
        const Vec3 d = top - all;
        roll = sonic::clampf(std::atan2(sonic::dot(d, right), std::max(d.y, 1e-3f)), -0.35f, 0.35f);
    }

    const Vec3 sonicAnchor((sonicPose->jointMin.x + sonicPose->jointMax.x) * 0.5f, sonicPose->jointMin.y,
                           (sonicPose->jointMin.z + sonicPose->jointMax.z) * 0.5f);
    const Vec3 driverAnchor((jmin.x + jmax.x) * 0.5f, jmin.y, (jmin.z + jmax.z) * 0.5f);
    const float pivot = sonicPose->jointMin.y + (sonicPose->jointMax.y - sonicPose->jointMin.y) * 0.3f;
    // rotZ(+a) tips +Y towards -X, which is Sonic's right in canonical space.
    const Mat4 lean = Mat4::translate(Vec3(0, pivot, 0)) * Mat4::rotZ(roll) * Mat4::translate(Vec3(0, -pivot, 0));
    // rotY(a) takes +Z to (sin a, 0, cos a).
    const Mat4 face = Mat4::rotY(std::atan2(forward.x, forward.z));
    out.sonicToView = frame.root * Mat4::translate(driverAnchor) * face * Mat4::scale(Vec3(scale, scale, scale)) *
                      Mat4::translate(-sonicAnchor) * lean;
    out.ok = true;
    return out;
}

}  // namespace sonic_mkw
