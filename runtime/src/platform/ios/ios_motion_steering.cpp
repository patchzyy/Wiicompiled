#include "ios_motion_steering.h"

#include <algorithm>
#include <cmath>

namespace IosMotionSteering {
namespace {

constexpr double kMinimumGravityProjection = 0.08;
constexpr double kDeadZoneRadians = 0.045;
constexpr double kFullLockRadians = 0.70;

double WrapAngle(double angle) {
    while (angle > M_PI) angle -= 2.0 * M_PI;
    while (angle < -M_PI) angle += 2.0 * M_PI;
    return angle;
}

} // namespace

bool AngleFromGravity(float x, float y, double& angle) {
    if (!std::isfinite(x) || !std::isfinite(y) || std::hypot(x, y) < kMinimumGravityProjection) {
        return false;
    }
    angle = std::atan2(static_cast<double>(y), static_cast<double>(x));
    return true;
}

void RotateForLandscape(IosMotionInput::LandscapeOrientation orientation, float deviceX, float deviceY,
                        float& interfaceX, float& interfaceY) {
    switch (orientation) {
    case IosMotionInput::LandscapeOrientation::Left:
        interfaceX = -deviceY;
        interfaceY = deviceX;
        return;
    case IosMotionInput::LandscapeOrientation::Right:
        interfaceX = deviceY;
        interfaceY = -deviceX;
        return;
    default:
        interfaceX = deviceX;
        interfaceY = deviceY;
        return;
    }
}

float SteeringValue(double angle, double centreAngle, float sensitivity, bool inverted) {
    if (!std::isfinite(angle) || !std::isfinite(centreAngle)) {
        return 0.0f;
    }
    const double delta = WrapAngle(angle - centreAngle);
    const double magnitude = std::abs(delta);
    if (magnitude <= kDeadZoneRadians) {
        return 0.0f;
    }
    const float boundedSensitivity = std::clamp(sensitivity, 0.5f, 2.0f);
    const double fullLock = kFullLockRadians / boundedSensitivity;
    double value = std::copysign(
        std::min(1.0, (magnitude - kDeadZoneRadians) / (fullLock - kDeadZoneRadians)), delta);
    if (inverted) {
        value = -value;
    }
    return static_cast<float>(value);
}

ShakeAction ShakeActionForSample(double accelerationMagnitude, double timestamp, bool armed,
                                 double lastTriggerTimestamp) {
    if (!std::isfinite(accelerationMagnitude) || accelerationMagnitude < 0.0 || !std::isfinite(timestamp)) {
        return ShakeAction::None;
    }
    constexpr double kRearmAccelerationG = 0.10;
    // deviceMotion.userAcceleration excludes gravity. Typical deliberate
    // wheel lifts and wrist shakes peak well below a full g, unlike the raw
    // accelerometer value a Wii Remote reports, so use a phone-appropriate
    // threshold and let Mario Kart decide whether a motion is relevant.
    constexpr double kTriggerAccelerationG = 0.30;
    // This only filters one physical jolt's repeated sensor samples. Mario
    // Kart owns the gameplay timing (one trick per jump, POW timing, etc.), so
    // keep it short enough for closely spaced ramps and repeated POW shakes.
    constexpr double kCooldownSeconds = 0.10;
    if (accelerationMagnitude <= kRearmAccelerationG) {
        return ShakeAction::Rearm;
    }
    if (!armed || accelerationMagnitude < kTriggerAccelerationG) {
        return ShakeAction::None;
    }
    if (lastTriggerTimestamp >= 0.0 && timestamp - lastTriggerTimestamp < kCooldownSeconds) {
        return ShakeAction::None;
    }
    return ShakeAction::Trigger;
}

} // namespace IosMotionSteering
