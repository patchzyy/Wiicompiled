#include "input_bindings.h"

#include "controller_button_names.h"
#include "input_expr.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <unordered_map>

#include <SDL3/SDL_gamepad.h>

namespace InputBindings {
namespace {

struct Binding {
    std::string text;
    InputExpr::Expression expr;
    bool active = false;
};

std::mutex g_mutex;
std::array<std::array<Binding, kControls.size()>, PAD_CHANMAX> g_bindings;
bool g_anyBound = false;
bool g_inputBlocked = false;

// Dolphin input names, mapped onto SDL. XInput-style names are exact; DInput
// "Button <n>" indices follow the common PlayStation layout, which is what
// DInput reports for a DualShock/DualSense. Other pads may number differently.
const std::unordered_map<std::string, SDL_GamepadButton>& ButtonNames() {
    static const std::unordered_map<std::string, SDL_GamepadButton> table = {
        {"Button A", SDL_GAMEPAD_BUTTON_SOUTH},      {"Button B", SDL_GAMEPAD_BUTTON_EAST},
        {"Button X", SDL_GAMEPAD_BUTTON_WEST},       {"Button Y", SDL_GAMEPAD_BUTTON_NORTH},
        {"Shoulder L", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"Shoulder R", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"Thumb L", SDL_GAMEPAD_BUTTON_LEFT_STICK},  {"Thumb R", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"Start", SDL_GAMEPAD_BUTTON_START},         {"Back", SDL_GAMEPAD_BUTTON_BACK},
        {"Guide", SDL_GAMEPAD_BUTTON_GUIDE},
        {"Pad N", SDL_GAMEPAD_BUTTON_DPAD_UP},       {"Pad S", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"Pad W", SDL_GAMEPAD_BUTTON_DPAD_LEFT},     {"Pad E", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"Hat 0 N", SDL_GAMEPAD_BUTTON_DPAD_UP},     {"Hat 0 S", SDL_GAMEPAD_BUTTON_DPAD_DOWN},
        {"Hat 0 W", SDL_GAMEPAD_BUTTON_DPAD_LEFT},   {"Hat 0 E", SDL_GAMEPAD_BUTTON_DPAD_RIGHT},
        {"Button 0", SDL_GAMEPAD_BUTTON_WEST},       {"Button 1", SDL_GAMEPAD_BUTTON_SOUTH},
        {"Button 2", SDL_GAMEPAD_BUTTON_EAST},       {"Button 3", SDL_GAMEPAD_BUTTON_NORTH},
        {"Button 4", SDL_GAMEPAD_BUTTON_LEFT_SHOULDER},
        {"Button 5", SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER},
        {"Button 8", SDL_GAMEPAD_BUTTON_BACK},       {"Button 9", SDL_GAMEPAD_BUTTON_START},
        {"Button 10", SDL_GAMEPAD_BUTTON_LEFT_STICK},
        {"Button 11", SDL_GAMEPAD_BUTTON_RIGHT_STICK},
        {"Button 12", SDL_GAMEPAD_BUTTON_GUIDE},     {"Button 13", SDL_GAMEPAD_BUTTON_TOUCHPAD},
    };
    return table;
}

// Signed axis names: SDL axis plus the direction that counts as positive.
struct AxisRef {
    SDL_GamepadAxis axis;
    int sign;
};

const std::unordered_map<std::string, AxisRef>& AxisNames() {
    static const std::unordered_map<std::string, AxisRef> table = {
        {"Axis X-", {SDL_GAMEPAD_AXIS_LEFTX, -1}},  {"Axis X+", {SDL_GAMEPAD_AXIS_LEFTX, 1}},
        {"Axis Y-", {SDL_GAMEPAD_AXIS_LEFTY, -1}},  {"Axis Y+", {SDL_GAMEPAD_AXIS_LEFTY, 1}},
        {"Axis Z-", {SDL_GAMEPAD_AXIS_RIGHTX, -1}}, {"Axis Z+", {SDL_GAMEPAD_AXIS_RIGHTX, 1}},
        {"Axis Zr-", {SDL_GAMEPAD_AXIS_RIGHTY, -1}},{"Axis Zr+", {SDL_GAMEPAD_AXIS_RIGHTY, 1}},
        {"Left X-", {SDL_GAMEPAD_AXIS_LEFTX, -1}},  {"Left X+", {SDL_GAMEPAD_AXIS_LEFTX, 1}},
        {"Left Y-", {SDL_GAMEPAD_AXIS_LEFTY, 1}},   {"Left Y+", {SDL_GAMEPAD_AXIS_LEFTY, -1}},
        {"Right X-", {SDL_GAMEPAD_AXIS_RIGHTX, -1}},{"Right X+", {SDL_GAMEPAD_AXIS_RIGHTX, 1}},
        {"Right Y-", {SDL_GAMEPAD_AXIS_RIGHTY, 1}}, {"Right Y+", {SDL_GAMEPAD_AXIS_RIGHTY, -1}},
        {"Full Axis Xr+", {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1}},
        {"Full Axis Yr+", {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1}},
        {"Trigger L", {SDL_GAMEPAD_AXIS_LEFT_TRIGGER, 1}},
        {"Trigger R", {SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 1}},
    };
    return table;
}

double ReadInput(SDL_Gamepad* gamepad, const std::string& name) {
    if (gamepad == nullptr) {
        return 0.0;
    }
    if (const auto it = ButtonNames().find(name); it != ButtonNames().end()) {
        return SDL_GetGamepadButton(gamepad, it->second) ? 1.0 : 0.0;
    }
    if (const auto it = AxisNames().find(name); it != AxisNames().end()) {
        const double raw = SDL_GetGamepadAxis(gamepad, it->second.axis) / 32767.0;
        return std::clamp(raw * it->second.sign, 0.0, 1.0);
    }
    // Fall back to this project's own positional names, so a binding written
    // here does not have to use Dolphin vocabulary.
    if (const auto* native = ControllerNames::FindNativeButton(name)) {
        if (native->nativeButton != PAD_NATIVE_BUTTON_INVALID) {
            return SDL_GetGamepadButton(gamepad, static_cast<SDL_GamepadButton>(native->nativeButton)) ? 1.0
                                                                                                      : 0.0;
        }
    }
    return 0.0;
}

SDL_Gamepad* GamepadForPort(uint32_t port) {
    const s32 index = PADGetIndexForPort(port);
    return index < 0 ? nullptr : PADGetSDLGamepadForIndex(static_cast<u32>(index));
}

size_t ControlIndexForDolphinName(const std::string& name) {
    for (size_t i = 0; i < kControls.size(); ++i) {
        if (name == kControls[i].dolphinName) {
            return i;
        }
    }
    return kControls.size();
}

void RecomputeAnyBoundLocked() {
    g_anyBound = false;
    for (const auto& port : g_bindings) {
        for (const auto& binding : port) {
            if (!binding.expr.Empty()) {
                g_anyBound = true;
                return;
            }
        }
    }
}

std::string ConfigKey(uint32_t port, size_t control) {
    std::string key = "expr_" + std::to_string(port + 1) + "_";
    for (const char* c = kControls[control].dolphinName; *c != '\0'; ++c) {
        key += (*c == '/' || *c == '-') ? '_' : static_cast<char>(std::tolower(*c));
    }
    return key;
}

} // namespace

void SetInputBlocked(bool blocked) noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_inputBlocked = blocked;
}

bool InputBlocked() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_inputBlocked;
}

void Reload() noexcept {
    std::lock_guard<std::mutex> lock(g_mutex);
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        for (size_t control = 0; control < kControls.size(); ++control) {
            Binding& binding = g_bindings[port][control];
            binding = Binding{};
            binding.text = RuntimeConfigFile::ControllerExpression(ConfigKey(port, control));
            std::string error;
            if (!binding.text.empty() &&
                !InputExpr::Expression::Parse(binding.text, binding.expr, error)) {
                RT_LOG(RT_TAG_CONFIG) << "expression for port " << (port + 1) << " "
                                      << kControls[control].dolphinName << ": " << error << std::endl;
            }
        }
    }
    RecomputeAnyBoundLocked();
}

void Apply(PADStatus* statuses) noexcept {
    if (statuses == nullptr) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_anyBound) {
        return;
    }
    const bool blocked = g_inputBlocked;
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        if (statuses[port].err != PAD_ERR_NONE) {
            continue;
        }
        SDL_Gamepad* gamepad = GamepadForPort(port);
        const InputExpr::InputSource source = [gamepad](const std::string& name) {
            return ReadInput(gamepad, name);
        };
        for (size_t control = 0; control < kControls.size(); ++control) {
            Binding& binding = g_bindings[port][control];
            if (binding.expr.Empty()) {
                continue;
            }
            if (blocked) {
                binding.active = false;
                continue;
            }
            const double value = binding.expr.Evaluate(source);
            binding.active = value > InputExpr::kConditionThreshold;
            const ControlInfo& info = kControls[control];
            if (info.padButton != 0 && binding.active) {
                statuses[port].button |= info.padButton;
            }
            if (info.analog != 0) {
                const double safe = std::isfinite(value) ? std::clamp(value, 0.0, 1.0) : 0.0;
                const auto scaled = static_cast<uint8_t>(safe * 255.0);
                uint8_t& target =
                    info.analog == 1 ? statuses[port].triggerLeft : statuses[port].triggerRight;
                target = std::max(target, scaled);
            }
        }
    }
}

std::string GetExpression(uint32_t port, size_t control) noexcept {
    if (port >= PAD_CHANMAX || control >= kControls.size()) {
        return {};
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bindings[port][control].text;
}

bool SetExpression(uint32_t port, size_t control, const std::string& text, std::string& error) noexcept {
    if (port >= PAD_CHANMAX || control >= kControls.size()) {
        error = "invalid control";
        return false;
    }
    InputExpr::Expression parsed;
    if (!InputExpr::Expression::Parse(text, parsed, error)) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        Binding& binding = g_bindings[port][control];
        binding.text = text;
        binding.expr = std::move(parsed);
        binding.active = false;
        RecomputeAnyBoundLocked();
    }
    RuntimeConfigFile::SetControllerExpression(ConfigKey(port, control), text);
    return true;
}

bool IsActive(uint32_t port, size_t control) noexcept {
    if (port >= PAD_CHANMAX || control >= kControls.size()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_bindings[port][control].active;
}

std::string DefaultDolphinConfigPath() noexcept {
    std::error_code ec;
    if (const char* appdata = std::getenv("APPDATA"); appdata != nullptr) {
        const std::filesystem::path roaming =
            std::filesystem::path(appdata) / "Dolphin Emulator" / "Config" / "GCPadNew.ini";
        if (std::filesystem::exists(roaming, ec)) {
            return RuntimeConfigFile::PathToUtf8(roaming);
        }
    }
    const auto executableDirectory = RuntimeConfigFile::ExecutableDirectory();
    return RuntimeConfigFile::PathToUtf8(executableDirectory ? *executableDirectory / "GCPadNew.ini"
                                                             : std::filesystem::path("GCPadNew.ini"));
}

int ImportDolphinConfig(const std::string& path, int padIndex, uint32_t port, std::string& summary,
                        std::string& error) noexcept {
    std::vector<std::pair<std::string, std::string>> controls;
    std::string device;
    if (!InputExpr::ReadDolphinConfig(RuntimeConfigFile::PathFromUtf8(path), padIndex, controls, device,
                                      error)) {
        return -1;
    }

    int imported = 0;
    std::vector<std::string> skipped;
    for (const auto& [name, text] : controls) {
        const size_t control = ControlIndexForDolphinName(name);
        if (control == kControls.size()) {
            if (name.rfind("Main Stick/", 0) == 0 || name.rfind("C-Stick/", 0) == 0) {
                skipped.push_back(name);
            }
            continue;
        }
        std::string parseError;
        if (!SetExpression(port, control, text, parseError)) {
            skipped.push_back(name);
            RT_LOG(RT_TAG_CONFIG) << "import " << name << ": " << parseError << std::endl;
            continue;
        }
        ++imported;
    }

    summary = "Imported " + std::to_string(imported) + " controls";
    if (!device.empty()) {
        summary += " from " + device;
    }
    if (!skipped.empty()) {
        summary += "; skipped " + std::to_string(skipped.size()) +
                   " (stick axes and unsupported inputs keep their existing mapping)";
    }
    return imported;
}

} // namespace InputBindings
