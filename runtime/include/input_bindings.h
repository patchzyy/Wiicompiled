#pragma once

// Per-port expression bindings for the GameCube controls, plus import of a
// Dolphin GCPadNew.ini.

#include <array>
#include <cstdint>
#include <string>

#include <dolphin/pad.h>

namespace InputBindings {

// The controls an expression can drive, in Dolphin's own naming so an
// imported config maps across without translation.
struct ControlInfo {
    const char* dolphinName;
    const char* label;
    uint16_t padButton;   // 0 for the analog-only controls below
    int analog;           // 0 none, 1 trigger L, 2 trigger R
};

inline constexpr std::array<ControlInfo, 14> kControls = {{
    {"Buttons/A", "A", PAD_BUTTON_A, 0},
    {"Buttons/B", "B", PAD_BUTTON_B, 0},
    {"Buttons/X", "X", PAD_BUTTON_X, 0},
    {"Buttons/Y", "Y", PAD_BUTTON_Y, 0},
    {"Buttons/Z", "Z", PAD_TRIGGER_Z, 0},
    {"Buttons/Start", "Start", PAD_BUTTON_START, 0},
    {"D-Pad/Up", "D-pad Up", PAD_BUTTON_UP, 0},
    {"D-Pad/Down", "D-pad Down", PAD_BUTTON_DOWN, 0},
    {"D-Pad/Left", "D-pad Left", PAD_BUTTON_LEFT, 0},
    {"D-Pad/Right", "D-pad Right", PAD_BUTTON_RIGHT, 0},
    {"Triggers/L", "L", PAD_TRIGGER_L, 1},
    {"Triggers/R", "R", PAD_TRIGGER_R, 2},
    {"Triggers/L-Analog", "L analog", 0, 1},
    {"Triggers/R-Analog", "R analog", 0, 2},
}};

void Reload() noexcept;

// The pad library has PADBlockInput but no matching query, so the settings
// overlay reports its own state here.
void SetInputBlocked(bool blocked) noexcept;
bool InputBlocked() noexcept;

// Mix expression output into a freshly read status set. Call once per guest
// PADRead, after every other input source has been merged.
void Apply(PADStatus* statuses) noexcept;

std::string GetExpression(uint32_t port, size_t control) noexcept;
// Returns false and fills error if the text does not parse; the binding is
// left unchanged in that case.
bool SetExpression(uint32_t port, size_t control, const std::string& text, std::string& error) noexcept;

// True while the control's expression is above the press threshold.
bool IsActive(uint32_t port, size_t control) noexcept;

// The default Dolphin config location on Windows, then next to the executable.
std::string DefaultDolphinConfigPath() noexcept;

// Imports [GCPad<padIndex>] into the given port. Returns the number of controls
// imported, or -1 on failure with error filled.
int ImportDolphinConfig(const std::string& path, int padIndex, uint32_t port,
                        std::string& summary, std::string& error) noexcept;

} // namespace InputBindings
