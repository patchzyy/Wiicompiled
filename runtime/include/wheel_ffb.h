#pragma once

#include <cstdint>

namespace wheel_ffb {

bool IsWheelInstance(uint32_t instance);
bool HasBuiltinLayout(uint32_t instance);
uint32_t PedalButtons(uint32_t port);
void NotifyControllersChanged();
void Tick();
bool OnMotorCommand(int32_t chan, uint32_t command);
bool IsWheelPort(uint32_t port);
const char* StatusText();
float SteeringPosition();
void ApplyStrength(int percent);
void ApplySpring(int percent);
void ApplyVibration(int percent);
void ApplySteeringSensitivity(int percent);
int32_t ShapeSteering(uint32_t port, int32_t stickX);

} // namespace wheel_ffb
