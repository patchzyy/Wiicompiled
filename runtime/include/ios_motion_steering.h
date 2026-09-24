#pragma once

#include "ios_motion_input.h"

// Pure steering math for a phone held as a Wii Wheel. Kept independent of
// Core Motion so it can be exercised on a development host.
namespace IosMotionSteering {

// The gravity-vector angle in the device plane, or false when the device is
// too close to perpendicular to that plane for a stable steering reading.
bool AngleFromGravity(float x, float y, double& angle);

// Rotates portrait device axes into the active landscape interface axes.
void RotateForLandscape(IosMotionInput::LandscapeOrientation orientation, float deviceX, float deviceY,
                        float& interfaceX, float& interfaceY);

// Maps an angle relative to a user-selected centre to the Wii Wheel's -1..1
// steering range. Sensitivity is constrained to the supported 0.5x..2x range.
float SteeringValue(double angle, double centreAngle, float sensitivity, bool inverted);

enum class ShakeAction { None, Rearm, Trigger };

// Classifies a gravity-free acceleration sample. A trigger requires a prior
// settled sample and is rate-limited so one physical shake yields one impulse.
ShakeAction ShakeActionForSample(double accelerationMagnitude, double timestamp, bool armed,
                                 double lastTriggerTimestamp);

} // namespace IosMotionSteering
