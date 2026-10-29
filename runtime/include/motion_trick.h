#pragma once

// Shake to trick: a flick of a controller's motion sensor briefly presses the
// matching D-pad direction, which the game treats as a trick (or a wheelie on
// bikes), like shaking a Wii Remote.

#include <cstdint>

#include <dolphin/pad.h>

#include "motion_trick_detector.h"

namespace MotionTrick {

bool Enabled() noexcept;
void SetEnabled(bool enabled) noexcept;
int Sensitivity() noexcept;
void SetSensitivity(int level) noexcept;

// The controller on this port reports a gyro or accelerometer of its own.
bool PortHasMotion(uint32_t port) noexcept;

// Call once per guest PADRead, after every other input source has been merged.
void Apply(PADStatus* statuses) noexcept;

struct PortStatus {
    float intensity = 0.0f; // decaying peak, 1 being the trigger threshold
    Direction lastTrick = Direction::None;
    uint64_t lastTrickUs = 0;
};
PortStatus Status(uint32_t port) noexcept;

uint64_t NowUs() noexcept;

} // namespace MotionTrick
