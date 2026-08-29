#include "controller_mapping_wizard.h"
#include "runtime_config.h"
#include "runtime_log.h"

#include <imgui.h>
#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_joystick.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

namespace controller_mapping_wizard {
namespace {

using Clock = std::chrono::steady_clock;

constexpr int16_t kStickThreshold = 16000;
constexpr int16_t kTriggerThreshold = 10000;
constexpr auto kCaptureDebounce = std::chrono::milliseconds(350);

enum class StepKind {
    Button,  // button or single-direction hat press
    Trigger, // button press or axis pull
    Stick,   // axis motion in the prompted direction
};

struct Step {
    const char* mappingKey;
    const char* prompt;
    StepKind kind;
};

// Prompts describe what the control does in-game; the SDL fields land on the
// right GC controls through pad.cpp's "standard" defaults (Z lives on
// rightshoulder, L/R on the trigger axes).
constexpr std::array<Step, 16> kSteps = {{
    {"a", "Press the button for A (accelerate / select)", StepKind::Button},
    {"b", "Press the button for B (brake / back)", StepKind::Button},
    {"x", "Press the button for X", StepKind::Button},
    {"y", "Press the button for Y", StepKind::Button},
    {"start", "Press the button for pause (Start)", StepKind::Button},
    {"rightshoulder", "Press the button for rear view (Z)", StepKind::Button},
    {"lefttrigger", "Press or pull the control for using items (L)", StepKind::Trigger},
    {"righttrigger", "Press or pull the control for hop / drift (R)", StepKind::Trigger},
    {"dpup", "Press D-pad Up", StepKind::Button},
    {"dpdown", "Press D-pad Down", StepKind::Button},
    {"dpleft", "Press D-pad Left", StepKind::Button},
    {"dpright", "Press D-pad Right", StepKind::Button},
    {"leftx", "Move the Control Stick LEFT", StepKind::Stick},
    {"lefty", "Move the Control Stick UP", StepKind::Stick},
    {"rightx", "Move the C-Stick LEFT (or Skip)", StepKind::Stick},
    {"righty", "Move the C-Stick UP (or Skip)", StepKind::Stick},
}};

struct WizardState {
    bool active = false;
    SDL_JoystickID instance = 0;
    SDL_Joystick* joystick = nullptr;
    bool ownsJoystick = false;
    std::string deviceName;
    size_t stepIndex = 0;
    std::array<std::optional<std::string>, kSteps.size()> bindings{};
    std::vector<int16_t> axisBaseline;
    Clock::time_point acceptAfter{};
    std::string status;
};

WizardState g_wizard;

std::filesystem::path MappingDbPath() {
    return RuntimeConfigFile::ApplicationDataDirectory() / "gamecontrollerdb.txt";
}

std::string GuidString(SDL_JoystickID instance) {
    char buf[33] = {};
    SDL_GUIDToString(SDL_GetJoystickGUIDForID(instance), buf, sizeof(buf));
    return buf;
}

bool BindingUsed(const std::string& value) {
    return std::any_of(g_wizard.bindings.begin(), g_wizard.bindings.end(),
                       [&](const std::optional<std::string>& b) { return b && *b == value; });
}

void SnapshotAxes() {
    g_wizard.axisBaseline.clear();
    const int axes = SDL_GetNumJoystickAxes(g_wizard.joystick);
    for (int i = 0; i < axes; ++i) {
        g_wizard.axisBaseline.push_back(SDL_GetJoystickAxis(g_wizard.joystick, i));
    }
}

void AdvanceStep(std::optional<std::string> value) {
    g_wizard.bindings[g_wizard.stepIndex] = std::move(value);
    ++g_wizard.stepIndex;
    g_wizard.acceptAfter = Clock::now() + kCaptureDebounce;
    SnapshotAxes();
}

void StopWizard() {
    if (g_wizard.ownsJoystick && g_wizard.joystick != nullptr) {
        SDL_CloseJoystick(g_wizard.joystick);
    }
    g_wizard = WizardState{};
}

void StartWizard(SDL_JoystickID instance) {
    StopWizard();
    SDL_Joystick* joystick = nullptr;
    bool owns = false;
    if (SDL_Gamepad* gamepad = SDL_GetGamepadFromID(instance)) {
        joystick = SDL_GetGamepadJoystick(gamepad);
    } else {
        joystick = SDL_OpenJoystick(instance);
        owns = true;
    }
    if (joystick == nullptr) {
        RT_LOG(RT_TAG_CONFIG) << "controller wizard: failed to open joystick " << instance << ": "
                              << SDL_GetError() << std::endl;
        return;
    }
    g_wizard.active = true;
    g_wizard.instance = instance;
    g_wizard.joystick = joystick;
    g_wizard.ownsJoystick = owns;
    const char* name = SDL_GetJoystickNameForID(instance);
    g_wizard.deviceName = name != nullptr ? name : "Controller";
    g_wizard.acceptAfter = Clock::now() + kCaptureDebounce;
    SnapshotAxes();
}

std::string BuildMappingString() {
    std::string name = g_wizard.deviceName;
    std::replace(name.begin(), name.end(), ',', ' ');
    std::string mapping = GuidString(g_wizard.instance) + "," + name + ",";
    for (size_t i = 0; i < kSteps.size(); ++i) {
        if (g_wizard.bindings[i]) {
            mapping += std::string(kSteps[i].mappingKey) + ":" + *g_wizard.bindings[i] + ",";
        }
    }
    mapping += "platform:Windows,";
    return mapping;
}

bool PersistMapping(const std::string& guid, const std::string& mapping) {
    const std::filesystem::path path = MappingDbPath();
    std::vector<std::string> lines;
    {
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line)) {
            if (line.rfind(guid + ",", 0) != 0) {
                lines.push_back(line);
            }
        }
    }
    lines.push_back(mapping);
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    std::ofstream out(path, std::ios::trunc);
    if (!out) {
        return false;
    }
    for (const auto& line : lines) {
        out << line << '\n';
    }
    out.close();
    return static_cast<bool>(out);
}

void FinishWizard() {
    const std::string guid = GuidString(g_wizard.instance);
    const std::string mapping = BuildMappingString();
    if (SDL_AddGamepadMapping(mapping.c_str()) < 0) {
        g_wizard.status = std::string("Failed to apply mapping: ") + SDL_GetError();
        RT_LOG(RT_TAG_CONFIG) << "controller wizard: " << g_wizard.status << " (" << mapping << ")"
                              << std::endl;
        return;
    }
    if (!PersistMapping(guid, mapping)) {
        g_wizard.status =
            "Failed to save mapping to " + RuntimeConfigFile::PathToUtf8(MappingDbPath());
        RT_LOG(RT_TAG_CONFIG) << "controller wizard: " << g_wizard.status << std::endl;
        return;
    }
    RT_LOG(RT_TAG_CONFIG) << "controller wizard: applied mapping " << mapping << std::endl;
    StopWizard();
}

struct SetupCandidate {
    SDL_JoystickID id;
    std::string name;
    bool incompleteMapping;
};

// A device needs setup when SDL has no gamepad mapping for it at all, or when
// the mapping it matched has no analog stick even though the hardware reports
// axes (SDL's built-in raphnet WUSBMote entry is button-only).
std::vector<SetupCandidate> CollectCandidates() {
    std::vector<SetupCandidate> candidates;
    int count = 0;
    SDL_JoystickID* ids = SDL_GetJoysticks(&count);
    if (ids == nullptr) {
        return candidates;
    }
    for (int i = 0; i < count; ++i) {
        const SDL_JoystickID id = ids[i];
        const char* rawName = SDL_GetJoystickNameForID(id);
        const std::string name = rawName != nullptr ? rawName : "Unknown controller";
        if (!SDL_IsGamepad(id)) {
            candidates.push_back({id, name, false});
            continue;
        }
        SDL_Gamepad* gamepad = SDL_GetGamepadFromID(id);
        if (gamepad == nullptr) {
            continue;
        }
        char* mapping = SDL_GetGamepadMappingForID(id);
        if (mapping == nullptr) {
            continue;
        }
        const std::string mappingStr = mapping;
        SDL_free(mapping);
        const bool hasStick = mappingStr.find("leftx:") != std::string::npos &&
                              mappingStr.find("lefty:") != std::string::npos;
        SDL_Joystick* joystick = SDL_GetGamepadJoystick(gamepad);
        if (!hasStick && joystick != nullptr && SDL_GetNumJoystickAxes(joystick) >= 2) {
            candidates.push_back({id, name, true});
        }
    }
    SDL_free(ids);
    return candidates;
}

void HandleButtonDown(const SDL_JoyButtonEvent& event) {
    const Step& step = kSteps[g_wizard.stepIndex];
    if (step.kind == StepKind::Stick) {
        return;
    }
    const std::string value = "b" + std::to_string(event.button);
    if (BindingUsed(value)) {
        g_wizard.status = "That button is already bound";
        return;
    }
    g_wizard.status.clear();
    AdvanceStep(value);
}

void HandleHatMotion(const SDL_JoyHatEvent& event) {
    const Step& step = kSteps[g_wizard.stepIndex];
    if (step.kind == StepKind::Stick) {
        return;
    }
    // Only single-direction presses bind cleanly; diagonals are ignored.
    if (event.value != SDL_HAT_UP && event.value != SDL_HAT_RIGHT && event.value != SDL_HAT_DOWN &&
        event.value != SDL_HAT_LEFT) {
        return;
    }
    const std::string value =
        "h" + std::to_string(event.hat) + "." + std::to_string(static_cast<int>(event.value));
    if (BindingUsed(value)) {
        g_wizard.status = "That direction is already bound";
        return;
    }
    g_wizard.status.clear();
    AdvanceStep(value);
}

void HandleAxisMotion(const SDL_JoyAxisEvent& event) {
    const Step& step = kSteps[g_wizard.stepIndex];
    if (step.kind == StepKind::Button) {
        return;
    }
    if (event.axis >= g_wizard.axisBaseline.size()) {
        return;
    }
    const int32_t delta =
        static_cast<int32_t>(event.value) - static_cast<int32_t>(g_wizard.axisBaseline[event.axis]);
    const int16_t threshold = step.kind == StepKind::Stick ? kStickThreshold : kTriggerThreshold;
    if (std::abs(delta) < threshold) {
        return;
    }
    // Stick prompts ask for LEFT/UP, which SDL expects to be negative; triggers
    // are expected to increase when pulled. A wrong-way delta means the raw
    // axis is inverted, which the mapping expresses with a '~' suffix.
    const bool expectNegative = step.kind == StepKind::Stick;
    const bool inverted = expectNegative ? delta > 0 : delta < 0;
    std::string value = "a" + std::to_string(event.axis);
    // Reject reusing an axis already bound (with or without inversion).
    if (BindingUsed(value) || BindingUsed(value + "~")) {
        g_wizard.status = "That axis is already bound";
        return;
    }
    if (inverted) {
        value += "~";
    }
    g_wizard.status.clear();
    AdvanceStep(value);
}

} // namespace

void LoadPersistedMappings() {
    std::ifstream in(MappingDbPath());
    if (!in) {
        return;
    }
    std::string line;
    int added = 0;
    while (std::getline(in, line)) {
        const std::string trimmed = RuntimeConfigFile::Trim(line);
        if (trimmed.empty() || trimmed[0] == '#') {
            continue;
        }
        if (SDL_AddGamepadMapping(trimmed.c_str()) >= 0) {
            ++added;
        } else {
            RT_LOG(RT_TAG_CONFIG) << "gamecontrollerdb.txt: rejected mapping: " << trimmed
                                  << " (" << SDL_GetError() << ")" << std::endl;
        }
    }
    if (added > 0) {
        RT_LOG(RT_TAG_CONFIG) << "gamecontrollerdb.txt: applied " << added << " custom mapping"
                              << (added == 1 ? "" : "s") << std::endl;
    }
}

void HandleSdlEvent(const SDL_Event& event) {
    if (!g_wizard.active) {
        return;
    }
    if (event.type == SDL_EVENT_JOYSTICK_REMOVED && event.jdevice.which == g_wizard.instance) {
        StopWizard();
        return;
    }
    if (event.type == SDL_EVENT_KEY_DOWN && event.key.scancode == SDL_SCANCODE_ESCAPE) {
        StopWizard();
        return;
    }
    if (g_wizard.stepIndex >= kSteps.size() || Clock::now() < g_wizard.acceptAfter) {
        return;
    }
    switch (event.type) {
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
        if (event.jbutton.which == g_wizard.instance) {
            HandleButtonDown(event.jbutton);
        }
        break;
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
        if (event.jhat.which == g_wizard.instance) {
            HandleHatMotion(event.jhat);
        }
        break;
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
        if (event.jaxis.which == g_wizard.instance) {
            HandleAxisMotion(event.jaxis);
        }
        break;
    default:
        break;
    }
}

void DrawSetupList() {
    const std::vector<SetupCandidate> candidates = CollectCandidates();
    if (candidates.empty()) {
        return;
    }
    ImGui::SeparatorText("Unrecognized controllers");
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 380.0f);
    ImGui::TextDisabled(
        "These devices have no usable gamepad mapping. Set one up by pressing "
        "each control when asked.");
    ImGui::PopTextWrapPos();
    for (const auto& candidate : candidates) {
        ImGui::PushID(static_cast<int>(candidate.id));
        ImGui::TextUnformatted(candidate.name.c_str());
        ImGui::SameLine();
        if (ImGui::SmallButton(candidate.incompleteMapping ? "Fix mapping" : "Set up")) {
            StartWizard(candidate.id);
        }
        if (candidate.incompleteMapping && ImGui::IsItemHovered()) {
            ImGui::SetTooltip("SDL matched a mapping without an analog stick for this device");
        }
        ImGui::PopID();
    }
}

void Draw() {
    if (!g_wizard.active) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x + viewport->Size.x * 0.5f,
                                   viewport->Pos.y + viewport->Size.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(420.0f, 0.0f), ImGuiCond_Appearing);
    bool open = true;
    if (ImGui::Begin("Controller setup", &open,
                     ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(g_wizard.deviceName.c_str());
        ImGui::Separator();
        if (g_wizard.stepIndex < kSteps.size()) {
            ImGui::Text("Step %d of %d", static_cast<int>(g_wizard.stepIndex + 1),
                        static_cast<int>(kSteps.size()));
            ImGui::Spacing();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0f);
            ImGui::TextUnformatted(kSteps[g_wizard.stepIndex].prompt);
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            if (ImGui::Button("Skip")) {
                g_wizard.status.clear();
                AdvanceStep(std::nullopt);
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(g_wizard.stepIndex == 0);
            if (ImGui::Button("Back")) {
                --g_wizard.stepIndex;
                g_wizard.bindings[g_wizard.stepIndex].reset();
                g_wizard.status.clear();
                g_wizard.acceptAfter = Clock::now() + kCaptureDebounce;
                SnapshotAxes();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
        } else {
            const size_t boundCount =
                std::count_if(g_wizard.bindings.begin(), g_wizard.bindings.end(),
                              [](const std::optional<std::string>& b) { return b.has_value(); });
            ImGui::Text("Captured %d of %d controls.", static_cast<int>(boundCount),
                        static_cast<int>(kSteps.size()));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 400.0f);
            ImGui::TextDisabled("Save applies the mapping now and remembers it for future launches.");
            ImGui::PopTextWrapPos();
            ImGui::Spacing();
            ImGui::BeginDisabled(boundCount == 0);
            if (ImGui::Button("Save")) {
                FinishWizard();
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ImGui::Button("Back")) {
                --g_wizard.stepIndex;
                g_wizard.bindings[g_wizard.stepIndex].reset();
                g_wizard.acceptAfter = Clock::now() + kCaptureDebounce;
                SnapshotAxes();
            }
            ImGui::SameLine();
            if (ImGui::Button("Cancel")) {
                open = false;
            }
        }
        if (!g_wizard.status.empty()) {
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1.0f, 0.65f, 0.3f, 1.0f), "%s", g_wizard.status.c_str());
        }
    }
    ImGui::End();
    if (!open && g_wizard.active) {
        StopWizard();
    }
}

bool IsActive() { return g_wizard.active; }

} // namespace controller_mapping_wizard
