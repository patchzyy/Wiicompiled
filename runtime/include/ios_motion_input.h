#pragma once

#include <array>

// iOS device-motion source. The bridge owns Core Motion and exposes its two
// useful, bias-separated components in units of g. Callers decide how those
// axes map to their virtual device.
namespace IosMotionInput {

enum class LandscapeOrientation { Unknown, Left, Right };

struct Sample {
    std::array<float, 3> gravity{};
    std::array<float, 3> userAcceleration{};
    double timestamp = 0.0;
};

// Starts 60 Hz device-motion updates when the device exposes the service.
// Calling it repeatedly is harmless.
void Start();

// Stops updates and drops the last sample. Calling it repeatedly is harmless.
void Stop();

// Copies the newest complete sample. False means motion is unavailable, has
// not produced a reading yet, or was stopped.
bool Read(Sample& sample);

// Whether Core Motion reports device-motion support on this device.
bool IsAvailable();

// Reads the active scene's interface orientation on the game/UI thread.
LandscapeOrientation CurrentLandscapeOrientation();

} // namespace IosMotionInput
