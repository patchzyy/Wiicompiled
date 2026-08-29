#pragma once

// The single vocabulary shared by everything that has to turn a Config.toml
// controller name into a real button: the F10 settings bar, the macro engine,
// and the startup mapping pass. Keeping one table here means a name that the
// settings bar offers is always a name the config parser accepts, and vice
// versa; the two used to drift because each side carried its own copy.

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include <SDL3/SDL_gamepad.h>
#include <dolphin/pad.h>

namespace ControllerNames {

// A GameCube button as the game sees it, with the Config.toml key that selects
// it. Order matches RuntimeConfigFile::kControllerButtonKeys.
struct GameCubeButtonItem {
    const char* configKey;
    const char* label;
    PADButton padButton;
};

inline constexpr std::array<GameCubeButtonItem, PAD_BUTTON_COUNT> kGameCubeButtons = {{
    {"a", "A", PAD_BUTTON_A},
    {"b", "B", PAD_BUTTON_B},
    {"x", "X", PAD_BUTTON_X},
    {"y", "Y", PAD_BUTTON_Y},
    {"start", "Start", PAD_BUTTON_START},
    {"z", "Z", PAD_TRIGGER_Z},
    {"l", "L", PAD_TRIGGER_L},
    {"r", "R", PAD_TRIGGER_R},
    {"up", "D-pad Up", PAD_BUTTON_UP},
    {"down", "D-pad Down", PAD_BUTTON_DOWN},
    {"left", "D-pad Left", PAD_BUTTON_LEFT},
    {"right", "D-pad Right", PAD_BUTTON_RIGHT},
}};

// A physical button on the host pad. Names are positional (south/east/...)
// rather than Xbox-labelled so one config reads the same on any hardware.
struct NativeButtonItem {
    const char* configName;
    const char* label;
    uint32_t nativeButton;
};

inline constexpr std::array<NativeButtonItem, SDL_GAMEPAD_BUTTON_COUNT + 1> kNativeButtons = {{
    {"unmapped", "Unmapped / analog trigger", PAD_NATIVE_BUTTON_INVALID},
    {"south", "South (A / Cross)", SDL_GAMEPAD_BUTTON_SOUTH},
    {"east", "East (B / Circle)", SDL_GAMEPAD_BUTTON_EAST},
    {"west", "West (X / Square)", SDL_GAMEPAD_BUTTON_WEST},
    {"north", "North (Y / Triangle)", SDL_GAMEPAD_BUTTON_NORTH},
    {"back", "Back / Select / Create", SDL_GAMEPAD_BUTTON_BACK},
    {"guide", "Guide / Home / PS", SDL_GAMEPAD_BUTTON_GUIDE},
    {"start", "Start / Options", SDL_GAMEPAD_BUTTON_START},
    {"left_stick", "Left stick click (L3)", SDL_GAMEPAD_BUTTON_LEFT_STICK},
    {"right_stick", "Right stick click (R3)", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
    {"left_shoulder", "Left bumper (LB / L1)", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
    {"right_shoulder", "Right bumper (RB / R1)", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
    {"dpad_up", "D-pad Up", SDL_GAMEPAD_BUTTON_DPAD_UP},
    {"dpad_down", "D-pad Down", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
    {"dpad_left", "D-pad Left", SDL_GAMEPAD_BUTTON_DPAD_LEFT},
    {"dpad_right", "D-pad Right", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
    {"misc1", "Misc 1 / Share / Mic", SDL_GAMEPAD_BUTTON_MISC1},
    {"right_paddle1", "Right paddle 1", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1},
    {"left_paddle1", "Left paddle 1", SDL_GAMEPAD_BUTTON_LEFT_PADDLE1},
    {"right_paddle2", "Right paddle 2", SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2},
    {"left_paddle2", "Left paddle 2", SDL_GAMEPAD_BUTTON_LEFT_PADDLE2},
    {"touchpad", "Touchpad click", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    {"misc2", "Misc 2", SDL_GAMEPAD_BUTTON_MISC2},
    {"misc3", "Misc 3 / GC L click", SDL_GAMEPAD_BUTTON_MISC3},
    {"misc4", "Misc 4 / GC R click", SDL_GAMEPAD_BUTTON_MISC4},
    {"misc5", "Misc 5", SDL_GAMEPAD_BUTTON_MISC5},
    {"misc6", "Misc 6", SDL_GAMEPAD_BUTTON_MISC6},
}};

inline std::string TrimToken(std::string_view token) {
    const size_t begin = token.find_first_not_of(" \t");
    if (begin == std::string_view::npos) {
        return {};
    }
    const size_t end = token.find_last_not_of(" \t");
    return std::string(token.substr(begin, end - begin + 1));
}

inline const NativeButtonItem* FindNativeButton(std::string_view configName) {
    const std::string name = TrimToken(configName);
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(),
                                 [&](const NativeButtonItem& item) { return name == item.configName; });
    return it == kNativeButtons.end() ? nullptr : &*it;
}

// Falls back to the "unmapped" entry so callers always have a label to draw.
inline const NativeButtonItem& NativeButtonForValue(uint32_t nativeButton) {
    const auto it = std::find_if(kNativeButtons.begin(), kNativeButtons.end(),
                                 [&](const NativeButtonItem& item) { return nativeButton == item.nativeButton; });
    return it == kNativeButtons.end() ? kNativeButtons.front() : *it;
}

inline const GameCubeButtonItem* FindGameCubeButton(std::string_view configKey) {
    const std::string key = TrimToken(configKey);
    const auto it = std::find_if(kGameCubeButtons.begin(), kGameCubeButtons.end(),
                                 [&](const GameCubeButtonItem& item) { return key == item.configKey; });
    return it == kGameCubeButtons.end() ? nullptr : &*it;
}

// "up" or "up,a" -> the OR of those GC button bits. Unknown names are skipped so
// a typo costs one button instead of the whole macro.
inline uint16_t GameCubeMaskFromKeys(std::string_view keys) {
    uint16_t mask = 0;
    size_t begin = 0;
    while (begin <= keys.size()) {
        const size_t comma = keys.find(',', begin);
        const std::string_view token =
            keys.substr(begin, comma == std::string_view::npos ? std::string_view::npos : comma - begin);
        if (const GameCubeButtonItem* item = FindGameCubeButton(token)) {
            mask |= static_cast<uint16_t>(item->padButton);
        }
        if (comma == std::string_view::npos) {
            break;
        }
        begin = comma + 1;
    }
    return mask;
}

inline std::string GameCubeKeysFromMask(uint16_t mask) {
    std::string keys;
    for (const auto& item : kGameCubeButtons) {
        if ((mask & static_cast<uint16_t>(item.padButton)) == 0) {
            continue;
        }
        if (!keys.empty()) {
            keys += ',';
        }
        keys += item.configKey;
    }
    return keys;
}

inline std::string GameCubeLabelsFromMask(uint16_t mask) {
    std::string labels;
    for (const auto& item : kGameCubeButtons) {
        if ((mask & static_cast<uint16_t>(item.padButton)) == 0) {
            continue;
        }
        if (!labels.empty()) {
            labels += " + ";
        }
        labels += item.label;
    }
    return labels.empty() ? std::string("None") : labels;
}

} // namespace ControllerNames
