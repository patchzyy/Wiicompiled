#include "motion_trick_detector.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace MotionTrick {
namespace {

constexpr float kGravity = 9.80665f;
constexpr float kRearmIntensity = 0.35f;
constexpr uint64_t kCooldownUs = 400'000;

// Indexed by sensitivity level - 1.
constexpr std::array<float, kMaxSensitivity> kGyroThresholds = {11.0f, 9.0f, 7.0f, 5.5f, 4.0f};
constexpr std::array<float, kMaxSensitivity> kAccelThresholds = {20.0f, 16.0f, 13.0f, 10.0f, 8.0f};

float Length(const float (&v)[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

Direction GyroDirection(const float (&gyro)[3]) {
    const float pitch = gyro[0];
    const float yaw = gyro[1];
    const float roll = gyro[2];
    if (std::fabs(pitch) >= std::max(std::fabs(yaw), std::fabs(roll))) {
        return pitch >= 0.0f ? Direction::Up : Direction::Down;
    }
    const float lateral = std::fabs(roll) >= std::fabs(yaw) ? roll : yaw;
    return lateral >= 0.0f ? Direction::Left : Direction::Right;
}

} // namespace

void ShakeDetector::SetSensitivity(int level) {
    const size_t index = static_cast<size_t>(std::clamp(level, kMinSensitivity, kMaxSensitivity) - 1);
    m_gyroThreshold = kGyroThresholds[index];
    m_accelThreshold = kAccelThresholds[index];
}

Direction ShakeDetector::Update(const MotionSample& sample, uint64_t nowUs) {
    // The accelerometer only stands in for a missing gyro: on a gyro pad it
    // mostly picks up bumps, and it carries no direction.
    if (sample.hasGyro) {
        m_intensity = Length(sample.gyro) / m_gyroThreshold;
    } else if (sample.hasAccel) {
        m_intensity = std::fabs(Length(sample.accel) - kGravity) / m_accelThreshold;
    } else {
        m_intensity = 0.0f;
    }
    if (!std::isfinite(m_intensity)) {
        m_intensity = 0.0f;
        return Direction::None;
    }

    if (!m_armed) {
        if (m_intensity < kRearmIntensity && nowUs - m_lastFireUs >= kCooldownUs) {
            m_armed = true;
        }
        return Direction::None;
    }
    if (m_intensity < 1.0f) {
        return Direction::None;
    }
    m_armed = false;
    m_lastFireUs = nowUs;
    return sample.hasGyro ? GyroDirection(sample.gyro) : Direction::Up;
}

} // namespace MotionTrick
