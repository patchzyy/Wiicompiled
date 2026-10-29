#pragma once

// Shake detection for controllers with motion sensors. Pure host code, so it is
// unit-testable without SDL.

#include <cstdint>

namespace MotionTrick {

enum class Direction : uint8_t { None, Up, Down, Left, Right };

inline constexpr int kMinSensitivity = 1;
inline constexpr int kMaxSensitivity = 5;
inline constexpr int kDefaultSensitivity = 3;

// SDL gamepad sensor axes: x right, y top, z toward the player.
struct MotionSample {
    bool hasGyro = false;
    bool hasAccel = false;
    float gyro[3]{};  // rad/s: pitch, yaw, roll
    float accel[3]{}; // m/s^2, gravity included
};

// Fed one sample per game frame; reports at most one trick per flick.
class ShakeDetector {
public:
    ShakeDetector() { SetSensitivity(kDefaultSensitivity); }

    void SetSensitivity(int level);
    Direction Update(const MotionSample& sample, uint64_t nowUs);
    void Reset();

    // Strength of the last sample, 1 being the trigger threshold.
    float Intensity() const { return m_intensity; }

private:
    float m_gyroThreshold = 0.0f;
    float m_accelThreshold = 0.0f;
    float m_intensity = 0.0f;
    bool m_armed = true;
    uint64_t m_lastFireUs = 0;
};

} // namespace MotionTrick
