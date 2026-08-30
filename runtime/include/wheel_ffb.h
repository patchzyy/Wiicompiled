#pragma once

#include <cstdint>

namespace wheel_ffb {

void NotifyControllersChanged();
void Tick();
bool OnMotorCommand(int32_t chan, uint32_t command);
bool IsWheelPort(uint32_t port);
const char* StatusText();
float SteeringPosition();
void ApplyStrength(int percent);
void ApplySpring(int percent);
void ApplyVibration(int percent);

} // namespace wheel_ffb
