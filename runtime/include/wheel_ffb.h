#pragma once

#include <cstdint>

namespace wheel_ffb {

bool IsWheelDevice(uint16_t vendor, uint16_t product);
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
