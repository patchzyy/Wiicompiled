#include "ios_motion_steering.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

namespace {

bool Near(float actual, float expected) {
    return std::abs(actual - expected) < 0.0001f;
}

bool Check(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

} // namespace

int main() {
    double centre = 0.0;
    bool pass = true;
    pass &= Check(IosMotionSteering::AngleFromGravity(1.0f, 0.0f, centre),
                  "level gravity did not produce an angle");
    pass &= Check(Near(IosMotionSteering::SteeringValue(centre, centre, 1.0f, false), 0.0f),
                  "centre did not produce neutral steering");
    pass &= Check(Near(IosMotionSteering::SteeringValue(0.03, centre, 1.0f, false), 0.0f),
                  "dead zone did not suppress small movement");
    pass &= Check(IosMotionSteering::SteeringValue(0.35, centre, 1.0f, false) > 0.4f,
                  "right turn did not steer right");
    pass &= Check(IosMotionSteering::SteeringValue(-0.35, centre, 1.0f, false) < -0.4f,
                  "left turn did not steer left");
    pass &= Check(Near(IosMotionSteering::SteeringValue(2.0, centre, 1.0f, false), 1.0f),
                  "full steering did not clamp");
    pass &= Check(Near(IosMotionSteering::SteeringValue(0.35, centre, 1.0f, true),
                       -IosMotionSteering::SteeringValue(0.35, centre, 1.0f, false)),
                  "inversion changed steering magnitude");
    pass &= Check(IosMotionSteering::SteeringValue(-3.05, 3.05, 1.0f, false) > 0.0f,
                  "angle wrap selected the wrong steering direction");
    pass &= Check(!IosMotionSteering::AngleFromGravity(0.0f, 0.0f, centre),
                  "invalid gravity projection produced an angle");
    float interfaceX = 0.0f, interfaceY = 0.0f;
    IosMotionSteering::RotateForLandscape(IosMotionInput::LandscapeOrientation::Left, 1.0f, 2.0f,
                                           interfaceX, interfaceY);
    pass &= Check(Near(interfaceX, -2.0f) && Near(interfaceY, 1.0f),
                  "landscape-left axis transform is wrong");
    IosMotionSteering::RotateForLandscape(IosMotionInput::LandscapeOrientation::Right, 1.0f, 2.0f,
                                           interfaceX, interfaceY);
    pass &= Check(Near(interfaceX, 2.0f) && Near(interfaceY, -1.0f),
                  "landscape-right axis transform is wrong");
    constexpr double neverTriggered = -1.0;
    pass &= Check(IosMotionSteering::ShakeActionForSample(0.1, 1.0, false, neverTriggered) ==
                      IosMotionSteering::ShakeAction::Rearm,
                  "settled motion did not rearm shake detection");
    pass &= Check(IosMotionSteering::ShakeActionForSample(0.29, 1.0, true, neverTriggered) ==
                      IosMotionSteering::ShakeAction::None,
                  "ordinary movement triggered a shake");
    pass &= Check(IosMotionSteering::ShakeActionForSample(1.5, 1.0, true, neverTriggered) ==
                      IosMotionSteering::ShakeAction::Trigger,
                  "deliberate shake did not trigger");
    pass &= Check(IosMotionSteering::ShakeActionForSample(1.5, 1.05, false, 1.0) ==
                      IosMotionSteering::ShakeAction::None,
                  "held movement retriggered a shake");
    pass &= Check(IosMotionSteering::ShakeActionForSample(1.5, 1.05, true, 1.0) ==
                      IosMotionSteering::ShakeAction::None,
                  "cooldown did not suppress a shake");
    pass &= Check(IosMotionSteering::ShakeActionForSample(1.5, 1.5, true, 1.0) ==
                      IosMotionSteering::ShakeAction::Trigger,
                  "shake did not trigger after cooldown");
    return pass ? EXIT_SUCCESS : EXIT_FAILURE;
}
