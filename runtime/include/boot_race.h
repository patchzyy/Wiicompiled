#pragma once

#include <string_view>

// `--race[=<course id>]`: boot straight into a 1P VS race instead of the title screen.
// `--race-cpu`: the same, with the local racer driven by the CPU.
namespace BootRace {
// False when `arg` is not a boot-race option; throws on a malformed one.
bool ParseArgument(std::string_view arg);
} // namespace BootRace
