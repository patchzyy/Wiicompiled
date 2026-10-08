// sonic_place.h - fitting Sonic onto the skeleton of the driver he replaces.
//
// Pure host math (no guest memory, no GX) so it can be tested on its own.
#pragma once

#include <vector>

#include "sonic/sonic_render.h"

namespace sonic_mkw {

// What the draw hook knows about one draw of the replaced driver.
struct DriverFrame {
    sonic::Mat4 root;                     // the driver's root bone: model -> view
    std::vector<sonic::Vec3> jointsView;  // origins of the other bones, view space
    float bindHeight = 0;                 // height of the model's bind-pose box (0 = unknown)
};

// Per model instance, kept between frames (seated/standing hysteresis).
struct DriverState {
    bool known = false;
    bool seated = false;
};

struct DriverPlacement {
    sonic::Mat4 sonicToView;  // Sonic's canonical space -> view space
    SonicPose pose = SonicPose::Driving;
    bool ok = false;
};

using PoseProvider = const PosedSonic* (*)(SonicPose pose);

// Reads the driver's skeleton (root frame, Y up): seated when the lowest joints
// (feet) are well in front of the highest (head), and then facing the way the
// feet point; otherwise standing, facing the way seated drivers last faced (+Z
// until one was seen). Sonic is scaled to the driver's
// size, anchored at the bottom centre of the driver's joints and leans sideways
// with the driver's upper body.
DriverPlacement PlaceSonicOnDriver(const DriverFrame& frame, DriverState& state, float sizeRatio,
                                   PoseProvider poses, float sonicStandingHeight);

}  // namespace sonic_mkw
