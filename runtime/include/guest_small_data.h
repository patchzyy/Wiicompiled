#pragma once

#include <cstdint>

// PowerPC EABI small-data base pointers of the translated game. Defined per game
// in runtime/game/runtime_config_values.cpp so the runtime itself stays game-independent.
namespace GuestSmallData {
extern const uint32_t kSda1Base; // _SDA_BASE_, loaded into r13
extern const uint32_t kSda2Base; // _SDA2_BASE_, loaded into r2
} // namespace GuestSmallData
